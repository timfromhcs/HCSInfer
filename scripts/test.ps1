<#
.SYNOPSIS  Run CTest for a preset. Logs are kept under artifacts/test-logs. Exit code is nonzero on any failure.
.EXAMPLE   .\scripts\test.ps1 -Preset windows-msvc-debug -Label unit
#>
[CmdletBinding()]
param(
    [string]$Preset = 'windows-msvc-debug',
    [ValidateSet('', 'unit', 'contract', 'parser', 'kernel-cpu', 'kernel-vulkan', 'integration', 'e2e', 'performance', 'soak')]
    [string]$Label = '',
    [string]$Regex = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')
. (Join-Path $PSScriptRoot 'env-d-e.ps1')   # nothing on C: (docs/build.md)

try {
    $root = Get-RepoRoot
    $buildDir = Get-BuildDir $Preset
    if (-not (Test-Path -LiteralPath $buildDir)) { throw "Build dir missing: run scripts/build.ps1 -Preset $Preset first." }
    Initialize-MsvcEnvironment
    $logDir = Join-Path $root 'artifacts\test-logs'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $log = Join-Path $logDir ("ctest-{0}-{1}.log" -f $Preset, (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $ctestArgs = @('--test-dir', $buildDir, '--output-on-failure', '--output-log', $log)
    if ($Label) { $ctestArgs += @('-L', $Label) }
    if ($Regex) { $ctestArgs += @('-R', $Regex) }
    & ctest @ctestArgs
    $code = $LASTEXITCODE
    Write-Host "ctest log: $log"
    exit $code
} catch {
    Write-Error $_
    exit 1
}
