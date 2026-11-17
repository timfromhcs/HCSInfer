<#
.SYNOPSIS
  clang-format check / fix for HCS-owned sources (src, include, tests, tools). Never touches external/ or build/.
.PARAMETER Fix
  Rewrite files in place. Default is check-only (clang-format --dry-run --Werror), exit code 1 on any difference.
.PARAMETER ClangFormat
  Explicit path to clang-format.exe. Otherwise PATH, then Visual Studio's bundled LLVM, then other known installs.
#>
[CmdletBinding()]
param(
    [switch]$Fix,
    [string]$ClangFormat
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')

$root = Get-RepoRoot

function Find-ClangFormat {
    if ($ClangFormat) {
        if (-not (Test-Path -LiteralPath $ClangFormat)) { throw "clang-format not found at '$ClangFormat'." }
        return (Resolve-Path -LiteralPath $ClangFormat).Path
    }
    # Deterministic version first: the Microsoft/Visual-Studio-bundled clang-format. The CI runner
    # (windows-latest) has Visual Studio 2022 with the "C++ Clang tools" component and therefore the same
    # formatter, so a formatting pass here matches the gate there. A clang-format that happens to be first on
    # PATH (e.g. a ROCm or LLVM install) is a *different* version and can wrap differently, so it is only a
    # fallback. The version actually used is printed below and must be recorded when the tree is reformatted.
    $candidates = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang-format.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\VC\Tools\Llvm\x64\bin\clang-format.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-format.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-format.exe")
    $hit = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if ($hit) { return $hit }
    $cmd = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $other = @("${env:ProgramFiles}\LLVM\bin\clang-format.exe")
    $hit = $other | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($hit) { return $hit }
    throw 'clang-format not found (install the VS "C++ Clang tools" component, LLVM, or pass -ClangFormat). Not treating a missing tool as success.'
}

$exe = Find-ClangFormat
Write-Host "clang-format: $exe ($(& $exe --version))"

$dirs = 'src', 'include', 'tests', 'tools' | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path -LiteralPath $_ }
$files = foreach ($d in $dirs) {
    Get-ChildItem -LiteralPath $d -Recurse -File |
        Where-Object { $_.Extension -in '.cpp', '.hpp', '.h', '.cc', '.cxx' -and $_.FullName -notmatch '[\\/](build|external|_deps)[\\/]' }
}
$files = @($files)
if ($files.Count -eq 0) { Write-Host 'No C++ sources found.'; exit 0 }

$bad = 0
foreach ($f in $files) {
    if ($Fix) {
        & $exe -i --style=file -- $f.FullName
        if ($LASTEXITCODE -ne 0) { $bad++ }
    } else {
        $ErrorActionPreference = 'Continue'   # clang-format writes diagnostics to stderr
        & $exe --dry-run --Werror --style=file -- $f.FullName *> $null
        $ErrorActionPreference = 'Stop'
        if ($LASTEXITCODE -ne 0) { Write-Host "needs formatting: $($f.FullName.Substring($root.Length + 1))"; $bad++ }
    }
}
if ($Fix) { Write-Host "Formatted $($files.Count) files."; exit $(if ($bad) { 1 } else { 0 }) }
if ($bad) { Write-Host "FORMAT CHECK FAILED: $bad of $($files.Count) files differ (run scripts\format.ps1 -Fix)."; exit 1 }
Write-Host "FORMAT CHECK OK: $($files.Count) files."
exit 0
