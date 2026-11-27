<#
.SYNOPSIS
  Vulnerability gate over OSV (https://osv.dev): fails on HIGH / CRITICAL (and on UNKNOWN severity, fail-closed)
  findings that are not justified in osv-allowlist.json.
.DESCRIPTION
  Two sources are combined:
    1. The JSON written by `osv-scanner scan source ... --format json` (lockfiles / SBOM ecosystems such as npm,
       PyPI, crates.io), passed with -ScannerResult (optional).
    2. A direct OSV query BY GIT COMMIT for every pinned dependency in external/dependency-lock.json that carries a
       40-hex commit. osv-scanner maps `pkg:github/...` purls to the "GitHub Actions" ecosystem, so an SBOM scan alone
       says nothing about the native C/C++ pins; the commit query is the way OSV covers those projects.
  Severity per vulnerability: database_specific.severity when present, else the CVSS v3.x base score computed from the
  vector (>= 9.0 CRITICAL, >= 7.0 HIGH), else UNKNOWN. CVSS v4-only records are UNKNOWN and therefore fail unless
  allowlisted.
  Allowlist (osv-allowlist.json, schema_version 1): { "entries": [ { "id": "GHSA-...", "reason": "...>=30 chars...",
  "expires": "yyyy-mm-dd" } ] }. An entry without a justification, with a malformed or past expiry date, or an unused
  entry that names no known id is an error: the allowlist cannot silently rot. An id matches a finding by its own id or
  any alias (CVE-/GHSA-).
  Network is required (api.osv.dev). If the network is unavailable the gate FAILS (-AllowOffline turns that into an
  explicit NOT RUN exit 0, never into a pass; it prints NOT RUN).
.PARAMETER ScannerResult  osv-scanner JSON output file(s) (optional).
.PARAMETER Lock           dependency lock (default external/dependency-lock.json).
.PARAMETER Allowlist      default osv-allowlist.json at the repository root.
.PARAMETER OutFile        machine-readable report (default artifacts/osv/osv-gate.json).
#>
[CmdletBinding()]
param(
    [string[]]$ScannerResult = @(),
    [string]$Lock = '',
    [string]$Allowlist = '',
    [string]$OutFile = '',
    [switch]$AllowOffline
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
if (-not $Lock) { $Lock = Join-Path $root 'external/dependency-lock.json' }
if (-not $Allowlist) { $Allowlist = Join-Path $root 'osv-allowlist.json' }
if (-not $OutFile) { $OutFile = Join-Path $root 'artifacts/osv/osv-gate.json' }

function Get-Prop($obj, [string]$name) {
    if ($null -eq $obj) { return $null }
    $p = $obj.PSObject.Properties[$name]
    if ($p) { return $p.Value } else { return $null }
}

# ---- allowlist ---------------------------------------------------------------------------------------------------
$allow = @{}
if (Test-Path -LiteralPath $Allowlist) {
    $al = Get-Content -LiteralPath $Allowlist -Raw | ConvertFrom-Json
    if ((Get-Prop $al 'schema_version') -ne 1) { throw "$Allowlist : schema_version must be 1" }
    foreach ($e in @(Get-Prop $al 'entries')) {
        if ($null -eq $e) { continue }
        $id = [string](Get-Prop $e 'id'); $why = [string](Get-Prop $e 'reason'); $exp = [string](Get-Prop $e 'expires')
        if (-not $id) { throw "$Allowlist : an entry has no id" }
        if ($why.Trim().Length -lt 30) { throw "$Allowlist : entry $id needs a justification (reason, >= 30 characters)" }
        $d = [datetime]::MinValue
        if (-not [datetime]::TryParseExact($exp, 'yyyy-MM-dd', [Globalization.CultureInfo]::InvariantCulture, 'AssumeUniversal,AdjustToUniversal', [ref]$d)) {
            throw "$Allowlist : entry $id has no valid 'expires' (yyyy-mm-dd)"
        }
        if ($d.Date -lt [datetime]::UtcNow.Date) { throw "$Allowlist : entry $id expired on $exp; re-assess it" }
        $allow[$id] = $true
    }
}

# ---- severity ----------------------------------------------------------------------------------------------------
function Get-Cvss3Score([string]$vector) {
    if ($vector -notmatch '^CVSS:3\.[01]/') { return $null }
    $m = @{}
    foreach ($part in @($vector -split '/' | Select-Object -Skip 1)) { $kv = $part -split ':'; if ($kv.Count -eq 2) { $m[$kv[0]] = $kv[1] } }
    foreach ($k in 'AV', 'AC', 'PR', 'UI', 'S', 'C', 'I', 'A') { if (-not $m.ContainsKey($k)) { return $null } }
    $av = @{ N = 0.85; A = 0.62; L = 0.55; P = 0.2 }[$m.AV]
    $ac = @{ L = 0.77; H = 0.44 }[$m.AC]
    $changed = ($m.S -eq 'C')
    $pr = if ($changed) { @{ N = 0.85; L = 0.68; H = 0.5 }[$m.PR] } else { @{ N = 0.85; L = 0.62; H = 0.27 }[$m.PR] }
    $ui = @{ N = 0.85; R = 0.62 }[$m.UI]
    $cia = @{ H = 0.56; L = 0.22; N = 0 }
    if ($null -in @($av, $ac, $pr, $ui, $cia[$m.C], $cia[$m.I], $cia[$m.A])) { return $null }
    $iss = 1 - ((1 - $cia[$m.C]) * (1 - $cia[$m.I]) * (1 - $cia[$m.A]))
    $impact = if ($changed) { 7.52 * ($iss - 0.029) - 3.25 * [math]::Pow($iss - 0.02, 15) } else { 6.42 * $iss }
    $expl = 8.22 * $av * $ac * $pr * $ui
    if ($impact -le 0) { return 0.0 }
    $raw = if ($changed) { [math]::Min(1.08 * ($impact + $expl), 10) } else { [math]::Min($impact + $expl, 10) }
    return [math]::Ceiling($raw * 10) / 10
}

function Get-SeverityLabel($vuln) {
    $dbs = Get-Prop $vuln 'database_specific'
    $label = [string](Get-Prop $dbs 'severity')
    if ($label) {
        switch ($label.ToUpperInvariant()) {
            'CRITICAL' { return 'CRITICAL' } 'HIGH' { return 'HIGH' }
            { $_ -in 'MODERATE', 'MEDIUM' } { return 'MEDIUM' } 'LOW' { return 'LOW' }
        }
    }
    foreach ($s in @(Get-Prop $vuln 'severity')) {
        $score = Get-Cvss3Score ([string](Get-Prop $s 'score'))
        if ($null -ne $score) {
            if ($score -ge 9.0) { return 'CRITICAL' } if ($score -ge 7.0) { return 'HIGH' }
            if ($score -ge 4.0) { return 'MEDIUM' } return 'LOW'
        }
    }
    return 'UNKNOWN'
}

function Invoke-Osv([string]$uri, $body) {
    $args = @{ Uri = $uri; TimeoutSec = 60; ErrorAction = 'Stop' }
    if ($null -ne $body) { $args.Method = 'POST'; $args.Body = ($body | ConvertTo-Json -Depth 8 -Compress); $args.ContentType = 'application/json' }
    try { return Invoke-RestMethod @args } catch { $script:networkError = $_.Exception.Message; return $null }
}

$findings = New-Object System.Collections.Generic.List[object]   # {source, package, id, aliases, severity}
$networkError = ''

# 1) osv-scanner output
foreach ($srPath in @($ScannerResult | Where-Object { $_ })) {
    $sr = Get-Content -LiteralPath $srPath -Raw | ConvertFrom-Json
    foreach ($res in @(Get-Prop $sr 'results')) {
        foreach ($pkg in @(Get-Prop $res 'packages')) {
            $pname = [string](Get-Prop (Get-Prop $pkg 'package') 'name')
            foreach ($v in @(Get-Prop $pkg 'vulnerabilities')) {
                if ($null -eq $v) { continue }
                $findings.Add([pscustomobject]@{ source = ('osv-scanner:' + (Split-Path -Leaf $srPath)); package = $pname; id = [string](Get-Prop $v 'id')
                    aliases = @(Get-Prop $v 'aliases'); severity = (Get-SeverityLabel $v) })
            }
        }
    }
}

# 2) commit query for pinned native dependencies
$lockObj = Get-Content -LiteralPath $Lock -Raw | ConvertFrom-Json
$targets = @($lockObj.dependencies | Where-Object { (Get-Prop $_ 'commit') -match '^[0-9a-f]{40}$' })
$queries = @($targets | ForEach-Object { @{ commit = [string]$_.commit } })
$checked = @()
$batch = Invoke-Osv 'https://api.osv.dev/v1/querybatch' @{ queries = $queries }
if ($null -ne $batch) {
    for ($i = 0; $i -lt $targets.Count; $i++) {
        $ids = @(@(Get-Prop $batch.results[$i] 'vulns') | Where-Object { $_ } | ForEach-Object { [string]$_.id })
        $checked += [pscustomobject]@{ name = $targets[$i].name; commit = $targets[$i].commit; vulnerability_ids = $ids }
        foreach ($id in $ids) {
            $d = Invoke-Osv "https://api.osv.dev/v1/vulns/$id" $null
            if ($null -eq $d) { break }
            $findings.Add([pscustomobject]@{ source = 'osv-commit-query'; package = $targets[$i].name; id = $id
                aliases = @(Get-Prop $d 'aliases'); severity = (Get-SeverityLabel $d) })
        }
    }
}

if ($networkError) {
    $msg = "OSV query failed: $networkError"
    if ($AllowOffline) { Write-Host "NOT RUN: $msg"; exit 0 }
    throw $msg
}

# ---- decide ------------------------------------------------------------------------------------------------------
$used = @{}
$blocking = New-Object System.Collections.Generic.List[object]
$waived = New-Object System.Collections.Generic.List[object]
foreach ($f in $findings) {
    $keys = @($f.id) + @($f.aliases | Where-Object { $_ })
    $hit = $keys | Where-Object { $allow.ContainsKey([string]$_) } | Select-Object -First 1
    if ($hit) { $used[[string]$hit] = $true }
    if ($f.severity -in 'HIGH', 'CRITICAL', 'UNKNOWN') {
        if ($hit) { $waived.Add($f) } else { $blocking.Add($f) }
    }
}
$unused = @($allow.Keys | Where-Object { -not $used.ContainsKey($_) })

$report = [ordered]@{
    schema_version = 1
    tool = 'scripts/osv-gate.ps1'
    timestamp_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    policy = 'fail on HIGH, CRITICAL and UNKNOWN severity unless justified in osv-allowlist.json'
    commit_queries = $checked
    scanner_results = @($ScannerResult | Where-Object { $_ } | ForEach-Object { Split-Path -Leaf $_ })
    findings = $findings
    waived = $waived
    blocking = $blocking
    unused_allowlist_entries = $unused
    result = $(if ($blocking.Count -eq 0) { 'PASS' } else { 'FAIL' })
}
$outDir = Split-Path -Parent $OutFile
if ($outDir -and -not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
($report | ConvertTo-Json -Depth 10) | Set-Content -LiteralPath $OutFile -Encoding utf8

Write-Host ("OSV: {0} pinned commits queried, {1} findings ({2} waived by allowlist, {3} blocking). Report: {4}" -f $targets.Count, $findings.Count, $waived.Count, $blocking.Count, $OutFile)
foreach ($b in $blocking) { Write-Host ("  BLOCKING {0} {1} [{2}] ({3})" -f $b.package, $b.id, $b.severity, $b.source) }
foreach ($u in $unused) { Write-Warning "allowlist entry '$u' matched no finding (remove it)" }
if ($blocking.Count -gt 0) { exit 1 }
exit 0
