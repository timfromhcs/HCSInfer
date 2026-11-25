<#
.SYNOPSIS  Configure + build HCSInfer via CMake presets (MSVC x64, Ninja).
.EXAMPLE   .\scripts\build.ps1 -Preset windows-msvc-debug -RunTests
.NOTES     -CleanBuildDir only ever removes <repo>/build/<preset> after a canonical path check.
#>
[CmdletBinding()]
param(
    [string]$Preset = 'windows-msvc-debug',
    [switch]$CleanBuildDir,
    [int]$Parallel = [Environment]::ProcessorCount,
    [switch]$RunTests,
    [switch]$Vulkan   # selects windows-msvc-release-vulkan
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')
. (Join-Path $PSScriptRoot 'env-d-e.ps1')   # nothing on C: (docs/build.md)

try {
    if ($Vulkan) { $Preset = 'windows-msvc-release-vulkan' }
    $root = Get-RepoRoot
    $buildDir = Get-BuildDir $Preset
    Initialize-MsvcEnvironment
    Push-Location $root
    try {
        if ($CleanBuildDir -and (Test-Path -LiteralPath $buildDir)) {
            Write-Verbose "Removing $buildDir"
            Remove-Item -LiteralPath $buildDir -Recurse -Force
        }
        Invoke-Native cmake @('--preset', $Preset)
        $buildArgs = @('--build', '--preset', $Preset, '--parallel', "$Parallel")
        if ($VerbosePreference -eq 'Continue') { $buildArgs += '--verbose' }
        Invoke-Native cmake $buildArgs
        if ($RunTests) { & (Join-Path $PSScriptRoot 'test.ps1') -Preset $Preset; if ($LASTEXITCODE -ne 0) { throw 'tests failed' } }
    } finally { Pop-Location }
    Write-Host "BUILD OK ($Preset)"
    exit 0
} catch {
    Write-Error $_
    exit 1
}
