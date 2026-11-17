# Real llama.cpp adapter tests. Need HCS_TEST_GGUF (path to a small GGUF); otherwise they report
# NOT RUN (skipped with a message) - never a fake pass. Argument-validation/error tests run always.
if(TARGET hcs_engine_llama)
  hcs_add_gtest(llama_engine
    SOURCES test_llama_engine.cpp
    LINK hcs_engine_llama hcs_core
    LABELS integration)
  if(HCS_ENABLE_VULKAN)
    target_compile_definitions(hcs_test_llama_engine PRIVATE HCS_TEST_EXPECT_VULKAN_BUILD=1)
  endif()
endif()
