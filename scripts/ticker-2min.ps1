# Development-only reminder loop. NOT a wait timer: the agent keeps working continuously.
# Sends a short message into the active OpenCode session every 120 s so a stalled/fallen
# agent is nudged back into the stable-release plan. Logs stay under D:\HCSScratch\ticker.
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
    $prompt = "TICK $n ($ts): reminder only. State your current step in one line and keep working on the stable-release plan; never wait for this ticker."
    # 'run' takes the message as a positional argument; --prompt belongs to the root command and prints help here.
    & opencode run -s $session $prompt 2>&1 | Out-File -FilePath $runLog -Append -Encoding utf8
}
