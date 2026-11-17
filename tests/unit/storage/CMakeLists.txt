hcs_add_gtest(storage
  SOURCES
    test_sha256.cpp test_codec.cpp test_hcsmodel_parser.cpp test_roundtrip.cpp
    test_stream.cpp test_gguf.cpp
  LINK hcs_storage hcs_core
  LABELS unit)
