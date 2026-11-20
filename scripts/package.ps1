<#
.SYNOPSIS
  Builds a PORTABLE FOLDER package from an existing build (the CPU EXE imports only system DLLs when the preset is static; this is not a clean-machine proof of a single file), with licences, SBOM and
  SHA256SUMS.txt. Does not build, sign, install or modify anything outside <repo>/artifacts/package.
.PARAMETER Preset
  CMake preset whose build directory (<repo>/build/<Preset>) is packaged. Default windows-msvc-release.
.PARAMETER Zip
  Also create <package>.zip and a .sha256 file next to it.
.NOTES
  The CRT depends on the preset (ADR-008: *static* presets use /MT). The package contains no model weights, no GPU driver and no Vulkan SDK.
  It is a release-candidate artefact: tests, the real-model suites and the clean-machine check are NOT run here.
#>
[CmdletBinding()]
param(
    [string]$Preset = 'windows-msvc-release',
    [switch]$Zip,
    # Optional second build (e.g. windows-msvc-release-static-vulkan) whose HCSInfer.exe is added as
    # HCSInfer-vulkan.exe next to the primary (CPU) HCSInfer.exe.
    [string]$VulkanPreset = ''
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
$sfx = Select-String -LiteralPath (Join-Path $root 'CMakeLists.txt') -Pattern 'set\(\s*HCS_VERSION_SUFFIX\s+"([^"]*)"' | Select-Object -First 1
if ($sfx) { $version = $version + $sfx.Matches[0].Groups[1].Value }
$commit = 'unknown'
try { $c = (& git -C $root rev-parse HEAD 2>$null); if ($LASTEXITCODE -eq 0 -and $c) { $commit = $c.Trim() } } catch { }
$dirty = 'unknown'
try { $st = (& git -C $root status --porcelain --untracked-files=no 2>$null); if ($LASTEXITCODE -eq 0) { $dirty = [bool]$st } } catch { }

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
$vkExeSrc = ''
if ($VulkanPreset) {
    $vkExeSrc = Join-Path (Get-BuildDir $VulkanPreset) 'bin\HCSInfer.exe'
    if (-not (Test-Path -LiteralPath $vkExeSrc)) { throw "Vulkan HCSInfer.exe not found at $vkExeSrc" }
    Copy-Item -LiteralPath $vkExeSrc -Destination (Join-Path $pkg 'HCSInfer-vulkan.exe')
}
Get-ChildItem -LiteralPath $bin -File -Filter *.dll | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $pkg }

foreach ($f in 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md', 'SECURITY.md', 'CHANGELOG.md', 'CONTRIBUTING.md') {
    $src = Join-Path $root $f
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $pkg } else { Write-Warning "missing $f" }
}

