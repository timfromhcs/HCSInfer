<#
.SYNOPSIS  Decide, from N independent `HCSInfer tune --out` evidence files, which HCS kernel/bucket pairs are promoted.
.DESCRIPTION
  The promotion rule (AGENTS.md 12.5 / 16.6, docs/vulkan-kernels.md) applied ACROSS repeated whole-suite runs:
  a (kernel, bucket) is PROMOTED only when, in EVERY run, it was correct, produced 3/3 bit-identical outputs, had a
  timing spread <= 0.25 (HCS and upstream), beat the CPU baseline by >= 1.05x AND beat the measured upstream GGML
  Vulkan operator by >= 1.05x (ratio = upstream ms / HCS ms). Anything else is NOT promoted, with the first failing
  reason. A single run is never enough ("repeat the whole tune 3 times"). Reads files only; changes nothing.
.PARAMETER Evidence   One or more evidence JSON files (promotion needs -MinRuns, default 3) written by `HCSInfer tune --out <file>`.
.PARAMETER OutJson    Optional path for the machine-readable decision.
.PARAMETER OutMarkdown Optional path for the markdown table.
.EXAMPLE   .\scripts\kernel-decision.ps1 -Evidence run1.json,run2.json,run3.json -OutMarkdown decision.md
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$Evidence,
    [string]$OutJson,
    [string]$OutMarkdown,
    [double]$MinSpeedup = 1.05,
    [double]$MaxSpread = 0.25,
    [int]$MinRuns = 3
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture
if ($Evidence.Count -lt 1) { throw 'Need at least 1 evidence file.' }

$runs = @()
foreach ($f in $Evidence) {
    $j = Get-Content -LiteralPath $f -Raw | ConvertFrom-Json
    if ($j.schema_version -ne 1 -or $j.generator -ne 'HCSInfer tune') { throw "$f is not a tune evidence file" }
    $runs += , $j
}
$dev0 = $runs[0].device.device_key
foreach ($r in $runs) { if ($r.device.device_key -ne $dev0) { throw "evidence files are from different devices/drivers: $dev0 vs $($r.device.device_key)" } }

function Get-Prop($o, [string]$n) { if ($null -ne $o -and $o.PSObject.Properties.Name -contains $n) { $o.$n } else { $null } }

$keys = [ordered]@{}
foreach ($r in $runs) {
    foreach ($res in $r.results) {
        if ($null -eq (Get-Prop $res 'bucket')) { continue }
        $keys["$($res.kernel)|$($res.bucket)"] = @($res.kernel, $res.bucket)
    }
}

