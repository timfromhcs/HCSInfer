<#
.SYNOPSIS
  Assembles every distributable from EXISTING builds into artifacts/release (no builds, no uploads, no tags):
  HCSInfer.exe (static CPU), HCSInfer-vulkan.exe (static CPU+Vulkan), the portable ZIP, Python sdist + platform wheel,
  the web demo dist zip, a CycloneDX SBOM and SHA256SUMS.txt.
.DESCRIPTION
  Run after scripts\build.ps1 -Preset windows-msvc-release-static (and ...-static-vulkan, optional). The Python wheel is built
  from a temporary copy of python/ so the working tree never contains the staged EXE. Needs a Python with
  build + twine (default: build\devtools\pybuild, or -Python).
.PARAMETER CpuPreset     Static CPU preset (default windows-msvc-release-static).
.PARAMETER VulkanPreset  Static Vulkan preset; skipped (reported) when its EXE does not exist.
#>
[CmdletBinding()]
param(
    [string]$CpuPreset = 'windows-msvc-release-static',
    [string]$VulkanPreset = 'windows-msvc-release-static-vulkan',
    [string]$Python = '',
    # Directory with extra release assets built elsewhere (for example the Linux/macOS HCSInfer-<ver>-<os>-<arch>.tar.gz
    # from scripts/package-posix.sh); every file in it is copied into the release set and covered by SHA256SUMS.txt.
    [string]$ExtraAssetDir = '',
    # Path to an official osv-scanner binary: runs the scan + scripts/osv-gate.ps1 and adds osv-report.json.
    # Without it the OSV gate is NOT RUN (reported, never skipped silently).
    [string]$OsvScanner = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')
$root = Get-RepoRoot
Initialize-MsvcEnvironment

$cpuExe = Join-Path (Get-BuildDir $CpuPreset) 'bin\HCSInfer.exe'
$vkExe = Join-Path (Get-BuildDir $VulkanPreset) 'bin\HCSInfer.exe'
if (-not (Test-Path -LiteralPath $cpuExe)) { throw "CPU EXE missing: $cpuExe" }
$haveVk = Test-Path -LiteralPath $vkExe
if (-not $Python) { $Python = Join-Path $root 'build\devtools\pybuild\Scripts\python.exe' }
if (-not (Test-Path -LiteralPath $Python)) { throw "Python with build+twine not found at $Python (pass -Python)" }

$out = [IO.Path]::GetFullPath((Join-Path $root 'artifacts\release'))
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }   # our own previous output only
New-Item -ItemType Directory -Path $out | Out-Null

Copy-Item $cpuExe (Join-Path $out 'HCSInfer.exe')
if ($haveVk) { Copy-Item $vkExe (Join-Path $out 'HCSInfer-vulkan.exe') } else { Write-Warning 'no Vulkan static EXE: skipped (NOT RUN)' }

# Portable ZIP (+ licences, SBOM, manifest with the dumpbin import report).
$pk = @{ Preset = $CpuPreset; Zip = $true }
if ($haveVk) { $pk.VulkanPreset = $VulkanPreset }
& (Join-Path $PSScriptRoot 'package.ps1') @pk
if ($LASTEXITCODE -ne 0) { throw 'package.ps1 failed' }
# Newest matching ZIP only: an older version's package left in artifacts\package must not leak into the release set.
Get-ChildItem (Join-Path $root 'artifacts\package') -Filter '*.zip' | Where-Object { $_.Name -like "*$CpuPreset*" } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 | Copy-Item -Destination $out

# Python: build in a temp copy so the repository tree stays clean.
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('hcs-py-' + [Guid]::NewGuid().ToString('N'))
Copy-Item (Join-Path $root 'python') $tmp -Recurse
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') $tmp   # the wheel ships third-party code inside the EXE
$bin = Join-Path $tmp 'src\hcsinfer\bin'
New-Item -ItemType Directory -Force $bin | Out-Null
try {
    & $Python -m build --sdist $tmp --outdir (Join-Path $out '_py')    # sdist first: no exe in it
    Copy-Item $cpuExe (Join-Path $bin 'HCSInfer.exe')
    if ($haveVk) { Copy-Item $vkExe (Join-Path $bin 'HCSInfer-vulkan.exe') }
    & $Python -m build --wheel $tmp --outdir (Join-Path $out '_py')
    & $Python -m twine check (Join-Path $out '_py\*')
    if ($LASTEXITCODE -ne 0) { throw 'twine check failed' }
    Get-ChildItem (Join-Path $out '_py') -File | Move-Item -Destination $out
    Remove-Item (Join-Path $out '_py') -Force
} finally {
    if ((Split-Path $tmp -Leaf) -like 'hcs-py-*') { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
}

# Web demo dist (built, not deployed).
Push-Location (Join-Path $root 'web')
try { node tools/build-dist.mjs; if ($LASTEXITCODE -ne 0) { throw 'web build failed' } } finally { Pop-Location }
Compress-Archive -Path (Join-Path $root 'web\dist\*') -DestinationPath (Join-Path $out 'hcs-studio-web-dist.zip')

if ($ExtraAssetDir) {
    if (-not (Test-Path -LiteralPath $ExtraAssetDir -PathType Container)) { throw "ExtraAssetDir not found: $ExtraAssetDir" }
    Get-ChildItem -LiteralPath $ExtraAssetDir -File | Copy-Item -Destination $out
}

& (Join-Path $PSScriptRoot 'sbom.ps1') -OutFile (Join-Path $out 'hcsinfer.cdx.json') -BuildDir (Get-BuildDir $(if ($haveVk) { $VulkanPreset } else { $CpuPreset }))

if ($OsvScanner) {
    $osvDir = Join-Path $root 'artifacts\osv'
    New-Item -ItemType Directory -Force $osvDir | Out-Null
    & $OsvScanner scan source -S (Join-Path $out 'hcsinfer.cdx.json') --allow-no-lockfiles --format json --output-file (Join-Path $osvDir 'osv-scanner-sbom.json')
    if ($LASTEXITCODE -gt 1) { throw "osv-scanner failed (exit $LASTEXITCODE)" }
    & (Join-Path $PSScriptRoot 'osv-gate.ps1') -ScannerResult @((Join-Path $osvDir 'osv-scanner-sbom.json')) -OutFile (Join-Path $osvDir 'osv-gate.json')
    $osvRc = $LASTEXITCODE
    Copy-Item (Join-Path $osvDir 'osv-gate.json') (Join-Path $out 'osv-report.json')
    if ($osvRc -ne 0) { Write-Warning 'OSV gate FAILED (see osv-report.json); a stable release must not be published with this result' }
} else {
    Write-Warning 'OSV gate NOT RUN (pass -OsvScanner <official osv-scanner binary>)'
}

$sum = Join-Path $out 'SHA256SUMS.txt'
$sumLines = Get-ChildItem $out -File | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } | Sort-Object Name | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
}
# LF-only (sha256sum-compatible); Set-Content would write CRLF.
[IO.File]::WriteAllText($sum, (($sumLines -join "`n") + "`n"), [Text.Encoding]::ASCII)
Get-Content $sum
Write-Host "Release files: $out"