# Documentation essentials, so the relative links of README.md work inside the ZIP: docs/*.md, the ADRs, the benchmark
# charts and the Studio screenshots. Test evidence, sources, scripts and the web/python trees are NOT shipped; links to
# them are rewritten below to plain text that names the path in the source repository.
$docsOut = Join-Path $pkg 'docs'
New-Item -ItemType Directory -Path $docsOut | Out-Null
Get-ChildItem -LiteralPath (Join-Path $root 'docs') -File -Filter *.md | Copy-Item -Destination $docsOut
foreach ($d in 'decisions', 'benchmarks', 'screenshots') {
    $dsrc = Join-Path $root "docs\$d"
    if (Test-Path -LiteralPath $dsrc) { Copy-Item -LiteralPath $dsrc -Destination (Join-Path $docsOut $d) -Recurse }
}
$linkRe = [regex]'(!?)\[([^\]]*)\]\(\s*([^)\s]+)(?:\s+"[^"]*")?\s*\)'
Get-ChildItem -LiteralPath $pkg -Recurse -File -Filter *.md | Where-Object { $_.Name -ne 'THIRD_PARTY_NOTICES.md' -and $_.FullName -notlike '*\licenses\*' } | ForEach-Object {
    $mdFile = $_
    $text = [IO.File]::ReadAllText($mdFile.FullName)
    $eval = [System.Text.RegularExpressions.MatchEvaluator]{
        param($m)
        $target = $m.Groups[3].Value.Trim('<', '>')
        if ($target -match '^[A-Za-z][A-Za-z0-9+.-]*:' -or $target.StartsWith('#')) { return $m.Value }
        $pathPart = ($target -split '#')[0] -split '\?' | Select-Object -First 1
        if (-not $pathPart) { return $m.Value }
        $dest = [IO.Path]::GetFullPath((Join-Path $mdFile.DirectoryName $pathPart))
        if (Test-Path -LiteralPath $dest) { return $m.Value }
                $label = $m.Groups[2].Value
        if ($m.Groups[1].Value -eq '!') { return "(${label}: image not shipped in this package)" }
        return "${label} (source repository: $($pathPart -replace '^(\.\./)+', ''))"
    }
    $new = $linkRe.Replace($text, $eval)
    if ($new -ne $text) { [IO.File]::WriteAllText($mdFile.FullName, $new, (New-Object Text.UTF8Encoding($false))) }
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

# Which DLLs does each shipped EXE import? dumpbin comes with the MSVC environment; without it the answer is 'unknown'.
$systemDlls = @('KERNEL32.dll', 'ADVAPI32.dll', 'WS2_32.dll', 'USER32.dll', 'GDI32.dll', 'SHELL32.dll', 'OLE32.dll', 'OLEAUT32.dll',
    'BCRYPT.dll', 'CRYPT32.dll', 'IPHLPAPI.DLL', 'SHLWAPI.dll', 'VERSION.dll', 'WINMM.dll', 'PSAPI.DLL', 'ntdll.dll', 'dxgi.dll', 'SETUPAPI.dll', 'ole32.dll', 'USERENV.dll', 'RPCRT4.dll', 'bcrypt.dll', 'cfgmgr32.dll', 'comdlg32.dll', 'IMM32.dll', 'WINSPOOL.DRV', 'MSVCRT.dll', 'NETAPI32.dll', 'SECHOST.dll', 'WINHTTP.dll', 'DWMAPI.DLL', 'UxTheme.dll', 'COMCTL32.dll', 'WTSAPI32.dll', 'api-ms-win-core-synch-l1-2-0.dll', 'dbghelp.dll')
$loaderDlls = @('vulkan-1.dll')   # system Vulkan loader: installed by the GPU driver, never bundled
$importReport = [ordered]@{}
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
foreach ($f in (Get-ChildItem -LiteralPath $pkg -File -Filter *.exe)) {
    if (-not $dumpbin) { $importReport[$f.Name] = 'unknown (dumpbin not on PATH)'; continue }
    $out = & $dumpbin.Source /nologo /dependents $f.FullName 2>&1
    $dlls = @($out | ForEach-Object { if ($_ -match '^\s+(\S+\.(?:dll|DLL|drv|DRV))\s*$') { $Matches[1] } })
    $importReport[$f.Name] = $dlls
}
$onlySystem = $true
foreach ($k in $importReport.Keys) {
    $v = $importReport[$k]
    if ($v -is [string]) { $onlySystem = $false; continue }
    foreach ($d in $v) { if (($systemDlls + $loaderDlls) -notcontains $d) { $onlySystem = $false } }
}
$isStatic = $Preset -match 'static'
$singleExe = [bool]($isStatic -and $onlySystem)

& (Join-Path $PSScriptRoot 'sbom.ps1') -OutFile (Join-Path $pkg 'sbom.cdx.json') -BuildDir $buildDir

$crtText = if ($isStatic) { 'static MSVC runtime (/MT): no Visual C++ Redistributable needed' } else { 'default dynamic MSVC runtime: the Microsoft Visual C++ Redistributable (x64) must be installed' }
$exeText = if ($singleExe) { "HCSInfer.exe imports only Windows system DLLs (checked with dumpbin /dependents at packaging time); no sidecar DLL is required.`n" } else { "Multi-file package: NOT a single-file executable (docs/decisions/ADR-008-static-crt-single-exe.md).`n" }
$vkText = if ($VulkanPreset) { "- HCSInfer-vulkan.exe is the same program with the Vulkan backend linked in. It additionally imports the system Vulkan loader vulkan-1.dll (installed by your GPU driver); it will not start on a machine without it. Use HCSInfer.exe (CPU) there.`n" } else { '' }
@"
HCSInfer $version portable folder ($Preset)

$exeText- Runtime: $crtText.
$vkText- No models, no graphics driver and no Vulkan SDK are included. Vulkan use needs the installed AMD/OEM driver.
- Run:  HCSInfer.exe --help   |   HCSInfer.exe doctor --json
- Verify files:  Get-FileHash -Algorithm SHA256 <file>  and compare with SHA256SUMS.txt.
- Third-party licences: THIRD_PARTY_NOTICES.md and the licenses/ folder. SBOM: sbom.cdx.json.
Documentation: README.md and docs/ (links to files not in this package - test evidence, sources - read "source repository: <path>").
Status: release candidate (see the version in the manifest). See README.md for what is implemented. Built from git commit $commit (manifest.json records the exact commit and whether the tree was dirty). Unsigned. A clean-machine run (no redistributable, fresh Windows) is NOT RUN.
"@ | Set-Content -LiteralPath (Join-Path $pkg 'PACKAGE-README.txt') -Encoding utf8

[ordered]@{
    name = $name; version = $version; preset = $Preset; git_commit = $commit; git_tree_dirty = $dirty
    created_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    single_exe = $singleExe; crt = $(if ($isStatic) { 'static (/MT)' } else { 'dynamic (/MD, default)' }); signed = $false
    imports = $importReport; vulkan_exe = [bool]$VulkanPreset
    clean_machine_test = 'NOT RUN'
    source_exe_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $pkg 'manifest.json') -Encoding utf8

# SHA256SUMS.txt over every file in the package except itself (sha256sum-compatible "hash  relative/path").
$sumFile = Join-Path $pkg 'SHA256SUMS.txt'
$lines = Get-ChildItem -LiteralPath $pkg -Recurse -File | Where-Object { $_.FullName -ne $sumFile } | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($pkg.Length + 1).Replace('\', '/')
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $rel
}
# LF-only (sha256sum-compatible); Set-Content would write CRLF.
[IO.File]::WriteAllText($sumFile, (($lines -join "`n") + "`n"), [Text.Encoding]::ASCII)
Write-Host "Package: $pkg ($(@($lines).Count) files hashed)"

if ($Zip) {
    $zipPath = "$pkg.zip"
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Compress-Archive -Path (Join-Path $pkg '*') -DestinationPath $zipPath
    $h = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText("$zipPath.sha256", "$h  $(Split-Path -Leaf $zipPath)`n", [Text.Encoding]::ASCII)
    Write-Host "Zip: $zipPath  sha256 $h"
}
