<#
.SYNOPSIS
  clang-tidy static analysis for HCS-owned sources (src, include, tests, tools). Never touches external/ or build/.

.DESCRIPTION
  Runs clang-tidy over the translation units of a configured build directory, using its compile_commands.json, and
  prints every diagnostic in HCS-owned code. This is the "static analysis" gate of AGENTS.md 9.4; it is report-only by
  default (-FailOnFindings turns findings into a non-zero exit code) because correctness, not a clean baseline, is what
  the gate must not silently weaken.

  Two environment pitfalls this script handles for the caller:
    * clang-tidy/clang-cl resolves the MSVC STL and Windows SDK through the registry and picks the NEWEST Visual Studio
      on the machine. A VS 18 BuildTools STL (MSVC 14.5x) demands Clang 20+ and fails hard with "STL1000" under the
      VS 2022-bundled clang-tidy. Therefore the VS 2022 environment is loaded first, so INCLUDE points at the 14.44
      STL that matches the compiler the build directory was configured with (MSVC 19.44, see cmake/version.hpp).
    * /WX and /W4 from the HCS targets would turn ordinary MSVC/clang warnings into errors; -Wno-error is appended so
      the analysis always completes. Only the checks selected by -Checks are reported, plus clang-diagnostic-* if the
      caller asks for them.

.PARAMETER Preset
  CMake preset whose build directory contains compile_commands.json. Default windows-msvc-release. Configure it with
  CMAKE_EXPORT_COMPILE_COMMANDS=ON (the project default) and build it at least once.

.PARAMETER Scope
  src (default), tests, tools, or all.

.PARAMETER Checks
  clang-tidy -checks value. The default enables clang-analyzer-* and bugprone-* but excludes four classes that are
  not defects in this code base (see docs/static-analysis.md for the full triage):
    -bugprone-easily-swappable-parameters  every hit is a documented positional parameter of a narrow API
    -bugprone-casting-through-void         every hit is the required Win32/Vulkan loader idiom (GetProcAddress,
                                           LoadLibrary, vkGetInstanceProcAddr)
    -bugprone-exception-escape             every real site was fixed (destructor, main, noexcept accessors); the
                                           rest are job bodies that JobQueue::worker_loop already catches for
    -clang-analyzer-optin.core.EnumCastOutOfRange  fires inside the MSVC STL's xfilesystem_abi.h, not in HCS code
  Re-enable any of them by passing -Checks explicitly.

.PARAMETER Parallel
  Number of concurrent clang-tidy processes. Default 4. (clang-tidy 19 has no -j, so this script starts processes.)

.PARAMETER FailOnFindings
  Exit 1 when at least one diagnostic was produced.

.PARAMETER ClangTidy
  Explicit path to clang-tidy.exe. Otherwise the VS 2022-bundled tool, then PATH, then C:\Program Files\LLVM.

.EXAMPLE
  .\scripts\static-analysis.ps1 -Preset windows-msvc-release -Scope src
.EXAMPLE
  .\scripts\static-analysis.ps1 -Scope all -Checks '-*,clang-analyzer-*,bugprone-*,performance-*' -FailOnFindings
