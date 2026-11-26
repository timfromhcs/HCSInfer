param([string]$Exe='D:\HCSInfer\build\f1\bin\HCSInfer.exe', [string]$Out='D:\HCSScratch\f1\evidence')
$ErrorActionPreference='Stop'
. D:\HCSInfer\.claude\worktrees\wf_3ff35c84-7c1-1\scripts\env-d-e.ps1 | Out-Null
New-Item -ItemType Directory -Force $Out | Out-Null
$repo='D:\HCSInfer\.claude\worktrees\wf_3ff35c84-7c1-1'
$state='D:\HCSScratch\f1\state'; New-Item -ItemType Directory -Force $state | Out-Null
$env:HCS_DATA_DIR=$state
$tok = -join ((1..32) | % { '{0:x}' -f (Get-Random -Maximum 16) })
$env:HCS_API_TOKEN=$tok
$port=18765
$base="http://127.0.0.1:$port"
$hdr=@{Authorization="Bearer $tok"}
$lock='D:\HCSScratch\realmodel.lock'
$have=$false
$srv=$null
function Log($m){ $l="$(Get-Date -Format o) $m"; Write-Host $l; Add-Content "$Out\driver.log" $l }
function FreeMiB { [int]((Invoke-RestMethod "$base/hcs/v1/runtime/overload" -Headers $hdr).sample.free_physical_bytes/1MB) }
function Start-Srv([string[]]$extra, [string]$name){
  $a=@('serve','--port',"$port",'--model-root','D:\HCSModels\vision')+$extra
  $p=Start-Process -FilePath $Exe -ArgumentList $a -PassThru -NoNewWindow -RedirectStandardError "$Out\server-$name.err.log" -RedirectStandardOutput "$Out\server-$name.out.log"
  for($i=0;$i -lt 120;$i++){ Start-Sleep -Milliseconds 500; try{ if((Invoke-WebRequest "$base/hcs/v1/health" -UseBasicParsing -TimeoutSec 2).StatusCode -eq 200){ return $p } }catch{} }
  throw "server $name did not come up"
}
function Stop-Srv($p){ if($p -and -not $p.HasExited){ Stop-Process -Id $p.Id -Force; $p.WaitForExit(15000) | Out-Null } }
function Stress([string]$name,[string[]]$extra){
  $rep="$Out\stress-$name.json"
  $o = & python -I "$repo\tools\stress\stress.py" --base-url $base --model $script:model --report $rep @extra 2>&1
  Log "stress[$name] rc=$LASTEXITCODE :: $($o -join ' | ')"
  return $LASTEXITCODE
}
try {
  $deadline=(Get-Date).AddMinutes(30)
  while(-not $have){
    try { $fs=[IO.File]::Open($lock,'CreateNew','Write','None'); $b=[Text.Encoding]::UTF8.GetBytes("f1 $(Get-Date -Format o)"); $fs.Write($b,0,$b.Length); $fs.Close(); $have=$true }
    catch { if((Get-Date) -gt $deadline){ throw 'lock wait timeout' }; Start-Sleep -Seconds 10 }
  }
  Log "lock acquired; free RAM (OS) MiB = $([int]((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory/1024))"
  & $Exe models scan D:\HCSModels\vision 2>&1 | Out-File "$Out\models-scan.txt"
  $list = (& $Exe models list --json 2>&1) -join "`n"
  Set-Content "$Out\models-list.json" $list
  $j = $list | ConvertFrom-Json
  $m = @($j.models | ? { $_.id -notmatch 'mmproj' -and $_.kind -ne 'image' })[0]
  if(-not $m){ $m = @($j)[0] }
  $script:model = $m.id; Log "model id: $($script:model) size=$($m.size_bytes)"

  # --- A: real server, realistic floor: the ramp completes
  $srv = Start-Srv @() 'A'
  Log "A: free MiB at start = $(FreeMiB)"
  Stress 'A-clean-dry' @('--dry','--axes','context,concurrency','--floor-mb','1500') | Out-Null
  $snapA = Invoke-RestMethod "$base/hcs/v1/runtime/overload" -Headers $hdr; $snapA | ConvertTo-Json -Depth 6 | Set-Content "$Out\overload-A-after.json"
  Stop-Srv $srv; $srv=$null

  # --- B1: harness floor just below current free RAM: loading the model during the ramp must trip it
  $srv = Start-Srv @() 'B'
  $free = FreeMiB; Log "B: free MiB at start = $free"
  $floor = $free - 250
  Stress 'B1-floor-midrun' @('--dry','--axes','context','--floor-mb',"$floor") | Out-Null
  Log "B1: free MiB after = $(FreeMiB); loaded model: $((Invoke-RestMethod "$base/hcs/v1/runtime" -Headers $hdr).loaded_model)"
  # --- B2: floor far above free RAM: aborts before sending anything
  Stress 'B2-floor-immediate' @('--dry','--axes','context','--floor-mb','60000') | Out-Null
  Stop-Srv $srv; $srv=$null

  # --- C: server-side soft floor above free RAM: 503 + Retry-After, harness records limit_reached
  $srv = Start-Srv @('--min-free-ram-mb','60000','--hard-floor-mb','100') 'C'
  Stress 'C-server-refuses' @('--dry','--axes','concurrency','--floor-mb','500') | Out-Null
  $r = Invoke-WebRequest "$base/v1/chat/completions" -Method Post -Headers $hdr -ContentType 'application/json' -Body (@{model=$script:model;messages=@(@{role='user';content='hi'})} | ConvertTo-Json -Depth 5) -UseBasicParsing -SkipHttpErrorCheck; Log "C: direct chat -> HTTP $($r.StatusCode) Retry-After=$($r.Headers['Retry-After']) body=$($r.Content)"
  Stop-Srv $srv; $srv=$null

  # --- D: watchdog on a real loaded model: a 1000 MiB memory hog pushes free RAM below the hard floor
  $srv = Start-Srv @() 'D0'
  $free = FreeMiB; Stop-Srv $srv; $srv=$null
  $hard = $free - 1400; $soft = $hard + 10
  Log "D: free MiB = $free -> hard floor $hard, soft floor $soft"
  $srv = Start-Srv @('--min-free-ram-mb',"$soft",'--hard-floor-mb',"$hard",'--watchdog-ms','250') 'D'
  Invoke-RestMethod "$base/hcs/v1/models/$($script:model)/load" -Method Post -Headers $hdr | Out-Null
  for($i=0;$i -lt 60;$i++){ Start-Sleep -Milliseconds 500; if((Invoke-RestMethod "$base/hcs/v1/runtime" -Headers $hdr).loaded_model){ break } }
  Log "D: loaded_model=[$((Invoke-RestMethod "$base/hcs/v1/runtime" -Headers $hdr).loaded_model)] free MiB=$(FreeMiB) level=$((Invoke-RestMethod "$base/hcs/v1/runtime/overload" -Headers $hdr).level)"
  $hogScript='D:\HCSScratch\f1\hog.py'
  Set-Content $hogScript "import time`nb=bytearray(1000*2**20)`nfor i in range(0,len(b),4096): b[i]=1`ntime.sleep(12)"
  $hog = Start-Process python -ArgumentList @('-I',$hogScript) -PassThru -WindowStyle Hidden
  try {
    for($i=0;$i -lt 60;$i++){ Start-Sleep -Milliseconds 500; $s=Invoke-RestMethod "$base/hcs/v1/runtime/overload" -Headers $hdr; if($s.counters.shed_count -ge 1){ break } }
    $s | ConvertTo-Json -Depth 6 | Set-Content "$Out\overload-D-after-hog.json"
    Log "D: shed_count=$($s.counters.shed_count) level=$($s.level) free MiB=$(FreeMiB) loaded_model=[$((Invoke-RestMethod "$base/hcs/v1/runtime" -Headers $hdr).loaded_model)] last_shed=$($s.last_shed | ConvertTo-Json -Compress)"
    $r = Invoke-WebRequest "$base/v1/chat/completions" -Method Post -Headers $hdr -ContentType 'application/json' -Body (@{model=$script:model;messages=@(@{role='user';content='hi'})} | ConvertTo-Json -Depth 5) -UseBasicParsing -SkipHttpErrorCheck
    Log "D: chat while critical -> HTTP $($r.StatusCode) Retry-After=$($r.Headers['Retry-After'])"
  } finally { if($hog -and -not $hog.HasExited){ Stop-Process -Id $hog.Id -Force } }
  Start-Sleep -Seconds 3
  $s2=Invoke-RestMethod "$base/hcs/v1/runtime/overload" -Headers $hdr
  Log "D: after hog exit: level=$($s2.level) free MiB=$(FreeMiB)"
  Stop-Srv $srv; $srv=$null
  # --- E: server killed mid-run -> process_gone
  $srv = Start-Srv @() 'E'
  $pidE = $srv.Id
  $job = Start-Job -ScriptBlock { param($s,$d) Start-Sleep -Seconds $d; Stop-Process -Id $s -Force } -ArgumentList $pidE, 6
  Stress 'E-process-gone' @('--axes','context','--context-steps','64,256,1024,2048,3000','--reps','3','--floor-mb','500','--server-pid',"$pidE") | Out-Null
  Remove-Job $job -Force -ErrorAction SilentlyContinue
  Stop-Srv $srv; $srv=$null
  Log 'all scenarios done'
}
finally {
  if($srv){ Stop-Srv $srv }
  if($have){ Remove-Item $lock -Force -ErrorAction SilentlyContinue; Log 'lock released' }
}


