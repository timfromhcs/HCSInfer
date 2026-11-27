<#
.SYNOPSIS  Test for scripts/bump-version.ps1 on a throw-away copy of the real version-bearing files.
.DESCRIPTION
  1. -WhatIf must write nothing (every copied file byte-identical afterwards).
  2. A real bump to a stable and to an rc version must leave scripts/check-version.ps1 green for the copy.
  3. Historic spellings (v1.0.0-rc.1) and the release-notes link target must survive; bad input must fail.
  The copy lives under $env:TEMP; the source tree itself is never modified.
#>
[CmdletBinding()]
param([string]$Source = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$files = 'CMakeLists.txt', 'python/pyproject.toml', 'web/package.json', 'web/js/config.js', 'README.md', 'SECURITY.md',
         'docs/install.md', 'docs/support-matrix.md', 'docs/pypi-publishing.md'
$tmp = if ($env:TEMP) { $env:TEMP } else { [System.IO.Path]::GetTempPath() }
$work = Join-Path $tmp ("hcs-bump-test-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$script:fail = @()
function Check([bool]$ok, [string]$msg) {
    if (-not $ok) { $script:fail += $msg; Write-Host "FAIL: $msg" } else { Write-Host "ok:   $msg" }
}
function Invoke-Bump([string[]]$extra) {
    $pwsh = (Get-Command pwsh, powershell -ErrorAction SilentlyContinue | Select-Object -First 1).Source
    $out = & $pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Source 'scripts/bump-version.ps1') -Root $work @extra 2>&1
    [pscustomobject]@{ Code = $LASTEXITCODE; Out = ($out | Out-String) }
}
function Get-Work([string]$rel) { Get-Content -LiteralPath (Join-Path $work $rel) -Raw }
try {
    foreach ($f in $files) {
        $dst = Join-Path $work $f
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
        Copy-Item -LiteralPath (Join-Path $Source $f) -Destination $dst
    }
    # check-version.ps1 reads these two as well; it lives in the source tree and is pointed at $work via -Root.
    $hashes = @{}
    foreach ($f in $files) { $hashes[$f] = (Get-FileHash -LiteralPath (Join-Path $work $f)).Hash }

    $r = Invoke-Bump @('-Version', '9.8.7', '-WhatIf')
    Check ($r.Code -eq 0) '-WhatIf exits 0'
    Check ($r.Out -match 'current: (\S+)') 'reports the current version'
    $oldSemver = $Matches[1]
    Check ($r.Out -match 'would update\s+CMakeLists.txt') '-WhatIf lists CMakeLists.txt'
    $same = $true
    foreach ($f in $files) { if ((Get-FileHash -LiteralPath (Join-Path $work $f)).Hash -ne $hashes[$f]) { $same = $false } }
    Check $same '-WhatIf leaves every file byte-identical'

    $r = Invoke-Bump @('-Version', '9.8.7')
    Check ($r.Code -eq 0) 'real bump to 9.8.7 exits 0 (check-version.ps1 green)'
    if ($r.Code -ne 0) { Write-Host $r.Out }
    Check ((Get-Work 'CMakeLists.txt') -match 'project\(HCSInfer VERSION 9\.8\.7 ') 'CMake base version set'
    Check ((Get-Work 'CMakeLists.txt') -match 'set\(HCS_VERSION_SUFFIX ""\)') 'CMake suffix emptied for a stable version'
    Check ((Get-Work 'python/pyproject.toml') -match '(?m)^version = "9\.8\.7"') 'pyproject PEP 440 version set'
    Check ((Get-Work 'web/package.json' | ConvertFrom-Json).version -eq '9.8.7') 'package.json version set'
    Check ((Get-Work 'web/js/config.js') -match 'version:\s*"9\.8\.7"') 'web config.js version set'
    $readme = Get-Work 'README.md'
    $stale = [regex]::Matches(($readme -replace [regex]::Escape("release-notes-$oldSemver.md"), ''), [regex]::Escape($oldSemver)).Count
    Check ($stale -eq 0) 'README has no stale current version (release-notes link target excluded)'
    Check ($readme -match 'v1\.0\.0-rc\.1') 'README historic v1.0.0-rc.1 untouched'
    Check ($readme -match 'version-9\.8\.7-') 'README shields.io badge updated'

    $r = Invoke-Bump @('-Version', '9.9.0-rc.2')
    Check ($r.Code -eq 0) 'bump to 9.9.0-rc.2 exits 0'
    Check ((Get-Work 'python/pyproject.toml') -match '(?m)^version = "9\.9\.0rc2"') 'rc spelled rc2 for PEP 440'
    Check ((Get-Work 'CMakeLists.txt') -match 'set\(HCS_VERSION_SUFFIX "-rc\.2"\)') 'CMake suffix -rc.2'
    Check ((Get-Work 'README.md') -match 'version-9\.9\.0--rc\.2-') 'badge spells -rc.2 with a doubled dash'

    $r = Invoke-Bump @('-Version', 'not-a-version')
    Check ($r.Code -ne 0) 'invalid version is rejected'
} finally {
    if ((Test-Path -LiteralPath $work) -and $work.StartsWith($tmp, [System.StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $work -Recurse -Force
    }
}
if ($script:fail.Count -gt 0) { Write-Host ("{0} check(s) failed" -f $script:fail.Count); exit 1 }
Write-Host 'bump-version test PASS'
exit 0
