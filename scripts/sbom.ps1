<#
.SYNOPSIS
  Generates a CycloneDX 1.5 style JSON SBOM from external/dependency-lock.json (plus the supplemental
  components listed below that the lock does not carry yet). Read-only with respect to the repository;
  writes only the output file.
.PARAMETER OutFile
  Default: <repo>/artifacts/sbom/hcsinfer.cdx.json (artifacts/ is git-ignored).
.NOTES
  The SBOM states what the lock file declares. It is not a vulnerability scan and not a substitute for the
  licence review required by AGENTS.md section 19.6; components with unknown licences are reported as such.
#>
[CmdletBinding()]
param([string]$OutFile)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')

$root = Get-RepoRoot
if (-not $OutFile) { $OutFile = Join-Path $root 'artifacts\sbom\hcsinfer.cdx.json' }
$lockPath = Join-Path $root 'external\dependency-lock.json'
if (-not (Test-Path -LiteralPath $lockPath)) { throw "Missing $lockPath" }
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json

function Get-Prop($obj, [string]$name) {
    $p = $obj.PSObject.Properties[$name]
    if ($p) { return $p.Value } else { return $null }
}

# Components that are built/fetched by CMake but were missing from external/dependency-lock.json when this
# script was written (src/hardware/CMakeLists.txt FetchContent, tag v1.4.357, commit seen in the build tree).
# Remove an entry here once the lock file carries it.
$supplemental = @(
    [pscustomobject]@{
        name = 'Vulkan-Headers'; url = 'https://github.com/KhronosGroup/Vulkan-Headers'
        version = 'v1.4.357'; commit = 'e3b1eec08173d6b825cd3ac88c885a63b621504a'
        role = 'header-only Vulkan declarations for src/hardware (vulkan-1.dll is loaded dynamically)'
        licence = 'Apache-2.0 OR MIT'; linked = $true; source = 'FetchContent (src/hardware/CMakeLists.txt)'
    })

$knownSpdx = 'MIT', 'BSD-2-Clause', 'BSD-3-Clause', 'Apache-2.0', 'Zlib', 'ISC'

function ConvertTo-LicenseEntry([string]$text) {
    if (-not $text) { return @(@{ license = @{ name = 'unknown' } }) }
    if ($knownSpdx -contains $text) { return @(@{ license = @{ id = $text } }) }
    if ($text -match '^[A-Za-z0-9.+-]+ OR [A-Za-z0-9.+ ()-]+$' -and -not ($text -match '\(')) {
        return @(@{ expression = $text })
    }
    return @(@{ license = @{ name = $text } })
}

function Get-Sha256([string]$path) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$version = '0.0.0'
$cml = Join-Path $root 'CMakeLists.txt'
if (Test-Path -LiteralPath $cml) {
    $m = Select-String -LiteralPath $cml -Pattern 'project\(\s*HCSInfer\s+VERSION\s+([0-9.]+)' | Select-Object -First 1
    if ($m) { $version = $m.Matches[0].Groups[1].Value }
}
$gitCommit = 'unknown'
try {
    $c = (& git -C $root rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $c) { $gitCommit = $c.Trim() }
} catch { }

$components = New-Object System.Collections.Generic.List[object]
$all = @($lock.dependencies) + $supplemental
foreach ($d in $all) {
    $name = [string](Get-Prop $d 'name')
    $commit = Get-Prop $d 'commit'
    $ver = Get-Prop $d 'version'
    $url = Get-Prop $d 'url'
    $versionStr = if ($commit) { [string]$commit } elseif ($ver) { [string]$ver } else { 'unspecified' }
    $comp = [ordered]@{
        type    = 'library'
        'bom-ref' = ('hcs:' + ($name -replace '[^A-Za-z0-9._-]', '_'))
        name    = $name
        version = $versionStr
        licenses = @(ConvertTo-LicenseEntry ([string](Get-Prop $d 'licence')))
    }
    $desc = Get-Prop $d 'role'
    if ($desc) { $comp['description'] = [string]$desc }
    if ($url -and $url -match '^https://github\.com/([^/]+)/([^/ ]+)') {
        $ref = if ($commit) { $commit } elseif ($ver) { $ver } else { $null }
        if ($ref) { $comp['purl'] = "pkg:github/$($Matches[1])/$($Matches[2])@$ref" }
    }
    if ($url) { $comp['externalReferences'] = @(@{ type = 'vcs'; url = [string]$url }) }
    $props = @()
    $linked = Get-Prop $d 'linked'
    if ($null -ne $linked) { $props += @{ name = 'hcs:linked'; value = ([string]$linked).ToLowerInvariant() } }
    $src = Get-Prop $d 'source'
    if ($src) { $props += @{ name = 'hcs:source'; value = [string]$src } }
    $lf = Get-Prop $d 'licence_file'
    if ($lf) {
        $props += @{ name = 'hcs:licence_file'; value = [string]$lf }
        $lfAbs = Join-Path $root ($lf -replace '/', '\')
        if (Test-Path -LiteralPath $lfAbs -PathType Leaf) {
            $props += @{ name = 'hcs:licence_file_sha256'; value = Get-Sha256 $lfAbs }
        } else {
            Write-Warning "licence file for '$name' not found: $lf"
        }
    }
    if ($props.Count) { $comp['properties'] = $props }
    $components.Add($comp)
}

$bom = [ordered]@{
    bomFormat    = 'CycloneDX'
    specVersion  = '1.5'
    serialNumber = 'urn:uuid:' + [guid]::NewGuid().ToString()
    version      = 1
    metadata     = [ordered]@{
        timestamp = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
        tools     = @(@{ vendor = 'HCSInfer'; name = 'scripts/sbom.ps1' })
        component = [ordered]@{
            type = 'application'; 'bom-ref' = 'hcs:HCSInfer'; name = 'HCSInfer'; version = $version
            licenses = @(@{ license = @{ id = 'MIT' } })
            properties = @(@{ name = 'hcs:git_commit'; value = $gitCommit })
        }
        properties = @(@{ name = 'hcs:source_lock'; value = 'external/dependency-lock.json' },
                       @{ name = 'hcs:note'; value = 'Declared dependencies only; no vulnerability scan performed.' })
    }
    components = $components
}

$outDir = Split-Path -Parent $OutFile
if ($outDir -and -not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
($bom | ConvertTo-Json -Depth 12) | Set-Content -LiteralPath $OutFile -Encoding utf8
Write-Host "SBOM written: $OutFile ($($components.Count) components, HCSInfer $version, commit $gitCommit)"
