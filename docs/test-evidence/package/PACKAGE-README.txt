HCSInfer 0.1.0 portable folder (img-api)

This is a portable multi-file package, NOT a single-file executable (docs/decisions/ADR-008-static-crt-single-exe.md).
- Built with the default dynamic MSVC runtime: the Microsoft Visual C++ Redistributable (x64) must be installed.
- No models, no graphics driver and no Vulkan SDK are included. Vulkan use needs the installed AMD/OEM driver.
- Run:  HCSInfer.exe --help   |   HCSInfer.exe doctor --json
- Verify files:  Get-FileHash -Algorithm SHA256 <file>  and compare with SHA256SUMS.txt.
- Third-party licences: THIRD_PARTY_NOTICES.md and the licenses/ folder. SBOM: sbom.cdx.json.
Status: alpha. See README.md for what is implemented. Unsigned.
