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
function Initialize-MsvcEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $candidates = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
    $vcvars = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $vcvars) { throw 'cl.exe not found and no Visual Studio 2022 vcvars64.bat located. Install the "Desktop development with C++" workload.' }
    $lines = & cmd.exe /c "`"$vcvars`" >nul 2>nul && set"
    foreach ($l in $lines) {
        if ($l -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'MSVC environment initialisation failed.' }
}

function Invoke-Native {
    param([Parameter(Mandatory)][string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($Arguments -join ' ') failed with exit code $LASTEXITCODE" }
}
