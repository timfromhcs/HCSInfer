<#
.SYNOPSIS
  Builds the hcsinfer Python wheel (and sdist) from EXISTING HCSInfer binaries, in a temporary copy of python/.
.PARAMETER Cpu      Path to the CPU HCSInfer binary (required).
.PARAMETER Vulkan   Path to the CPU+Vulkan binary (optional).
.PARAMETER Plat     Wheel platform tag (HCS_WHEEL_PLAT allow-list); default: derived from this host.
.PARAMETER Python   Python with build + twine installed (default: python on PATH).
.PARAMETER OutDir   Output directory (default artifacts\python).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Cpu,
    [string]$Vulkan = '',
    [string]$Plat = '',
    [string]$Python = 'python',
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $root 'artifacts\python' }
if (-not (Test-Path -LiteralPath $Cpu -PathType Leaf)) { throw "CPU binary missing: $Cpu" }
if ($Vulkan -and -not (Test-Path -LiteralPath $Vulkan -PathType Leaf)) { throw "Vulkan binary missing: $Vulkan" }
$sfx = [IO.Path]::GetExtension($Cpu)   # '.exe' on Windows, '' elsewhere
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("hcs-pywheel-" + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $tmp, $OutDir -Force | Out-Null
    Copy-Item (Join-Path $root 'python') (Join-Path $tmp 'python') -Recurse
    $bin = Join-Path $tmp 'python\src\hcsinfer\bin'
    New-Item -ItemType Directory -Path $bin -Force | Out-Null
    Copy-Item $Cpu (Join-Path $bin "HCSInfer$sfx")
    if ($Vulkan) { Copy-Item $Vulkan (Join-Path $bin "HCSInfer-vulkan$sfx") }
    if ($Plat) { $env:HCS_WHEEL_PLAT = $Plat } else { Remove-Item Env:\HCS_WHEEL_PLAT -ErrorAction SilentlyContinue }
    & $Python -m build --wheel (Join-Path $tmp 'python') --outdir $OutDir
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
    & $Python -m twine check --strict (Get-ChildItem $OutDir -Filter *.whl | ForEach-Object FullName)
    if ($LASTEXITCODE -ne 0) { throw "twine check failed ($LASTEXITCODE)" }
    Get-ChildItem $OutDir -Filter *.whl | ForEach-Object { Write-Host "built $($_.FullName)" }
} finally {
    Remove-Item Env:\HCS_WHEEL_PLAT -ErrorAction SilentlyContinue
    $full = [IO.Path]::GetFullPath($tmp)
    if ($full.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) -and (Split-Path -Leaf $full) -like 'hcs-pywheel-*') {
        Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue
    }
}
