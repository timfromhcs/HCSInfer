# Real VLM tests for the llama adapter (libmtmd). They need HCS_TEST_VLM + HCS_TEST_MMPROJ (a multimodal
# GGUF and its projector); otherwise each reports "NOT RUN" (skipped with a message), never a fake pass.
# HCS_TEST_GGUF (a plain text GGUF) additionally enables the mismatched-projector test.
if(TARGET hcs_engine_llama)
  hcs_add_gtest(llama_vision
    SOURCES test_llama_vision.cpp
    LINK hcs_engine_llama hcs_core
    LABELS integration)
endif()
