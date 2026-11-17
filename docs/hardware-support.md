# Hardware support and detection

Status: M0 (doctor) implemented. Everything below marked "measured" comes from
`docs/test-evidence/M0/` on the development machine; it is not a general compatibility claim.

## How HCSInfer detects hardware

`HCSInfer doctor` (module `src/hardware`, target `hcs_hardware`) is read-only and never changes system
settings. Detection is capability-based and happens at runtime; the target specification is never
hard-coded as a detected fact. A value that cannot be detected is reported as the string `"unknown"`.

| Area | Method | Notes |
|---|---|---|
| CPU identity / features | `cpuid` leaves 0, 1, 7, 0x80000001-4 and `xgetbv` | AVX/AVX2/FMA3/F16C/AVX-512 are reported usable only if the CPU bit **and** the OS state (XCR0) are present |
| CPU cores / threads | `GetLogicalProcessorInformationEx` | physical cores and logical processors; nominal MHz from the registry |
| RAM | `GlobalMemoryStatusEx`, `GetPhysicallyInstalledSystemMemory` | total usable, installed, available, commit limit |
| Memory channels, BIOS UMA setting | not detectable without WMI/firmware access | always `unknown` (see below) |
| Windows build | `RtlGetVersion` plus registry (`UBR`, `DisplayVersion`) | |
| GPU adapters, UMD version, memory budget | DXGI (`dxgi.dll` loaded dynamically, `IDXGIAdapter3::QueryVideoMemoryInfo`) | `DedicatedVideoMemory` on an iGPU is the firmware carve-out as seen by DXGI, not a statement about the BIOS option |
| Display driver version | display-class registry key | no WMI |
| Vulkan | `vulkan-1.dll` loaded at runtime, entry points via `vkGetInstanceProcAddr` | no SDK, no import library, nothing needed at runtime besides the system loader/driver |
| Storage identity | `GetVolumeInformationW`, `GetDiskFreeSpaceExW`, `IOCTL_STORAGE_QUERY_PROPERTY` (bus type, seek penalty, vendor/model) | metadata only; volume label and serial numbers are deliberately not read |
| Read speed | bounded sequential `ReadFile` of a user-supplied existing file | strictly read-only; cold/warm state is `unknown` |

Vulkan report content per device: API version, vendor/device ID, device type, driver ID/name/info,
subgroup size (min/max where `Vulkan 1.3` properties exist), supported subgroup stages and operations,
compute limits (shared memory, invocations, work-group size/count, storage buffer range, push constants),
shader `float16`/`int8`/`int16`/`int64`, 16/8-bit storage, integer dot product, cooperative matrix feature
and shapes, timeline semaphore, buffer device address, `VK_EXT_memory_budget` availability, memory heaps
and types with budget/usage, queue families, and the full extension list.
Optional features are never inferred from the GPU name; they are only reported when the driver says so.

## Commands

```powershell
HCSInfer.exe doctor                          # human-readable
HCSInfer.exe doctor --json                   # schema_version 1
HCSInfer.exe doctor --no-vulkan              # CPU-fallback mode: vulkan-1.dll is never loaded
HCSInfer.exe doctor --storage-root E:\Models # volume / filesystem / bus type of a path
HCSInfer.exe doctor --read-file E:\Models\m.gguf --read-repeats 2 --read-max-mib 256 --read-buffer-kib 4096
```

Read benchmark limits: buffer 64 KiB to 64 MiB, 1 to 5 samples, at most `--read-max-mib` per sample.
Invalid options return exit code 2. A missing or unreadable file is reported inside the report
(`read_benchmark.status = "error"`), not as a crash.

