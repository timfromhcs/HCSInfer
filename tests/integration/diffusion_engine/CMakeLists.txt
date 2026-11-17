# Real stable-diffusion.cpp adapter tests. Need HCS_TEST_DIFFUSION_MODEL (path to a small diffusion GGUF, e.g.
# SD1.5 Q8_0); otherwise they report NOT RUN (skipped with a message) - never a fake pass.
if(TARGET hcs_engine_diffusion)
  hcs_add_gtest(diffusion_engine
    SOURCES test_diffusion_engine.cpp
    LINK hcs_engine_diffusion hcs_core
    LABELS integration)
endif()
