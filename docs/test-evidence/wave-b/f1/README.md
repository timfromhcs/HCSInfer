# Evidence wave-b / f1 (overload protection + stress harness skeleton)

Machine: Windows 11, 21 GB shared RAM. Build: `D:\HCSInfer\build\f1` (Release, CPU engines, no Vulkan), commit of this branch.
Model: `D:\HCSModels\vision\SmolVLM2-500M-Video-Instruct-Q8_0.gguf` (436 MB), CPU, real inference, under `D:\HCSScratch\realmodel.lock`.

- `ctest-full.log`: full suite once, 889 tests, 100 % passed; the real-model/e2e-real tests are Skipped (no HCS_TEST_* env) = NOT RUN here.
- `driver.log`, `drive.ps1`: the scenario driver. `stress-*.json`: harness reports. `overload-*.json`: controller snapshots. `server-*.log`: server logs (token was supplied via env, never printed).
- A  clean dry ramp, real floor: PASS, 5/5 steps with real generations.
- B1 harness floor 250 MiB below free RAM: the model load during step 1 dropped free RAM (~480 MiB) -> `free_ram_below_floor`, ramp stopped after 1 step, kill switch pulled (model unloaded).
- B2 floor above free RAM: aborted before a single request was sent.
- C  server `--min-free-ram-mb 60000`: HTTP 503 `overloaded` + `Retry-After: 5`; harness records `limit_reached`, not an error.
- D  watchdog on a real loaded model: a 1000 MiB memory hog pushed free RAM below `--hard-floor-mb`; watchdog shed within 28 ms (`model_unloaded: true`), level critical, new chat 503 + Retry-After, level back to normal after the hog exited.
- E  server process killed mid-ramp: `process_gone`.
Not proven: Vulkan/GPU heap behaviour on this machine (DXGI figure only unit-tested as "unknown or finite"), image/diffusion axis against a real server, behaviour at true OOM.
