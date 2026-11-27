<#
.SYNOPSIS  Set the HCSInfer version consistently in every file that carries it. Supports -WhatIf (writes nothing).
.DESCRIPTION
  Input is one semver: 1.1.0, 1.1.0-rc.2, 1.2.0-beta.1 ...  (tag spelling without the leading "v").
  Files rewritten (only the version, nothing else):
    CMakeLists.txt            project(HCSInfer VERSION x.y.z) and set(HCS_VERSION_SUFFIX "...") (suffix "" for a stable version)
    python/pyproject.toml     PEP 440 spelling (-rc.N -> rcN, -beta.N -> bN, -alpha.N -> aN)
    web/package.json          semver; web/package-lock.json (top-level "version" and packages[""].version) when present
    web/js/config.js          version: "..."
    README.md, SECURITY.md, docs/install.md, docs/support-matrix.md, docs/pypi-publishing.md
                              every occurrence of the CURRENT version (semver, PEP 440 and the shields.io "--" spelling),
                              except links to docs/release-notes-<current>.md, which keep pointing at the file that exists.
  Historic spellings (v1.0.0-rc.1 ...), CHANGELOG, release notes, test evidence and STATUS are never touched.
  After a bump this script runs scripts/check-version.ps1 (skipped with -WhatIf). It does NOT edit prose such as
  "release candidate (not yet tagged)": review README/SECURITY/install wording by hand when moving from rc to stable.
  It never runs git, never tags, never commits.
.EXAMPLE   .\scripts\bump-version.ps1 -Version 1.1.0 -WhatIf
.EXAMPLE   .\scripts\bump-version.ps1 -Version 1.1.0-rc.2
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function ConvertTo-Pep440([string]$semver) {
    if ($semver -notmatch '^(\d+\.\d+\.\d+)(?:-(alpha|beta|rc)\.(\d+))?$') { throw "unsupported version spelling: '$semver' (expected x.y.z or x.y.z-(alpha|beta|rc).N)" }
    $base = $Matches[1]
    if (-not $Matches[2]) { return $base }
    $map = @{ alpha = 'a'; beta = 'b'; rc = 'rc' }
    return "$base$($map[$Matches[2]])$($Matches[3])"
}

function Read-Text([string]$path) { [System.IO.File]::ReadAllText($path, [System.Text.UTF8Encoding]::new($false)) }
function Write-Text([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, [System.Text.UTF8Encoding]::new($false)) }

$Version = $Version -replace '^v', ''
$newPep = ConvertTo-Pep440 $Version
$null = $Version -match '^(\d+\.\d+\.\d+)(-.+)?$'
$newBase = $Matches[1]
$newSuffix = if ($Matches.Count -gt 2 -and $Matches[2]) { $Matches[2] } else { '' }

$cmakePath = Join-Path $Root 'CMakeLists.txt'
$cmake = Read-Text $cmakePath
if ($cmake -notmatch '(?m)^\s*project\(\s*HCSInfer\s+VERSION\s+(\d+\.\d+\.\d+)') { throw 'CMakeLists.txt: project(HCSInfer VERSION x.y.z) not found' }
$oldBase = $Matches[1]
$oldSuffix = ''
if ($cmake -match '(?m)^\s*set\(\s*HCS_VERSION_SUFFIX\s+"([^"]*)"') { $oldSuffix = $Matches[1] }
$oldSemver = "$oldBase$oldSuffix"
$oldPep = ConvertTo-Pep440 $oldSemver

Write-Host "current: $oldSemver (PEP 440 $oldPep)  ->  new: $Version (PEP 440 $newPep)"
if ($oldSemver -eq $Version) { Write-Host 'already at that version: nothing to do'; exit 0 }

$changes = [System.Collections.Generic.List[object]]::new()
function Add-Change([string]$path, [string]$old, [string]$new) {
    if ($old -ne $new) { $changes.Add([pscustomobject]@{ Path = $path; New = $new }) }
}

