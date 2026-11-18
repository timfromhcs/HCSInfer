<#
.SYNOPSIS
  Real-model reliability soak (AGENTS.md 16.1G, M10): load/unload cycles, continuous generation, vision and
  diffusion repeats, cancellation storms, shutdown while loading/generating, HDD-unplug fault injection, and an
  HTTP disconnect storm against `HCSInfer serve`. Read-only with respect to the model drive: models are only read;
  the unplug scenarios use fault injection and private temp copies, never the real drive.

.PARAMETER BuildDir     CMake build dir holding bin/hcs_test_soak_core.exe and bin/HCSInfer.exe (default build/soak).
.PARAMETER ModelRoot    Folder with LFM3-Omni-2B Q8_0 (+mmproj) and SD1.5 Q8_0 (default D:\HCSModels).
.PARAMETER Profile      quick (minutes) | full (100 cycles, 30 min generation, ...). Default quick.
.PARAMETER Out          Evidence directory (default docs/test-evidence/soak/<label>).
.PARAMETER Label        Run label, e.g. cpu or vulkan.
.PARAMETER Only         Optional gtest filter suffix list, e.g. LoadUnloadCycles,CancelStorm (default all). 'HttpStorm' selects the HTTP phase.
#>
param(
    [string]$BuildDir = 'build/soak',
    [string]$ModelRoot = 'D:\HCSModels',
    [ValidateSet('quick', 'full')][string]$Profile = 'quick',
    [string]$Out = '',
    [string]$Label = 'cpu',
    [string[]]$Only = @()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bd = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }
$exeTest = Join-Path $bd 'bin/hcs_test_soak_core.exe'
$exeApp = Join-Path $bd 'bin/HCSInfer.exe'
if (-not $Out) { $Out = Join-Path $repo "docs/test-evidence/soak/$Label" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path   # evidence dir only; nothing outside it is deleted

foreach ($p in @($exeTest, $exeApp)) {
    if (-not (Test-Path $p)) { throw "missing $p - build targets hcs_test_soak_core and hcsinfer first" }
}
if (-not (Test-Path $ModelRoot)) { throw "model root not found: $ModelRoot (BLOCKED)" }

# knobs per profile (docs/soak.md)
$k = if ($Profile -eq 'full') {
    @{ HCS_SOAK_CYCLES = 100; HCS_SOAK_SWITCH_CYCLES = 20; HCS_SOAK_GEN_SECONDS = 1800; HCS_SOAK_VISION_RUNS = 30;
       HCS_SOAK_IMAGE_RUNS = 20; HCS_SOAK_CANCEL_ROUNDS = 60; HCS_SOAK_SHUTDOWN_ROUNDS = 5; HCS_SOAK_UNPLUG_ROUNDS = 100; Storm = 40 }
} else {
    @{ HCS_SOAK_CYCLES = 8; HCS_SOAK_SWITCH_CYCLES = 3; HCS_SOAK_GEN_SECONDS = 60; HCS_SOAK_VISION_RUNS = 4;
       HCS_SOAK_IMAGE_RUNS = 3; HCS_SOAK_CANCEL_ROUNDS = 10; HCS_SOAK_SHUTDOWN_ROUNDS = 2; HCS_SOAK_UNPLUG_ROUNDS = 5; Storm = 12 }
}
foreach ($n in $k.Keys) { if ($n -ne 'Storm') { Set-Item -Path "Env:$n" -Value ([string]$k[$n]) } }
$env:HCS_SOAK_MODEL_ROOT = $ModelRoot
$env:HCS_SOAK_OUT = $Out

$meta = [ordered]@{
    schema_version = 1; label = $Label; profile = $Profile
    timestamp_utc = (Get-Date).ToUniversalTime().ToString('o')
    git_commit = (git -C $repo rev-parse --short=12 HEAD).Trim()
    git_dirty = [bool](git -C $repo status --porcelain --untracked-files=no)
    os = (Get-CimInstance Win32_OperatingSystem).Caption + ' ' + (Get-CimInstance Win32_OperatingSystem).BuildNumber
    cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name)
    ram_bytes = [int64](Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
    exe_test = $exeTest; knobs = $k
}
$meta | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $Out 'run-meta.json')

# order matters little; heavy ones last so a failure early is cheap
$cases = @('UnplugGgufParseFaultInjection', 'UnplugTruncatedHcsModelStream', 'UnplugEngineLoadTruncatedAndVanishedFile',
           'LoadUnloadCycles', 'ModelSwitchCycles', 'VisionRepeat', 'DiffusionRepeat', 'CancelStorm',
           'ShutdownWhileLoading', 'ShutdownWhileGenerating', 'ShutdownWhileRenderingImage', 'ContinuousGeneration')
if ($Only.Count -gt 0) { $cases = $cases | Where-Object { $Only -contains $_ } }

$results = @()
foreach ($c in $cases) {
    Write-Host "=== Soak.$c ($Label/$Profile) ==="
    $log = Join-Path $Out "$c.log"
    $t0 = Get-Date
    & $exeTest "--gtest_filter=Soak.$c" "--gtest_output=json:$(Join-Path $Out "$c.gtest.json")" 2>&1 | Out-File -Encoding utf8 $log
    $rc = $LASTEXITCODE
    $dur = ((Get-Date) - $t0).TotalSeconds
    $status = if ($rc -eq 0) { 'PASS' } else { 'FAIL' }
    if ($rc -eq 0 -and (Select-String -Path $log -Pattern 'NOT RUN' -Quiet)) { $status = 'NOT RUN' }
    Write-Host ("    {0}  rc={1}  {2:N0}s" -f $status, $rc, $dur)
    $results += [ordered]@{ test = "Soak.$c"; status = $status; exit_code = $rc; seconds = [math]::Round($dur, 1); log = "$c.log" }
}

if ($Only.Count -eq 0 -or $Only -contains 'HttpStorm') {
    Write-Host "=== HTTP storm ($Label/$Profile) ==="
    $py = (Get-Command python -ErrorAction SilentlyContinue)
    if (-not $py) {
        $results += [ordered]@{ test = 'HttpStorm'; status = 'BLOCKED'; exit_code = 77; seconds = 0; log = 'python not found' }
    } else {
        $log = Join-Path $Out 'http-storm.log'
        $t0 = Get-Date
        & $py.Source -I (Join-Path $repo 'tests/soak/http_storm/http_storm.py') --exe $exeApp --model-root $ModelRoot `
            --out $Out --rounds $k.Storm 2>&1 | Out-File -Encoding utf8 $log
        $rc = $LASTEXITCODE
        $status = switch ($rc) { 0 { 'PASS' } 77 { 'BLOCKED' } default { 'FAIL' } }
        $results += [ordered]@{ test = 'HttpStorm'; status = $status; exit_code = $rc; seconds = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1); log = 'http-storm.log' }
        Write-Host ("    {0}  rc={1}" -f $status, $rc)
    }
}

$summary = [ordered]@{ meta = $meta; results = $results }
$summary | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $Out 'summary.json')
$results | ForEach-Object { '{0,-52} {1,-8} {2,8}s' -f $_.test, $_.status, $_.seconds }
if ($results | Where-Object { $_.status -eq 'FAIL' }) { exit 1 }
exit 0
