<#
.SYNOPSIS  "Nothing on C:" environment. Dot-source it:  . .\scripts\env-d-e.ps1
.DESCRIPTION
  Creates the scratch layout under $env:HCS_SCRATCH_ROOT (default D:\HCSScratch) and points every tool cache / temp
  location at it for the current process (child processes inherit). Refuses to run when the scratch root, or an
  already-set HCS_DATA_DIR, resolves to drive C:. Never prints or stores secrets (HF_TOKEN is not read).
  CMake caches stay in the per-preset build directories; this script does not touch them.
.NOTES     Process-scope only: it changes no system or user environment variables and deletes nothing.
#>
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-HcsDriveLetter([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $root = [System.IO.Path]::GetPathRoot($full)
    if ($root -match '^([A-Za-z]):') { return $Matches[1].ToUpperInvariant() }
    return ''   # UNC or unknown: not drive C:
}

function Initialize-HcsScratchEnv {
    [CmdletBinding()]
    param([string]$Root = $(if ($env:HCS_SCRATCH_ROOT) { $env:HCS_SCRATCH_ROOT } else { 'D:\HCSScratch' }))

    $rootFull = [System.IO.Path]::GetFullPath($Root)
    if ((Get-HcsDriveLetter $rootFull) -eq 'C') {
        throw "Refusing: scratch root '$rootFull' is on drive C:. Set HCS_SCRATCH_ROOT to a path on another drive."
    }
    $driveRoot = [System.IO.Path]::GetPathRoot($rootFull)
    if (-not (Test-Path -LiteralPath $driveRoot)) { throw "Scratch drive '$driveRoot' does not exist." }

    $layout = [ordered]@{
        TEMP                 = 'tmp'
        TMP                  = 'tmp'
        TMPDIR               = 'tmp'
        PIP_CACHE_DIR        = 'pip'
        npm_config_cache     = 'npm'
        CARGO_HOME           = 'cargo'
        RUSTUP_HOME          = 'rustup'
        HF_HOME              = 'hf'
        TORCH_HOME           = 'torch'
        XDG_CACHE_HOME       = 'cache'
        PYTHONPYCACHEPREFIX  = 'pycache'
        NUGET_PACKAGES       = 'nuget'
    }
    foreach ($name in $layout.Keys) {
        $dir = [System.IO.Path]::GetFullPath((Join-Path $rootFull $layout[$name]))
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Set-Item -LiteralPath "Env:$name" -Value $dir
    }

    # HCS-owned state (settings, registry, logs, cache stats, sessions): honoured by the app as HCS_DATA_DIR.
    if ($env:HCS_DATA_DIR) {
        if ((Get-HcsDriveLetter $env:HCS_DATA_DIR) -eq 'C') {
            throw "Refusing: HCS_DATA_DIR='$($env:HCS_DATA_DIR)' is on drive C:."
        }
    } else {
        $env:HCS_DATA_DIR = Join-Path $rootFull 'hcsdata'
    }
    New-Item -ItemType Directory -Force -Path $env:HCS_DATA_DIR | Out-Null

    # Final self-check: nothing we export may be on C:.
    $exported = @($layout.Keys) + 'HCS_DATA_DIR'
    foreach ($name in $exported) {
        $v = (Get-Item -LiteralPath "Env:$name").Value
        if ((Get-HcsDriveLetter $v) -eq 'C') { throw "Refusing: $name resolved to '$v' on drive C:." }
    }

    Write-Host "[env-d-e] scratch root : $rootFull"
    Write-Host "[env-d-e] HCS_DATA_DIR : $($env:HCS_DATA_DIR)"
    Write-Host ("[env-d-e] redirected   : " + (($exported | Where-Object { $_ -ne 'HCS_DATA_DIR' }) -join ', '))
}

if ($env:HCS_SKIP_SCRATCH_ENV -eq '1') {
    # Opt-out for CI runners that have no D: drive. Local development must not set this.
    Write-Host '[env-d-e] HCS_SKIP_SCRATCH_ENV=1: scratch redirection skipped.'
} else {
    Initialize-HcsScratchEnv
}
