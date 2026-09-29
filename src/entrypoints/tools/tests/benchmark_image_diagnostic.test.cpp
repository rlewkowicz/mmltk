#include "src/backend/data/compiled/compiled_file_utils.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/subprocess_test_utils.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <parquet/arrow/writer.h>
#include <stb_image_write.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>
namespace {
namespace data = mmltk::backend::data;
using Json = nlohmann::json;
using mmltk::testsupport::ScopedTempDir;
using mmltk::testsupport::SubprocessResult;
SubprocessResult diagnose(const std::vector<std::string>& arguments, bool requirements = false) {
 std::vector<std::string> command{
  "timeout", "--signal=KILL", "120s", "env", "MMLTK_COMPILED_SAMPLE_DIAGNOSTIC=" MMLTK_TEST_COMPILED_DIAGNOSTIC, "MMLTK_COCONUT_MASK_DIAGNOSTIC=" MMLTK_TEST_MASK_DIAGNOSTIC,
  std::string("MMLTK_BENCHMARK_NATIVE_REQUIREMENTS=") + (requirements ? "1" : "0"), MMLTK_TEST_PYTHON, "-I", "-B", MMLTK_TEST_BENCHMARK_DIAGNOSTIC
 };
 command.insert(command.end(), arguments.begin(), arguments.end());
 return mmltk::testsupport::run_subprocess_capture_output(command);
}
Json successful_record(const SubprocessResult& result) {
 INFO(result.output_text);
 REQUIRE(result.exit_code == 0);
 CHECK(result.stderr_text.empty());
 return Json::parse(result.stdout_text);
}
void rejected(const SubprocessResult& result, const std::string& cause) {
 INFO(result.output_text);
 CHECK(result.exit_code != 0);
 CHECK(result.stderr_text.find(cause) != std::string::npos);
 CHECK(result.stderr_text.find("Traceback") == std::string::npos);
}
// Sparse pixel/label capacity is intentionally never materialized by this
// fixture. Two eligible slots straddle a block boundary with inactive holes.
struct CompiledSampleFixture {
 ScopedTempDir root{"benchmark-diagnostic-compiled"};
 std::filesystem::path path = root.path() / "samples.bin";
 data::FileLayout layout = data::compute_file_layout(66, 4 * 3 * sizeof(float));
 std::array<std::string, 1> classes{"sample"};
 data::FileHeader header = data::make_file_header({.num_images = 2, .width = 2, .height = 2, .channels = 3, .max_instances_per_image = 1, .image_stride = layout.image_stride}, classes, layout);
 mmltk::common::io::FileHandle file = mmltk::common::io::FileHandle::create_output(path.string(), 0);
 data::ImageEntry entry{
  .status = data::ImageStatus::Eligible, .has_source_image_id = 1, .original_width = 4, .original_height = 3, .source = data::AnnotationSource::CoconutCoco, .source_image_id = 2212
 };
 data::PackedInstance label{
  .class_id = 0,
  .flags = data::kAnnotationMask | data::kAnnotationId | data::kAnnotationCategory | data::kAnnotationCrowd,
  .bbox_x1 = 0,
  .bbox_y1 = 0,
  .bbox_x2 = 2,
  .bbox_y2 = 1,
  .mask_rle_offset = 0,
  .mask_rle_pairs = 1,
  .original_area = 2.0,
  .annotation_id = 8123,
  .source_category_id = 1,
  .source_ordinal = 9
 };
 std::array<data::RLEPair, 1> runs{{{0, 2}}};
 CompiledSampleFixture() {
  file.resize_sparse(layout.total_size);
  write_header();
  data::set_annotation_extent(entry, 1, runs.size(), "diagnostic fixture");
  write_entry();
  data::write_compiled_annotations(file, layout, 65, entry, {{&label, 1}, runs});
  const data::ImageEntry background{.status = data::ImageStatus::Eligible, .original_width = 2, .original_height = 2};
  file.pwrite_all(&background, sizeof(background), layout.index_offset);
 }
 void write_header() { file.pwrite_all(&header, sizeof(header), 0); }
 void write_entry() { file.pwrite_all(&entry, sizeof(entry), layout.index_offset + 65 * sizeof(entry)); }
 SubprocessResult inspect(const std::string& slot = "65") const { return diagnose({"--compiled", path.string(), "--sample", slot}); }
};
template <class T>
T arrow_result(arrow::Result<T> result) {
 INFO(result.status().ToString());
 REQUIRE(result.ok());
 return std::move(result).ValueOrDie();
}
// Only the fields consumed by the diagnostic are needed. The first row has a
// different source ID and invalid bytes, proving selection precedes PNG decode.
void write_parquet_mask(const std::filesystem::path& path, const std::string& png) {
 arrow::Int64Builder ids;
 REQUIRE(ids.AppendValues({99, 2212}).ok());
 const auto images = arrow_result(arrow::StructArray::Make({arrow_result(ids.Finish())}, std::vector<std::string>{"id"}));
 arrow::BinaryBuilder encoded;
 REQUIRE(encoded.Append("not a PNG").ok());
 REQUIRE(encoded.Append(png).ok());
 const auto masks = arrow_result(arrow::StructArray::Make({arrow_result(encoded.Finish())}, std::vector<std::string>{"bytes"}));
 arrow::Int64Builder segment_ids;
 REQUIRE(segment_ids.AppendValues({0x030201, 42}).ok());
 const auto segments = arrow_result(arrow::StructArray::Make({arrow_result(segment_ids.Finish())}, std::vector<std::string>{"id"}));
 arrow::Int32Builder offsets;
 REQUIRE(offsets.AppendValues({0, 0, 2}).ok());
 const auto lists = arrow_result(arrow::ListArray::FromArrays(*arrow_result(offsets.Finish()), *segments));
 const auto annotations = arrow_result(arrow::StructArray::Make({lists}, std::vector<std::string>{"segments_info"}));
 const auto schema = arrow::schema({arrow::field("image_info", images->type()), arrow::field("mask", masks->type()), arrow::field("segments_info", annotations->type())});
 const auto output = arrow_result(arrow::io::FileOutputStream::Open(path.string()));
 REQUIRE(parquet::arrow::WriteTable(*arrow::Table::Make(schema, {images, masks, annotations}), arrow::default_memory_pool(), output, 1).ok());
 REQUIRE(output->Close().ok());
}
}  // namespace
TEST_CASE("benchmark image diagnostic reads eligible physical slots and canonical annotations", "[entrypoints][tools][benchmark]") {
 CompiledSampleFixture fixture;
 const auto record = successful_record(fixture.inspect());
 CHECK(
  record ==
  Json{
   {"kind", "compiled_sample"}, {"path", fixture.path.string()}, {"sample", 65}, {"image_count", 2}, {"slot_count", 66}, {"width", 2}, {"height", 2}, {"original_width", 4}, {"original_height", 3},
   {"source", 4}, {"image_id", 2212}, {"label_bytes", +fixture.entry.label_bytes},
   {"instances",
    Json::array(
     {{{"class_id", 0}, {"class_name", "sample"}, {"flags", +fixture.label.flags}, {"bbox", {0, 0, 2, 1}}, {"mask_pairs", 1}, {"annotation_id", 8123}, {"source_category_id", 1}, {"original_area", 2.0}, {"source_ordinal", 9}}})}
  });
 const auto background = successful_record(fixture.inspect("0"));
 CHECK(background.at("image_id").is_null());
 CHECK(background.at("label_bytes") == 0);
 CHECK(background.at("instances").empty());
 CHECK(successful_record(diagnose({"--compiled", fixture.path.string(), "--sample", "65", "--image-id", "2212"})) == record);
 rejected(diagnose({"--compiled", fixture.path.string(), "--sample", "65", "--image-id", "7"}), "--image-id disagrees with the compiled sample");
 rejected(diagnose({"--compiled", fixture.path.string(), "--sample", "0", "--image", "unused.png"}), "this compiled sample has no source image ID");
}
TEST_CASE("benchmark image diagnostic rejects holes range errors and malformed compiled metadata", "[entrypoints][tools][benchmark]") {
 CompiledSampleFixture fixture;
 SECTION("inactive slot") { rejected(fixture.inspect("64"), "sample slot is not eligible"); }
 SECTION("out of range") { rejected(fixture.inspect("66"), "sample slot is outside the compiled dataset"); }
 SECTION("negative slot") { rejected(fixture.inspect("-1"), "invalid physical sample slot"); }
 SECTION("slot overflow") { rejected(fixture.inspect("4294967296"), "invalid physical sample slot"); }
 SECTION("metadata ceiling") {
  fixture.header.num_slots = std::numeric_limits<std::uint32_t>::max();
  fixture.write_header();
  rejected(fixture.inspect(), "compiled resident metadata exceeds");
 }
 SECTION("layout mismatch") {
  ++fixture.header.index_offset;
  fixture.write_header();
  rejected(fixture.inspect(), "compiled fixed slot layout or file size is invalid");
 }
 SECTION("eligible population exceeds slots") {
  fixture.header.num_images = fixture.header.num_slots + 1;
  fixture.write_header();
  rejected(fixture.inspect(), "compiled file has invalid image dimensions");
 }
 SECTION("label counts exceed capacity") {
  fixture.entry.num_runs = std::numeric_limits<std::uint32_t>::max();
  fixture.write_entry();
  rejected(fixture.inspect(), "compiled label capacity exceeded");
 }
 SECTION("used label extent mismatch") {
  ++fixture.entry.label_bytes;
  fixture.write_entry();
  rejected(fixture.inspect(), "compiled label extent differs");
 }
 SECTION("instance count exceeds header") {
  fixture.header.max_instances_per_image = 0;
  fixture.write_header();
  rejected(fixture.inspect(), "compiled annotation counts differ");
 }
 SECTION("missing benchmark identity") {
  fixture.entry.has_source_image_id = 0;
  fixture.entry.source_image_id = 0;
  fixture.write_entry();
  rejected(fixture.inspect(), "compiled benchmark image lacks source identity");
 }
 SECTION("invalid mask run") {
  fixture.runs[0].length = 5;
  data::write_compiled_annotations(fixture.file, fixture.layout, 65, fixture.entry, {{&fixture.label, 1}, fixture.runs});
  rejected(fixture.inspect(), "compiled RLE run is invalid");
 }
 SECTION("class catalog admission") {
  fixture.header.class_names[0][0] = '\0';
  fixture.write_header();
  rejected(fixture.inspect(), "empty or unterminated class name");
 }
}
TEST_CASE("benchmark image diagnostic selects PNG and Parquet masks with exact RGB segment IDs", "[entrypoints][tools][benchmark]") {
 ScopedTempDir root("benchmark-diagnostic-mask");
 const auto png = root.path() / "mask.png";
 const std::array<unsigned char, 12> pixels{1, 2, 3, 0, 0, 0, 1, 2, 3, 1, 2, 3};
 REQUIRE(stbi_write_png(png.c_str(), 2, 2, 3, pixels.data(), 6) != 0);
 const Json support = Json::array({{{"id", 0}, {"pixels", 1}}, {{"id", 0x030201}, {"pixels", 3}}});
 const auto plain = successful_record(diagnose({"--image-id", "2212", "--panoptic", png.string()}));
 CHECK(plain == Json{{"kind", "panoptic_mask"}, {"path", png.string()}, {"image_id", 2212}, {"width", 2}, {"height", 2}, {"support", support}});
 std::ifstream stream(png, std::ios::binary);
 const std::string encoded{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
 const auto parquet = root.path() / "mask.parquet";
 write_parquet_mask(parquet, encoded);
 const auto nested = successful_record(diagnose({"--image-id", "2212", "--parquet", parquet.string()}));
 CHECK(nested == Json{
                  {"kind", "panoptic_mask"}, {"path", parquet.string()}, {"image_id", 2212}, {"width", 2}, {"height", 2}, {"support", support}, {"row", 1}, {"image_info", {{"id", 2212}}},
                  {"segments", Json::array({{{"id", 0x030201}, {"observed_pixels", 3}}, {{"id", 42}, {"observed_pixels", 0}}})}
                 });
 rejected(diagnose({"--image-id", "8", "--parquet", parquet.string()}), "image ID is absent from this Parquet shard");
 rejected(diagnose({"--image-id", "99", "--parquet", parquet.string()}), "invalid or oversized mask geometry");
 const auto jpeg = root.path() / "mask.jpg";
 REQUIRE(stbi_write_jpg(jpeg.c_str(), 2, 2, 3, pixels.data(), 90) != 0);
 rejected(diagnose({"--image-id", "2212", "--panoptic", jpeg.string()}), "invalid or oversized mask geometry");
 const auto image = successful_record(diagnose({"--image-id", "2212", "--image", jpeg.string()}));
 CHECK(image.at("encoding") == "JPEG");
 CHECK(image.at("width") == 2);
 CHECK(image.at("height") == 2);
}
TEST_CASE("benchmark diagnostic preparation uses its argument parser without inspecting sources", "[entrypoints][tools][benchmark]") {
 for (const auto& [arguments, expected] : std::vector<std::pair<std::vector<std::string>, std::string>>{
       {{"--image-id", "2212", "--image", "absent.jpg"}, "native:\n"}, {{"--comp=absent.bin", "--sample=65"}, "native:compiled\n"},
       {{"--image-id", "2212", "--parq=absent.parquet", "--pano=absent.png"}, "native:mask\n"}, {{"--compiled", "absent.bin", "--sample", "65", "--panoptic", "absent.png"}, "native:compiled,mask\n"}
      }) {
  const auto result = diagnose(arguments, true);
  INFO(result.output_text);
  CHECK(result.exit_code == 0);
  CHECK(result.stdout_text == expected);
  CHECK(result.stderr_text.empty());
 }
 const auto help = diagnose({"--compiled", "absent.bin", "--help"}, true);
 CHECK(help.exit_code == 0);
 CHECK(help.stdout_text.find("--panoptic") != std::string::npos);
 rejected(diagnose({"--compiled", "absent.bin"}, true), "--compiled and --sample are required together");
}
