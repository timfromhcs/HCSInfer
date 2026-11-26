// Safetensors header reader: validation of hostile / truncated inputs. Synthetic bytes only.
#include <gtest/gtest.h>

#include "hcs/safetensors.hpp"
#include "safetensors_builder.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using hcs::test::build_safetensors;
using hcs::test::f32_tensor;

namespace {
Result<SafetensorsHeader> parse(const std::string& s) {
    return parse_safetensors_header(reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), s.size());
}
std::string with_header(const std::string& json, std::size_t data_bytes) {
    std::string out;
    const std::uint64_t n = json.size();
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
    out += json;
    out.append(data_bytes, '\0');
    return out;
}
}  // namespace

TEST(Safetensors, ParsesTensorsAndMetadata) {
    const std::string f = build_safetensors(
        {f32_tensor("b", {2}, {1, 2}), f32_tensor("a", {2, 3}, {1, 2, 3, 4, 5, 6})}, {{"format", "pt"}});
    auto r = parse(f);
    ASSERT_TRUE(r.ok()) << r.error().message;
    ASSERT_EQ(r.value().tensors.size(), 2u);
    EXPECT_EQ(r.value().tensors[0].name, "b");  // sorted by data offset
    EXPECT_EQ(r.value().find("a")->element_count(), 6u);
    EXPECT_EQ(r.value().find("a")->byte_size(), 24u);
    EXPECT_EQ(r.value().metadata.at("format"), "pt");
    EXPECT_EQ(r.value().find("zzz"), nullptr);
}

TEST(Safetensors, ScalarTensorHasOneElement) {
    auto r = parse(build_safetensors({f32_tensor("s", {}, {3.0f})}));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().tensors[0].element_count(), 1u);
}

TEST(Safetensors, RejectsMalformedHeaders) {
    EXPECT_FALSE(parse("").ok());
    EXPECT_FALSE(parse("abc").ok());
    // Length prefix larger than the file.
    std::string huge(8, '\xff');
    EXPECT_FALSE(parse(huge + "{}").ok());
    EXPECT_TRUE(parse(with_header("{}", 0)).ok());  // an empty object is a valid file with no tensors
    EXPECT_FALSE(parse(with_header("[1,2]", 0)).ok());
    EXPECT_FALSE(parse(with_header("{not json", 0)).ok());
    EXPECT_FALSE(parse(with_header(R"({"x":{"dtype":"F32","shape":[1]}})", 4)).ok());
    EXPECT_FALSE(parse(with_header(R"({"x":{"dtype":"FOO","shape":[1],"data_offsets":[0,4]}})", 4)).ok());
    EXPECT_FALSE(parse(with_header(R"({"x":{"dtype":"F32","shape":[-1],"data_offsets":[0,4]}})", 4)).ok());
    EXPECT_FALSE(parse(with_header(R"({"x":{"dtype":"F32","shape":[2],"data_offsets":[0,4]}})", 8)).ok());
}

TEST(Safetensors, RejectsOutOfRangeOverflowAndOverlap) {
    // Range past the data section.
    EXPECT_FALSE(parse(with_header(R"({"x":{"dtype":"F32","shape":[2],"data_offsets":[0,8]}})", 4)).ok());
    // end < begin
    EXPECT_FALSE(parse(with_header(R"({"x":{"dtype":"F32","shape":[1],"data_offsets":[4,0]}})", 8)).ok());
    // Element-count overflow in the shape.
    EXPECT_FALSE(
        parse(with_header(
                  R"({"x":{"dtype":"F32","shape":[4294967296,4294967296,4294967296],"data_offsets":[0,4]}})",
                  4))
            .ok());
    // Overlapping tensors.
    EXPECT_FALSE(parse(with_header(R"({"a":{"dtype":"F32","shape":[2],"data_offsets":[0,8]},)"
                                   R"("b":{"dtype":"F32","shape":[2],"data_offsets":[4,12]}})",
                                   12))
                     .ok());
}

TEST(Safetensors, HeaderCapIsEnforced) {
    const std::string f = build_safetensors({f32_tensor("a", {1}, {1})});
    auto r = parse_safetensors_header(reinterpret_cast<const std::uint8_t*>(f.data()), f.size(), f.size(), 4);
    EXPECT_FALSE(r.ok());
}

TEST(Safetensors, ReadsHeaderFromDiskAndReportsMissing) {
    test::TempDir td;
    test::write_file(td / "a.safetensors", build_safetensors({f32_tensor("a", {2}, {1, 2})}));
    auto ok = read_safetensors_header(td / "a.safetensors");
    ASSERT_TRUE(ok.ok());
    EXPECT_EQ(ok.value().tensors.size(), 1u);
    EXPECT_EQ(ok.value().data_offset + 8, ok.value().file_size);
    auto missing = read_safetensors_header(td / "nope.safetensors");
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
    test::write_file(td / "t.safetensors", build_safetensors({f32_tensor("a", {2}, {1, 2})}).substr(0, 30));
    EXPECT_FALSE(read_safetensors_header(td / "t.safetensors").ok());
}

TEST(Safetensors, DtypeSizes) {
    EXPECT_EQ(safetensors_dtype_size("F32"), 4u);
    EXPECT_EQ(safetensors_dtype_size("F16"), 2u);
    EXPECT_EQ(safetensors_dtype_size("BF16"), 2u);
    EXPECT_EQ(safetensors_dtype_size("I64"), 8u);
    EXPECT_EQ(safetensors_dtype_size("nope"), 0u);
}
