#!/bin/bash
export TEMP=D:/HCSScratch/tmp TMP=D:/HCSScratch/tmp HF_HOME=D:/HCSScratch/hf XDG_CACHE_HOME=D:/HCSScratch/cache
W=/d/HCSInfer/.claude/worktrees/wf_5e9ee287-140-3
EXE=/d/HCSInfer/build/a3/bin/HCSInfer.exe
OUT=$W/docs/gallery
EV=$W/docs/test-evidence/wave-a/a3/gallery
mkdir -p $OUT $EV
LOCK=/d/HCSScratch/realmodel.lock
render() {
  name=$1; shift
  for i in $(seq 1 180); do
    ( set -o noclobber; echo a3 > $LOCK ) 2>/dev/null && break
    sleep 10
  done
  if [ "$(cat $LOCK)" != a3 ]; then echo "no lock for $name" >> $EV/run.log; return; fi
  echo "== $name free_mb=$(powershell -c '[int]((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory/1024)')" >> $EV/run.log
  "$EXE" image generate --json --force --output "$(cygpath -m $EV)/$name.png" "$@" > $EV/$name.json 2> $EV/$name.err
  echo "exit=$?" >> $EV/run.log
  rm -f $LOCK
}
SD=D:/HCSModels/diffusion/sd15-q8_0.gguf
FX=D:/HCSModels/diffusion/flux3/flux-3-image-klein-UD-Q4_K_XL.gguf
NEG="blurry, low quality, deformed, watermark, text"
render sd15-apple --model $SD --prompt "a ripe red apple on a wooden table, soft window light, photograph, sharp focus" --negative "$NEG" --width 512 --height 512 --steps 20 --seed 1234 --backend cpu
render sd15-lighthouse --model $SD --prompt "a lighthouse on a rocky coast at sunset, dramatic sky, oil painting" --negative "$NEG" --width 512 --height 512 --steps 20 --seed 42 --backend cpu
render sd15-cabin --model $SD --prompt "a cozy log cabin in a snowy pine forest at dusk, warm glowing windows, highly detailed" --negative "$NEG" --width 512 --height 512 --steps 20 --seed 7 --backend cpu
render flux-apple --model $FX --prompt "a ripe red apple on a wooden table, soft window light, photograph" --width 512 --height 512 --preset fast --seed 1234 --backend cpu
render flux-lighthouse --model $FX --prompt "a lighthouse on a rocky coast at sunset, dramatic sky, oil painting" --width 512 --height 512 --preset fast --seed 42 --backend cpu
render flux-fox --model $FX --prompt "a red fox standing in an autumn forest, golden leaves, soft morning light, photograph" --width 512 --height 512 --preset fast --seed 7 --backend cpu
echo DONE >> $EV/run.log
