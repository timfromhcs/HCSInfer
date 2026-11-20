<#
.SYNOPSIS
  Builds a PORTABLE FOLDER package (not a single-file EXE) from an existing build, with licences, SBOM and
  SHA256SUMS.txt. Does not build, sign, install or modify anything outside <repo>/artifacts/package.
.PARAMETER Preset
  CMake preset whose build directory (<repo>/build/<Preset>) is packaged. Default windows-msvc-release.
.PARAMETER Zip
  Also create <package>.zip and a .sha256 file next to it.
.NOTES
  The package is dynamic-CRT (ADR-008) and contains no model weights, no GPU driver and no Vulkan SDK.
  It is a development/alpha artefact: tests, the real-model suites and the clean-machine check are NOT run here.
#>
[CmdletBinding()]
param(
    [string]$Preset = 'windows-msvc-release',
    [switch]$Zip
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')

$root = Get-RepoRoot
$buildDir = Get-BuildDir $Preset
$bin = Join-Path $buildDir 'bin'
$exe = Join-Path $bin 'HCSInfer.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "HCSInfer.exe not found at $exe. Build first: scripts\build.ps1 -Preset $Preset" }

$version = '0.0.0'
$m = Select-String -LiteralPath (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(\s*HCSInfer\s+VERSION\s+([0-9.]+)' | Select-Object -First 1
if ($m) { $version = $m.Matches[0].Groups[1].Value }
$commit = 'unknown'
try { $c = (& git -C $root rev-parse HEAD 2>$null); if ($LASTEXITCODE -eq 0 -and $c) { $commit = $c.Trim() } } catch { }

$pkgRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'artifacts\package'))
$name = "HCSInfer-$version-$Preset-portable"
$pkg = [System.IO.Path]::GetFullPath((Join-Path $pkgRoot $name))
if (-not $pkg.StartsWith($pkgRoot.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw "Refusing package path outside $pkgRoot" }
if (Test-Path -LiteralPath $pkg) { Remove-Item -LiteralPath $pkg -Recurse -Force }   # only our own previous output
New-Item -ItemType Directory -Path $pkg | Out-Null
New-Item -ItemType Directory -Path (Join-Path $pkg 'licenses') | Out-Null

# Runtime files only: the CLI/server executable and any DLLs next to it. Test executables and the storage
# benchmark are excluded on purpose.
Copy-Item -LiteralPath $exe -Destination $pkg
Get-ChildItem -LiteralPath $bin -File -Filter *.dll | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $pkg }

foreach ($f in 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md', 'SECURITY.md') {
    $src = Join-Path $root $f
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $pkg } else { Write-Warning "missing $f" }
}

# Licence texts declared in the lock file (submodule/vendored). FetchContent licences come from the build tree.
$lock = Get-Content -LiteralPath (Join-Path $root 'external\dependency-lock.json') -Raw | ConvertFrom-Json
foreach ($d in $lock.dependencies) {
    $lfProp = $d.PSObject.Properties['licence_file']
    if (-not $lfProp) { continue }
    $src = Join-Path $root ($lfProp.Value -replace '/', '\')
    if (Test-Path -LiteralPath $src -PathType Leaf) {
        $dst = Join-Path $pkg ('licenses\' + ($d.name -replace '[^A-Za-z0-9._-]', '_') + '-' + (Split-Path -Leaf $src))
        Copy-Item -LiteralPath $src -Destination $dst
    } else { Write-Warning "licence file missing for $($d.name): $($lfProp.Value)" }
}
$fetched = @{
    'nlohmann_json' = 'nlohmann_json-src\LICENSE.MIT'; 'cpp_httplib' = 'cpp_httplib-src\LICENSE'
    'lz4' = 'lz4-src\LICENSE'; 'zstd' = 'zstd-src\LICENSE'; 'vulkan_headers' = 'vulkan_headers-src\LICENSE.md' }
$cacheFile = Join-Path $buildDir 'CMakeCache.txt'
foreach ($k in $fetched.Keys) {
    $src = Join-Path $buildDir ('_deps\' + $fetched[$k])
    if (-not (Test-Path -LiteralPath $src -PathType Leaf) -and (Test-Path -LiteralPath $cacheFile)) {
        # FETCHCONTENT_SOURCE_DIR_<NAME> overrides (offline builds) keep the sources outside <build>/_deps.
        $ov = Select-String -LiteralPath $cacheFile -Pattern ('^FETCHCONTENT_SOURCE_DIR_' + $k.ToUpper() + ':[A-Z]+=(.+)$') | Select-Object -First 1
        if ($ov) { $src = Join-Path $ov.Matches[0].Groups[1].Value.Trim() (Split-Path -Leaf $fetched[$k]) }
    }
    if (Test-Path -LiteralPath $src -PathType Leaf) { Copy-Item -LiteralPath $src -Destination (Join-Path $pkg ("licenses\$k-" + (Split-Path -Leaf $src))) }
    else { Write-Warning "FetchContent licence not found for $k at $src (configure the preset first)" }
}

& (Join-Path $PSScriptRoot 'sbom.ps1') -OutFile (Join-Path $pkg 'sbom.cdx.json')

@"
HCSInfer $version portable folder ($Preset)

This is a portable multi-file package, NOT a single-file executable (docs/decisions/ADR-008-static-crt-single-exe.md).
- Built with the default dynamic MSVC runtime: the Microsoft Visual C++ Redistributable (x64) must be installed.
- No models, no graphics driver and no Vulkan SDK are included. Vulkan use needs the installed AMD/OEM driver.
- Run:  HCSInfer.exe --help   |   HCSInfer.exe doctor --json
- Verify files:  Get-FileHash -Algorithm SHA256 <file>  and compare with SHA256SUMS.txt.
- Third-party licences: THIRD_PARTY_NOTICES.md and the licenses/ folder. SBOM: sbom.cdx.json.
Status: alpha. See README.md for what is implemented. Unsigned.
"@ | Set-Content -LiteralPath (Join-Path $pkg 'PACKAGE-README.txt') -Encoding utf8

[ordered]@{
    name = $name; version = $version; preset = $Preset; git_commit = $commit
    created_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    single_exe = $false; crt = 'dynamic (/MD, default)'; signed = $false
    source_exe_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $pkg 'manifest.json') -Encoding utf8

# SHA256SUMS.txt over every file in the package except itself (sha256sum-compatible "hash  relative/path").
$sumFile = Join-Path $pkg 'SHA256SUMS.txt'
$lines = Get-ChildItem -LiteralPath $pkg -Recurse -File | Where-Object { $_.FullName -ne $sumFile } | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($pkg.Length + 1).Replace('\', '/')
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $rel
}
$lines | Set-Content -LiteralPath $sumFile -Encoding ascii
Write-Host "Package: $pkg ($(@($lines).Count) files hashed)"

if ($Zip) {
    $zipPath = "$pkg.zip"
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Compress-Archive -Path (Join-Path $pkg '*') -DestinationPath $zipPath
    $h = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    "$h  $(Split-Path -Leaf $zipPath)" | Set-Content -LiteralPath "$zipPath.sha256" -Encoding ascii
    Write-Host "Zip: $zipPath  sha256 $h"
}
