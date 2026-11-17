// libFuzzer harness (AGENTS.md 16.4). Not built by default; see docs/model-vault.md "Fuzzing".
// Input layout: first byte selects the target and checksum mode, the rest is the file image.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "fuzz_common.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 1)
        return 0;
    const std::uint8_t mode = data[0];
    const std::span<const std::uint8_t> body(data + 1, size - 1);
    bool ok = true;
    if (mode & 1) {
        (void)hcs::storage::fuzz::check_gguf(body, &ok);
    } else {
        (void)hcs::storage::fuzz::check_container(body, (mode & 2) != 0, &ok);
    }
    if (!ok)
        std::abort();
    return 0;
}
