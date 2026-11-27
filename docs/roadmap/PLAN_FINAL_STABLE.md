# HCSInfer: Endplan ab 2026-11-27 bis zum stabilen Release (S0–S5)

Stand: 2026-11-27. Dieser Plan ersetzt keine bestehende Datei; er ist die **ausführbare Arbeitsliste** für den
Zustand nach den Wellen A/B/C. Verbindlich sind `AGENTS.md` (insbesondere §27), die Gates in `STATUS.md` und die
Evidence-Regel: kein PASS ohne Artefakt, kein Fake-PASS aus einem Mock, `BLOCKED`/`NOT RUN` bleiben sichtbar.

## 1. Bestandsaufnahme (diesmal tatsächlich geprüft)

| Frage | Antwort mit Beleg |
|---|---|
| Lokaler Stand | `main` = `1469638` (`merge: wave-b/c1 into main`), Arbeitsbaum sauber. |
| GitHub-Stand | `origin/main` = `0853143` (`ci(release): write SHA256SUMS.txt with LF endings`). |
| Diff | **41 Commits, 365 Dateien, +47.134 / −427 Zeilen** lokal, die auf GitHub fehlen. `git log --oneline origin/main..main` und `git diff --stat origin/main..main` sind in `docs/test-evidence/final-s0/repo-diff.txt` gespeichert. |
| Inhalt der 41 Commits | Welle A: `env-d-e.ps1` + C:-Guard, `HCS_DATA_DIR`, Bildqualitäts-Envelope + `allow_low_quality`, `tools/imgqa`, frischer Baseline-CTest, Doc-Audit. Welle B: Safari/GGUF-LoRA-Bibliothek, ONNX-4K-Upscaler, Streaming-Merge (linear/SLERP/TIES/DARE), Overload-Admission-Controller + Telemetrie, Vault-Prefetch im Ladepfad (`serve --vault-prefetch`), `models load/unload/status` als HTTP-Client, Stress-Harness mit Abbruchbedingungen, HCSUDA-Port (Caps/Planner/Oracle/Profile-Cache/8 Vulkan-Kernel), `tune`. Welle C: Bildfamilien-Profile, gemessener Upstream-GGML-Vulkan-Baseline (0 Kernel promoviert), echte gated Real-ESRGAN-Tests, Merge nach `main`. |
| Alte Worktrees | `claude/flux3-agent-engine-v1-665ab0` (38 ungetrackte Dateien) und `claude/repo-analysis-v1-stable-80f8d6` (6 Dateien) enthalten Reste, die W2/W3/W4 portiert oder ersetzt haben. **Nicht löschen, nicht bulk-kopieren**, nur dokumentieren. |
| CI | `pr-quick-gate` grün auf `0853143`, `release` grün auf `40bf581` (Draft mit 8 Assets). `hardware-nightly-release` seit Tagen `cancelled`/`queued`, nie erfolgreich. Die 41 lokalen Commits sind dort **nicht** geprüft. |
| Bekannte Merge-Narbe | `613fc58` fixt ein doppeltes `bool lan` in `src/api/runtime.cpp` aus dem c1-Merge; in S0 trotzdem frisch bauen. |
| Version heute | `1.0.0-rc.1` (Tag `v1.0.0-rc.1` gepusht, Release Draft). Ziel: `1.1.0`, nur wenn alle Nicht-Owner-Gates grün sind; sonst `1.1.0-rc.1` mit expliziter BLOCKED-Liste. |

**Konsequenz:** Der nächste Schritt ist nicht "mehr Features", sondern **Verifikation und Integration des
vorhandenen, ungeprüften Codes**. Alles andere baut auf einem grünen S0 auf.

## 2. Branching, Push-Kadenz, Merge (ab hier verbindlich)

```
main (GitHub: 0853143, rc.1)
  \-- release/v1.1-stable   <-- alle neuen Commits, von lokalem main 1469638 abgezweigt
        |  push NACH jeder grünen Welle S0..S4:  git push origin release/v1.1-stable
        |  niemals force, niemals rot pushen, keine Secrets in Commits/Logs
        \-- S5: merge commit nach main -> git push origin main -> Tag -> Draft-Release
```

1. **Arbeitsbranch:** `release/v1.1-stable`. Keine direkten Commits auf `main`, keine Force-Pushes, keine Rebases
   bereits gepusster Commits.
