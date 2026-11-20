<#
.SYNOPSIS
  Partial clean-machine proxy for the single-EXE gate (AGENTS.md M7, ADR-008): runs HCSInfer.exe copied ALONE
  into a fresh directory with an EMPTY PATH and a minimal environment (no Visual Studio, no Vulkan SDK, no
  repository directory), then checks which DLLs the live process actually loaded.

.DESCRIPTION
  This is NOT a clean-machine test: the host still has the VC++ redistributable, the Vulkan driver and the
  developer tools installed, they are merely not reachable through PATH or the application directory. It does
  prove that the EXE needs no sidecar file and no PATH entry, and that every DLL it loads is a Windows system
  DLL. A real clean-machine run (fresh VM or Windows Sandbox) remains a separate, NOT RUN gate.

  Read-only with respect to the system: it creates one temp directory and removes only that directory.
  Changes no setting, enables no Windows feature, needs no administrator rights.

.PARAMETER Exe          Path to HCSInfer.exe (the build under test).
.PARAMETER Model        Optional GGUF for a real CPU generation (skipped = NOT RUN when omitted).
.PARAMETER OutJson      Where to write the machine-readable result.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Exe,
    [string] $Model = '',
    [Parameter(Mandatory)] [string] $OutJson
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Exe = (Resolve-Path -LiteralPath $Exe).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("hcs-restricted-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$work = (Resolve-Path -LiteralPath $work).Path
$appDir = Join-Path $work 'app'
$state = Join-Path $work 'state'
New-Item -ItemType Directory -Path $appDir, $state | Out-Null
Copy-Item -LiteralPath $Exe -Destination (Join-Path $appDir 'HCSInfer.exe')
$exeCopy = Join-Path $appDir 'HCSInfer.exe'

$results = [ordered]@{
    schema_version = 1
    timestamp_utc  = (Get-Date).ToUniversalTime().ToString('o')
    exe_source     = $Exe
    exe_sha256     = (Get-FileHash -LiteralPath $exeCopy -Algorithm SHA256).Hash.ToLower()
    exe_bytes      = (Get-Item -LiteralPath $exeCopy).Length
    files_next_to_exe = @((Get-ChildItem -LiteralPath $appDir | ForEach-Object Name))
    environment    = 'PATH empty; only SystemRoot/windir/TEMP/TMP/HCS_STATE_DIR set; cwd = a fresh temp dir'
    checks         = @()
}

function New-RestrictedPsi([string] $args_) {
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $exeCopy
    $psi.Arguments = $args_
    $psi.WorkingDirectory = $work
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.Environment.Clear()
    $psi.Environment['SystemRoot'] = $env:SystemRoot
    $psi.Environment['windir'] = $env:SystemRoot
    $psi.Environment['TEMP'] = $work
    $psi.Environment['TMP'] = $work
    $psi.Environment['HCS_STATE_DIR'] = $state
    $psi.Environment['PATH'] = ''
    return $psi
}

function Invoke-Restricted([string] $name, [string] $args_, [int] $timeoutSec = 120) {
    $psi = New-RestrictedPsi $args_
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $so = $p.StandardOutput.ReadToEndAsync()
    $se = $p.StandardError.ReadToEndAsync()
    $done = $p.WaitForExit($timeoutSec * 1000)
    if (-not $done) { $p.Kill($true); $p.WaitForExit() }
    $rec = [ordered]@{
        name      = $name
        args      = $args_
        exit_code = $(if ($done) { $p.ExitCode } else { $null })
        timed_out = (-not $done)
        seconds   = [math]::Round($sw.Elapsed.TotalSeconds, 2)
        stdout    = ($so.Result.Trim())
        stderr    = ($se.Result.Trim())
    }
    $script:lastStdout = $rec.stdout
    if ($rec.stdout.Length -gt 1500) { $rec.stdout = $rec.stdout.Substring(0, 1500) + ' ...[truncated]' }
    if ($rec.stderr.Length -gt 1500) { $rec.stderr = $rec.stderr.Substring(0, 1500) + ' ...[truncated]' }
    $script:results.checks += $rec
    return $rec
}

try {
    # 0. Prove the restriction itself: a system tool launched with the same environment sees an empty PATH.
    $cmdPsi = [Diagnostics.ProcessStartInfo]::new()
    $cmdPsi.FileName = (Join-Path $env:SystemRoot 'System32\cmd.exe')
    $cmdPsi.Arguments = '/c "echo [%PATH%]"'
    $cmdPsi.UseShellExecute = $false; $cmdPsi.RedirectStandardOutput = $true; $cmdPsi.CreateNoWindow = $true
    $cmdPsi.Environment.Clear(); $cmdPsi.Environment['SystemRoot'] = $env:SystemRoot; $cmdPsi.Environment['PATH'] = ''
    $cp = [Diagnostics.Process]::Start($cmdPsi); $pathSeen = $cp.StandardOutput.ReadToEnd().Trim(); $cp.WaitForExit()
    $results['path_seen_by_child'] = $pathSeen + '  (cmd.exe prints the literal %PATH% when the variable is not defined)'

    # 1. Startup, help, diagnostics.
    $r = Invoke-Restricted 'help' '--help'
    $r = Invoke-Restricted 'doctor_json' 'doctor --json'
    $results['doctor_valid_json'] = $false
    try { $null = ($script:lastStdout | ConvertFrom-Json); $results['doctor_valid_json'] = $true } catch { }

    # 2. Real CPU generation (only with a model).
    if ($Model) {
        $m = (Resolve-Path -LiteralPath $Model).Path
        $r = Invoke-Restricted 'chat_cpu_real' ("chat --model `"$m`" --backend cpu --ctx 1024 --max-tokens 12 --temp 0 --json --prompt `"Reply with exactly: static ok`"") 300
    } else {
        $results.checks += [ordered]@{ name = 'chat_cpu_real'; status = 'NOT RUN (no -Model given)' }
    }

    # 3. serve: health over loopback, then inspect the modules the live process loaded.
    $port = Get-Random -Minimum 20000 -Maximum 40000
    $psi = New-RestrictedPsi ("serve --port $port --token restricted-env-token-0123456789")
    $srv = [Diagnostics.Process]::Start($psi)
    $health = $null
    for ($i = 0; $i -lt 60 -and -not $srv.HasExited; $i++) {
        Start-Sleep -Milliseconds 250
        try { $health = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$port/hcs/v1/health" -TimeoutSec 2; break } catch { }
    }
    $mods = @()
    if (-not $srv.HasExited) {
        $srv.Refresh()
        $mods = @($srv.Modules | ForEach-Object { $_.FileName })
    }
    $sysRoot = $env:SystemRoot.TrimEnd('\')
    $nonSystem = @($mods | Where-Object { -not $_.StartsWith($sysRoot, [StringComparison]::OrdinalIgnoreCase) -and $_ -ne $exeCopy })
    $results['serve'] = [ordered]@{
        started          = -not $srv.HasExited
        health_status    = $(if ($health) { [int]$health.StatusCode } else { $null })
        modules_loaded   = $mods.Count
        modules_outside_windows_dir = $nonSystem
        windows_modules  = @($mods | Where-Object { $_.StartsWith($sysRoot, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Split-Path $_ -Leaf } | Sort-Object -Unique)
    }
    if (-not $srv.HasExited) { $srv.Kill($true); $srv.WaitForExit() }   # only the process this script started
}
finally {
    $results['verdict'] = 'see checks; PASS needs: exit_code 0 for help/doctor, health 200, modules_outside_windows_dir empty'
    $results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutJson -Encoding UTF8
    # Remove only the directory this script created.
    if ($work -and (Split-Path $work -Leaf) -like 'hcs-restricted-*' -and (Test-Path -LiteralPath $work)) {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}
Write-Host "wrote $OutJson"