JSON top level (`schema_version` 1): `hcsinfer_version`, `timestamp_utc`, `options`, `os`, `memory`, `cpu`,
`gpu` (DXGI adapters and display driver registry entries), `vulkan`, `storage`, optional `read_benchmark`,
and a flat `summary` using the keys of the benchmark metadata in AGENTS.md section 21.1
(`cpu`, `gpu`, `gpu_vendor_id`, `gpu_device_id`, `driver_version`, `vulkan_api_version`, `ram_bytes`,
`os_build`, plus `uma_reservation_bytes`, `registry_driver_version`, `vulkan_status`).
`vulkan.status` is one of `ok`, `skipped` (`--no-vulkan`), `unavailable` (no loader), `failed`, `no_devices`.

## Measured on the development machine (2026-11-17)

Source: `docs/test-evidence/M0/doctor.json`, `doctor.txt`, `vulkaninfo-summary.txt`.

| Item | Value |
|---|---|
| OS | Windows 11 (version 10.0, build 26200, UBR 9974, 25H2) |
| CPU | AMD Ryzen 7 7735HS with Radeon Graphics; family 0x19, model 0x44, stepping 1; 8 cores / 16 threads |
| CPU features (usable) | SSE4.2, AVX, AVX2, FMA3, F16C, BMI1, BMI2, SHA, AES, POPCNT; no AVX-512 |
| RAM | 21,245,952,000 bytes usable (19.79 GiB) of 24.00 GiB installed |
| GPU (DXGI and Vulkan) | AMD Radeon(TM) Graphics, vendor 0x1002, device 0x1681, integrated |
| Driver | Vulkan: AMD proprietary driver 26.6.1; Windows driver 32.0.21043.12001 (5-29-2026) |
| Vulkan | instance 1.4.357, device API 1.4.315 |
| Subgroup | size 64 (min 32, max 64); basic, vote, arithmetic, ballot, shuffle, shuffle_relative, clustered, quad, rotate |
| Shader features | float16, int8, int16, 16-bit and 8-bit storage, integer dot product: yes; cooperative matrix (KHR): yes |
| Limits | 32 KiB compute shared memory, 1024 invocations per work group, max allocation 2 GiB |
| Memory heaps | 9.10 GiB device-local (budget 8.64 GiB), 4.55 GiB host, 256 MiB device-local; `VK_EXT_memory_budget` present |
| DXGI dedicated video memory | 3.97 GiB (firmware carve-out as seen by DXGI), shared system memory 9.89 GiB |
| Queue families | family 0: 8 x graphics/compute/transfer; family 1: 4 x compute/transfer; family 2: 1 x transfer; families 3 and 4: 1 x no capability flags |
| `E:` volume | NTFS, bus type USB, enclosure model string reported by the device, 465.76 GiB; rotational state `unknown` (the USB bridge does not answer the seek-penalty query) |
| Sequential read, `E:` (256 MiB, 4 MiB buffer) | 40.7 and 55.2 MiB/s in `readbench-usb-hdd.json`; cold/warm state is `unknown` |

Caveats: the read speed of one file on one occasion is not a storage benchmark. A repeated read of the same
file is served by the Windows file cache (3.8 GiB/s was measured in `doctor.json` for a file read shortly
before), which is exactly why `cold_or_warm` is reported as `unknown` unless it is controlled.

## Known gaps (reported as `unknown`)

- Memory channel configuration and the BIOS UMA size: no safe, WMI-free source. DXGI's dedicated video memory
  is exposed separately as `uma_reservation_bytes` with that exact meaning.
- Rotational vs. SSD for some USB bridges (the query is unsupported by the bridge).
- GPU temperature, clocks and utilisation: not collected in M0.
- Linux (Mesa RADV): the CPU `cpuid` path compiles on GCC/Clang, but system, DXGI, Vulkan loading and storage
  detection are Windows-only for now and report `unavailable` or `unknown` elsewhere.

## Tested hardware matrix

| GPU | Driver | OS | Vulkan | Doctor | Real inference |
|---|---|---|---|---|---|
| AMD Radeon 680M (0x1002:0x1681) | AMD proprietary 26.6.1 | Windows 11 build 26200 | 1.4.315 | PASS (this document) | see M1 evidence |

No other GPU has been tested; nothing here is a claim about other RDNA parts.
