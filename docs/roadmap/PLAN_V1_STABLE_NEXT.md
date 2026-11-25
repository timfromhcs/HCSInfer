# HCSInfer: Plan bis zur nächsten großen stabilen Version (V1.1-stable)

Stand: 2026-11-25, Basis `main` = `origin/main` = `0853143` (1.0.0-rc.1 + Audit-Fixes). Die beiden Seiten-Branches
(`claude/flux3-agent-engine-v1-665ab0`, `claude/repo-analysis-v1-stable-80f8d6`) haben 0 eigene Commits gegenüber `main`
(nur uncommittete Reste in den Worktrees, 38 bzw. 6 Dateien, werden nicht angefasst). Dieses Dokument ersetzt nichts in
`IMPLEMENTATION_PLAN.md`/`STATUS.md`, es ist die Arbeitsliste für die nächsten Waves. Regeln aus `AGENTS.md` gelten
unverändert (kein Fake-PASS, Evidenz pro Gate, BLOCKED/NOT RUN sichtbar).

## 1. Ergebnis der Bestandsaufnahme

### 1.1 Was belegt funktioniert (siehe STATUS.md)
Text (qwen3-0.6b, LFM3-Omni-2B Q8_0), Vision (SmolVLM2, LFM3-Omni), SD1.5 Q8_0 und FLUX.3 Klein (CPU + Vulkan, 512x512),
HTTP/MCP/CLI/GUI, 815 CTest-Tests, statische EXEs, Python-Wheel, Web-Demo-Shell, HCSMODEL-Container, Q8_0-GEMV-Kernel (aus).

### 1.2 Fehler und Lücken, die ich gefunden habe

**Bilder (Hauptproblem):**
| # | Befund | Beleg |
|---|---|---|
| I1 | Die als Evidenz abgelegten Smoke-Bilder sind unbrauchbar: `cpu-256x256-2step.png` (Repo-Root) ist eine rote Fläche, `M3-image/*-256x256-2steps.png` sind Farbmuster, `M2/image/vulkan-512x512-4step.png` ist ein unscharfer Fleck. Sie beweisen nur "Pipeline läuft", nicht "Bildgenerierung funktioniert". 256px + 2 Steps ist für SD1.5 grundsätzlich zu wenig. | die genannten PNGs |
| I2 | Der Engine-/API-Default erlaubt solche Parameter ohne Warnung. Es gibt keine Mindest-Auflösung/-Steps pro Modellfamilie und keine Qualitätsprüfung. | `src/engines/diffusion`, `docs/api.md` |
| I3 | `--params-backend disk` (B7) liefert ein völlig anderes, degeneriertes Bild als `cpu`-Parameter bei gleichem Seed: Äquivalenz "NOT verified", also potenzieller echter Numerik-/Offload-Fehler. | `B7-params/compute-vulkan0_params-disk.png` vs `compute-cpu_params-cpu.png` |
| I4 | Vulkan-Diffusion war einmal nicht byte-reproduzierbar (nie untersucht). | STATUS "Known limitations" |
| I5 | FLUX.3: Text-Encoder ist Ersatz (Qwen3-VL-4B Q4_K_M), nicht der offizielle; Vulkan nicht schneller als CPU (566 s vs 508 s, ungeklärt); nur N=1; CPU-i2i ohne Evidenz; kein LPIPS. | STATUS Wave D/F |
| I6 | Nur zwei Bildmodelle (SD1.5 Q8_0, FLUX.3 Klein). Keine schnellen Few-Step-Modelle (SD-Turbo/SDXL-Turbo/LCM), kein SDXL, keine LoRA-Nutzung in der Bildengine, kein Inpainting/Mask. | README Feature-Matrix |
| I7 | 256x256-Qualität von SD1.5 ist modellbedingt schlecht; Studio-UI bietet es trotzdem als Preset an (zu prüfen). | `src/gui/assets` |

