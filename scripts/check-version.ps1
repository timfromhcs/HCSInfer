<#
.SYNOPSIS  Assert that the release tag, CMake version, python/pyproject.toml and web/package.json agree.
.DESCRIPTION
  Canonical version = CMake `project(HCSInfer VERSION x.y.z)` + `set(HCS_VERSION_SUFFIX "-rc.N")`.
  Spellings that must match (AGENTS.md 22, release.yml):
    tag            v1.1.0-rc.1   (or v1.1.0 for stable)
    CMake          1.1.0 + "-rc.1"
    web/package    1.1.0-rc.1
    pyproject      1.1.0rc1      (PEP 440: -rc.N -> rcN, -beta.N -> bN, -alpha.N -> aN)
  Writes `prerelease=true|false` and `version=<semver>` to $GITHUB_OUTPUT when that variable is set.
  Read-only: never modifies any file.
.EXAMPLE   .\scripts\check-version.ps1 -Tag v1.1.0-rc.1
#>
[CmdletBinding()]
param(
    [string]$Tag = '',
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function ConvertTo-Pep440([string]$semver) {
    if ($semver -notmatch '^(\d+\.\d+\.\d+)(?:-(alpha|beta|rc)\.(\d+))?$') { throw "unsupported version spelling: '$semver'" }
    $base = $Matches[1]
    if (-not $Matches[2]) { return $base }
    $map = @{ alpha = 'a'; beta = 'b'; rc = 'rc' }
    return "$base$($map[$Matches[2]])$($Matches[3])"
}

$cmake = Get-Content (Join-Path $Root 'CMakeLists.txt') -Raw
if ($cmake -notmatch '(?m)^\s*project\(\s*HCSInfer\s+VERSION\s+(\d+\.\d+\.\d+)') { throw 'CMakeLists.txt: project(HCSInfer VERSION x.y.z) not found' }
$base = $Matches[1]
$suffix = ''
if ($cmake -match '(?m)^\s*set\(\s*HCS_VERSION_SUFFIX\s+"([^"]*)"') { $suffix = $Matches[1] }
$semver = "$base$suffix"
$pep = ConvertTo-Pep440 $semver

$pyproj = Get-Content (Join-Path $Root 'python/pyproject.toml') -Raw
if ($pyproj -notmatch '(?m)^version\s*=\s*"([^"]+)"') { throw 'python/pyproject.toml: version not found' }
$pyVersion = $Matches[1]
$pkgVersion = (Get-Content (Join-Path $Root 'web/package.json') -Raw | ConvertFrom-Json).version

$errors = @()
if ($pyVersion -ne $pep) { $errors += "python/pyproject.toml version '$pyVersion' != expected PEP 440 '$pep'" }
if ($pkgVersion -ne $semver) { $errors += "web/package.json version '$pkgVersion' != CMake '$semver'" }
if ($Tag) {
    $t = $Tag -replace '^refs/tags/', ''
    if ($t -ne "v$semver") { $errors += "tag '$t' != 'v$semver' (CMake HCS_VERSION + HCS_VERSION_SUFFIX)" }
}
if ($semver -match '-(alpha|beta|rc)\.\d+$') { $pre = 'true' } else { $pre = 'false' }
if ($Tag -and (($Tag -match '-(rc|beta|alpha)') -ne ($pre -eq 'true'))) { $errors += "tag pre-release marker disagrees with CMake suffix '$suffix'" }

"cmake=$semver pyproject=$pyVersion (expected $pep) web=$pkgVersion tag='$Tag' prerelease=$pre"
if ($env:GITHUB_OUTPUT) {
    "version=$semver" | Out-File -Append -Encoding utf8 $env:GITHUB_OUTPUT
    "prerelease=$pre" | Out-File -Append -Encoding utf8 $env:GITHUB_OUTPUT
}
if ($errors.Count -gt 0) { $errors | ForEach-Object { Write-Error $_ -ErrorAction Continue }; exit 1 }
Write-Host 'VERSION CONSISTENT'
exit 0
