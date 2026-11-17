#pragma once
// HCSInfer doctor: read-only hardware / driver / Vulkan / storage diagnostics.
// Unknown or undetectable values are reported as the string "unknown" (never guessed).
#include <cstdint>
#include <iosfwd>
#include <string>

namespace hcs::hardware {

struct DoctorOptions {
    bool json = false;
    bool no_vulkan = false;           // CPU-fallback mode: Vulkan is not even loaded
    std::string storage_root;         // optional existing path: volume/filesystem/bus info
    std::string read_file;            // optional existing file for the read-only read benchmark
    std::uint64_t read_max_bytes = 256ull << 20;  // upper bound of bytes read per sample
    std::uint32_t read_buffer_bytes = 4u << 20;   // bounded read buffer (64 KiB .. 64 MiB)
    std::uint32_t read_repeats = 1;               // 1 .. 5 samples
};

// Contract used by the CLI: writes the report (JSON when `json`) to `out`, returns an hcs exit code.
int run_doctor(bool json, std::ostream& out);
// Full-option variant; argument errors are written to `err` and return an hcs exit code (2).
int run_doctor(const DoctorOptions& options, std::ostream& out, std::ostream& err);

// Builds the report and returns it as pretty-printed JSON text (schema_version 1).
// Option validation errors are returned as a JSON object {"error": "..."} with `ok=false`.
std::string collect_report_json(const DoctorOptions& options, bool* ok = nullptr);

}  // namespace hcs::hardware