**Docs/Repo:**
- D1 Stray-Dateien im Repo-Root (`cpu-256x256-2step.png`, `cpu-edit-256x256-2step.png`, Ordner `image/` leer); gitignored, aber im Arbeitsbaum.
- D2 README-Feature-Matrix sagt "Experimental" korrekt, verweist aber auf die schlechten Smoke-Bilder nur indirekt; Studio-Screenshots zeigen UI, nicht echte Bildqualität. Eine Bild-Galerie mit echten 512px-Renders (Apfel, Fuchs) fehlt im README.
- D3 STATUS.md meldet CI "unknown" (nie gepusht). `hardware.yml` nie gelaufen.
- D4 `models load|unload` und `tune` geben Exit 10 (Planned).

**Daten auf C: (neue Anforderung, Verstoß-Risiko):**
- C: ist zu 97 % voll (29 GB frei). `TEMP`/`TMP` zeigen auf `C:\Users\hcsme\AppData\Local\Temp`; pip/npm/cargo/HF/torch-Caches liegen per Default auf C:. Modelle (`D:\HCSModels`), Builds (`D:\HCSInfer\build`) und Venv (`D:\HCSModels\train\venv`) liegen schon auf D:.
- `HF_TOKEN` steht im Prozess-Environment. Darf nie in Logs, Evidenz, Commits oder Subagent-Prompts landen (nur per Env-Name verwenden).

**Fehlende Funktionen laut Plan/AGENTS.md:**
| Thema | Stand |
|---|---|
| Echtes Training/Fine-Tuning | Pipeline + Eval-Gate + Adapter-Book vorhanden, **kein echter Trainer-Lauf** (NOT RUN). `D:\HCSModels\train\venv` hat torch 2.15.0+cpu. |
| Model Merging | **nicht vorhanden** (weder Gewichts-Merge noch LoRA-Merge). |
| Overload Protection | nur `MemoryBudget` + Job-Cap + Verbindungslimits. Fehlt: systemweiter Admission-Controller (RAM-Floor, Commit, GPU-TDR/Device-Lost-Guard, Thermik-Hinweis, Auto-Drosselung, Kill-Switch im Stress-Harness). `src/memory`, `src/scheduler`, `src/telemetry` fehlen ganz. |
| Laden/Offload/Streaming | Stage B (Vault in Ladepfad) BLOCKED; kein automatisches GPU-Layer-Offload nach Budget; Streaming-Server (SSE) vorhanden, aber ohne Backpressure-/Last-Tests unter Limit. |
| HCSUDA | Standalone-Rust-Runtime in `E:\hcslinux222\hcsuda` (caps, Planner mit "unknown = CPU", Kernel f32/f16/int8/coopmat-GEMM, Softmax/Norm/RoPE, GGUF-Reader, 41 Tests, auf 680M verifiziert). **Nicht** in HCSInfer integriert. Lizenz GPL-3.0-or-later vs. HCSInfer MIT: Owner-Entscheidung nötig. |
| Benchmarks "ans Limit" | Medianwerte vorhanden; fehlen: Thermik, flushed-cache, Max-Kontext/Max-Batch-Sweep bis OOM-Grenze, Dauerlast, Parallel-Last, Bild-Qualitätsmetriken (LPIPS/CLIP). |
| M9 Forschungsfeatures | NOT RUN |

## 2. Harte Randbedingungen (gelten für jeden Agent)

1. **Nichts auf C:.** Alle Daten nur auf `D:` oder `E:`: Repo/Build/Worktrees `D:\HCSInfer`, Modelle `D:\HCSModels`, Scratch `D:\HCSScratch`
   (neu), HCSUDA-Quellen `E:\hcslinux222`. Jede Shell setzt zuerst `scripts/env-d-e.ps1` (neu, Wave A1):
   `TEMP`/`TMP`=`D:\HCSScratch\tmp`, `PIP_CACHE_DIR`, `npm_config_cache`, `CARGO_HOME`, `RUSTUP_HOME`, `HF_HOME`, `HF_HUB_CACHE`,
   `TORCH_HOME`, `XDG_CACHE_HOME`, `PYTHONPYCACHEPREFIX`, `NUGET_PACKAGES` auf D:. Ein Guard-Test misst C:-Schreibzugriffe
   (freier Platz vor/nach, `Get-ChildItem` auf bekannte Cache-Pfade) und scheitert bei Zuwachs > 50 MB. Unvermeidbares (Windows-Pagefile,
   Defender, VS-Installer) wird dokumentiert, nicht von uns erzeugt.
