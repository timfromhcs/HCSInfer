<#
.SYNOPSIS  Runs `HCSInfer doctor` from a built preset. Read-only: changes no system settings.
.EXAMPLE   .\scripts\doctor.ps1 -Preset windows-msvc-release -Json -OutFile artifacts\doctor.json
#>
[CmdletBinding()]
param(
    [string]$Preset = 'windows-msvc-release',
    [switch]$Json,
    [string]$OutFile = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')
. (Join-Path $PSScriptRoot 'env-d-e.ps1')   # nothing on C: (docs/build.md)

try {
    $root = Get-RepoRoot
    $exe = Join-Path (Get-BuildDir $Preset) 'bin\HCSInfer.exe'
    if (-not (Test-Path -LiteralPath $exe)) { throw "Not built: $exe (run scripts/build.ps1 -Preset $Preset)." }
    $a = @('doctor'); if ($Json) { $a += '--json' }
    $output = & $exe @a
    $code = $LASTEXITCODE
    if ($OutFile) {
        $full = [System.IO.Path]::GetFullPath((Join-Path $root $OutFile))
        $artifacts = [System.IO.Path]::GetFullPath((Join-Path $root 'artifacts')) + [System.IO.Path]::DirectorySeparatorChar
        $docs = [System.IO.Path]::GetFullPath((Join-Path $root 'docs\test-evidence')) + [System.IO.Path]::DirectorySeparatorChar
        if (-not ($full.StartsWith($artifacts, 'OrdinalIgnoreCase') -or $full.StartsWith($docs, 'OrdinalIgnoreCase'))) {
            throw 'OutFile must be inside artifacts/ or docs/test-evidence/.'
        }
        New-Item -ItemType Directory -Force -Path (Split-Path $full) | Out-Null
        $output | Set-Content -LiteralPath $full -Encoding utf8
    }
    $output
    exit $code
} catch {
    Write-Error $_
    exit 1
}
