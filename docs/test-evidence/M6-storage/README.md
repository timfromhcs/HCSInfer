# M6 storage evidence

| File | Content |
|---|---|
| ctest-debug-storage.txt / ctest-release-storage.txt | ctest of the storage tests (59 unit + 3 parser + 2 integration with HCS_TEST_GGUF set), build dirs build/storage-{debug,release} |
| storage-bench-qwen3-0.6b-q4_k_m.json | benchmark JSON (schema_version 1) from hcs_storage_bench, Release build |
| libfuzzer-run.txt | coverage-guided fuzz smoke run notes (UBSan, no ASan) |

Machine/driver facts used: Windows 11 10.0.26200, MSVC 19.44, CMake 4.4.0. E: and D: are both USB-attached disks
(Get-PhysicalDisk: IB-1232CL-U3, BusType USB, MediaType Unspecified): whether they are spinning or solid state is unknown.
Other engineers were building/running on the same machine during the measurements.