2. **Push nach jeder grünen Welle** (S0, S1, S2, S3, S4). Ein Push ist erst erlaubt, wenn die Exit-Kriterien der
   Welle erfüllt sind: Configure+Build, Voll-CTest (CPU, wo relevant Vulkan), Format/Tidy, neue Tests rot→grün,
   STATUS-Eintrag, Evidence-Dateien. Ein fehlgeschlagener Push wird protokolliert und nicht durch einen Force-Push
   "gelöst".
3. **Merge nach main nur in S5**, wenn der komplette Gate (CPU + Vulkan + Realmodelle + Paket + Docs) grün ist.
   Merge-Commit oder Fast-Forward; danach `git push origin main`.
4. **Tag:** `v1.1.0` nur bei "alle Nicht-Owner-Gates PASS". Bleibt ein Kern-Gate BLOCKED, wird `v1.1.0-rc.1`
   getaggt und der Grund in `docs/release-notes-1.1.0-rc.1.md` (bzw. `-1.1.0.md`) stehen.
5. **Owner-abhängig, nie automatisch:** Signatur, Veröffentlichen des Draft-Releases, PyPI-Upload, Clean-Machine-VM,
   Vercel-Deploy, Self-hosted-Runner, physischer USB-Abzug, BIOS/Driver-Änderungen. Bleiben `BLOCKED`/`NOT RUN`.
