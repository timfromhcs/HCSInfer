#pragma once
// Deterministic runtime options for the stable-diffusion.cpp adapter.
//
// Upstream defaults `flash_attn` and `conv_direct` to "auto", which on a GPU backend runs a short timing
// probe at context creation and picks a kernel from the measured milliseconds. That makes the selected kernel
// (and so the pixels) depend on GPU clocks and CPU load (docs/test-evidence/wave-a/a2). HCSInfer therefore
// pins the choice explicitly on GPU backends. CPU backends have no probe ("flash attention auto: CPU -> on
// (CPU)") and keep upstream behaviour.
#include <string>

namespace hcs {

struct DiffusionRuntimeOptions {
    std::string flash_attn_mode;   // empty = leave the upstream default
    std::string conv_direct_mode;  // empty = leave the upstream default
};

inline DiffusionRuntimeOptions diffusion_runtime_options(bool gpu) {
    DiffusionRuntimeOptions o;
    if (gpu) {
        o.flash_attn_mode = "off";   // the probe measured flash ~2x slower than normal on Radeon 680M anyway
        o.conv_direct_mode = "off";  // the probe's output check fails on Radeon 680M anyway
    }
    return o;
}

}  // namespace hcs
