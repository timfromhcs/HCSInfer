# Shared helpers (dot-sourced by build.ps1 / test.ps1 / doctor.ps1). No system changes.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-RepoRoot { return (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }

function Assert-PresetName([string]$Preset) {
    if ($Preset -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') { throw "Invalid preset name '$Preset'." }
}

# Returns the build directory for a preset, guaranteed to be a child of <repo>/build.
function Get-BuildDir([string]$Preset) {
    Assert-PresetName $Preset
    $root = Get-RepoRoot
    $buildRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'build'))
    $dir = [System.IO.Path]::GetFullPath((Join-Path $buildRoot $Preset))
    $prefix = $buildRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $dir.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing: build dir '$dir' is not inside '$buildRoot'."
    }
    return $dir
}

# Loads the MSVC x64 environment into this process if cl.exe is not already on PATH.
#
# Detection order (a CI runner is not necessarily laid out like a workstation):
#   1. vswhere.exe - the canonical Microsoft way to locate Visual Studio - preferring an install whose version is
#      17.x, i.e. Visual Studio 2022. That is the toolchain every result in docs/test-evidence/ was produced with and
#      the one on the GitHub windows-latest image, so a newer VS that happens to be installed here (a VS 18 BuildTools
#      with a different cl.exe) must not silently take over. AGENTS.md 0.2: no arbitrary upgrade;
#   2. any vswhere install (newest first) when no VS 2022 is present;
#   3. the well-known per-edition vcvars64.bat paths;
#   4. a bounded recursive search under "Microsoft Visual Studio\2022".
# The environment-variable lookups use [Environment]::GetEnvironmentVariable because "ProgramFiles(x86)" cannot be
# written with the $env: syntax and some CI shells leave the variable unset. The chosen vcvars64.bat and the compiler
# banner are printed so every log records which toolchain was used.
function Initialize-MsvcEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $pf = [Environment]::GetEnvironmentVariable('ProgramFiles')
    $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $tried = [System.Collections.Generic.List[string]]::new()

    $vcvars = $null
    $vswhere = if ($pf86) { Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe' } else { $null }
    $require = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    if ($vswhere -and (Test-Path -LiteralPath $vswhere)) {
        foreach ($selector in @(@('-version', '[17.0,18.0)'), @('-latest'))) {
            $vsArgs = @('-products', '*', '-requires', $require, '-property', 'installationPath') + $selector
            $installs = & $vswhere @vsArgs 2>$null
            foreach ($install in @($installs)) {
                if (-not $install) { continue }
                $cand = Join-Path $install.Trim() 'VC\Auxiliary\Build\vcvars64.bat'
                $tried.Add("vswhere $($selector -join ' '): $cand")
                if (Test-Path -LiteralPath $cand) { $vcvars = $cand; break }
            }
            if ($vcvars) { break }
        }
    } else {
        $tried.Add("vswhere: not present at '$vswhere'")
    }

    if (-not $vcvars) {
        $candidates = @()
        foreach ($base in @($pf, $pf86)) {
            if (-not $base) { continue }
            foreach ($edition in 'Community', 'Professional', 'Enterprise', 'BuildTools') {
                $candidates += (Join-Path $base "Microsoft Visual Studio\2022\$edition\VC\Auxiliary\Build\vcvars64.bat")
            }
        }
        foreach ($c in $candidates) {
            $tried.Add("candidate: $c")
            if (Test-Path -LiteralPath $c) { $vcvars = $c; break }
        }
    }

    if (-not $vcvars) {
        foreach ($base in @($pf, $pf86)) {
            if (-not $base) { continue }
            $root = Join-Path $base 'Microsoft Visual Studio'
            if (-not (Test-Path -LiteralPath $root)) { continue }
            $hit = Get-ChildItem -LiteralPath $root -Filter 'vcvars64.bat' -Recurse -File -ErrorAction SilentlyContinue |
                Select-Object -First 1 -ExpandProperty FullName
            if ($hit) { $vcvars = $hit; break }
        }
    }

    if (-not $vcvars) {
        throw ("cl.exe not found and no Visual Studio vcvars64.bat located. Install the 'Desktop development with C++' " +
               "workload. ProgramFiles='$pf' ProgramFiles(x86)='$pf86'. Looked at: " + ($tried -join '; '))
    }
    Write-Host "MSVC environment: $vcvars"
    $lines = & cmd.exe /c "`"$vcvars`" >nul 2>nul && set"
    foreach ($l in $lines) {
        if ($l -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "MSVC environment initialisation failed: '$vcvars' ran but cl.exe is still not on PATH."
    }
    Write-Host "compiler: $((cl 2>&1 | Select-Object -First 1))"
}

# Locates a tool from the Visual Studio 2022 LLVM/Clang component ("C++ Clang tools"): clang-format, clang-tidy.
#
# The Visual-Studio-bundled build is preferred over whatever happens to be first on PATH so that local development and
# the CI runner (windows-latest, which has Visual Studio 2022 with that component) use the same version: clang-format
# and clang-tidy results are version-dependent. Only VS 2022 editions are listed on purpose - a newer Visual Studio
# installed on the same machine must not silently take over (AGENTS.md 0.2, no arbitrary upgrade). Returns $null when
# no such tool is installed; the caller decides whether to fall back or to fail loudly.
function Find-VsLlvmTool {
    param([Parameter(Mandatory)][string]$Name)
    $pf = [Environment]::GetEnvironmentVariable('ProgramFiles')
    $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    # Every installed Visual Studio, but with 2022 first: that is the toolchain every result in
    # docs/test-evidence/ was produced with and the one the GitHub windows-latest image used when this
    # script was written. The editions are then tried in order. A newer VS (e.g. a VS 18 that also ships
    # LLVM) is used only when no VS 2022 has the component - never silently preferred over it.
    $installRoots = [System.Collections.Generic.List[string]]::new()
    foreach ($base in @($pf, $pf86)) {
        if (-not $base) { continue }
        $root = Join-Path $base 'Microsoft Visual Studio'
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $versions = @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^[0-9]+' } | Sort-Object Name -Descending)
        $ordered = @($versions | Where-Object { $_.Name -eq '2022' }) +
                   @($versions | Where-Object { $_.Name -ne '2022' })
        foreach ($v in $ordered) {
            foreach ($edition in 'Community', 'Professional', 'Enterprise', 'BuildTools', 'Preview', 'Insiders') {
                $installRoots.Add((Join-Path $v.FullName $edition))
            }
        }
    }
    foreach ($vs in $installRoots) {
        $cand = Join-Path $vs "VC\Tools\Llvm\x64\bin\$Name.exe"
        if (Test-Path -LiteralPath $cand) { return $cand }
    }
    $cmd = Get-Command "$Name.exe" -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if ($pf) {
        $other = Join-Path $pf "LLVM\bin\$Name.exe"
        if (Test-Path -LiteralPath $other) { return $other }
    }
    return $null
}

function Invoke-Native {
    param([Parameter(Mandatory)][string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($Arguments -join ' ') failed with exit code $LASTEXITCODE" }
}