6. **Ticker:** Ein Hintergrund-Shell-Loop sendet alle 120 s eine kurze Erinnerung in diesen Chat
   (`scripts/ticker-2min.ps1`, Log unter `D:\HCSScratch\ticker\`). Er ist **nur ein Reminder für den Fall, dass
   der Agent ausfällt** – nie ein Warte-Timer. Es wird kontinuierlich gearbeitet.

## 3. Umgebung und Standardkommandos

Jede Build-/Test-Shell (auch der Ticker) setzt zuerst:

```powershell
. D:\HCSInfer\scripts\env-d-e.ps1   # TEMP/TMP, PIP_CACHE_DIR, npm_config_cache, CARGO_HOME, HF_HOME, ... -> D:
$ErrorActionPreference = 'Stop'; Set-StrictMode -Version Latest
```

- Builds: MSVC x64 Developer PowerShell, CMake/Ninja aus `CMakePresets.json`, `-j4` (21 GB RAM, geteilt mit iGPU).
- Realmodell-Tests: serialisiert über `D:\HCSScratch\realmodel.lock`, freien RAM vorher loggen. Modelle:
  `D:\HCSModels` (LFM3-Omni-2B Q8_0, qwen3-0.6b Q4_K_M, SmolVLM2-500M + mmproj, SD1.5 Q8_0, FLUX.3 Klein).
- `HF_TOKEN` wird **nie** ausgegeben, geloggt, committet oder in Subagent-Prompts übergeben.
- Fresh configure bei geänderter Toolchain/Generatoren/Optionen; kein Cache-Recycling über Generatoren hinweg.
- Standard-Gates pro Welle:

```powershell
cmake --preset windows-msvc-release            # CPU
cmake --build --preset windows-msvc-release --parallel 4
ctest --preset windows-msvc-release --output-on-failure       # ggf. -j1 mit Realmodellen
cmake --preset windows-msvc-release-vulkan     # GPU relevante Wellen
cmake --build --preset windows-msvc-release-vulkan --parallel 4
ctest --preset windows-msvc-release-vulkan --output-on-failure
.\scripts\format.ps1
.\scripts\static-analysis.ps1 -Scope src -FailOnFindings
```

## 4. Build-Test-Fix-Loop (pro Änderung, ohne Ausnahme)

```text
INSPECT -> AKZEPTANZKRITERIEN -> BASELINE (reproduzieren/messen)
  -> KLEINSTE ÄNDERUNG -> FORMAT + STATIC ANALYSIS -> CONFIGURE
  -> BUILD (kleinstes betroffenes Target) -> TARGETED TEST (rot -> grün)
  -> VOLLES CTest (CPU; Vulkan wenn relevant; Realmodelle seriell mit Lock)
  -> SECURITY/RESOURCE-CHECK -> BENCHMARK wenn performance-relevant
  -> DOCS + EVIDENCE -> COMMIT auf release/v1.1-stable -> PUSH wenn Wellen-Gate grün
```

Bei Rot: vollständiges Log + Umwelt speichern → Root-Cause klassifizieren (`code|configuration|dependency|driver|
hardware|model|environment`) → **eine** isolierte Hypothese → Rebuild → exakt denselben Test erneut →
Regressionen → Fix nur behalten, wenn die Evidenz besser wird, sonst revert. Maximal 3 Hypothesen pro Fehler,
danach Minimal-Reproducer und Upstream-/Historienanalyse.Assertions werden nie geschwächt, Tests nie gelöscht,
Backends nie abgeschaltet, um grün zu werden.

## 5. Wellen S0–S5

### S0 — Integration und Baseline (erste Welle, sofort)

Ziel: Die 41 lokalen Commits auf einem frischen Baum beweisen oder reparieren.

1. Branch `release/v1.1-stable` anlegen (erledigt), Diff/Log nach `docs/test-evidence/final-s0/repo-diff.txt`.
2. Frisch konfigurieren + bauen: `windows-msvc-release`, `windows-msvc-release-vulkan`,
   `windows-msvc-release-kernels`. Jede Warnung in HCS-Code ist ein Fehler (`/W4 /WX`).
3. `scripts/format.ps1`, `scripts/static-analysis.ps1 -Scope src -FailOnFindings`.
4. Voll-CTest CPU und Vulkan mit gesetzten `HCS_TEST_*`, seriell, Lock, freier RAM vor jeder Phase.
5. `web/`: `npm test`; `python/`: `pytest`/`mypy` wie in `release.yml`; `scripts/package.ps1 -Zip` trocken laufen
   lassen (SBOM/Hashes).
6. Jeden roten Test mit dem Loop in §4 fixen; Baselines (Testzahlen, Versionstrings, SHA-256 der EXEs) in
   `docs/test-evidence/final-s0/` ablegen. `STATUS.md` um eine S0-Sektion ergänzen.
7. **Exit:** 0 Fehler in CPU+Vulkan-CTest, 0 Tidy-Findings, Paket erzeugt, Docs aktualisiert → Commit →
   **erster Push** auf `origin/release/v1.1-stable`.

### S1 — Korrektheit der neuen Funktionen

Für jedes neue Feature aus Welle A/B/C: Acceptance-Kriterium formulieren, vorhandene Tests laufen lassen, Lücken
schließen. Mindestens:

- C:-Guard (`check-no-c-writes.ps1`, CTest-Label `guard`) und `HCS_DATA_DIR`.
- Bildqualität: Mindestauflösung/Steps pro Familie, `allow_low_quality`, Galerie mit angesehenen 512px-Renders.
- LoRA-Bibliothek (IDs, approved roots, Familienkompatibilität), Upscale (Lanczos-Golden, Tile-Nähte, Abbruch),
  Merge (NumPy-Golden, echter Streaming-Schreiber, geladene Ausgabe).
- Admission Controller (429/503 + Retry-After, Kill-Switch, Watchdog), Telemetrie.
- Vault-Prefetch (SHA-256, bounded chunks, kein SSD-Staging; echte USB-HDD wenn verfügbar).
- `models load/unload/status` gegen laufenden `serve`, `tune` Exit 0 mit Profil, HCSUDA-Kernel + Planner.
- Image-Familienprofile, Upstream-Vulkan-Baseline-Entscheid (0 promoviert = korrekt dokumentiert).

**Exit:** Jedes Feature hat einen Test und einen Evidence-Pfad (PASS oder BLOCKED/NOT RUN mit Grund), der Guard ist
grün, `STATUS.md` enthält neue Zeilen → Commit → Push.

### S2 — Echte Modelle und E2E

1. Voll-CTest Vulkan mit allen Realmodellen; CPU-Voll-CTest erneut; 3 Läufe der zuvor flaky Tests.
2. CLI/API/MCP/Studio-Workflows gegen die gebauten Binaries (nicht gegen Mocks): Text, Vision, SD1.5, FLUX.3,
   LoRA/Upscale wo belegbar, Cancel, Model-Switch.
3. Benchmarks B0/B1, `benchmark --suite full`, `POST /hcs/v1/benchmarks`, MCP `hcs.runtime.benchmark`, `tune
   --suite full`. Roh-JSON mit vollständigen Metadaten nach AGENTS.md §21.1; Median/p90, keine Cold/Warm-Vergleiche.
4. Drei aufeinanderfolgende Läufe ohne neue Fehler.

**Exit:** Echte Text/VLM/SD1.5/FLUX.3-PASS, 0 Flakes, Benchmark-Metadaten vollständig → Commit → Push.

### S3 — Härte und Dauerlast

1. Soak-Vollprofil (100 Zyklen Load/Unload, 30 min Generation) soweit die 21 GB und die Zeit es erlauben; sonst
   Teilprofil mit expliziter BLOCKED-Begründung. Handles/Commit vorher/nachher, Locks sauber.
2. Fault Injection: OOM (Simulation), USB-Abzug (Simulation), Device-Lost, Admission-Control-Ablehnung,
   Abbruch mitten im Laden/Rendern, Shutdown idle/loading/generating.
3. Parser-Fuzz-Smoke (60 s+; ASan/UBSan wo der Compiler es trägt), Stress-Harness mit künstlich niedrigem
   RAM-Floor (Abbruchbedingungen müssen wirken), C:-Guard erneut.

**Exit:** PASS mit Logs oder BLOCKED mit konkreter fehlender Abhängigkeit; keine Ressourcenleaks → Commit → Push.

### S4 — Paket und Doku

1. Static-CPU- und Static-Vulkan-EXEs aus dem finalen Commit neu bauen; `dumpbin`-Importe, restricted-env-check,
   keine neuen Sidecar-DLLs (außer System-`vulkan-1.dll`).
2. Portables ZIP, Wheel/sdist, Web-Dist, CycloneDX-SBOM, `SHA256SUMS.txt`, Package-Manifest
   (`single_file_claim: false`, solange kein Clean-Machine-Test vorliegt).
3. Docs final abgleichen: README-Featurematrix, `docs/support-matrix.md`, `docs/licence-audit.md`,
   `THIRD_PARTY_NOTICES.md`, `CHANGELOG.md`, Release-Notes, `STATUS.md`, ADRs. Versionstrings konsistent
   (CMake, EXE, Wheel, `web/package.json`, README).
4. Versionsentscheidung nach §2.4 dokumentieren.

**Exit:** Paket verifiziert, Docs = Binary, keine Überclaims → Commit → Push.

### S5 — Final-Gate und Merge

1. Auf dem finalen Commit: frischer CPU- und Vulkan-Build, Voll-CTest mit Realmodellen, Format/Tidy, Fuzz-Smoke,
   Paket-Hash-Verifikation, Web/Python-CI-Äquivalent, C:-Guard.
2. `STATUS.md` mit abschließender Release-Readiness-Matrix (PASS/FAIL/BLOCKED/NOT RUN) und dem Report nach
   AGENTS.md §23 aktualisieren.
3. `git checkout main && git merge --no-ff release/v1.1-stable` (kein Force-Push), `git push origin main`, Tag
   setzen und pushen, GitHub-Draft vorbereiten (Veröffentlichen nur mit Owner-Freigabe).
4. **Exit:** Merge-Commit in `STATUS.md` und CHANGELOG, finaler Report im Chat.

## 6. Evidenz und Dateien

- Pro Welle: `docs/test-evidence/final-s<0..5>/` mit `commands.txt`, `ctest-cpu.txt`, `ctest-vulkan.txt`,
  `format.txt`, `clang-tidy.txt`, `environment.json`, `package.txt`, `status-update.txt`.
- Secrets/Prompts/Bilder mit Personen oder `HF_TOKEN` werden nie eingecheckt; Logs vor dem Commit auf Redaktion
  prüfen.
- `docs/roadmap/STATUS.md` wird **in derselben Änderung** wie der Code aktualisiert; das Gleiche gilt für README,
  wenn sich sichtbares Verhalten ändert.
- `external/dependency-lock.json` nur bei echter Pin-Änderung anfassen (Upstream-Pins heute: llama.cpp
  `d3c3e242…`, sd.cpp `532d2c15…`, prime-agent `8980e66f…`).

## 7. Was nicht automatisch passiert

Keine Modelldownloads ohne Freigabe pro Datei (Name/Quelle/Größe/Lizenz), keine Distillation aus
Claude/Anthropic-Ausgaben, keine unbeaufsichtigte Selbstmodifikation von Binaries/Sicherheitsregeln, kein
Deaktivieren von Windows-Sicherheit/Pagefile/AV, keine BIOS-/Treiber-Änderung, kein `git clean -xfd`,
`git reset --hard` oder `checkout -- .`. BLOCKED bleibt BLOCKED, bis die echte Abhängigkeit vorhanden ist.