$rows = @()
foreach ($k in $keys.Keys) {
    $kernel = $keys[$k][0]; $bucket = $keys[$k][1]
    $ratios = @(); $cpu = @(); $spH = @(); $spU = @(); $reason = $null; $seen = 0; $ups = 'n/a'
    foreach ($r in $runs) {
        $e = $r.results | Where-Object { $_.kernel -eq $kernel -and (Get-Prop $_ 'bucket') -eq $bucket } | Select-Object -First 1
        if ($null -eq $e) { if (-not $reason) { $reason = 'missing in a run (skipped or not run)' }; continue }
        $seen++
        if (-not $e.correct) { if (-not $reason) { $reason = 'incorrect result in a run' }; continue }
        if ($e.identical_runs -ne $e.total_runs -or $e.total_runs -lt 3) { if (-not $reason) { $reason = "not 3/3 bit-identical ($($e.identical_runs)/$($e.total_runs))" } }
        $spH += [double]$e.wall_spread_p10_p90_over_median
        $cpu += [double]$e.speedup_vs_baseline
        $u = $e.upstream_ggml_vulkan
        $ups = [string]$u.status
        if ($u.status -ne 'measured') { if (-not $reason) { $reason = "upstream baseline $($u.status)" + $(if ($u.note) { ": $($u.note)" } else { '' }) } }
        else {
            $ratios += [double]$u.ratio_upstream_over_hcs
            $spU += [double]$u.spread_p10_p90_over_median
        }
    }
    if ($seen -ne $runs.Count -and -not $reason) { $reason = 'missing in a run' }
    $minCpu = if ($cpu.Count) { ($cpu | Measure-Object -Minimum).Minimum } else { 0 }
    $minRatio = if ($ratios.Count -eq $runs.Count) { ($ratios | Measure-Object -Minimum).Minimum } else { $null }
    $maxRatio = if ($ratios.Count) { ($ratios | Measure-Object -Maximum).Maximum } else { $null }
    $maxSpH = if ($spH.Count) { ($spH | Measure-Object -Maximum).Maximum } else { 0 }
    $maxSpU = if ($spU.Count) { ($spU | Measure-Object -Maximum).Maximum } else { 0 }
    if (-not $reason) {
        if ($maxSpH -gt $MaxSpread) { $reason = ('HCS timing too noisy in a run (spread {0:N3} > {1})' -f $maxSpH, $MaxSpread) }
        elseif ($maxSpU -gt $MaxSpread) { $reason = ('upstream timing too noisy in a run (spread {0:N3} > {1})' -f $maxSpU, $MaxSpread) }
        elseif ($minCpu -lt $MinSpeedup) { $reason = ('below {0}x vs the CPU baseline in a run (min {1:N2}x)' -f $MinSpeedup, $minCpu) }
        elseif ($null -eq $minRatio -or $minRatio -lt $MinSpeedup) { $reason = ('slower than / not >= {0}x faster than the upstream GGML Vulkan operator (ratio min {1:N2}x, max {2:N2}x)' -f $MinSpeedup, $minRatio, $maxRatio) }
        elseif ($runs.Count -lt $MinRuns) { $reason = ('only {0} run(s): the rule needs {1} whole-suite runs, so nothing is promoted yet (single-run ratio min {2:N2}x)' -f $runs.Count, $MinRuns, $minRatio) }
    }
    $rows += [pscustomobject]@{
        kernel = $kernel; bucket = $bucket
        promoted = ($null -eq $reason)
        vs_cpu_min = [math]::Round($minCpu, 2)
        vs_upstream_min = if ($null -ne $minRatio) { [math]::Round($minRatio, 3) } else { $null }
        vs_upstream_max = if ($null -ne $maxRatio) { [math]::Round($maxRatio, 3) } else { $null }
        hcs_spread_max = [math]::Round($maxSpH, 3); upstream_spread_max = [math]::Round($maxSpU, 3)
        upstream_status = $ups
        reason = $reason
    }
}

$promoted = @($rows | Where-Object promoted)
$decision = [ordered]@{
    schema_version = 1
    rule = "promoted iff in ALL $($runs.Count) runs: correct, 3/3 identical, spread <= $MaxSpread, >= ${MinSpeedup}x vs CPU baseline, >= ${MinSpeedup}x vs upstream GGML Vulkan (ratio = upstream ms / HCS ms)"
    runs = $runs.Count
    evidence = $Evidence
    device_key = $dev0
    device = $runs[0].device.name
    git_commit = $runs[0].git_commit
    promoted_count = $promoted.Count
    total_pairs = $rows.Count
    rows = $rows
}
if ($OutJson) { $decision | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutJson -Encoding utf8 }

$md = New-Object System.Text.StringBuilder
[void]$md.AppendLine("Decision over $($runs.Count) runs on $($decision.device) ($dev0), HCS commit $($decision.git_commit).")
[void]$md.AppendLine("Rule: $($decision.rule)")
[void]$md.AppendLine('')
[void]$md.AppendLine('| kernel | bucket | vs CPU (min) | vs upstream GGML (min..max) | HCS/upstream spread (max) | promoted | first failing reason |')
[void]$md.AppendLine('|---|---|---|---|---|---|---|')
foreach ($r in $rows) {
    $u = if ($null -ne $r.vs_upstream_min) { ('{0:N2}x .. {1:N2}x' -f $r.vs_upstream_min, $r.vs_upstream_max) } elseif ($r.upstream_status -ne 'measured') { $r.upstream_status } else { 'n/a' }
    [void]$md.AppendLine(('| {0} | {1} | {2:N2}x | {3} | {4:N2} / {5:N2} | {6} | {7} |' -f $r.kernel, $r.bucket, $r.vs_cpu_min, $u, $r.hcs_spread_max, $r.upstream_spread_max, $(if ($r.promoted) { 'YES' } else { 'no' }), $r.reason))
}
[void]$md.AppendLine('')
[void]$md.AppendLine("Promoted: $($promoted.Count) of $($rows.Count) kernel/bucket pairs.")
if ($OutMarkdown) { $md.ToString() | Set-Content -LiteralPath $OutMarkdown -Encoding utf8 }
$md.ToString()
