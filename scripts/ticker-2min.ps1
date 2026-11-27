# Development-only reminder loop. NOT a wait timer: the agent keeps working continuously.
# Posts a short "steer" message into the active OpenCode session every 120 s so a stalled/fallen
# agent is nudged back into the stable-release plan. Uses the server API (non-blocking) rather than
# `opencode run`, which blocks while the session is busy. Logs stay under D:\HCSScratch\ticker.
$ErrorActionPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest

$session = if ($env:HCS_TICKER_SESSION) { $env:HCS_TICKER_SESSION } else { 'ses_de5b8535cffe9R1DJ59Z7mb8ZT' }
$dir = 'D:\HCSScratch\ticker'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$env:TEMP = 'D:\HCSScratch\tmp'
$env:TMP = $env:TEMP
$log = Join-Path $dir 'ticker.log'
$runLog = Join-Path $dir 'run.log'
$n = 0

while ($true) {
    Start-Sleep -Seconds 120
    $n++
    $ts = Get-Date -Format o
    "$ts tick=$n" | Out-File -FilePath $log -Append -Encoding utf8
    $text = "TICK $n ($ts): reminder only. State your current step in one line and keep working on the stable-release plan; never wait for this ticker."
    $body = @{ text = $text } | ConvertTo-Json -Compress
    # The server API returns immediately (unlike `opencode run`, which blocks while the session is busy).
    & opencode api POST "/api/session/$session/prompt" --data $body 2>&1 |
        Out-File -FilePath $runLog -Append -Encoding utf8
}