# --- CMakeLists.txt
$c = [regex]::Replace($cmake, '(?m)^(\s*project\(\s*HCSInfer\s+VERSION\s+)\d+\.\d+\.\d+', "`${1}$newBase")
if ($c -match '(?m)^\s*set\(\s*HCS_VERSION_SUFFIX\s+"[^"]*"\)') {
    $c = [regex]::Replace($c, '(?m)^(\s*set\(\s*HCS_VERSION_SUFFIX\s+)"[^"]*"\)[^\r\n]*',
        "`${1}`"$newSuffix`")  # $Version; see docs/release-notes-$Version.md")
} else { throw 'CMakeLists.txt: set(HCS_VERSION_SUFFIX "...") not found' }
Add-Change $cmakePath $cmake $c

# --- python/pyproject.toml
$pyPath = Join-Path $Root 'python/pyproject.toml'
$py = Read-Text $pyPath
if ($py -notmatch '(?m)^version\s*=\s*"[^"]+"') { throw 'python/pyproject.toml: version not found' }
Add-Change $pyPath $py ([regex]::Replace($py, '(?m)^(version\s*=\s*)"[^"]+"', "`${1}`"$newPep`""))

# --- web/package.json (+ lock)
$pkgPath = Join-Path $Root 'web/package.json'
$pkg = Read-Text $pkgPath
if ($pkg -notmatch '(?m)^\s*"version"\s*:\s*"[^"]+"') { throw 'web/package.json: version not found' }
Add-Change $pkgPath $pkg ([regex]::Replace($pkg, '(?m)^(\s*"version"\s*:\s*)"[^"]+"', "`${1}`"$Version`"", 1))
$lockPath = Join-Path $Root 'web/package-lock.json'
if (Test-Path -LiteralPath $lockPath) {
    $lock = Read-Text $lockPath
    # the first two "version" members: top level and packages[""]
    $n = 0
    $l = [regex]::Replace($lock, '(?m)^(\s*"version"\s*:\s*)"[^"]+"', {
            param($m) $script:n++; if ($script:n -le 2) { $m.Groups[1].Value + '"' + $Version + '"' } else { $m.Value } })
    Add-Change $lockPath $lock $l
}

# --- web/js/config.js
$cfgPath = Join-Path $Root 'web/js/config.js'
if (Test-Path -LiteralPath $cfgPath) {
    $cfg = Read-Text $cfgPath
    Add-Change $cfgPath $cfg ([regex]::Replace($cfg, '(?m)^(\s*version\s*:\s*)"[^"]+"', "`${1}`"$Version`""))
}

# --- prose / docs: replace the current version spellings only
$shield = { param($s) $s -replace '-', '--' }
$docFiles = 'README.md', 'SECURITY.md', 'docs/install.md', 'docs/support-matrix.md', 'docs/pypi-publishing.md'
foreach ($rel in $docFiles) {
    $p = Join-Path $Root $rel
    if (-not (Test-Path -LiteralPath $p)) { continue }
    $t = Read-Text $p
    $guard = "__KEEP_RELEASE_NOTES_LINK__"
    $u = $t.Replace("release-notes-$oldSemver.md", "$guard")
    $u = $u.Replace((& $shield $oldSemver), (& $shield $Version))
    $u = $u.Replace($oldSemver, $Version).Replace($oldPep, $newPep)
    $u = $u.Replace($guard, "release-notes-$oldSemver.md")
    Add-Change $p $t $u
}

if ($changes.Count -eq 0) { Write-Host 'no file needs a change'; exit 0 }
foreach ($ch in $changes) {
    $rel = $ch.Path.Substring($Root.TrimEnd('\', '/').Length + 1)
    if ($PSCmdlet.ShouldProcess($rel, "set version $Version")) {
        Write-Text $ch.Path $ch.New
        Write-Host "updated  $rel"
    } else {
        Write-Host "would update  $rel"
    }
}
if ($WhatIfPreference) {
    Write-Host "WhatIf: nothing written. Re-run without -WhatIf to apply; then run scripts/check-version.ps1."
    exit 0
}
& (Join-Path $PSScriptRoot 'check-version.ps1') -Root $Root
exit $LASTEXITCODE