2. **Kein Secret in Logs/Prompts.** `HF_TOKEN` nur als Variable, nie ausgeben.
3. **RAM-Disziplin (21 GB, geteilt mit iGPU).** Maximal ein Real-Modell-Test gleichzeitig (Lock-Datei `D:\HCSScratch\realmodel.lock`),
   Builds mit `-j4`, damit die Budget-Verweigerungen (`out_of_memory`) der Wave R nicht wiederkehren. Vor jedem Real-Test freien RAM loggen.
4. **Parallele Agents isoliert:** je Agent eigener Git-Worktree unter `D:\HCSInfer\.claude\worktrees\<name>` und eigenes Build-Verzeichnis
   `D:\HCSInfer\build\<name>`. Merge nach `main` nur durch den Koordinator nach grünem Gate (kein Force-Push, kein Push ohne Owner-Freigabe).
5. **Downloads:** nur nach ausdrücklicher Freigabe pro Datei (Name, Quelle, Größe, Lizenz). Gated Modelle brauchen den Owner-Token.
   Kandidaten stehen in 4.2; bis zur Freigabe wird nichts geladen.
6. **Lizenz:** Daten für Training nur permissiv oder eigene; **keine Distillation aus Claude/Anthropic-Ausgaben**.
7. **Kein Gate wird aus einem Mock PASS.** Bildqualität nur mit angesehenem Bild plus Metrik, nicht "Datei existiert".

## 3. Wellen (je 4 Agents pro Wave, danach Koordinator-Merge, Voll-CTest, STATUS-Update)

Jede Wave endet mit: Format/Tidy grün, Voll-CTest (CPU, ggf. Vulkan) grün, neue Tests rot→grün, Evidenz unter
`docs/test-evidence/<wave>/`, STATUS.md/README aktualisiert, Merge-Commit auf `main` (lokal).

### Wave A: Fundament, Sicherheitsnetz, Bildfehler-Diagnose
| Agent | Auftrag | Exit |
|---|---|---|
| A1 Storage-Guard | `scripts/env-d-e.ps1`, Guard-Test "keine Daten auf C:", Scratch-Struktur auf D:, Build-/Test-Skripte nutzen es; Doku in `docs/build.md`; alte C:-Reste nur auflisten, nichts löschen ohne Freigabe | Guard-Test grün; Messung freier C:-Platz vorher/nachher eines Builds + Tests |
| A2 Bild-Root-Cause | I3/I4 reproduzieren: gleicher Seed, `cpu`/`vulkan0`/`disk` Params, SD1.5 512px 20 Steps; PSNR/SSIM/LPIPS-Skript (`tools/imgqa/`), Ursache klassifizieren (Code/Config/Upstream), Fix oder Upstream-Repro; Reproduzierbarkeit Vulkan messen (N=5 Hash-Vergleich) | Ursache dokumentiert; entweder behoben (rot→grün) oder BLOCKED mit Minimal-Repro; Tabelle der Äquivalenz |
| A3 Bild-Defaults + Guards | Mindest-/Default-Auflösung und -Steps pro Modellfamilie (SD1.5: 512, 20-30 Steps Euler a/DPM++; FLUX.3: Presets), API/CLI/UI warnen oder lehnen Unsinn ab (256px/2 Steps nur mit `allow_low_quality`), Studio-Presets korrigieren, schlechte Smoke-Bilder aus Evidenz-Index markieren/ersetzen durch echte 512px-Galerie `docs/gallery/` (angesehen) | Tests für Validierung; Galerie mit je 3 Prompts SD1.5 + FLUX.3; README-Galerie |
| A4 Baseline + Doku-Audit | Frischer Build CPU + Vulkan von `main`, Voll-CTest mit Real-Modellen (seriell), Diff README/STATUS/Docs gegen Realität, Stray-Dateien melden, Repo-Root aufräumen (nur ungetrackte PNGs, nach Auflistung) | Baseline-Evidenz `wave-a/`; Liste Doku-Abweichungen als Issues für B..G |

