<#
.SYNOPSIS  Proves a command did not grow the known cache directories on drive C: ("nothing on C:" guard).
.DESCRIPTION
  Records the free space of C: and the size of the known per-user cache directories before and after running
  <Exe> <ArgString>. Exits 1 when the summed growth of those directories exceeds -MaxGrowthMB (default 50).
  Free space of C: is reported as well, but it is system-wide (other processes, page file, parallel jobs), so it is
  informational unless -StrictFree is passed. Read-only: it never deletes anything. Prints one JSON document to stdout
  (and to -OutFile when given). Exit codes: 0 PASS, 1 FAIL (growth), 2 command failed / bad usage, 77 NOT RUN (no C:).
.EXAMPLE   .\scripts\check-no-c-writes.ps1 -Exe build\windows-msvc-debug\bin\HCSInfer.exe -ArgString 'doctor --json'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Exe,
    [string]$ArgString = '',
    [int]$MaxGrowthMB = 50,
    [switch]$StrictFree,
    [string[]]$ExcludeDirName = @('claude'),   # directory NAMES skipped at any depth: the agent harness's own scratch
    [string[]]$ExtraWatchDir = @(),   # additional directories to watch (any drive; used by the self-test)
    [string]$OutFile = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-DirBytes([string]$Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return [int64]0 }
    # robocopy /L only counts (copies nothing); it is far faster than Get-ChildItem on a 100k-file Temp directory.
    $rcArgs = @($Path, 'NUL', '/L', '/S', '/XJ', '/NJH', '/NFL', '/NDL', '/NP', '/BYTES', '/R:0', '/W:0')
    if ($ExcludeDirName.Count -gt 0) { $rcArgs += '/XD'; $rcArgs += $ExcludeDirName }
    $out = & robocopy @rcArgs 2>$null
    foreach ($line in $out) {
        if ($line -match '^\s*Bytes\s*:\s*(\d+)') { return [int64]$Matches[1] }
    }
    return [int64]0
}

function Get-Watched {
    # Real per-user locations on C: (not the process TEMP, which env-d-e.ps1 may have redirected to D:).
    $la = [Environment]::GetEnvironmentVariable('LOCALAPPDATA')
    $ad = [Environment]::GetEnvironmentVariable('APPDATA')
    $up = [Environment]::GetEnvironmentVariable('USERPROFILE')
    $w = [ordered]@{}
    if ($la) {
        $w['localappdata_temp'] = Join-Path $la 'Temp'
        $w['pip']               = Join-Path $la 'pip'
        $w['hcsinfer_state']    = Join-Path $la 'HCSInfer'
    }
    if ($ad) { $w['npm_cache'] = Join-Path $ad 'npm-cache' }
    if ($up) {
        $w['cargo']        = Join-Path $up '.cargo'
        $w['hf_cache']     = Join-Path $up '.cache\huggingface'
        $w['torch_cache']  = Join-Path $up '.cache\torch'
    }
    foreach ($k in @($w.Keys)) {
        if (-not $w[$k].StartsWith('C:', [System.StringComparison]::OrdinalIgnoreCase)) { $w.Remove($k) }
    }
    return $w
}

function Get-Snapshot($watched) {
    $drive = Get-PSDrive -Name C -ErrorAction SilentlyContinue
    $dirs = [ordered]@{}
    foreach ($k in $watched.Keys) { $dirs[$k] = Get-DirBytes $watched[$k] }
    return [pscustomobject]@{ c_free_bytes = [int64]$drive.Free; dir_bytes = $dirs }
}

if (-not (Get-PSDrive -Name C -ErrorAction SilentlyContinue)) {
    Write-Output '{"result":"NOT RUN","reason":"no drive C: on this machine"}'
    exit 77
}
if (-not (Test-Path -LiteralPath $Exe)) {
    [Console]::Error.WriteLine("check-no-c-writes: executable not found: $Exe")
    exit 2
}

$watched = Get-Watched
foreach ($extra in $ExtraWatchDir) { $watched['extra_' + $watched.Count] = $extra }
$before = Get-Snapshot $watched
$cmdArgs = if ($ArgString.Trim()) { $ArgString.Trim() -split '\s+' } else { @() }
$null = & $Exe @cmdArgs 2>&1
$cmdExit = $LASTEXITCODE
$after = Get-Snapshot $watched

$growth = [ordered]@{}
$total = [int64]0
foreach ($k in $watched.Keys) {
    $d = [int64]($after.dir_bytes[$k] - $before.dir_bytes[$k])
    $growth[$k] = $d
    if ($d -gt 0) { $total += $d }
}
$freeDrop = [int64]($before.c_free_bytes - $after.c_free_bytes)
$limit = [int64]$MaxGrowthMB * 1MB
$failed = ($total -gt $limit) -or ($StrictFree -and $freeDrop -gt $limit)
$result = if ($cmdExit -ne 0) { 'ERROR' } elseif ($failed) { 'FAIL' } else { 'PASS' }

$report = [ordered]@{
    schema_version   = 1
    result           = $result
    command          = (@($Exe) + $cmdArgs) -join ' '
    command_exit     = $cmdExit
    limit_mb         = $MaxGrowthMB
    watched_dirs     = $watched
    before           = $before
    after            = $after
    dir_growth_bytes = $growth
    dir_growth_total_positive_bytes = $total
    c_free_drop_bytes = $freeDrop
    c_free_informational_only = (-not $StrictFree)
}
$json = $report | ConvertTo-Json -Depth 6
if ($OutFile) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent ([System.IO.Path]::GetFullPath($OutFile))) | Out-Null
    Set-Content -LiteralPath $OutFile -Value $json -Encoding utf8
}
Write-Output $json
if ($cmdExit -ne 0) { exit 2 }
if ($failed) { exit 1 }
exit 0