#>
[CmdletBinding()]
param(
    [string]$Preset = 'windows-msvc-release',
    [ValidateSet('src', 'tests', 'tools', 'all')][string]$Scope = 'src',
    [string]$Checks = '-*,clang-analyzer-*,bugprone-*,-bugprone-easily-swappable-parameters,-bugprone-casting-through-void,-bugprone-exception-escape,-clang-analyzer-optin.core.EnumCastOutOfRange',
    [int]$Parallel = 4,
    [switch]$FailOnFindings,
    [string]$ClangTidy
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')

$root = Get-RepoRoot
$buildDir = Get-BuildDir $Preset
$db = Join-Path $buildDir 'compile_commands.json'
if (-not (Test-Path -LiteralPath $db)) {
    throw "No compile database at '$db'. Configure first: cmake --preset $Preset ; cmake --build --preset $Preset"
}

$exe = if ($ClangTidy) {
    if (-not (Test-Path -LiteralPath $ClangTidy)) { throw "clang-tidy not found at '$ClangTidy'." }
    (Resolve-Path -LiteralPath $ClangTidy).Path
} else { Find-VsLlvmTool 'clang-tidy' }
if (-not $exe) { throw 'clang-tidy not found (install the VS "C++ Clang tools" component, LLVM, or pass -ClangTidy). Not treating a missing tool as success.' }

Write-Host "clang-tidy: $exe ($(((& $exe --version) | Select-Object -Index 1).Trim()))"
Write-Host "checks: $Checks"

# See the .DESCRIPTION: without the VS 2022 environment clang picks the newest STL on the machine.
Initialize-MsvcEnvironment

$scopes = switch ($Scope) {
    'all' { @('src', 'tests', 'tools') }
    default { @($Scope) }
}
$prefixes = $scopes | ForEach-Object { (Join-Path $root $_).Replace('\', '/') + '/*' }
# CMake writes forward slashes into compile_commands.json, including on Windows - match on that form.
# Windows PowerShell 5.1 pipes ConvertFrom-Json's array as ONE System.Object[] into the next cmdlet;
# $_.file then yields an array and Select-Object -ExpandProperty file fails. Materialise the parsed
# entries first so both Windows PowerShell 5.1 and PowerShell 7 enumerate the same 445 entries.
$compileDb = Get-Content -LiteralPath $db -Raw | ConvertFrom-Json
$tu = @($compileDb |
    Where-Object { $f = $_.file; $prefixes | Where-Object { $f -like $_ } } |
    Select-Object -ExpandProperty file -Unique | Sort-Object)
if ($tu.Count -eq 0) { throw "No translation units found for scope '$Scope' in $db." }
Write-Host "translation units: $($tu.Count) ($($scopes -join ', '))"

$headerFilter = '.*[\\/](src|include)[\\/]hcs[\\/].*'
function Invoke-TidyOne([string]$File) {
    # -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH: a Windows runner can carry a Visual Studio newer than the
    # clang-tidy that analyses it (e.g. VS 18's MSVC 14.52 STL hard-errors STL1000 under Clang < 22). The macro
    # is the MSVC STL's own documented escape hatch; without it the gate fails on the STL banner, not on HCS code.
    # clang-tidy writes "N warnings generated." to stderr. Under Windows PowerShell 5.1 with
    # $ErrorActionPreference='Stop' that native stderr becomes a terminating RemoteException, so
    # pause Stop only around the native call; the full text is still captured for the findings filter.
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $exe -p $buildDir --checks=$Checks --header-filter=$headerFilter `
            --extra-arg=-Wno-error --extra-arg=-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH $File 2>&1 | Out-String
    } finally { $ErrorActionPreference = $oldEap }
    [pscustomobject]@{ file = $File; text = $out }
}
if ($PSVersionTable.PSVersion.Major -ge 7) {
    $results = @($tu | ForEach-Object -Parallel {
            $ct = $using:exe
            $dbDir = $using:buildDir
            $checks = $using:Checks
            $hf = $using:headerFilter
            $oldEap = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'  # native stderr must not abort the parallel worker
            try {
                $out = & $ct -p $dbDir --checks=$checks --header-filter=$hf --extra-arg=-Wno-error `
                    --extra-arg=-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH $_ 2>&1 | Out-String
            } finally { $ErrorActionPreference = $oldEap }
            [pscustomobject]@{ file = $_; text = $out }
        } -ThrottleLimit $Parallel)
} else {
    # Windows PowerShell 5.1 has no ForEach-Object -Parallel; run sequentially rather than fail.
    Write-Host "PowerShell $($PSVersionTable.PSVersion): running sequentially."
    $results = @($tu | ForEach-Object { Invoke-TidyOne $_ })
}

$findings = 0
# Third-party locations that must never count as HCS findings: the submodules under external/ and anything the
# build generated, e.g. the patched httplib.h copy cmake/HcsHttplib.cmake writes into the build tree. The gate
# covers src/, include/, tests/ and tools/ only.
$thirdParty = @('[\\/]external[\\/]', [regex]::Escape($buildDir))
foreach ($r in $results | Sort-Object file) {
    $lines = @($r.text -split "`r?`n" | Where-Object {
            if ($_ -notmatch 'warning:|error:') { return $false }
            foreach ($pattern in $thirdParty) { if ($_ -match $pattern) { return $false } }
            return $true
        })
    if ($lines.Count -gt 0) {
        Write-Host ""
        Write-Host "== $($r.file.Substring($root.Length + 1))"
        $lines | ForEach-Object { Write-Host $_ }
        $findings += $lines.Count
    }
}
if ($findings) {
    Write-Host ""
    Write-Host "STATIC ANALYSIS: $findings diagnostic(s) in $($tu.Count) translation units."
    if ($FailOnFindings) { exit 1 }
    Write-Host "Report-only run (-FailOnFindings to make this an error)."
    exit 0
}
Write-Host "STATIC ANALYSIS OK: no diagnostic in $($tu.Count) translation units."
exit 0