### Wave B: Echte Bildgenerierung (SD1.5 + FLUX.3 Klein + weitere Modelle)
| Agent | Auftrag | Exit |
|---|---|---|
| B1 SD1.5 voll + Studio/API | Sampler/Scheduler-Auswahl, i2i/Inpaint-Maske (wenn sd.cpp trägt), Progress-Events pro Step, Abbruch, UI-Wiring im echten Browser, CFG, Negative Prompt, Seed, Batch=1, VAE-Tiling, Hires-Option; Prompt-Suite 10 Prompts, Qualität angesehen + Metrik; Q8_0 vs Q4_K/F16 | CLI/API/UI-Parameter wirksam (Tests), Galerie, Zeit/Speicher je Preset |
| B2 FLUX.3 Klein | Steps 4/6/8/12/16 mit N>=3, Vulkan-vs-CPU-Ursache (566 vs 508 s), offizieller Encoder falls freigegeben, i2i CPU+Vulkan, Multi-Reference-Edit prüfen, LPIPS gegen 16-Step | Messreihe mit Median/p90; Presets neu gesetzt; Root-Cause oder BLOCKED |
| B3 Weitere Modelle + vorab geladene LoRAs | Kandidaten (4.2) hinter Adapter: SD-Turbo/SDXL-Turbo (1-4 Steps), LCM-LoRA auf SD1.5, SDXL-Base GGUF. **LoRA-Bibliothek:** vorab (nach Freigabe) heruntergeladene Bild-LoRAs unter `D:\HCSModels\lora\image\` mit Manifest (Name, Basis-Familie, Quelle, Lizenz, SHA-256, Trigger-Wörter, empfohlene Stärke); Registry scannt sie und prüft Basis-Kompatibilität (SD1.5-LoRA nie auf FLUX); Engine lädt 0..n LoRAs mit Gewicht pro Request (`lora: [{id, weight}]` in API/CLI/UI/MCP), Pfad nur aus Approved-Root, Stärke begrenzt. Modellkatalog mit Capability-Flags in Registry/UI | je Modell/LoRA: Render angesehen (mit/ohne LoRA im Vergleich), Hash, Lizenz, Zeit, Peak-RAM; inkompatible Kombination = klarer Fehler, kein stiller Müll |
| B4 4K-Upscaler (ONNX, separates Modell) | Eigenes Modul `src/engines/upscale` (`IUpscaleEngine`), ONNX Runtime als **optionale**, dynamisch geladene Komponente (Execution Provider DirectML auf der 680M falls verfügbar, sonst CPU; Wahl per Messung, nie still). Upscaler-Modell separat in `D:\HCSModels\upscale\` (Kandidaten Real-ESRGAN `realesr-general-x4v3` / `RealESRGAN_x4plus`, Lizenz je Datei prüfen). Pipeline: nach jeder Generierung optional `upscale: "4k"` = 4x-Modell **gekachelt** (Tile+Overlap, begrenzter RAM, Budget-Check), dann schneller Lanczos auf exakt 3840 lange Kante. Schalter pro Request/Preset, Standard AUS bis "schnell und gut" gemessen ist. Messung: Zeit 512->2048->4K, Peak-RAM, Qualität (PSNR/SSIM/LPIPS, angesehen), EP-Vergleich | echtes 3840px-PNG angesehen; Zeit/RAM-Tabelle; Abbruch mitten im Upscale; Tile-Naht-Test; fehlendes ORT/Modell = klarer `Unsupported`-Fehler; EXE bleibt ohne ORT lauffähig |

### Wave C: HCSUDA umgebaut für HCSInfer (Vulkan-Compute-Schicht)
ADR-014 (Entscheidung des Owners 2026-11-25: beide Projekte gehören ihm, die Lizenzen bleiben unverändert; HCSUDA-Code darf
voll übernommen/umgebaut werden, die übernommenen Dateien tragen den Herkunftsvermerk und werden in THIRD_PARTY_NOTICES/Lizenz-Audit
als eigener Code des Owners mit der jeweils geltenden Lizenz geführt).
| Agent | Auftrag | Exit |
|---|---|---|
| C1 Caps + Planner (C++) | `src/hardware`/`src/kernels/hcscuda`: Capability-Probe (Subgroup, Coopmat, int8-dot, Heaps, Timestamp) und konservativer Planner "unbekannt = CPU", Promotionsregel (Korrektheit, 3 identische Läufe, >=1.05x) als Code, JSON-Export, in `doctor` eingebunden | Unit-Tests aus HCSUDA-Fällen portiert; `doctor --json` zeigt Planner-Tabelle |
| C2 Kernel-Port | GLSL-Shader (tiled f32/f16 GEMM, coopmat-GEMM, int8-dot, softmax/rms/rope) übernehmen, `glslc`+`spirv-val` im Build, CPU-Oracle je Kernel, Differenztests auf der 680M | `kernel-vulkan`-Tests grün, Toleranzen deklariert, Validation-Layer 0 Meldungen |
| C3 Einsatzstellen | Wo bringt es etwas: (a) HCS-eigene Ops (LoRA-Delta/Merge, Quantisieren, Dequant, Embedding-Ops, Bild-Metriken) auf GPU, (b) Experiment `ggml_backend`-Registrierung/Op-Override im Engine-Pfad (nur wenn messbar >= 1.05x gegen upstream Vulkan, sonst OFF) | Messung vs upstream, ehrliche Default-Entscheidung, Feature-Flag |
| C4 Autotuner + Schutz | Kernel-Registry-Cache (Schlüssel: Device-ID, Treiber, Shader-Hash, Shape-Bucket), nur nach bestandener Korrektheit schreiben, TDR-Schutz: Dispatch-Zerlegung unter ~1.5 s, Device-Lost-Recovery-Pfad, Reset/Rebenchmark per CLI (`tune` wird real) | `tune` Exit 0 mit Tabelle; TDR-Test (Soft-Simulation), Cache-Invalidierung getestet |

### Wave D: Training, Fine-Tuning, Merging
| Agent | Auftrag | Exit |
|---|---|---|
| D1 Echter LoRA-Lauf | Trainer-Adapter (`torch` CPU aus `D:\HCSModels\train\venv`, sonst PEFT/TRL falls installierbar auf D:) auf kleinem Basismodell (HF-Safetensors-Variante eines kleinen Modells, Freigabe nötig), Dataset aus permissiven Quellen + eigenen Traces (`docs/research/train-datasets.md`), tokens/s und Wallzeit ehrlich | Ein echter Lauf mit Loss-Kurve, Adapter-Datei, Hash; Eval vor/nach über `eval_gate`; kein Qualitätsclaim ohne Held-out-Eval |
| D2 Adapter in der Inferenz | GGUF-LoRA-Export (llama.cpp `convert_lora_to_gguf`), Laden/Entladen/Skalieren eines Adapters in der llama-Engine + API/CLI/UI, Eval-Gate behält/verwirft | Real-Modell-Test mit/ohne Adapter, messbarer Unterschied auf Eval, Rollback |
| D3 Model Merging | `HCSInfer merge` + API/MCP-Job: (a) LoRA in Basis mergen und neu als GGUF schreiben, (b) lineare Interpolation, SLERP, TIES/DARE für gleiche Architektur (Shape-/Tensorname-Validierung), Streaming-Schreiber mit begrenztem RAM, Ausgabe nur in Approved-Root auf D:/E:, Hash-Manifest; GPU-Pfad optional über C3 | Merge-Ergebnis lädt und generiert real; Unit-Tests gegen NumPy-Referenz; kaputte/inkompatible Eingaben fail-closed |
| D4 RSI-Schleife real | `rsi_loop` verdrahten (HTTP/MCP/UI "Train"-Tab live, Job mit Cancel), Eval-Set fest, Snapshots/Rollback, keine unbeaufsichtigte Selbstmodifikation von Binärdateien/Safety-Regeln | Ein vollständiger Zyklus mit echten Metriken; Abbruch/Rollback getestet |

### Wave E: Laden, Offload, Streaming, Server
| Agent | Auftrag | Exit |
|---|---|---|
| E1 Vault im Ladepfad | M5 Stage B: begrenzte asynchrone Leseschlange in den Engine-Loader (llama + sd), RAM-Peak gemessen, HDD-Abzug simuliert, kein SSD-Staging; Messung nur wo ehrlich möglich (Page-Cache-kalt braucht Admin: BLOCKED, wenn keine Rechte) | Tests + Messung; sonst BLOCKED mit Grund |
| E2 Auto-Offload | Layer-Offload (`-ngl`) und `params-backend`/Modul-Platzierung automatisch aus Budget und Messung (UMA-bewusst), KV-Cache-Typ/Kontext nach Budget, Erklärung im Diagnosemodus | Tabelle Modell x Budget -> Plan; Real-Tests mit LFM3-Omni und FLUX.3 |
| E3 Server/Streaming | `models load/unload` als CLI-Client gegen laufenden `serve` (Exit 10 entfällt), SSE-Backpressure, Cancel bei Client-Abbruch unter Last, Concurrency-/Queue-Limits, Fortschritts-Streaming für Bild, Auth/Origin unverändert | Contract + E2E Tests, Last-Test 50 parallele Clients ohne Deadlock |
| E4 Schneller Start | Cold-Start/Modellwechsel messen und kürzen (Shader-/Pipeline-Cache auf D:, Warm-Keep-Policy, Unload-Zeit, Speicherrückgabe messen) | Vorher/nachher-Tabelle, Median/p90 |

### Wave F: Overload Protection und Limit-Benchmarks
| Agent | Auftrag | Exit |
|---|---|---|
| F1 Overload-Controller | Neues `src/scheduler` + `src/telemetry`: Admission-Control (freier RAM-Floor, Commit-Limit, Vulkan-Heap-Budget), Last-Samples (CPU %, RAM, GPU-Heap), Backpressure-Stufen (drosseln, Queue, ablehnen mit `429/503` + Retry-After), harter Kill-Switch pro Job, Auto-Fallback (CPU-Only/kleinerer Kontext/kleinere Auflösung nur mit Hinweis, nie still) | Fault-Injection-Tests (RAM knapp, Heap knapp, Device-Lost), Controller bleibt ansprechbar |
| F2 Stress-/Limit-Harness | `tools/bench/limits.py`: Kontext-Sweep bis zur Budgetgrenze, Batch/Thread-Sweep, Bildauflösung-Sweep, parallele Clients, mit **Abbruchbedingungen** (RAM-Floor 2 GB, C:-Schreibzuwachs, Laufzeitlimit) und Sicherung, dass der Rechner nicht einfriert | JSON je Lauf nach AGENTS.md 21.1, Abbruch wirkt (getestet mit künstlich niedrigem Floor) |
| F3 Voll-Benchmarks | B0-B8 neu mit Thermik-Notiz, N>=5, p90, KV-Typen, Vulkan vs CPU vs HCSUDA-Kernels (falls aktiv), Bild: Steps/s, Qualitätsmetriken, Dauerlast 30 min Text + 10 Bilder | `docs/benchmarks.md` neu, Roh-JSON, Regression-Check |
| F4 Soak voll | Volles Soak-Profil (100 Load/Unload, 30 min Generierung, ASan-Fuzz-Lauf >= 1 h oder dokumentiert BLOCKED, Vulkan-Soak, SIGINT-Pfad) | PASS nur mit Evidenz, sonst NOT RUN |

### Wave G: Stabilisierung und Release
| Agent | Auftrag | Exit |
|---|---|---|
| G1 Regression/Fix-Runde | Alle roten/flaky Tests aus A-F beheben (Root-Cause, kein Schwächen von Assertions) | Voll-CTest CPU + Vulkan grün, 0 flaky über 3 Läufe |
| G2 UI/Studio Endabnahme | Alle Tabs gegen echte Backends im Browser (Playwright falls auf D: installierbar), Bildergalerie, Train/Merge-Tab, Overload-Anzeige, Screenshots nur echte | UI-E2E-Suite grün, README-Screenshots aktuell |
| G3 Packaging/Docs | README/STATUS/Support-Matrix/CHANGELOG/Release-Notes, SBOM, Checksums, EXE-Build static CPU + Vulkan, Wheel, Web-Dist, Lizenz-Audit (inkl. HCSUDA-Herkunftsvermerke) | Release-Gate-Tabelle ehrlich, Version `1.1.0-rc.1` |
| G4 Unabhängiges Review | Adversarieller Review des Diffs (Korrektheit, Security, Claims vs Evidenz, C:-Guard) | Findings behoben oder begründet abgelehnt |

Push, Tag, Vercel-Deploy, Signierung, Clean-Machine-Test, Self-hosted-Runner bleiben **Owner-abhängig** und werden nicht ohne Freigabe ausgeführt.

## 4. Anhang

### 4.1 Definition "nächste große stabile Version" (V1.1-stable)
- Bild: SD1.5 und FLUX.3 Klein liefern in CLI, API, MCP und UI nachweislich gute 512px-Bilder (angesehen + Metrik), mindestens ein
  zusätzliches Few-Step-Modell läuft, schlechte Parameter werden abgefangen, Offload-Äquivalenz ist geklärt.
- Train/Fine-Tune/Merge: ein echter LoRA-Lauf, Adapter in der Inferenz nutzbar, Merge-Befehl erzeugt ladbare GGUF.
- Overload Protection: Controller + Stress-Harness mit Abbruchbedingungen, kein Absturz/Freeze bei Überlast.
- HCSUDA: Planner + Kernel portiert, nur aktiv, wo >= 1.05x gemessen; `tune` real.
- Laden/Streaming/Server: Vault im Ladepfad (oder BLOCKED mit Grund), `models load/unload` CLI, Backpressure getestet.
- Nichts auf C: durch HCSInfer-Prozesse (Guard-Test grün).
- Voll-CTest grün (CPU + Vulkan), Doku = Realität.

### 4.2 Modellkandidaten (nur nach Owner-Freigabe laden; Lizenz je Modell prüfen)
SD-Turbo / SDXL-Turbo (GGUF/safetensors, 1-4 Steps), LCM-LoRA für SD1.5, Stil-/Detail-LoRAs für SD1.5/SDXL/FLUX (nur frei nutzbar, Lizenz je Datei), Upscaler-ONNX (Real-ESRGAN-Klasse), SDXL-Base-1.0 (GGUF), FLUX.3 Klein offizieller Encoder/VAE
(gated, Owner-Token), kleines Trainingsbasismodell (Qwen3-0.6B oder LFM-Klasse, Safetensors), Embedding-/Eval-Datensätze (permissiv).

### 4.3 Risiken
- RAM 21 GB geteilt mit iGPU: parallele Builds + Real-Tests führen zu `out_of_memory` (Lock + `-j4`).
- Vulkan-FLUX auf 680M nicht schneller als CPU: kann UMA-Bandbreite sein; Messung statt Annahme.
- Echtes Training auf CPU ist winzig; kein Qualitätsclaim ohne Eval.
- Ein GGML für llama + sd.cpp: neue Upstream-Pins nur als separate Kompatibilitätsänderung.
- Lizenz HCSUDA (GPL) vs HCSInfer (MIT): kein Blocker mehr (Owner-Entscheid); nur im Lizenz-Audit sauber dokumentieren.
