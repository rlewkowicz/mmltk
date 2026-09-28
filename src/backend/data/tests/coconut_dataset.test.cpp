#include "src/backend/data/benchmark/coconut/detail/coconut_physical.h"
#include "src/backend/data/benchmark/detail/benchmark_writer.h"
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include "src/backend/data/benchmark/detail/benchmark_annotation_cache.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_annotations.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_mask_recovery.h"
#include "detail/mask_rle_utils.h"
#include "src/backend/data/benchmark/detail/benchmark_recipe.h"
#include "src/backend/data/benchmark/detail/benchmark_images.h"
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/backend/data/benchmark/detail/benchmark_curl.h"
#include <sys/socket.h>
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "benchmark_http_fixture.h"
#include "src/backend/data/compiled/compiled_dataset.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <cerrno>
#include "src/common/io/file_memory.h"
#include "src/common/io/file_digest.h"
#include "src/backend/data/benchmark/benchmark_dataset_options.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/async_test_utils.hpp"
#include "src/common/system/cpu_affinity.h"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>
#include <archive.h>
#include <archive_entry.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <stb_image_write.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using namespace mmltk::backend::data;
using namespace mmltk::backend::data::benchmark_internal;
namespace {
NormalizedAnnotationIndex fixture_index(const NormalizedAnnotationBuilder& builder) {
 return seal_normalized_annotation_metadata(NormalizedAnnotationBuilder(builder));
}
const NormalizedAnnotationIndex& fixture_index(const NormalizedAnnotationIndex& index) { return index; }
using Json = nlohmann::json;
using mmltk::testsupport::ScopedTempDir;
void arrow_ok(const arrow::Status& status) {
 if (!status.ok()) throw std::runtime_error(status.ToString());
}
template <class T>
T arrow_value(arrow::Result<T> value) {
 arrow_ok(value.status());
 return std::move(value).ValueOrDie();
}
std::string png(int width, int height, std::span<const std::uint32_t> ids) {
 REQUIRE(ids.size() == static_cast<std::size_t>(width * height));
 std::vector<unsigned char> pixels(ids.size() * 3);
 for (std::size_t i = 0; i < ids.size(); ++i) {
  pixels[i * 3] = ids[i] & 255;
  pixels[i * 3 + 1] = (ids[i] >> 8) & 255;
  pixels[i * 3 + 2] = (ids[i] >> 16) & 255;
 }
 std::string encoded;
 REQUIRE(stbi_write_png_to_func(
          [](void* context, void* data, int size) { static_cast<std::string*>(context)->append(static_cast<const char*>(data), size); }, &encoded, width, height, 3, pixels.data(), width * 3) != 0);
 return encoded;
}
void tar(const std::filesystem::path& path, std::span<const std::pair<std::string, std::string>> members, bool symlink = false, int format = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED) {
 std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
 REQUIRE(archive_write_set_format(writer.get(), format) == ARCHIVE_OK);
 if (path.extension() == ".gz") REQUIRE(archive_write_add_filter_gzip(writer.get()) == ARCHIVE_OK);
 REQUIRE(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
 for (const auto& [name, bytes] : members) {
  std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
  archive_entry_set_pathname(entry.get(), name.c_str());
  archive_entry_set_perm(entry.get(), 0644);
  if (format == ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE) archive_entry_set_mtime(entry.get(), 1, 1);
  const bool directory = name == "." || name == "./";
  archive_entry_set_filetype(entry.get(), symlink ? AE_IFLNK : directory ? AE_IFDIR : AE_IFREG);
  if (symlink) archive_entry_set_symlink(entry.get(), "other");
  archive_entry_set_size(entry.get(), symlink || directory ? 0 : bytes.size());
  REQUIRE(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
  if (!symlink && !directory) REQUIRE(archive_write_data(writer.get(), bytes.data(), bytes.size()) == static_cast<la_ssize_t>(bytes.size()));
 }
 REQUIRE(archive_write_close(writer.get()) == ARCHIVE_OK);
}
Json category_catalog() {
 Json rows = Json::array();
 // CLEANUP-OFF: independent external COCO metadata is the oracle for production category mapping.
 constexpr std::pair<unsigned, const char*> categories[]{
  {1, "person"}, {2, "bicycle"}, {3, "car"}, {4, "motorcycle"}, {5, "airplane"}, {6, "bus"}, {7, "train"}, {8, "truck"}, {9, "boat"}, {10, "traffic light"}, {11, "fire hydrant"}, {13, "stop sign"},
  {14, "parking meter"}, {15, "bench"}, {16, "bird"}, {17, "cat"}, {18, "dog"}, {19, "horse"}, {20, "sheep"}, {21, "cow"}, {22, "elephant"}, {23, "bear"}, {24, "zebra"}, {25, "giraffe"},
  {27, "backpack"}, {28, "umbrella"}, {31, "handbag"}, {32, "tie"}, {33, "suitcase"}, {34, "frisbee"}, {35, "skis"}, {36, "snowboard"}, {37, "sports ball"}, {38, "kite"}, {39, "baseball bat"},
  {40, "baseball glove"}, {41, "skateboard"}, {42, "surfboard"}, {43, "tennis racket"}, {44, "bottle"}, {46, "wine glass"}, {47, "cup"}, {48, "fork"}, {49, "knife"}, {50, "spoon"}, {51, "bowl"},
  {52, "banana"}, {53, "apple"}, {54, "sandwich"}, {55, "orange"}, {56, "broccoli"}, {57, "carrot"}, {58, "hot dog"}, {59, "pizza"}, {60, "donut"}, {61, "cake"}, {62, "chair"}, {63, "couch"},
  {64, "potted plant"}, {65, "bed"}, {67, "dining table"}, {70, "toilet"}, {72, "tv"}, {73, "laptop"}, {74, "mouse"}, {75, "remote"}, {76, "keyboard"}, {77, "cell phone"}, {78, "microwave"},
  {79, "oven"}, {80, "toaster"}, {81, "sink"}, {82, "refrigerator"}, {84, "book"}, {85, "clock"}, {86, "vase"}, {87, "scissors"}, {88, "teddy bear"}, {89, "hair drier"}, {90, "toothbrush"}
 };
 // CLEANUP-ON
 for (const auto& [id, name] : categories) rows.push_back({{"id", id}, {"name", name}, {"isthing", 0}});
 rows.push_back({{"id", 200}, {"name", "unrelated stuff"}, {"isthing", 1}});
 return rows;
}
void json_file(const std::filesystem::path& path, Json value) {
 if (value.contains("images") && value.contains("annotations") && !value.contains("categories")) value["categories"] = category_catalog();
 mmltk::testsupport::write_text_file(path, value.dump());
}
Json segment(unsigned id = 1, unsigned category = 1, bool thing = true) { return {{"id", id}, {"category_id", category}, {"isthing", thing ? 1 : 0}, {"iscrowd", 0}, {"area", nullptr}}; }
std::shared_ptr<arrow::DataType> segment_type(bool integer_area) {
 return arrow::struct_(
  {arrow::field("area", integer_area ? arrow::int64() : arrow::float64()), arrow::field("category_id", arrow::int64()), arrow::field("id", arrow::int64()), arrow::field("iscrowd", arrow::int64()),
   arrow::field("isthing", arrow::int64())});
}
std::shared_ptr<arrow::Schema> hf_schema(bool integer_area = false) {
 return arrow::schema(
  {arrow::field("mask", arrow::struct_({arrow::field("bytes", arrow::binary()), arrow::field("path", arrow::utf8())})),
   arrow::field(
    "segments_info", arrow::struct_({arrow::field("file_name", arrow::utf8()), arrow::field("image_id", arrow::int64()), arrow::field("segments_info", arrow::list(segment_type(integer_area)))})),
   arrow::field("image_info", arrow::struct_(
                               {arrow::field("coco_url", arrow::utf8()), arrow::field("date_captured", arrow::utf8()), arrow::field("file_name", arrow::utf8()), arrow::field("height", arrow::int64()),
                                arrow::field("id", arrow::int64()), arrow::field("license", arrow::int64()), arrow::field("width", arrow::int64())}))});
}
void append_value(arrow::ArrayBuilder& builder, const Json& value) {
 if (value.is_null()) {
  arrow_ok(builder.AppendNull());
  return;
 }
 switch (builder.type()->id()) {
  case arrow::Type::STRUCT: {
   auto& typed = static_cast<arrow::StructBuilder&>(builder);
   arrow_ok(typed.Append());
   const auto type = std::static_pointer_cast<const arrow::StructType>(builder.type());
   for (int i = 0; i < type->num_fields(); ++i) append_value(*typed.field_builder(i), value.at(type->field(i)->name()));
   break;
  }
  case arrow::Type::LIST: {
   auto& typed = static_cast<arrow::ListBuilder&>(builder);
   arrow_ok(typed.Append());
   for (const auto& entry : value) append_value(*typed.value_builder(), entry);
   break;
  }
  case arrow::Type::INT64: arrow_ok(static_cast<arrow::Int64Builder&>(builder).Append(value.get<std::int64_t>())); break;
  case arrow::Type::DOUBLE: arrow_ok(static_cast<arrow::DoubleBuilder&>(builder).Append(value.get<double>())); break;
  case arrow::Type::STRING: arrow_ok(static_cast<arrow::StringBuilder&>(builder).Append(value.get<std::string>())); break;
  case arrow::Type::BINARY: arrow_ok(static_cast<arrow::BinaryBuilder&>(builder).Append(value.get<std::string>())); break;
  default: throw std::runtime_error("unsupported fixture type");
 }
}
void parquet_file(const std::filesystem::path& path, const Json& rows, std::shared_ptr<arrow::Schema> schema = hf_schema(), parquet::Compression::type codec = parquet::Compression::SNAPPY) {
 std::vector<std::shared_ptr<arrow::Array>> columns;
 for (const auto& field : schema->fields()) {
  std::unique_ptr<arrow::ArrayBuilder> builder;
  arrow_ok(arrow::MakeBuilder(arrow::default_memory_pool(), field->type(), &builder));
  for (const auto& row : rows) append_value(*builder, row.at(field->name()));
  std::shared_ptr<arrow::Array> column;
  arrow_ok(builder->Finish(&column));
  columns.push_back(std::move(column));
 }
 auto output = arrow_value(arrow::io::FileOutputStream::Open(path.string()));
 parquet::WriterProperties::Builder properties;
 properties.compression(codec);
 arrow_ok(parquet::arrow::WriteTable(*arrow::Table::Make(schema, columns), arrow::default_memory_pool(), output, 1, properties.build()));
 arrow_ok(output->Close());
}
Json hf_row(unsigned id, std::string encoded, Json segments, int width = 3, int height = 3, std::string_view physical_source = "train2017") {
 const auto digits = std::to_string(id);
 const auto stem = std::string(12U - digits.size(), '0') + digits;
 return {
  {"mask", {{"bytes", std::move(encoded)}, {"path", nullptr}}}, {"segments_info", {{"file_name", stem + ".png"}, {"image_id", id}, {"segments_info", std::move(segments)}}},
  {"image_info", {{"coco_url", "http://images.cocodataset.org/" + std::string(physical_source) + "/" + stem + ".jpg"}, {"date_captured", "2017"}, {"file_name", stem + ".jpg"}, {"height", height}, {"width", width}, {"id", id},
                  {"license", 1}}}
 };
}
CoconutPhysicalImage coco(unsigned id, CoconutImageNamespace source = CoconutImageNamespace::CocoTrain) {
 const auto digits = std::to_string(id);
 const auto prefix = source == CoconutImageNamespace::CocoTrain ? "train2017/" : source == CoconutImageNamespace::CocoUnlabeled ? "unlabeled2017/" : "val2017/";
 return {source, id, 0, prefix + std::string(12U - digits.size(), '0') + digits + ".jpg", "physical-coco-archive"};
}
CoconutPhysicalImage objects(unsigned id, CoconutImageNamespace source = CoconutImageNamespace::Objects365V2) {
 const auto digits = std::to_string(id);
 return {
  source, id, 32, std::string(source == CoconutImageNamespace::Objects365V1 ? "image/objects365_v1_" : "patch32/objects365_v2_") + std::string(8U - digits.size(), '0') + digits + ".jpg",
  "physical-objects-archive"
 };
}
struct LocalCoconutImportRequest : CoconutImportRequest {
 std::unique_ptr<CoconutPhysicalMembership> membership;
 LocalCoconutImportRequest(std::span<const CoconutPhysicalImage> physical, CoconutEdition selected) : membership(std::make_unique<CoconutPhysicalMembership>(physical)) {
  edition = selected;
  input_identity = "pinned-local-fixture-inputs";
  physical_membership = membership.get();
 }
};
LocalCoconutImportRequest request(std::span<const CoconutPhysicalImage> physical, CoconutEdition edition = CoconutEdition::Base) { return LocalCoconutImportRequest(physical, edition); }
void reversed_coco_parquet(const std::filesystem::path& path) {
 const std::array<std::uint32_t, 1> ids{1};
 const auto mask = png(1, 1, ids);
 parquet_file(path, Json::array({hf_row(8, mask, Json::array({segment()}), 1, 1), hf_row(7, mask, Json::array({segment()}), 1, 1)}));
}
struct PollCancellation {
 mutable std::size_t polls = 0;
 std::size_t stop_at = std::numeric_limits<std::size_t>::max();
 bool cancelled() const noexcept { return polls++ >= stop_at; }
};
std::string file_bytes(const std::filesystem::path& path) {
 std::ifstream input(path, std::ios::binary);
 return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
// Independent fixture encodings: stored blocks provide ordinary compressed
// partitions; one fixed-Huffman block exceeds the decoder's expanded slot.
std::string gzip_fixture(std::string_view input, bool single_block = false) {
 if (!single_block) {
  z_stream stream{};
  REQUIRE(deflateInit2(&stream, Z_NO_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
  struct End { z_stream& stream; ~End() { deflateEnd(&stream); } } end{stream};
  std::string encoded(deflateBound(&stream, input.size()), '\0');
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = reinterpret_cast<Bytef*>(encoded.data());
  stream.avail_out = static_cast<uInt>(encoded.size());
  REQUIRE(deflate(&stream, Z_FINISH) == Z_STREAM_END);
  encoded.resize(stream.total_out);
  return encoded;
 }
 std::string encoded("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
 unsigned pending = 0, bits = 0;
 const auto append = [&](unsigned value, unsigned count) {
  pending |= value << bits; bits += count;
  while (bits >= 8) { encoded.push_back(static_cast<char>(pending)); pending >>= 8; bits -= 8; }
 };
 const auto symbol = [&](unsigned value) {
  const unsigned count = value < 144 ? 8 : value < 256 ? 9 : value < 280 ? 7 : 8;
  const unsigned code = value < 144 ? value + 0x30 : value < 256 ? value - 144 + 0x190 : value < 280 ? value - 256 : value - 280 + 0xc0;
  unsigned reversed = 0;
  for (unsigned bit = 0; bit < count; ++bit) reversed = (reversed << 1) | ((code >> bit) & 1);
  append(reversed, count);
 };
 append(3, 3); // BFINAL=1, BTYPE=01: exactly one fixed-Huffman block.
 for (std::size_t offset = 0; offset < input.size();) {
  const auto value = static_cast<unsigned char>(input[offset]);
  std::size_t end = offset + 1;
  while (end < input.size() && input[end] == input[offset]) ++end;
  symbol(value); ++offset;
  while (end - offset >= 258) { symbol(285); append(0, 5); offset += 258; } // length=258, distance=1.
  while (offset < end) { symbol(value); ++offset; }
 }
 symbol(256);
 if (bits) encoded.push_back(static_cast<char>(pending));
 const auto checksum = static_cast<std::uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(input.data()), static_cast<uInt>(input.size())));
 for (const auto value : {checksum, static_cast<std::uint32_t>(input.size())})
  for (unsigned shift = 0; shift < 32; shift += 8) encoded.push_back(static_cast<char>(value >> shift));
 return encoded;
}
void check_publication_bytes(const std::filesystem::path& output, std::string_view train, std::string_view validation, std::string_view manifest) {
 CHECK(file_bytes(output / "train.bin") == train);
 CHECK(file_bytes(output / "val.bin") == validation);
 CHECK(file_bytes(output / "benchmark_manifest.json") == manifest);
}
CoconutComponentBuilder component_builder(const CoconutComponent& component) {
 CoconutComponentBuilder builder;
 builder.edition = component.edition(); builder.source = component.source(); builder.input_identity = component.input_identity();
 builder.recovery_policy = component.recovery_policy(); builder.original_annotation_identity = component.original_annotation_identity();
 static_cast<NormalizedAnnotationMetadata&>(builder.index) = component.index();
 for (std::size_t i = 0; i < component.index().image_count(); ++i) {
  append_normalized_image_slice(builder.index, component.index().storage(), component.index().source_position(i));
  builder.inventory.push_back(component.inventory_image(i));
  if (builder.recovery_policy) builder.recovery.push_back(component.recovery_image(i));
 }
 return builder;
}
void expect_runs(const CoconutComponent& component, std::span<const RLEPair> expected) {
 REQUIRE(component.index().run_count() == expected.size());
 for (std::size_t i = 0; i < expected.size(); ++i) {
  CHECK(component.index().storage().mask_rle_pairs[i].start == expected[i].start);
  CHECK(component.index().storage().mask_rle_pairs[i].length == expected[i].length);
 }
}
NormalizedAnnotationBuilder recovery_originals(unsigned image_id, std::span<const RLEPair> masks) {
 NormalizedAnnotationBuilder index;
 index.annotation_sha256 = std::string(64, 'a');
 index.split = "train2017";
 index.images.push_back({.source_image_id = image_id, .box_count = static_cast<std::uint32_t>(masks.size()), .width = 3, .height = 3});
 for (std::size_t i = 0; i < masks.size(); ++i) {
  NormalizedBox box;
  box.x1 = 0.1F;
  box.y1 = 0.2F;
  box.x2 = 0.8F;
  box.y2 = 0.9F;
  box.mask_rle_offset = i;
  box.mask_rle_pairs = 1;
  box.class_id = 16;
  box.flags = kAnnotationMask | kAnnotationId | kAnnotationCategory;
  box.annotation_id = 200 - i * 100;
  box.source_category_id = 18;
  box.original_area = 42 + static_cast<double>(i);
  index.boxes.push_back(box);
  index.mask_rle_pairs.push_back(masks[i]);
 }
 return index;
}
CoconutSegmentSupport recovery_support(std::span<const RLEPair> runs, dataset::MaskDimensions dimensions = {3, 3}) {
 CoconutSegmentSupport support;
 support.runs.assign(runs.begin(), runs.end());
 support.bounds = dataset::row_major_mask_bounds(runs, dimensions);
 for (const auto run : runs) support.area += run.length;
 return support;
}
}  // namespace
TEST_CASE("COCONut release selection and fixed catalog are pinned", "[coconut]") {
 CHECK(BenchmarkDatasetSelection{}.dataset == BenchmarkDatasetVariant::CocoCustom);
 CHECK(BenchmarkDatasetSelection{}.validation == CoconutValidation::Coconut);
 CHECK_FALSE(BenchmarkDatasetSelection{}.recover_dropped_masks);
 CHECK_FALSE(valid_benchmark_selection({static_cast<BenchmarkDatasetVariant>(4), CoconutValidation::Coconut}));
 CHECK_FALSE(valid_benchmark_selection({BenchmarkDatasetVariant::Coconut, static_cast<CoconutValidation>(4)}));
 REQUIRE(coconut_release_catalog().size() == 5);
 const auto& base = coconut_release_component(CoconutEdition::Base);
 CHECK(base.expected_rows == 241602);
 REQUIRE(base.annotations.size() == 4);
 CHECK(base.annotations[0].expected_size == 417218921);
 CHECK(base.annotations[0].expected_sha256 == "cfddd65e62ffeafe32229a43f06c475facfb039d2107be02b380134cb4be38ae");
 CHECK(coconut_release_component(CoconutEdition::RelabeledValidation).expected_rows == 5000);
 CHECK(coconut_objects_training_artifacts(CoconutEdition::Large).size() == 4);
 CHECK(coconut_objects_training_artifacts(CoconutEdition::XLarge).size() == 7);
 CHECK(coconut_objects_training_artifacts(CoconutEdition::Large)[0].artifact_id == objects365_train_image_artifacts()[32].artifact_id);
 CHECK(coconut_unlabeled_images_artifact().expected_size == 20126613414ULL);
 CHECK(coconut_validation_images_artifact().expected_size == 5084467200ULL);
}
TEST_CASE("COCONut Parquet shards preserve exact holes singleton crowd null area and empty images", "[coconut]") {
 ScopedTempDir root("coconut-parquet");
 const std::array<std::uint32_t, 9> ids{1, 1, 1, 1, 2, 1, 1, 0, 1};
 auto first = segment();
 first["iscrowd"] = 1;
 auto second = segment(2, 2);
 second["area"] = 17;
 const auto mask = png(3, 3, ids);
 auto row = hf_row(7, mask, Json::array({first, second}));
 const std::array<std::uint32_t, 9> void_ids{};
 auto empty = hf_row(8, png(3, 3, void_ids), Json::array());
 auto nonthing = segment(1, 1, false);
 nonthing["area"] = 0;
 const std::array<std::uint32_t, 9> filled{1, 1, 1, 1, 1, 1, 1, 1, 1};
 parquet_file(root.path() / "a.parquet", Json::array({row, empty}));
 parquet_file(root.path() / "b.parquet", Json::array({hf_row(9, png(3, 3, filled), Json::array({nonthing}))}), hf_schema(true), parquet::Compression::ZSTD);
 const std::array physical{coco(7), coco(8, CoconutImageNamespace::CocoUnlabeled), coco(9)};
 auto input = request(physical);
 input.parquet_shards = {root.path() / "a.parquet", root.path() / "b.parquet"};
 input.expected_rows = 3;
 auto components = import_coconut_annotations(input);
 REQUIRE(components.size() == 2);
 const auto& train = components[0];
 REQUIRE(train.index().image_count() == 2);
 REQUIRE(train.index().box_count() == 2);
 CHECK(train.index().images()[0].source_image_id == 7);
 CHECK(train.index().images()[1].source_image_id == 9);
 CHECK(train.index().images()[1].box_count == 0);
 const auto& box = train.index().storage().boxes[0];
 CHECK(box.original_area == 7);
 CHECK(box.x1 == 0.0F);
 CHECK(box.y1 == 0.0F);
 CHECK(box.x2 == 1.0F);
 CHECK(box.y2 == 1.0F);
 CHECK((box.flags & kAnnotationCrowd) != 0);
 CHECK(box.annotation_id == 1);
 CHECK(box.source_ordinal == 0);
 CHECK(box.class_id == 0);
 const auto& singleton = train.index().storage().boxes[1];
 CHECK(singleton.x1 == 1.0F / 3.0F);
 CHECK(singleton.y1 == 1.0F / 3.0F);
 CHECK(singleton.x2 == 2.0F / 3.0F);
 CHECK(singleton.y2 == 2.0F / 3.0F);
 CHECK(singleton.original_area == 17);
 CHECK(singleton.source_ordinal == 1);
 CHECK(singleton.source_category_id == 2);
 const std::array<RLEPair, 4> runs{{{0, 4}, {5, 2}, {8, 1}, {4, 1}}};
 expect_runs(train, runs);
 CHECK(components[1].source() == CoconutImageNamespace::CocoUnlabeled);
 REQUIRE(components[1].index().image_count() == 1);
 CHECK(components[1].index().images()[0].source_image_id == 8);
 CHECK(components[1].index().box_count() == 0);
 const auto cache = root.path() / "index.bin";
 store_coconut_component(cache, train);
 auto loaded = load_coconut_component(cache, train.edition(), train.source(), input.input_identity);
 REQUIRE(loaded);
 CHECK(std::ranges::equal(loaded->inventory(), train.inventory()));
 expect_runs(*loaded, runs);
 CHECK_FALSE(load_coconut_component(cache, train.edition(), train.source(), "another-edition"));
 auto manifest = read_json_file(cache.string() + ".complete.json");
 manifest["coconut"]["inventory_identity"] = std::string(64, '0');
 json_file(cache.string() + ".complete.json", manifest);
 CHECK_FALSE(load_coconut_component(cache, train.edition(), train.source(), input.input_identity));
}
TEST_CASE("COCONut Parquet rejects malformed nested records missing membership and bounded overflows", "[coconut]") {
 ScopedTempDir root("coconut-malformed");
 const std::array<std::uint32_t, 9> ids{1, 1, 1, 1, 1, 1, 1, 1, 1};
 auto row = hf_row(7, png(3, 3, ids), Json::array({segment()}));
 const std::array physical{coco(7)};
 auto input = request(physical);
 input.parquet_shards = {root.path() / "a.parquet"};
 input.expected_rows = 1;
 SECTION("null image metadata") { row["image_info"] = nullptr; }
 SECTION("null required segment ID") { row["segments_info"]["segments_info"][0]["id"] = nullptr; }
 SECTION("mismatched release IDs") { row["segments_info"]["image_id"] = 8; }
 SECTION("unknown thing category") { row["segments_info"]["segments_info"][0]["category_id"] = 12; }
 SECTION("contradictory dimensions") { row["image_info"]["width"] = 4; }
 SECTION("missing embedded bytes") { row["mask"]["bytes"] = nullptr; }
 SECTION("PNG admission") { input.limits.max_png_bytes = 1; }
 SECTION("pixel admission") { input.limits.max_pixels = 8; }
 SECTION("segment admission") { input.limits.max_segments = 0; }
 SECTION("release completeness") { input.expected_rows = 2; }
 SECTION("missing physical member") {
  input.membership = std::make_unique<CoconutPhysicalMembership>(std::span<const CoconutPhysicalImage>{});
  input.physical_membership = input.membership.get();
 }
 SECTION("undeclared void-independent RGB ID") { row["segments_info"]["segments_info"][0]["id"] = 2; }
 SECTION("duplicate segments") { row["segments_info"]["segments_info"].push_back(segment()); }
 parquet_file(input.parquet_shards[0], Json::array({row}));
 CHECK_THROWS(import_coconut_annotations(input));
}
TEST_CASE("COCONut Parquet cancellation and nested type validation occur before normalization", "[coconut]") {
 ScopedTempDir root("coconut-parquet-cancel");
 const std::array<std::uint32_t, 1> ids{1};
 auto row = hf_row(7, png(1, 1, ids), Json::array({segment()}), 1, 1);
 const std::array physical{coco(7)};
 auto input = request(physical);
 input.parquet_shards = {root.path() / "a.parquet"};
 SECTION("wrong nested type") {
  auto schema = hf_schema();
  auto fields = schema->fields();
  fields[0] = arrow::field("mask", arrow::utf8());
  row["mask"] = "wrong";
  parquet_file(input.parquet_shards[0], Json::array({row}), arrow::schema(fields));
  CHECK_THROWS(import_coconut_annotations(input));
 }
 SECTION("between records") {
  parquet_file(input.parquet_shards[0], Json::array({row, hf_row(8, png(1, 1, ids), Json::array({segment()}), 1, 1)}));
  std::atomic<bool> stop{false};
  input.cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(stop);
  unsigned observed = 0;
  input.progress = [&](std::uint64_t) {
   ++observed;
   stop = true;
  };
  CHECK_THROWS(import_coconut_annotations(input));
  CHECK(observed == 1);
 }
}
TEST_CASE("COCONut JSON joins retain heterogeneous Large rows and validation physical namespaces", "[coconut]") {
 ScopedTempDir root("coconut-json");
 const std::array<std::uint32_t, 4> ids{1, 0, 0, 1};
 const auto mask = png(2, 2, ids);
 const std::array physical{objects(91105, CoconutImageNamespace::Objects365V1), objects(91105)};
 auto input = request(physical, CoconutEdition::ObjectsValidation);
 input.annotation_json = root.path() / "val.json";
 input.mask_archive = root.path() / "val.tar";
 input.expected_rows = 2;
 auto supported = segment();
 supported["ignore"] = 1;
 supported["bbox"] = Json::array({0.25, 0.5, 1.5, 1.0});
 Json document{
  {"images", Json::array({{{"id", 691105}, {"file_name", "691105.jpg"}, {"width", 2}, {"height", 2}}, {{"id", 91105}, {"file_name", "objects365_v2_00091105.png"}, {"width", 2}, {"height", 2}}})},
  {"annotations", Json::array(
                   {{{"image_id", 691105}, {"file_name", "691105.png"}, {"object365_file_name", "objects365_v1_00091105"}, {"segments_info", Json::array({supported})}},
                    {{"image_id", 91105}, {"file_name", "objects365_v2_00091105.png"}, {"segments_info", Json::array({segment()})}}})}
 };
 json_file(input.annotation_json, document);
 // Deliberately reversed archive order.
 const std::array members{
  std::pair{"panoptic_o365val_v3/objects365_v2_00091105.png", mask}, std::pair{"panoptic_o365val_v3/objects365_v1_00091105.png", mask}, std::pair{".DS_Store", std::string("unrelated")}
 };
 std::vector<std::pair<std::string, std::string>> owned;
 for (const auto& [name, bytes] : members) owned.emplace_back(name, bytes);
 tar(input.mask_archive, owned);
 auto components = import_coconut_annotations(input);
 REQUIRE(components.size() == 2);
 CHECK(components[0].source() == CoconutImageNamespace::Objects365V1);
 CHECK(components[1].source() == CoconutImageNamespace::Objects365V2);
 for (const auto& component : components) CHECK(component.index().images()[0].source_image_id == 91105);
 CHECK(components[0].inventory()[0].release_image_id == 691105);
 CHECK(components[0].inventory()[0].physical.member == "image/objects365_v1_00091105.jpg");
 const auto& box = components[0].index().storage().boxes[0];
 CHECK(box.x1 == 0.125F);
 CHECK(box.y1 == 0.25F);
 CHECK(box.x2 == 0.875F);
 CHECK(box.y2 == 0.75F);
 CHECK((box.flags & kAnnotationIgnore) != 0);
 CHECK(box.original_area == 2);
 const std::array<RLEPair, 2> runs{{{0, 1}, {3, 1}}};
 expect_runs(components[0], runs);
 CHECK(components[0].index().storage().boxes[0].source_ordinal == 0);
 CHECK(components[1].index().storage().boxes[components[1].index().image(0).first_box].source_ordinal == 1);
 const auto cache = root.path() / "val.bin";
 store_coconut_component(cache, components[0]);
 auto loaded = load_coconut_component(cache, input.edition, CoconutImageNamespace::Objects365V1, input.input_identity);
 REQUIRE(loaded);
 CHECK(loaded->inventory()[0].release_image_id == 691105);
}
TEST_CASE("COCONut Large shapes and sorted XL masks retain complete rows with Large precedence", "[coconut]") {
 ScopedTempDir root("coconut-extensions");
 const std::array<std::uint32_t, 1> ids{1};
 const auto mask = png(1, 1, ids);
 const std::array physical{objects(1), objects(2), objects(3)};
 auto input = request(physical, CoconutEdition::Large);
 input.expected_rows = 2;
 input.annotation_json = root.path() / "large.json";
 input.mask_archive = root.path() / "large.tar";
 json_file(input.annotation_json, {{"images", Json::array(
                                               {{{"id", 900}, {"file_name", "900.jpg"}, {"object365_name", "objects365_v2_00000002"}, {"width", 1}, {"height", 1}},
                                                {{"id", 1}, {"file_name", "objects365_v2_00000001.png"}, {"width", 1}, {"height", 1}}})},
                                   {"annotations", Json::array(
                                                    {{{"image_id", 900}, {"file_name", "900.png"}, {"segments_info", Json::array({segment()})}},
                                                     {{"image_id", 1}, {"file_name", "objects365_v2_00000001.png"}, {"segments_info", Json::array({segment()})}}})}});
 const std::array<std::pair<std::string, std::string>, 2> members{{{"panoptic_object365/objects365_v2_00000001.png", mask}, {"panoptic_object365/objects365_v2_00000002.png", mask}}};
 tar(input.mask_archive, members);
 auto components = import_coconut_annotations(input);
 REQUIRE(components.size() == 1);
 REQUIRE(components[0].index().image_count() == 2);
 CHECK(components[0].inventory()[1].release_image_id == 900);
 CHECK(components[0].index().storage().boxes[0].source_ordinal == 1);
 CHECK(components[0].index().storage().boxes[1].source_ordinal == 0);
 input.edition = CoconutEdition::XLarge;
 input.annotation_json.clear();
 input.mask_archive = root.path() / "xl.tar";
 const std::array<std::pair<std::string, std::string>, 4> xl_members{
  {{"coconuts_xlarge/panseg/objects365_v2_00000003.png", mask}, {"coconuts_xlarge/panseg_info/objects365_v2_00000003.json", Json::array({segment()}).dump()},
   {"coconuts_xlarge/panseg_info/objects365_v2_00000002.json", Json::array({segment()}).dump()}, {"coconuts_xlarge/panseg/objects365_v2_00000002.png", mask}}
 };
 tar(input.mask_archive, xl_members);
 auto xl = import_coconut_annotations(input);
 REQUIRE(xl.size() == 1);
 REQUIRE(xl[0].index().image_count() == 2);
 CHECK(xl[0].index().images()[0].source_image_id == 2);
 CHECK(xl[0].index().images()[0].width == 1);
 CHECK(xl[0].index().images()[0].height == 1);
 CHECK(xl[0].index().storage().boxes[0].source_ordinal == 0);
 CHECK(xl[0].index().storage().boxes[1].source_ordinal == 1);
 components.push_back(std::move(xl[0]));
 const auto full_path = root.path() / "full-xl.bin";
 store_coconut_component(full_path, components[1]);
 const auto full_component = load_coconut_component(full_path, components[1].edition(), components[1].source(), components[1].input_identity());
 REQUIRE(full_component);
 components[1] = *full_component;
 const auto original = components;
 const auto* retained_boxes = components[1].index().storage().boxes.data();
 const auto* retained_runs = components[1].index().storage().mask_rle_pairs.data();
 const auto* retained_inventory = &components[1].inventory_image(1);
 const auto full_box_count = components[1].index().storage().boxes.size(), full_run_count = components[1].index().storage().mask_rle_pairs.size();
 const auto previous_identity = components[1].index().annotation_sha256;
 PollCancellation observed;
 CHECK(reconcile_coconut_extensions(components, mmltk::common::concurrency::CancellationObservation::Borrow(observed)) == 1);
 for (std::size_t cut = 0; cut < observed.polls; ++cut) {
  auto interrupted = original;
  PollCancellation stop;
  stop.stop_at = cut;
  CHECK_THROWS(reconcile_coconut_extensions(interrupted, mmltk::common::concurrency::CancellationObservation::Borrow(stop)));
  CHECK(interrupted[1].index().annotation_sha256 == previous_identity);
  CHECK(&interrupted[1].inventory_image(1) == retained_inventory);
 }
 CHECK(components[1].index().storage().boxes.data() == retained_boxes);
 CHECK(components[1].index().storage().mask_rle_pairs.data() == retained_runs);
 CHECK(&components[1].inventory_image(0) == retained_inventory);
 CHECK(components[1].index().storage().boxes.size() == full_box_count);
 CHECK(components[1].index().storage().mask_rle_pairs.size() == full_run_count);
 CHECK(components[1].index().annotation_sha256 != previous_identity);
 CHECK(components[1].inventory()[0] == original[1].inventory()[1]);
 CHECK(components[1].index().storage().mask_rle_pairs[0].start == 0);
 CHECK(components[1].index().storage().mask_rle_pairs[0].length == 1);
 REQUIRE(components[1].index().image_count() == 1);
 CHECK(components[1].index().images()[0].source_image_id == 3);
 CHECK(components[1].index().storage().boxes[components[1].index().image(0).first_box].source_ordinal == 1);
 CHECK(components[1].index().images()[0].first_box == original[1].index().image(1).first_box);
 CHECK(components[1].index().storage().boxes[components[1].index().image(0).first_box].mask_rle_offset == original[1].index().storage().boxes[original[1].index().image(1).first_box].mask_rle_offset);
 CHECK(reconcile_coconut_extensions(components) == 0);
 CHECK(components[1].index().storage().boxes.data() == retained_boxes);
 CHECK(components[1].index().box_count() == 1);
 CHECK(components[1].index().run_count() == 1);
 const auto selected_path = root.path() / "selected.bin";
 store_coconut_component(selected_path, components[1]);
 const auto persisted_selection = load_coconut_component(selected_path, components[1].edition(), components[1].source(), components[1].input_identity());
 REQUIRE(persisted_selection);
 CHECK(persisted_selection->index().image_count() == 1);
 CHECK(persisted_selection->index().image(0).first_box == 0);
 CHECK(persisted_selection->index().storage().boxes[0].mask_rle_offset == 0);
 CHECK(persisted_selection->index().annotation_sha256 == components[1].index().annotation_sha256);
 CHECK(components[1].completion() == original[1].completion());
 CHECK(components[1].index().storage().completion == original[1].index().storage().completion);
 CHECK(components[1].completion()->images == 2);
 REQUIRE(persisted_selection->completion());
 CHECK(persisted_selection->completion()->images == 1);
 CHECK(persisted_selection->completion()->identity != components[1].completion()->identity);
 CHECK(components[1].index().storage().boxes.data() == retained_boxes);
 auto covered = original;
 auto covering = component_builder(original[1]);
 covering.edition = CoconutEdition::Large;
 covering.index.split = "coconut-" + std::to_string(static_cast<unsigned>(covering.edition)) + "-" + std::to_string(static_cast<unsigned>(covering.source));
 covering.index.annotation_sha256.clear();
 covered[0] = std::move(covering).finish({}, root.path());
 CHECK(reconcile_coconut_extensions(covered) == 2);
 CHECK(covered[1].inventory().empty());
 CHECK(covered[1].index().image_count() == 0);
 CHECK(covered[1].index().box_count() == 0);
 CHECK(covered[1].index().run_count() == 0);
 CHECK(covered[0].inventory().size() == 2);
}
TEST_CASE("COCONut archives reject unresolved duplicate extra and unsafe offered members", "[coconut]") {
 ScopedTempDir root("coconut-archive-reject");
 const std::array<std::uint32_t, 1> ids{1};
 const auto mask = png(1, 1, ids);
 const std::array physical{objects(1)};
 auto input = request(physical, CoconutEdition::Large);
 input.annotation_json = root.path() / "large.json";
 input.mask_archive = root.path() / "large.tar";
 Json annotation{{"image_id", 1}, {"file_name", "objects365_v2_00000001.png"}, {"segments_info", Json::array({segment()})}};
 Json document{{"images", Json::array()}, {"annotations", Json::array({annotation})}};
 std::vector<std::pair<std::string, std::string>> members{{"panoptic_object365/objects365_v2_00000001.png", mask}};
 bool link = false;
 SECTION("missing mask") { members.clear(); }
 SECTION("extra mask") { members.emplace_back("panoptic_object365/objects365_v2_00000002.png", mask); }
 SECTION("duplicate mask") { members.push_back(members.front()); }
 SECTION("traversal") { members.emplace_back("../outside", "x"); }
 SECTION("symlink") { link = true; }
 SECTION("duplicate offered annotation") { document["annotations"].push_back(annotation); }
 SECTION("unknown physical namespace") { document["annotations"][0]["file_name"] = "00000001.png"; }
 SECTION("contradictory physical names") { document["annotations"][0]["object365_file_name"] = "objects365_v2_00000002"; }
 SECTION("unknown PNG segment") {
  const std::array<std::uint32_t, 1> unknown{2};
  members[0].second = png(1, 1, unknown);
 }
 json_file(input.annotation_json, document);
 tar(input.mask_archive, members, link);
 CHECK_THROWS(import_coconut_annotations(input));
}
TEST_CASE("COCONut version-1 physical inventory has fixed bytes and admits existing caches", "[coconut]") {
 ScopedTempDir root("coconut-inventory-v1");
 static constexpr char expected_bytes[] =
  "\x43\x4e\x55\x54\x49\x56\x4e\x31"                                                                                  // magic
  "\x01\x00\x00\x00"                                                                                                  // version 1
  "\x02\x00\x00\x00"                                                                                                  // cache schema 2
  "\x18\x00\x00\x00\x63\x6f\x63\x6f\x6e\x75\x74\x2d\x65\x78\x61\x63\x74\x2d\x72\x67\x62\x2d\x72\x6c\x65\x2d\x76\x31"  // normalization
  "\x01\x00\x00\x00\x61"                                                                                              // input identity
  "\x00\x03\x34\x12\x00"                                                                                              // Base, Objects365V1, shard 0x1234, physical
  "\x01\x00\x00\x00\x00\x00\x00\x00"                                                                                  // one record
  "\x03\xe1\x63\x01\x00\x00\x00\x00\x00\x34\x12"                                                                      // namespace, physical ID 91105, shard
  "\x20\x00\x00\x00\x69\x6d\x61\x67\x65\x2f\x6f\x62\x6a\x65\x63\x74\x73\x33\x36\x35\x5f\x76\x31\x5f\x30\x30\x30\x39\x31\x31\x30\x35\x2e\x6a\x70"
  "\x67"                                                                                                                               // physical member
  "\x01\x00\x00\x00\x61"                                                                                                               // archive identity
  "\x93\xe4\x7a\xfa\x75\xda\x69\x5b\x47\xe8\xc5\x40\x1f\xb2\x30\xea\xde\x72\x66\xf7\x98\xbc\xea\x05\x7b\x52\x8f\xbd\x0b\x94\x5e\x17";  // SHA-256
                                                                                                                                       // footer
                                                                                                                                       // at byte
                                                                                                                                       // 114
 const std::string expected(expected_bytes, sizeof(expected_bytes) - 1);
 const auto archive = root.path() / "images.tar";
 const auto cache = root.path() / "inventory.bin";
 const std::vector<CoconutPhysicalImage> wanted{{CoconutImageNamespace::Objects365V1, 91105, 0x1234, "image/objects365_v1_00091105.jpg", "a"}};
 // A preexisting v1 inventory must load without its source archive.
 mmltk::testsupport::write_text_file(cache, expected);
 CHECK(coconut_image_archive_inventory(archive, cache, CoconutImageNamespace::Objects365V1, 0x1234, "a") == wanted);
 CHECK_FALSE(std::filesystem::exists(archive));
 const std::array<std::pair<std::string, std::string>, 3> members{{{".", ""}, {"./", ""}, {"./image//./objects365_v1_00091105.jpg", "jpeg"}}};
 tar(archive, members);
 const auto emitted = root.path() / "emitted.bin";
 StorageReservationPool storage(root.path(), {});
 CHECK(coconut_image_archive_inventory(archive, emitted, CoconutImageNamespace::Objects365V1, 0x1234, "a", {}, &storage) == wanted);
 CHECK(storage.outstanding() == 0);
 struct stat status{};
 REQUIRE(::stat(emitted.c_str(), &status) == 0);
 CHECK((status.st_mode & 0777) == 0644);
 CHECK(file_bytes(emitted) == expected);
 CHECK(expected.size() == 114 + 32);
 CHECK(file_bytes(cache) == expected);
}
TEST_CASE("COCONut version-1 component inventory pins nested physical release and ordinal identities", "[coconut]") {
 ScopedTempDir root("coconut-component-v1");
 static constexpr char expected_bytes[] =
  "\x43\x4e\x55\x54\x49\x56\x4e\x31"                                                                                  // magic
  "\x01\x00\x00\x00"                                                                                                  // version 1
  "\x02\x00\x00\x00"                                                                                                  // cache schema 2
  "\x18\x00\x00\x00\x63\x6f\x63\x6f\x6e\x75\x74\x2d\x65\x78\x61\x63\x74\x2d\x72\x67\x62\x2d\x72\x6c\x65\x2d\x76\x31"  // normalization
  "\x01\x00\x00\x00\x69"                                                                                              // input identity
  "\x04\x03\x00\x00\x01"                                                                                              // ObjectsValidation, Objects365V1, header shard 0, component
  "\x01\x00\x00\x00\x00\x00\x00\x00"                                                                                  // one record
  "\x03\xe1\x63\x01\x00\x00\x00\x00\x00\x34\x12"                                                                      // namespace, physical ID 91105, shard
  "\x20\x00\x00\x00\x69\x6d\x61\x67\x65\x2f\x6f\x62\x6a\x65\x63\x74\x73\x33\x36\x35\x5f\x76\x31\x5f\x30\x30\x30\x39\x31\x31\x30\x35\x2e\x6a\x70"
  "\x67"                                                                                                                               // physical member
  "\x01\x00\x00\x00\x61"                                                                                                               // archive identity
  "\xa1\x8b\x0a\x00\x00\x00\x00\x00"                                                                                                   // declared release ID 691105
  "\x02\x00\x00\x00\x00\x00\x00\x00"                                                                                                   // source ordinal 2
  "\xc5\x30\x4c\xa7\xa1\x6f\x7e\x76\xd6\xc2\x1b\xfd\x7b\xda\x2b\x39\xa0\x25\xcc\x26\x8d\xde\x23\x36\xd9\x5a\x21\x57\xeb\x9d\x46\x32";  // SHA-256
                                                                                                                                       // footer
                                                                                                                                       // at byte
                                                                                                                                       // 130
 const std::string expected(expected_bytes, sizeof(expected_bytes) - 1);
 CoconutComponentBuilder builder;
 builder.edition = CoconutEdition::ObjectsValidation;
 builder.source = CoconutImageNamespace::Objects365V1;
 builder.input_identity = "i";
 builder.inventory = {{{CoconutImageNamespace::Objects365V1, 91105, 0x1234, "image/objects365_v1_00091105.jpg", "a"}, 691105, 2}};
 builder.index.source = BenchmarkDatasetSource::kObjects365V2;
 builder.index.split = "coconut-4-3";
 builder.index.annotation_sha256 = "c5304ca7a16f7e76d6c21bfd7bda2b39a025cc268dde2336d95a2157eb9d4632";
 builder.index.images.push_back({.source_image_id = 91105, .width = 1, .height = 1, .source_shard = 0x1234});
 const auto component = std::move(builder).finish({}, root.path());
 const auto path = root.path() / "component.bin";
 StorageReservationPool storage(root.path(), {});
 store_coconut_component(path, component, {}, &storage);
 CHECK(storage.outstanding() == 0);
 const auto index_bytes = file_bytes(path);
 const auto inventory_path = path.string() + ".inventory";
 CHECK(file_bytes(inventory_path) == expected);
 CHECK(expected.size() == 130 + 32);
 // Substitute independent v1 bytes, retaining the index's joint completion.
 mmltk::testsupport::write_text_file(inventory_path, expected);
 const auto loaded = load_coconut_component(path, component.edition(), component.source(), "i");
 REQUIRE(loaded);
 CHECK(loaded->index().annotation_sha256 == component.index().annotation_sha256);
 CHECK(std::ranges::equal(loaded->inventory(), component.inventory()));
 REQUIRE(loaded->index().image_count() == 1);
 CHECK(loaded->index().images()[0].source_image_id == 91105);
 CHECK(loaded->index().box_count() == 0);
 CHECK_FALSE(load_coconut_component(path, component.edition(), component.source(), "changed"));
 store_coconut_component(path, *loaded, {}, &storage);
 CHECK(storage.outstanding() == 0);
 CHECK(file_bytes(inventory_path) == expected);
 CHECK(file_bytes(path) == index_bytes);
}
TEST_CASE("COCONut physical member spelling preserves exact paths and rejects unsafe names", "[coconut]") {
 CHECK(canonical_benchmark_archive_member("././train2017//./000000000007.jpg") == "train2017/000000000007.jpg");
 CHECK(canonical_benchmark_archive_member("nested/./image//objects365_v1_00091105.jpg") == "nested/image/objects365_v1_00091105.jpg");
 for (const std::string& raw : {std::string("/train2017/a.jpg"), std::string("../a.jpg"), std::string("train2017/../a.jpg"), std::string("train2017\\a.jpg"), std::string("train2017/a\0.jpg", 16)}) {
  CHECK_THROWS(canonical_benchmark_archive_member(raw));
 }
}
TEST_CASE("COCONut full physical inventories are identity-bound and independent of foreground labels", "[coconut]") {
 ScopedTempDir root("coconut-inventory");
 const auto archive = root.path() / "images.tar", cache = root.path() / "inventory.json";
 const std::array<std::pair<std::string, std::string>, 3> members{{{"unlabeled2017/000000000007.jpg", "jpeg7"}, {"unlabeled2017/000000000008.jpg", "jpeg8"}, {".DS_Store", "unrelated"}}};
 tar(archive, members);
 auto result = coconut_image_archive_inventory(archive, cache, CoconutImageNamespace::CocoUnlabeled, 0, "pinned-archive");
 REQUIRE(result.size() == 2);
 CHECK(result[0].image_id == 7);
 CHECK(result[1].image_id == 8);
 std::filesystem::remove(archive);
 CHECK(coconut_image_archive_inventory(archive, cache, CoconutImageNamespace::CocoUnlabeled, 0, "pinned-archive") == result);
 CHECK_THROWS(coconut_image_archive_inventory(archive, cache, CoconutImageNamespace::CocoUnlabeled, 0, "changed-archive"));
 const std::array<std::pair<std::string, std::string>, 2> duplicate{{members[0], {"./unlabeled2017//./000000000007.jpg", "jpeg7"}}};
 tar(archive, duplicate);
 CHECK_THROWS(coconut_image_archive_inventory(archive, {}, CoconutImageNamespace::CocoUnlabeled, 0, "duplicate-archive"));
}
TEST_CASE("COCONut preserves RGB24 IDs across compressed Parquet batches and relabeled integer areas", "[coconut]") {
 ScopedTempDir root("coconut-rgb24");
 const std::array<std::uint32_t, 1> ids{0x030201};
 auto seg = segment(0x030201, 90);
 seg["area"] = 1;
 Json rows = Json::array();
 std::vector<CoconutPhysicalImage> physical;
 for (unsigned i = 1; i <= 18; ++i) {
  rows.push_back(hf_row(i, png(1, 1, ids), Json::array({seg}), 1, 1));
  physical.push_back(coco(i, CoconutImageNamespace::CocoValidation));
 }
 auto input = request(physical, CoconutEdition::RelabeledValidation);
 input.parquet_shards = {root.path() / "val.parquet"};
 input.expected_rows = 18;
 parquet_file(input.parquet_shards[0], rows, hf_schema(true), parquet::Compression::GZIP);
 auto components = import_coconut_annotations(input);
 REQUIRE(components.size() == 1);
 REQUIRE(components[0].index().image_count() == 18);
 REQUIRE(components[0].index().box_count() == 18);
 for (std::size_t i = 0; i < 18; ++i) {
  CHECK(components[0].index().storage().boxes[i].annotation_id == 0x030201);
  CHECK(components[0].index().storage().boxes[i].class_id == 79);
  CHECK(components[0].index().storage().boxes[i].source_ordinal == i);
  CHECK(components[0].index().storage().mask_rle_pairs[i].start == 0);
  CHECK(components[0].index().storage().mask_rle_pairs[i].length == 1);
 }
}
TEST_CASE("COCONut native selection admission precedes cache mutation", "[coconut]") {
 ScopedTempDir root("coconut-selection-admission");
 BenchmarkCompilerConfig config;
 config.cache_dir = root.path() / "new-cache";
 config.output_dir = root.path() / "new-output";
 config.selection.dataset = static_cast<BenchmarkDatasetVariant>(255);
 CHECK_THROWS_AS(compile_benchmark_dataset(config), std::invalid_argument);
 CHECK_FALSE(std::filesystem::exists(config.cache_dir));
 CHECK_FALSE(std::filesystem::exists(config.output_dir));
}
TEST_CASE("COCONut duplicate and ambiguous B membership never silently loses rows", "[coconut]") {
 ScopedTempDir root("coconut-base-membership");
 const std::array<std::uint32_t, 1> ids{1};
 auto row = hf_row(7, png(1, 1, ids), Json::array({segment()}), 1, 1);
 std::vector<CoconutPhysicalImage> physical{coco(7)};
 Json rows = Json::array({row});
 SECTION("duplicate rows") { rows.push_back(row); }
 SECTION("ambiguous physical subsets") { physical.push_back(coco(7, CoconutImageNamespace::CocoUnlabeled)); }
 SECTION("duplicate physical members") {
  physical.push_back(physical[0]);
  CHECK_THROWS(request(physical));
  return;
 }
 auto input = request(physical);
 input.parquet_shards = {root.path() / "b.parquet"};
 parquet_file(input.parquet_shards[0], rows);
 CHECK_THROWS(import_coconut_annotations(input));
}
TEST_CASE("COCONut authoritative box retains an empty supplied mask and rejects coordinate overflow", "[coconut]") {
 ScopedTempDir root("coconut-authoritative-box");
 const std::array<std::uint32_t, 1> ids{0};
 const std::array physical{objects(1)};
 auto input = request(physical, CoconutEdition::Large);
 input.annotation_json = root.path() / "large.json";
 input.mask_archive = root.path() / "large.tar";
 auto seg = segment();
 seg["bbox"] = Json::array({-1, 0, 3, 1});
 seg["area"] = 23;
 bool overflow = false;
 SECTION("outside-image authoritative bounds") {}
 SECTION("unrepresentable bounds") {
  seg["bbox"] = Json::array({1e30, 0, 1, 1});
  overflow = true;
 }
 json_file(input.annotation_json, {{"images", Json::array()}, {"annotations", Json::array({{{"image_id", 1}, {"file_name", "objects365_v2_00000001.png"}, {"segments_info", Json::array({seg})}}})}});
 const std::array<std::pair<std::string, std::string>, 1> members{{{"panoptic_object365/objects365_v2_00000001.png", png(1, 1, ids)}}};
 tar(input.mask_archive, members);
 if (overflow) {
  CHECK_THROWS(import_coconut_annotations(input));
  return;
 }
 const auto components = import_coconut_annotations(input);
 REQUIRE(components.size() == 1);
 REQUIRE(components[0].index().box_count() == 1);
 const auto& box = components[0].index().storage().boxes[0];
 CHECK(box.x1 == -1.0F);
 CHECK(box.x2 == 2.0F);
 CHECK(box.original_area == 23);
 CHECK((box.flags & kAnnotationMask) != 0);
 CHECK(box.mask_rle_pairs == 0);
}
TEST_CASE("COCONut JSON requires canonical categories and exact validation image joins", "[coconut]") {
 ScopedTempDir root("coconut-semantic-joins");
 const std::array physical{objects(1, CoconutImageNamespace::Objects365V1), objects(2, CoconutImageNamespace::Objects365V1)};
 auto input = request(physical, CoconutEdition::ObjectsValidation);
 input.annotation_json = root.path() / "val.json";
 input.mask_archive = root.path() / "val.tar";
 Json annotation{{"image_id", 11}, {"file_name", "11.png"}, {"object365_file_name", "objects365_v1_00000001"}, {"segments_info", Json::array({segment()})}};
 Json document{{"categories", category_catalog()}, {"images", Json::array({{{"id", 11}, {"file_name", "11.png"}, {"width", 1}, {"height", 1}}})}, {"annotations", Json::array({annotation})}};
 bool valid = false;
 SECTION("canonical names and record-level thing admission") { valid = true; }
 SECTION("mismatched mapped name") { document["categories"][0]["name"] = "car"; }
 SECTION("missing mapped category") { document["categories"].erase(0); }
 SECTION("duplicate mapped category") { document["categories"].push_back(document["categories"][0]); }
 SECTION("malformed mapped category") { document["categories"][0]["id"] = "1"; }
 SECTION("orphan validation annotation") { document["images"] = Json::array(); }
 SECTION("two physical members join one image row") {
  annotation["object365_file_name"] = "objects365_v1_00000002";
  document["annotations"].push_back(annotation);
 }
 json_file(input.annotation_json, document);
 const std::array<std::uint32_t, 1> ids{1};
 const std::array<std::pair<std::string, std::string>, 1> members{{{"panoptic_o365val_v3/objects365_v1_00000001.png", png(1, 1, ids)}}};
 // CLEANUP-IGNORE: These independent malformed-input matrices assert admission around the shared importer; their valid records and expected category mappings differ.
 tar(input.mask_archive, members);
 if (!valid) {
  CHECK_THROWS(import_coconut_annotations(input));
  return;
 }
 const auto components = import_coconut_annotations(input);
 REQUIRE(components.size() == 1);
 REQUIRE(components[0].index().box_count() == 1);
 CHECK(components[0].index().storage().boxes[0].class_id == 0);
 CHECK(components[0].index().storage().boxes[0].source_category_id == 1);
 CHECK(components[0].inventory()[0].release_image_id == 11);
 CHECK(components[0].index().images()[0].source_image_id == 1);
}
TEST_CASE("COCONut preparatory arrays accept either order and reject duplicate arrays", "[coconut]") {
 ScopedTempDir root("coconut-envelope-order");
 const std::array physical{objects(1, CoconutImageNamespace::Objects365V1)};
 auto input = request(physical, CoconutEdition::ObjectsValidation);
 input.annotation_json = root.path() / "val.json";
 input.mask_archive = root.path() / "val.tar";
 const auto categories = category_catalog().dump();
 const auto images = Json::array({{{"id", 11}, {"file_name", "11.png"}, {"width", 3}, {"height", 1}}}).dump();
 const auto annotations =
  Json::array({{{"image_id", 11}, {"file_name", "11.png"}, {"object365_file_name", "objects365_v1_00000001"}, {"segments_info", Json::array({segment(1, 1), segment(2, 13), segment(3, 90)})}}}).dump();
 std::string document;
 bool valid = true;
 SECTION("categories before images") { document = "{\"categories\":" + categories + ",\"images\":" + images + ",\"annotations\":" + annotations + "}"; }
 SECTION("annotations before images before categories") { document = "{\"annotations\":" + annotations + ",\"images\":" + images + ",\"categories\":" + categories + "}"; }
 SECTION("duplicate categories after images") {
  valid = false;
  document = "{\"categories\":" + categories + ",\"images\":" + images + ",\"categories\":" + categories + ",\"annotations\":" + annotations + "}";
 }
 SECTION("duplicate images after categories") {
  valid = false;
  document = "{\"images\":" + images + ",\"categories\":" + categories + ",\"images\":" + images + ",\"annotations\":" + annotations + "}";
 }
 mmltk::testsupport::write_text_file(input.annotation_json, document);
 const std::array<std::uint32_t, 3> ids{1, 2, 3};
 const std::array<std::pair<std::string, std::string>, 1> members{{{"panoptic_o365val_v3/objects365_v1_00000001.png", png(3, 1, ids)}}};
 tar(input.mask_archive, members);
 if (!valid) {
  CHECK_THROWS(import_coconut_annotations(input));
  return;
 }
 const auto components = import_coconut_annotations(input);
 REQUIRE(components.size() == 1);
 REQUIRE(components[0].index().box_count() == 3);
 const auto& boxes = components[0].index().storage().boxes;
 CHECK(boxes[0].source_category_id == 1);
 CHECK(boxes[0].class_id == 0);
 CHECK(boxes[1].source_category_id == 13);
 CHECK(boxes[1].class_id == 11);
 CHECK(boxes[2].source_category_id == 90);
 CHECK(boxes[2].class_id == 79);
 CHECK(components[0].inventory()[0].release_image_id == 11);
 CHECK(components[0].index().images()[0].source_image_id == 1);
}
TEST_CASE("COCONut compact inventory is deterministic and jointly admitted under corruption and cancellation", "[coconut]") {
 ScopedTempDir root("coconut-compact-inventory");
 const std::array physical{coco(7), coco(8)};
 auto input = request(physical);
 input.parquet_shards = {root.path() / "base.parquet"};
 reversed_coco_parquet(input.parquet_shards[0]);
 const auto first = import_coconut_annotations(input);
 const auto rebuilt = import_coconut_annotations(input);
 REQUIRE(first.size() == 1);
 REQUIRE(rebuilt.size() == 1);
 CHECK(first[0].index().annotation_sha256 == rebuilt[0].index().annotation_sha256);
 REQUIRE(first[0].index().image_count() == 2);
 REQUIRE(first[0].index().box_count() == 2);
 CHECK(first[0].index().images()[0].source_image_id == 7);
 CHECK(first[0].index().images()[1].source_image_id == 8);
 CHECK(first[0].index().images()[0].first_box == 0);
 CHECK(first[0].index().images()[1].first_box == 1);
 CHECK(first[0].index().storage().boxes[0].source_ordinal == 1);
 CHECK(first[0].index().storage().boxes[1].source_ordinal == 0);
 CHECK(first[0].inventory()[0].release_image_id == 7);
 CHECK(first[0].inventory()[1].release_image_id == 8);
 const auto path = root.path() / "first.bin", second = root.path() / "second.bin";
 PollCancellation baseline;
 store_coconut_component(path, first[0], mmltk::common::concurrency::CancellationObservation::Borrow(baseline));
 store_coconut_component(second, rebuilt[0]);
 const auto original = file_bytes(path.string() + ".inventory");
 CHECK(original == file_bytes(second.string() + ".inventory"));
 CHECK(file_bytes(path) == file_bytes(second));
 REQUIRE(original.size() > 64);
 CHECK(original.substr(0, 8) == "CNUTIVN1");
 const auto loaded = load_coconut_component(path, first[0].edition(), first[0].source(), input.input_identity);
 REQUIRE(loaded);
 CHECK(std::ranges::equal(loaded->inventory(), first[0].inventory()));
 SECTION("truncation") {
  std::filesystem::resize_file(path.string() + ".inventory", original.size() - 1);
  CHECK_FALSE(load_coconut_component(path, first[0].edition(), first[0].source(), input.input_identity));
 }
 SECTION("corruption") {
  auto corrupt = original;
  corrupt[corrupt.size() / 2] ^= 1;
  mmltk::testsupport::write_text_file(path.string() + ".inventory", corrupt);
  CHECK_FALSE(load_coconut_component(path, first[0].edition(), first[0].source(), input.input_identity));
 }
 SECTION("reordered inventory cannot replace an admitted index") {
  auto reordered = component_builder(first[0]);
  std::swap(reordered.inventory[0], reordered.inventory[1]);
  CHECK_THROWS(std::move(reordered).finish({}, root.path()));
  CHECK(file_bytes(path.string() + ".inventory") == original);
 }
 SECTION("shared aligned views outlive the opening component") {
  const auto selected = [&] {
   auto opened = load_coconut_component(path, first[0].edition(), first[0].source(), input.input_identity, {}, true);
   REQUIRE(opened);
   const auto* physical = &opened->inventory_image(1);
   auto view = opened->select_images({1});
   CHECK(&view.inventory_image(0) == physical);
   CHECK(view.index().source_position(0) == 1);
   return view;
  }();
  const auto membership = selected.membership();
  CHECK(&membership.inventory_image(0) == &selected.inventory_image(0));
  CHECK(membership.index().storage().boxes.data() == selected.index().storage().boxes.data());
  CHECK(membership.recovery().empty());
  CHECK(membership.recovery_policy() == 0);
  CHECK(membership.original_annotation_identity().empty());
  CHECK_THROWS(store_coconut_component(root.path() / "membership.bin", membership));
  admit_coconut_component(selected);
  const auto selection = root.path() / "selection.bin";
  store_coconut_component(selection, selected);
  CHECK(selected.completion()->images == 2);
  const auto restored = load_coconut_component(selection, selected.edition(), selected.source(), selected.input_identity());
  REQUIRE(restored);
  REQUIRE(restored->index().image_count() == 1);
  CHECK(restored->inventory_image(0) == selected.inventory_image(0));
 }
 SECTION("retained inventory survives replacement and unlink for every destination") {
  const auto inventory_path = std::filesystem::path(path.string() + ".inventory");
  const auto replacement = root.path() / "replacement.inventory";
  mmltk::testsupport::write_text_file(replacement, std::string(original.size(), 'x'));
  std::filesystem::rename(replacement, inventory_path);
  store_coconut_component(path, *loaded);
  CHECK(file_bytes(inventory_path) == original);
  CHECK(load_coconut_component(path, loaded->edition(), loaded->source(), loaded->input_identity()));
  std::filesystem::remove(inventory_path);
  const auto elsewhere = root.path() / "elsewhere" / "component.bin";
  store_coconut_component(elsewhere, *loaded);
  CHECK(file_bytes(elsewhere.string() + ".inventory") == original);
  CHECK(file_bytes(elsewhere) == file_bytes(path));
  std::filesystem::remove(elsewhere.string() + ".inventory");
  store_coconut_component(elsewhere, *loaded);
  CHECK(file_bytes(elsewhere.string() + ".inventory") == original);
  store_coconut_component(path, *loaded);
  CHECK(file_bytes(inventory_path) == original);
 }
 SECTION("standalone construction admits all aligned facts") {
  auto malformed = component_builder(first[0]);
  SECTION("duplicate release-row ordinal") { malformed.inventory[1].source_ordinal = malformed.inventory[0].source_ordinal; }
  SECTION("wrong source") { malformed.inventory[1].physical.source = CoconutImageNamespace::CocoUnlabeled; }
  SECTION("wrong shard") { ++malformed.index.images[1].source_shard; }
  SECTION("unreferenced box") { malformed.index.images.back().box_count = 0; }
  SECTION("malformed run") { malformed.index.mask_rle_pairs.front().length = 0; }
  SECTION("changed header cannot reuse identity") { malformed.input_identity = "different-generation"; }
  CHECK_THROWS(std::move(malformed).finish({}, root.path()));
  CHECK(file_bytes(path.string() + ".inventory") == original);
 }
 SECTION("membership defers malformed normalized masks until full admission") {
  auto bytes = file_bytes(path);
  REQUIRE(first[0].index().run_count() != 0);
  const auto offset = bytes.size() - first[0].index().run_count() * sizeof(RLEPair);
  const RLEPair malformed{0, 0};
  std::memcpy(bytes.data() + offset, &malformed, sizeof(malformed));
  const auto replacement = root.path() / "malformed-index.bin";
  mmltk::testsupport::write_text_file(replacement, bytes);
  std::filesystem::rename(replacement, path);
  const auto deferred = load_coconut_component(path, first[0].edition(), first[0].source(), input.input_identity, {}, true);
  REQUIRE(deferred);
  const auto membership = deferred->membership();
  CHECK(std::ranges::equal(membership.inventory(), first[0].inventory()));
  CHECK_THROWS(admit_coconut_component(*deferred));
  CHECK_THROWS(store_coconut_component(root.path() / "rejected.bin", *deferred));
  CHECK_FALSE(load_coconut_component(path, first[0].edition(), first[0].source(), input.input_identity));
  CHECK(loaded->index().storage().mask_rle_pairs.front().length != 0);
 }
 SECTION("every store cancellation point prevents fresh admission") {
  for (std::size_t cut = 0; cut < baseline.polls; ++cut) {
   const auto cancelled_path = root.path() / ("cancelled-" + std::to_string(cut) + ".bin");
   PollCancellation stop;
   stop.stop_at = cut;
   CHECK_THROWS(store_coconut_component(cancelled_path, first[0], mmltk::common::concurrency::CancellationObservation::Borrow(stop)));
   CHECK_FALSE(load_coconut_component(cancelled_path, first[0].edition(), first[0].source(), input.input_identity));
  }
 }
 SECTION("load cancellation propagates instead of becoming a cache miss") {
  PollCancellation observed;
  REQUIRE(load_coconut_component(path, first[0].edition(), first[0].source(), input.input_identity, mmltk::common::concurrency::CancellationObservation::Borrow(observed)));
  for (std::size_t cut = 0; cut < observed.polls; ++cut) {
   PollCancellation stop;
   stop.stop_at = cut;
   CHECK_THROWS(load_coconut_component(path, first[0].edition(), first[0].source(), input.input_identity, mmltk::common::concurrency::CancellationObservation::Borrow(stop)));
  }
 }
}
TEST_CASE("COCONut cancellation after the last record covers consolidation and identity encoding", "[coconut]") {
 ScopedTempDir root("coconut-finalization-cancel");
 const std::array physical{coco(7), coco(8)};
 auto input = request(physical);
 input.parquet_shards = {root.path() / "base.parquet"};
 reversed_coco_parquet(input.parquet_shards[0]);
 struct FinalRecordCancellation {
  mutable PollCancellation counter;
  bool armed = false;
  bool cancelled() const noexcept { return armed && counter.cancelled(); }
 } state;
 input.cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(state);
 input.progress = [&](std::uint64_t count) {
  if (count == 2) state.armed = true;
 };
 REQUIRE(import_coconut_annotations(input).size() == 1);
 const auto checks = state.counter.polls;
 REQUIRE(checks > 1);
 for (std::size_t cut = 0; cut < checks; ++cut) {
  state.armed = false;
  state.counter.polls = 0;
  state.counter.stop_at = cut;
  CHECK_THROWS(import_coconut_annotations(input));
  CHECK(state.armed);
 }
}
namespace {
std::string white_jpeg(int width = 3, int height = 3, std::size_t minimum_bytes = 0) {
 const std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 3U, 255U);
 std::string bytes;
 REQUIRE(
  stbi_write_jpg_to_func([](void* context, void* data, int size) { static_cast<std::string*>(context)->append(static_cast<const char*>(data), size); }, &bytes, width, height, 3, pixels.data(), 100));
 if (bytes.size() < minimum_bytes) {
  bytes.resize(minimum_bytes, '\0');
  bytes[bytes.size() - 2] = static_cast<char>(0xff);
  bytes.back() = static_cast<char>(0xd9);
 }
 return bytes;
}
void replace_physical_images(const BenchmarkCacheLayout& cache, CoconutRecipeCatalog& catalog, CoconutImageNamespace source, std::span<const std::pair<std::string, std::string>> members) {
 const auto archive = std::ranges::find(catalog.images, source, &RecipeImageArchive::source);
 REQUIRE(archive != catalog.images.end());
 const auto path = cache.source_downloads(benchmark_source_name(archive->artifact.source)) / archive->artifact.filename;
 tar(path, members);
 archive->artifact.expected_size = std::filesystem::file_size(path);
}
struct ServedPhysicalArchive {
 RecipeImageArchive& source;
 const std::filesystem::path path;
 std::string payload;
 mmltk::backend::data::testsupport::HttpServer server;
 ServedPhysicalArchive(const BenchmarkCacheLayout& cache, CoconutRecipeCatalog& catalog, CoconutImageNamespace image_source, const std::string& endpoint)
     : source(*std::ranges::find(catalog.images, image_source, &RecipeImageArchive::source)),
       path(cache.source_downloads(benchmark_source_name(source.artifact.source)) / source.artifact.filename),
       payload(file_bytes(path)),
       server(payload) {
  source.artifact.url = server.url(endpoint);
 }
};
struct LocalCoconutRecipe {
 const std::filesystem::path output;
 BenchmarkCacheLayout cache;
 CoconutRecipeCatalog catalog;
 explicit LocalCoconutRecipe(const std::filesystem::path& root, bool fully_covered = false, bool disjoint = false, bool alternate_members = false)
     : output(root / "compiled"), cache(BenchmarkCacheLayout::create(root / "cache")) {
  catalog.coco_validation_images = 1;
  const auto jpeg = white_jpeg();
  const auto physical_archive = [&](CoconutImageNamespace source, BenchmarkDatasetSource progress_source, std::string shard, std::uint16_t number, std::vector<std::string> members) {
   const auto id = std::string(coconut_namespace_name(source));
   const auto path = cache.source_downloads(benchmark_source_name(progress_source)) / (id + ".tar");
   std::vector<std::pair<std::string, std::string>> contents;
   if (alternate_members) {
    contents.emplace_back(".", "");
    contents.emplace_back("./", "");
   }
   for (const auto& name : members) {
    auto spelling = name;
    if (alternate_members) {
     spelling.insert(spelling.find('/'), "/./");
     spelling = "././" + spelling;
    }
    contents.emplace_back(std::move(spelling), jpeg);
   }
   tar(path, contents);
   catalog.images.push_back({source, number, std::move(shard), {id, "http://127.0.0.1:1/" + id, path.filename().string(), std::filesystem::file_size(path), "", progress_source}});
  };
  physical_archive(CoconutImageNamespace::CocoTrain, BenchmarkDatasetSource::kCoco2017, "train2017", 0, {coco(7).member});
  physical_archive(CoconutImageNamespace::CocoUnlabeled, BenchmarkDatasetSource::kCoco2017, "unlabeled2017", 0, {coco(8, CoconutImageNamespace::CocoUnlabeled).member});
  physical_archive(CoconutImageNamespace::CocoValidation, BenchmarkDatasetSource::kCoco2017, "val2017", 0, {coco(9, CoconutImageNamespace::CocoValidation).member});
  physical_archive(CoconutImageNamespace::Objects365V2, BenchmarkDatasetSource::kObjects365V2, "patch-32", 32, {objects(1).member, objects(2).member});
  physical_archive(CoconutImageNamespace::Objects365V1, BenchmarkDatasetSource::kObjects365V1, "validation", 0, {objects(1, CoconutImageNamespace::Objects365V1).member});
  const std::array<std::uint32_t, 9> support{1, 1, 1, 1, 2, 1, 1, 0, 1}, empty{};
  auto first = segment();
  first["iscrowd"] = 1;
  auto second = segment(2, 2);
  second["area"] = 17;
  const auto mask = png(3, 3, support);
  const auto release = [&](CoconutEdition edition, std::string_view name, std::uint64_t count) -> CoconutReleaseComponent& {
   catalog.releases.push_back({edition, name, "local-release-v1", count, {}});
   return catalog.releases.back();
  };
  const auto annotation = [&](CoconutReleaseComponent& component, std::string filename, auto write) {
   const auto path = cache.source_downloads("coconut-" + std::string(component.name)) / filename;
   write(path);
   component.annotations.push_back({std::string(component.name) + "-" + filename, "http://127.0.0.1:1/" + filename, filename, std::filesystem::file_size(path), "", BenchmarkDatasetSource::kCoconut});
  };
  auto& base = release(CoconutEdition::Base, "fixture-base", 2);
  annotation(base, "base.parquet", [&](const auto& path) { parquet_file(path, Json::array({hf_row(7, mask, Json::array({first, second})), hf_row(8, png(3, 3, empty), Json::array(), 3, 3, "unlabeled2017")})); });
  auto& validation = release(CoconutEdition::RelabeledValidation, "fixture-val", 1);
  annotation(validation, "val.parquet", [&](const auto& path) { parquet_file(path, Json::array({hf_row(9, mask, Json::array({first, second}), 3, 3, "val2017")})); });
  auto& large = release(CoconutEdition::Large, "fixture-large", 1);
  annotation(large, "large.json", [&](const auto& path) {
   json_file(path, {{"images", Json::array({{{"id", 601}, {"file_name", "601.jpg"}, {"width", 3}, {"height", 3}}})},
                    {"annotations", Json::array({{{"image_id", 601}, {"file_name", "601.png"}, {"object365_file_name", "objects365_v2_00000001"}, {"segments_info", Json::array({first, second})}}})}});
  });
  annotation(large, "large.tar", [&](const auto& path) {
   const std::array<std::pair<std::string, std::string>, 1> rows{{{"panoptic_object365/objects365_v2_00000001.png", mask}}};
   tar(path, rows);
  });
  auto& xl = release(CoconutEdition::XLarge, "fixture-xl", fully_covered || disjoint ? 1 : 2);
  annotation(xl, "xl.tar", [&](const auto& path) {
   std::vector<std::pair<std::string, std::string>> rows;
   for (auto id : fully_covered ? std::vector<unsigned>{1} : disjoint ? std::vector<unsigned>{2} : std::vector<unsigned>{1, 2}) {
    const auto stem = "objects365_v2_0000000" + std::to_string(id);
    rows.emplace_back("coconuts_xlarge/panseg/" + stem + ".png", mask);
    rows.emplace_back("coconuts_xlarge/panseg_info/" + stem + ".json", Json::array({first, second}).dump());
   }
   tar(path, rows);
  });
  auto& objects_val = release(CoconutEdition::ObjectsValidation, "fixture-objects-val", 1);
  annotation(objects_val, "objects.json", [&](const auto& path) {
   json_file(
    path, {{"images", Json::array({{{"id", 600001}, {"file_name", "600001.jpg"}, {"width", 3}, {"height", 3}}})},
           {"annotations", Json::array({{{"image_id", 600001}, {"file_name", "600001.png"}, {"object365_file_name", "objects365_v1_00000001"}, {"segments_info", Json::array({first, second})}}})}});
  });
  annotation(objects_val, "objects.tar", [&](const auto& path) {
   const std::array<std::pair<std::string, std::string>, 1> rows{{{"panoptic_o365val_v3/objects365_v1_00000001.png", mask}}};
   tar(path, rows);
  });
  Json stock{
   {"images", Json::array({{{"id", 9}, {"file_name", "000000000009.jpg"}, {"width", 3}, {"height", 3}}})},
   {"annotations", Json::array({{{"id", 900}, {"image_id", 9}, {"category_id", 3}, {"bbox", Json::array({0, 0, 3, 3})}, {"area", 9}, {"iscrowd", 0}}})}, {"categories", category_catalog()}
  };
  const auto stock_path = cache.source_downloads("coco") / "stock.tar";
  const std::array<std::pair<std::string, std::string>, 1> stock_members{{{"annotations/instances_val2017.json", stock.dump()}}};
  tar(stock_path, stock_members);
  catalog.stock_annotations = {"fixture-stock", "http://127.0.0.1:1/stock", "stock.tar", std::filesystem::file_size(stock_path), "", BenchmarkDatasetSource::kCoco2017};
 }
 BenchmarkCompilerConfig compiler_config(BenchmarkDatasetSelection selection = {}, bool overwrite = false) const {
  BenchmarkCompilerConfig config;
  config.output_dir = output;
  config.cache_dir = cache.root;
  config.resolution = 3;
  config.num_workers = 1;
  config.overwrite = overwrite;
  config.selection = selection;
  return config;
 }
 CoconutRecipeCatalog selected(CoconutValidation choice) const {
  auto result = catalog;
  std::erase_if(result.releases, [&](const auto& release) {
   return (release.edition == CoconutEdition::RelabeledValidation && choice == CoconutValidation::Stock) ||
          (release.edition == CoconutEdition::ObjectsValidation && choice != CoconutValidation::Coconut);
  });
  if (choice != CoconutValidation::Coconut) std::erase_if(result.images, [](const auto& image) { return image.source == CoconutImageNamespace::Objects365V1; });
  return result;
 }
};
std::vector<AdmittedRecipeArchive> local_physical_archives(const BenchmarkCacheLayout& cache, const CoconutRecipeCatalog& catalog) {
 std::vector<AdmittedRecipeArchive> admitted;
 for (const auto& archive : catalog.images) {
  const auto path = cache.source_downloads(benchmark_source_name(archive.artifact.source)) / archive.artifact.filename;
  admitted.push_back({archive, {.path = path, .size = std::filesystem::file_size(path), .identity = archive.artifact.artifact_id}, 0,
   BenchmarkArchive::workspace_bytes(path, 64ULL << 20)});
 }
 return admitted;
}
}  // namespace
TEST_CASE("COCONut private catalog compiles all validation choices through the production transaction", "[coconut][benchmark]") {
 ScopedTempDir root("coconut-recipe");
 bool alternate_members = false;
 bool mismatched_geometry = false;
 bool letterbox = false;
 std::size_t descriptors = 0;
 SECTION("canonical physical members") {}
 SECTION("complete recipe with thirteen descriptors") { descriptors = 13; }
 SECTION("complete recipe with sixteen descriptors") { descriptors = 16; }
 SECTION("complete recipe with twenty-four descriptors") { descriptors = 24; }
 SECTION("complete recipe with twenty-six descriptors") { descriptors = 26; }
 SECTION("equivalent physical members and archive roots") { alternate_members = true; }
 SECTION("mismatched mask geometry drops objects and retains original images") { mismatched_geometry = true; }
 SECTION("mismatched mask geometry preserves physical image letterboxing") {
  mismatched_geometry = true;
  letterbox = true;
 }
 LocalCoconutRecipe local(root.path(), false, false, alternate_members);
 if (mismatched_geometry) {
  auto& physical = *std::ranges::find(local.catalog.images, CoconutImageNamespace::Objects365V2, &RecipeImageArchive::source);
  const auto archive = local.cache.source_downloads("objects365") / physical.artifact.filename;
  const auto pixels = white_jpeg(6, 4);
  const std::array<std::pair<std::string, std::string>, 2> members{{{objects(1).member, pixels}, {objects(2).member, pixels}}};
  tar(archive, members);
  physical.artifact.expected_size = std::filesystem::file_size(archive);
 }
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Coconut}, true);
 if (letterbox) config.resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Letterbox;
 std::uint64_t dropped_instances = 0;
 config.progress = [&](const BenchmarkCompileProgress& update) { dropped_instances = update.dropped_instances; };
 std::string original_train;
 std::filesystem::file_time_type jpeg_time;
 ino_t jpeg_inode = 0;
 const auto jpeg_path = cached_image_path(local.cache.source_images("coco") / "train2017", 7);
 std::string enhanced_identity;
 int rebuild = 0;
 for (const auto choice : {CoconutValidation::Coconut, CoconutValidation::Stock, CoconutValidation::CoconutStock, CoconutValidation::Coconut}) {
  config.selection.validation = choice;
  config.num_workers = 1 + (rebuild++ % 2);
  const auto catalog = local.selected(choice);
  compile_benchmark_recipe(config, &catalog, nullptr, {}, {}, {.descriptors = descriptors});
  const auto train = CompiledDataset::open(config.output_dir / "train.bin");
  const auto val = CompiledDataset::open(config.output_dir / "val.bin");
  REQUIRE(train.image_entries().size() == 4);
  const std::array<std::uint64_t, 4> expected{7, 8, 1, 2};
  for (std::uint32_t i = 0; i < expected.size(); ++i) CHECK(train.image_entry(i).source_image_id == expected[i]);
  CHECK(train.image_entry(0).source == AnnotationSource::CoconutCoco);
  CHECK(train.image_entry(2).source == AnnotationSource::CoconutObjects365V2);
  if (mismatched_geometry) {
   for (const auto row : {2U, 3U}) {
    CHECK(train.image_labels(row).empty());
    CHECK(train.image_entry(row).original_width == 6U);
    CHECK(train.image_entry(row).original_height == 4U);
    for (std::size_t pixel = 0; pixel < 27; ++pixel) CHECK(train.image_pixels(row)[pixel] == (letterbox && pixel % 9U >= 6U ? 0.0F : 1.0F));
   }
   std::ifstream report(local.cache.root / "failed.txt");
   std::size_t failures = 0;
   for (std::string line; std::getline(report, line); ++failures) {
    const auto failed = Json::parse(line);
    CHECK((failed["image_id"] == 1U || failed["image_id"] == 2U));
    CHECK((failed["object_id"] == 1U || failed["object_id"] == 2U));
    CHECK(failed["reason"] == "annotation dimensions 3x3 do not match image dimensions 6x4");
    CHECK(failed["image"] == objects(failed["image_id"].get<unsigned>()).member);
   }
   CHECK(failures == 4U * static_cast<unsigned>(rebuild));
  }
  CHECK(dropped_instances == (mismatched_geometry ? 4U : 0U));
  CHECK(train.image_labels(1).empty());
  REQUIRE(train.image_labels(0).size() == 2);
  CHECK(train.image_labels(0)[0].is_crowd());
  CHECK(train.image_labels(0)[0].original_area == 7);
  CHECK(train.image_labels(0)[1].original_area == 17);
  CHECK(train.image_labels(0)[1].source_category_id == 2);
  CHECK(train.image_labels(0)[0].source_ordinal == 0);
  CHECK(train.image_labels(0)[1].source_ordinal == 1);
  const auto runs = train.instance_rle(train.image_labels(0)[0]);
  REQUIRE(runs.size() == 3);
  CHECK(runs[0].start == 0);
  CHECK(runs[0].length == 4);
  CHECK(runs[1].start == 5);
  CHECK(runs[1].length == 2);
  CHECK(runs[2].start == 8);
  CHECK(runs[2].length == 1);
  for (std::size_t i = 0; i < 27; ++i) CHECK(train.image_pixels(0)[i] == 1.0F);
  REQUIRE(train.class_names().size() == 80);
  CHECK(train.class_names()[0] == "person");
  CHECK(train.class_names()[79] == "toothbrush");
  CHECK(val.image_entries().size() == (choice == CoconutValidation::Coconut ? 2 : 1));
  CHECK(val.image_entry(0).source_image_id == 9);
  CHECK(val.image_entry(0).source == (choice == CoconutValidation::Stock ? AnnotationSource::Coco : AnnotationSource::CoconutCoco));
  CHECK(val.image_labels(0).size() == (choice == CoconutValidation::Stock ? 1 : 2));
  if (choice == CoconutValidation::Coconut) {
   CHECK(val.image_entry(1).source_image_id == 1);
   CHECK(val.image_entry(1).source == AnnotationSource::CoconutObjects365V1);
  }
  const auto manifest = read_json_file(config.output_dir / "benchmark_manifest.json");
  CHECK_FALSE(manifest.contains("supplemental_sampling"));
  CHECK_FALSE(manifest["mappings"].contains("objects365"));
  CHECK(manifest["recipe"]["duplicate_xl_images"] == 1);
  CHECK(manifest["train"]["images"] == 4);
  struct stat status{};
  REQUIRE(::stat(jpeg_path.c_str(), &status) == 0);
  if (original_train.empty()) {
   original_train = file_bytes(config.output_dir / "train.bin");
   jpeg_time = std::filesystem::last_write_time(jpeg_path);
   jpeg_inode = status.st_ino;
  } else {
   CHECK(file_bytes(config.output_dir / "train.bin") == original_train);
   CHECK(std::filesystem::last_write_time(jpeg_path) == jpeg_time);
   CHECK(status.st_ino == jpeg_inode);
  }
  if (choice != CoconutValidation::Stock) {
   const auto path = local.cache.source_indexes("coconut-fixture-val") / "coco-val2017.normalized.bin";
   const auto identity = read_json_file(path.string() + ".complete.json")["identity"].get<std::string>();
   if (enhanced_identity.empty())
    enhanced_identity = identity;
   else
    CHECK(identity == enhanced_identity);
  }
 }
 // A same-size damaged cached JPEG retains the proof fast path until the
 // writer requests repair, exercising the second production parser caller.
 const auto validation_bytes = file_bytes(config.output_dir / "val.bin");
 mmltk::testsupport::write_text_file(jpeg_path, std::string(std::filesystem::file_size(jpeg_path), 'x'));
 const auto repair_catalog = local.selected(config.selection.validation);
 unsigned replacement_downloads = 0;
 config.trace = [&](std::string_view event, std::string_view) {
  if (event == "benchmark.download.complete") ++replacement_downloads;
 };
 compile_benchmark_recipe(config, &repair_catalog);
 CHECK(replacement_downloads == 0);
 CHECK(file_bytes(config.output_dir / "train.bin") == original_train);
 CHECK(file_bytes(config.output_dir / "val.bin") == validation_bytes);
 const auto published = file_bytes(config.output_dir / "train.bin");
 std::atomic<bool> cancel{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancel);
 config.progress = [&](const BenchmarkCompileProgress& value) {
  if (value.phase == DatasetCompilePhase::Publishing) cancel.store(true);
 };
 const auto catalog = local.selected(CoconutValidation::Coconut);
 CHECK_THROWS(compile_benchmark_recipe(config, &catalog));
 CHECK(file_bytes(config.output_dir / "train.bin") == published);
}
TEST_CASE("COCONut fully covered XL keeps Large rows and rebuilds without an empty index", "[coconut][benchmark]") {
 ScopedTempDir root("coconut-covered");
 LocalCoconutRecipe local(root.path(), true);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Coconut}, true);
 const auto catalog = local.selected(CoconutValidation::Coconut);
 compile_benchmark_recipe(config, &catalog);
 CHECK(CompiledDataset::open(config.output_dir / "train.bin").image_entries().size() == 3);
 const auto previous = file_bytes(config.output_dir / "train.bin");
 compile_benchmark_recipe(config, &catalog);
 CHECK(file_bytes(config.output_dir / "train.bin") == previous);
 const auto recipe = read_json_file(config.output_dir / "benchmark_manifest.json")["recipe"];
 CHECK(recipe["duplicate_xl_images"] == 1);
 const auto xl = std::ranges::find_if(recipe["components"], [](const auto& component) { return component["edition"] == CoconutEdition::XLarge; });
 REQUIRE(xl != recipe["components"].end());
 CHECK((*xl)["admitted_images"] == 0);
 CHECK((*xl)["covered_by_large_images"] == 1);
 CHECK((*xl)["selected_annotation_sha256"].is_null());
}
TEST_CASE("shared root repair invalidates all proofs and preserves valid JPEG allocations", "[coconut][benchmark][cache]") {
 ScopedTempDir root("coconut-shared-repair");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Coconut}, true);
 const auto catalog = local.selected(CoconutValidation::Coconut);
 compile_benchmark_recipe(config, &catalog);
 const auto images = local.cache.source_images("objects365") / "patch-32";
 const std::array<std::uint64_t, 2> ids{1, 2};
 const auto proof = images / ".recipe-proofs" / ("coconut-0-" + cached_image_selection_digest(ids) + ".json");
 REQUIRE(std::filesystem::exists(proof));
 const auto original = read_json_file(proof);
 {
  auto lease = ArtifactLease::acquire(local.cache.locks / "objects365-patch-32.images.lock", {});
  complete_cached_image_group(images, images / ".complete.json", original["identity"].get<std::string>(), ids, original["image_bytes"].get<std::uint64_t>(), {});
  std::filesystem::remove(cached_image_path(images, 2));
 }
 struct stat before{}, after{};
 REQUIRE(::stat(cached_image_path(images, 1).c_str(), &before) == 0);
 const auto time = std::filesystem::last_write_time(cached_image_path(images, 1));
 compile_benchmark_recipe(config, &catalog);
 REQUIRE(std::filesystem::exists(cached_image_path(images, 2)));
 CHECK_FALSE(std::filesystem::exists(images / ".complete.json"));
 REQUIRE(::stat(cached_image_path(images, 1).c_str(), &after) == 0);
 CHECK(before.st_ino == after.st_ino);
 CHECK(std::filesystem::last_write_time(cached_image_path(images, 1)) == time);
 {
  auto lease = ArtifactLease::acquire(local.cache.locks / "objects365-patch-32.images.lock", {});
  // A custom extraction discovers invalid bytes during its ordinary cache scan.
  mmltk::testsupport::write_text_file(cached_image_path(images, 2), "bad JPEG");
  const auto archive = std::ranges::find(catalog.images, CoconutImageNamespace::Objects365V2, &RecipeImageArchive::source);
  REQUIRE(archive != catalog.images.end());
  const auto extracted = extract_selected_archive_images(
   {.archive_path = local.cache.source_downloads("objects365") / archive->artifact.filename,
    .source_identity = original["identity"].get<std::string>(),
    .output_root = images,
    .source = "objects365",
    .shard = "patch-32",
    .selected_image_ids = ids,
    .image_id_parser = [](std::string_view member) -> std::optional<std::uint64_t> {
   if (member == "patch32/objects365_v2_00000001.jpg") return 1;
   if (member == "patch32/objects365_v2_00000002.jpg") return 2;
   return {};
  },
    .validator =
     [](std::uint64_t, std::span<const std::uint8_t> bytes) {
   if (!has_complete_image_markers(bytes)) throw std::runtime_error("invalid JPEG");
   return BenchmarkImageDecoder{}.read_header(bytes);
  },
    .quarantine_unavailable = true,
    .decompression_workers = 0,
    .cache_write_workers = 0});
  CHECK(extracted.image_count == 2);
  CHECK_FALSE(std::filesystem::exists(proof));
 }
 compile_benchmark_recipe(config, &catalog);
 CHECK(std::filesystem::exists(proof));
 CHECK(std::filesystem::exists(images / ".complete.json"));
 REQUIRE(::stat(cached_image_path(images, 1).c_str(), &after) == 0);
 CHECK(before.st_ino == after.st_ino);
}
TEST_CASE("missing offered COCONut masks cannot replace the previous publication", "[coconut][benchmark]") {
 ScopedTempDir root("coconut-missing-mask");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Coconut}, true);
 auto catalog = local.selected(CoconutValidation::Coconut);
 compile_benchmark_recipe(config, &catalog);
 const auto train = file_bytes(config.output_dir / "train.bin"), val = file_bytes(config.output_dir / "val.bin");
 auto& large = *std::ranges::find(catalog.releases, CoconutEdition::Large, &CoconutReleaseComponent::edition);
 auto& masks = *std::ranges::find(large.annotations, std::string("large.tar"), &CatalogArtifact::filename);
 const auto path = local.cache.source_downloads("coconut-fixture-large") / masks.filename;
 const std::array<std::pair<std::string, std::string>, 1> unrelated{{{"unrelated", "missing required mask"}}};
 tar(path, unrelated);
 masks.expected_size = std::filesystem::file_size(path);
 masks.url += "?missing-mask";
 CHECK_THROWS(compile_benchmark_recipe(config, &catalog));
 CHECK(file_bytes(config.output_dir / "train.bin") == train);
 CHECK(file_bytes(config.output_dir / "val.bin") == val);
}
TEST_CASE("COCONut disjoint XL membership remains complete", "[coconut][benchmark]") {
 ScopedTempDir root("coconut-disjoint");
 LocalCoconutRecipe local(root.path(), false, true);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, false);
 const auto catalog = local.selected(CoconutValidation::CoconutStock);
 compile_benchmark_recipe(config, &catalog);
 CHECK(CompiledDataset::open(config.output_dir / "train.bin").image_entries().size() == 4);
 CHECK(read_json_file(config.output_dir / "benchmark_manifest.json")["recipe"]["duplicate_xl_images"] == 0);
}
TEST_CASE("COCONut rejects physical COCO train validation reuse before publication", "[coconut][benchmark]") {
 ScopedTempDir root("coconut-split-reuse");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 compile_benchmark_recipe(config, &catalog);
 const auto previous = file_bytes(config.output_dir / "train.bin");
 auto& archive = *std::ranges::find(catalog.images, CoconutImageNamespace::CocoValidation, &RecipeImageArchive::source);
 const auto archive_path = local.cache.source_downloads("coco") / archive.artifact.filename;
 const std::array<std::pair<std::string, std::string>, 1> members{{{coco(7, CoconutImageNamespace::CocoValidation).member, white_jpeg()}}};
 tar(archive_path, members);
 archive.artifact.expected_size = std::filesystem::file_size(archive_path);
 archive.artifact.url += "?overlap";
 auto& release = *std::ranges::find(catalog.releases, CoconutEdition::RelabeledValidation, &CoconutReleaseComponent::edition);
 auto& artifact = release.annotations.front();
 const auto path = local.cache.source_downloads("coconut-fixture-val") / artifact.filename;
 const std::array<std::uint32_t, 9> ids{1, 1, 1, 1, 1, 1, 1, 1, 1};
 parquet_file(path, Json::array({hf_row(7, png(3, 3, ids), Json::array({segment()}))}));
 artifact.expected_size = std::filesystem::file_size(path);
 artifact.url += "?overlap";
 CHECK_THROWS_WITH(compile_benchmark_recipe(config, &catalog), "COCONut train and validation reuse a physical member");
 CHECK(file_bytes(config.output_dir / "train.bin") == previous);
}
TEST_CASE("physical archive structural repair retains one admitted acquisition through extraction", "[coconut][benchmark][download]") {
 ScopedTempDir root("coconut-physical-repair");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 ServedPhysicalArchive served(local.cache, catalog, CoconutImageNamespace::CocoTrain, "physical");
 compile_benchmark_recipe(config, &catalog);
 REQUIRE(served.server.requests() == 0);
 const auto before = file_bytes(config.output_dir / "train.bin");
 const auto unaffected = cached_image_path(local.cache.source_images("objects365") / "patch-32", 1);
 struct stat inode{};
 REQUIRE(::stat(unaffected.c_str(), &inode) == 0);
 const auto proof = local.cache.source_images("objects365") / "patch-32/.recipe-proofs";
 const auto proof_count = std::distance(std::filesystem::directory_iterator(proof), std::filesystem::directory_iterator{});
 std::filesystem::remove(local.cache.source_indexes("coco") / (served.source.artifact.artifact_id + ".inventory.bin"));
 mmltk::testsupport::write_text_file(served.path, std::string(served.payload.size(), 'x'));
 unsigned physical_settlements = 0, failure_hashes = 0, cached_admissions = 0;
 config.trace = [&](std::string_view event, std::string_view fields) {
  const auto facts = Json::parse(fields);
  if (facts.value("artifact", std::string{}) != served.source.artifact.artifact_id) return;
  if (event == "benchmark.download.complete") ++physical_settlements;
  if (event == "benchmark.download.cache_hit" || event == "benchmark.download.preseeded") ++cached_admissions;
  if (event == "benchmark.download.failure_sha256") ++failure_hashes;
 };
 compile_benchmark_recipe(config, &catalog);
 CHECK(served.server.requests() == 1);
 CHECK(physical_settlements == 1);
 CHECK(failure_hashes == 1);
 CHECK(file_bytes(config.output_dir / "train.bin") == before);
 struct stat after{};
 REQUIRE(::stat(unaffected.c_str(), &after) == 0);
 CHECK(after.st_ino == inode.st_ino);
 CHECK(std::distance(std::filesystem::directory_iterator(proof), std::filesystem::directory_iterator{}) == proof_count);
 const auto admissions_before = cached_admissions;
 compile_benchmark_recipe(config, &catalog);
 CHECK(served.server.requests() == 1);
 CHECK(physical_settlements == 1);
 CHECK(cached_admissions == admissions_before + 1);
 std::ranges::fill(served.payload, 'x');
 mmltk::testsupport::write_text_file(served.path, std::string(served.payload.size(), 'x'));
 std::filesystem::remove(local.cache.source_indexes("coco") / (served.source.artifact.artifact_id + ".inventory.bin"));
 CHECK_THROWS(compile_benchmark_recipe(config, &catalog));
 CHECK(file_bytes(config.output_dir / "train.bin") == before);
 REQUIRE(::stat(unaffected.c_str(), &after) == 0);
 CHECK(after.st_ino == inode.st_ino);
 CHECK(std::distance(std::filesystem::directory_iterator(proof), std::filesystem::directory_iterator{}) == proof_count);
 served.server.Check();
}
TEST_CASE("additional download storage counts allocated retained bytes once", "[benchmark][storage]") {
 ScopedTempDir root("additional-source-storage");
 const auto path = root.path() / "source.tar";
 constexpr std::uint64_t target = 65536;
 CHECK(additional_download_bytes(path, target) == target);
 mmltk::testsupport::write_text_file(path, std::string(target, 'a'));
 CHECK(additional_download_bytes(path, target) == 0);
 std::filesystem::rename(path, path.string() + ".part");
 CHECK(additional_download_bytes(path, target) == 0);
 std::filesystem::resize_file(path.string() + ".part", 4096);
 struct stat partial{};
 REQUIRE(::stat((path.string() + ".part").c_str(), &partial) == 0);
 CHECK(additional_download_bytes(path, target) == target - std::min(target, static_cast<std::uint64_t>(partial.st_blocks) * 512U));
}
TEST_CASE("missing physical membership repairs its archive without replacing annotation inputs", "[coconut][benchmark][download]") {
 ScopedTempDir root("coconut-membership-repair");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 ServedPhysicalArchive served(local.cache, catalog, CoconutImageNamespace::CocoTrain, "membership");
 const std::array<std::pair<std::string, std::string>, 1> wrong{{{coco(6).member, white_jpeg()}}};
 tar(served.path, wrong);
 REQUIRE(std::filesystem::file_size(served.path) == served.payload.size());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, false);
 std::vector<std::string> diagnosed;
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.download.failure_sha256") diagnosed.push_back(Json::parse(fields).at("artifact").get<std::string>());
 };
 compile_benchmark_recipe(config, &catalog);
 CHECK(served.server.requests() == 1);
 CHECK(diagnosed == std::vector<std::string>{served.source.artifact.artifact_id});
 served.server.Check();
}
TEST_CASE("COCONut normalization observations are bounded with exact unknown-total settlement", "[coconut][progress]") {
 ScopedTempDir root("coconut-progress-quantum");
 std::vector<CoconutPhysicalImage> physical;
 Json rows = Json::array();
 const std::array<std::uint32_t, 1> ids{1};
 const auto mask = png(1, 1, ids);
 for (unsigned id = 1; id <= 130; ++id) {
  physical.push_back(coco(id));
  rows.push_back(hf_row(id, mask, Json::array({segment()}), 1, 1));
 }
 auto input = request(physical);
 input.expected_rows = 0;
 input.parquet_shards = {root.path() / "base.parquet"};
 parquet_file(input.parquet_shards[0], rows);
 std::vector<std::uint64_t> observed;
 input.progress = [&](auto value) { observed.push_back(value); };
 REQUIRE(import_coconut_annotations(input).front().index().image_count() == 130);
 CHECK(observed == std::vector<std::uint64_t>{0, 64, 128, 130});
 SECTION("disabled observation installs no callback") {
  const BenchmarkTraceSink silent;
  ProgressReporter reporter({}, silent);
  CHECK_FALSE(reporter.normalization_observer_enabled());
  input.progress = {};
  observed.clear();
  REQUIRE(import_coconut_annotations(input).front().index().image_count() == 130);
  CHECK(observed.empty());
  const BenchmarkTraceSink trace = [](std::string_view, const Json&) {};
  ProgressReporter traced({}, trace);
  CHECK_FALSE(traced.normalization_observer_enabled());
 }
 SECTION("cancellation remains visible between observation quanta") {
  struct BetweenObservations {
   mutable unsigned polls = 0;
   bool armed = false;
   bool cancelled() const noexcept { return armed && ++polls == 20; }
  } stop;
  input.cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(stop);
  observed.clear();
  input.progress = [&](auto value) {
   observed.push_back(value);
   if (value == 64) stop.armed = true;
  };
  CHECK_THROWS(import_coconut_annotations(input));
  CHECK(observed == std::vector<std::uint64_t>{0, 64});
 }
}
namespace {
CustomRecipeCatalog local_custom_catalog(LocalCoconutRecipe& local, std::span<const unsigned> selected_train_ids = {}) {
 auto catalog = custom_recipe_catalog();
 catalog.coco_train_images_count = selected_train_ids.empty() ? 2 : static_cast<std::uint32_t>(selected_train_ids.size());
 catalog.coco_validation_images_count = 1;
 for (const auto& physical : local.catalog.images) {
  if (physical.source == CoconutImageNamespace::CocoTrain) catalog.coco_train_images = physical.artifact;
  if (physical.source == CoconutImageNamespace::CocoValidation) catalog.coco_val_images = physical.artifact;
  if (physical.source == CoconutImageNamespace::Objects365V2) catalog.objects_images.at(physical.shard) = physical.artifact;
 }
 catalog.coco_annotations = local.catalog.stock_annotations;
 const auto index = [&](BenchmarkDatasetSource source, std::string split, std::vector<NormalizedImage> images, std::vector<NormalizedBox> boxes) {
  NormalizedAnnotationBuilder value;
  value.source = source;
  value.split = std::move(split);
  value.annotation_sha256 = std::string(64, 'a');
  value.images = std::move(images);
  value.boxes = std::move(boxes);
  value.rejected.raw_records = value.boxes.size();
  return value;
 };
 const auto box = [](std::uint8_t target, std::uint64_t category, std::uint64_t id) {
  NormalizedBox value;
  value.x2 = 1;
  value.y2 = 1;
  value.class_id = target;
  value.flags = kAnnotationId | kAnnotationCategory;
  value.original_area = 9;
  value.annotation_id = id;
  value.source_category_id = category;
  return value;
 };
 // Ordinary preacquired normalized source facts: six rows per supplemental
 // source, with one rare-class row, make the selected membership exact.
 const auto coco_index = [&](std::string split, std::span<const unsigned> ids) {
  Json document{{"images", Json::array()}, {"annotations", Json::array()}, {"categories", category_catalog()}};
  for (const auto id : ids) {
   document["images"].push_back({{"id", id}, {"width", 3}, {"height", 3}, {"file_name", coco(id).member}});
   document["annotations"].push_back({{"id", id * 10}, {"image_id", id}, {"category_id", 3}, {"bbox", Json::array({0, 0, 3, 3})}, {"area", 9}, {"iscrowd", id == 7 ? 1 : 0}});
  }
  const auto path = local.cache.source_indexes("coco") / (split + ".fixture.json");
  json_file(path, document);
  return parse_coco_style_annotations(
   path, std::string(64, 'a'), coco_category_mappings(), {BenchmarkDatasetSource::kCoco2017, std::move(split), static_cast<std::uint32_t>(ids.size()), 1, true, {}, {}});
 };
 const std::array<unsigned, 2> train_ids{7, 10};
 const std::array<unsigned, 1> val_ids{9};
 auto train = coco_index("train2017", selected_train_ids.empty() ? std::span<const unsigned>(train_ids) : selected_train_ids);
 auto val = coco_index("val2017", val_ids);
 const auto object_mapping = *std::ranges::find(objects365_category_mappings(), std::uint8_t{0}, &NumericCategoryMapping::target_id);
 const auto open_mapping = *std::ranges::find(open_images_category_mappings(), std::uint8_t{1}, &StringCategoryMapping::target_id);
 auto objects_index = index(BenchmarkDatasetSource::kObjects365V2, "train", {}, {box(0, object_mapping.source_id, 11)});
 auto open_index = index(BenchmarkDatasetSource::kOpenImagesV7, "train", {}, {box(1, encode_open_images_category(open_mapping.source_id), 17)});
 open_index.boxes.front().original_area = 1;
 for (unsigned i = 0; i < 6; ++i) {
  objects_index.images.push_back({i + 1, i, 1, 3, 3, 32});
  open_index.images.push_back({i + 17, i, 1, 3, 3});
  if (i != 0) {
   auto object_box = objects_index.boxes.front();
   object_box.class_id = 2;
   object_box.source_category_id = std::ranges::find(objects365_category_mappings(), std::uint8_t{2}, &NumericCategoryMapping::target_id)->source_id;
   objects_index.boxes.push_back(object_box);
   auto open_box = open_index.boxes.front();
   open_box.class_id = 2;
   open_box.source_category_id = encode_open_images_category(std::ranges::find(open_images_category_mappings(), std::uint8_t{2}, &StringCategoryMapping::target_id)->source_id);
   open_index.boxes.push_back(open_box);
  }
 }
 objects_index.rejected.raw_records = objects_index.boxes.size();
 open_index.rejected.raw_records = open_index.boxes.size();
 store_normalized_annotation_index(local.cache.source_indexes("coco") / "train2017.normalized.bin", fixture_index(train), {});
 store_normalized_annotation_index(local.cache.source_indexes("coco") / "val2017.normalized.bin", fixture_index(val), {});
 store_normalized_annotation_index(local.cache.source_indexes("objects365") / "train.normalized.bin", fixture_index(objects_index), {});
 store_normalized_annotation_index(local.cache.source_indexes("open-images") / "train.normalized.bin", fixture_index(open_index), {});
 const auto open_path = cached_image_path(local.cache.source_images("open-images") / "train", 17);
 std::filesystem::create_directories(open_path.parent_path());
 mmltk::testsupport::write_text_file(open_path, white_jpeg());
 return catalog;
}
void check_custom_compilation(const std::filesystem::path& output) {
 const auto train = CompiledDataset::open(output / "train.bin");
 const auto val = CompiledDataset::open(output / "val.bin");
 REQUIRE(train.image_entries().size() == 3);
 REQUIRE(val.image_entries().size() == 1);
 const std::array<std::uint64_t, 3> ids{7, 1, 17};
 const std::array<AnnotationSource, 3> sources{AnnotationSource::Coco, AnnotationSource::Objects365, AnnotationSource::OpenImages};
 const std::array<unsigned, 3> classes{2, 0, 1};
 // COCO preserves the annotation object's byte offset after {"annotations":[.
 const std::array<std::uint64_t, 3> source_ordinals{16, 0, 0};
 for (std::uint32_t i = 0; i < 3; ++i) {
  CHECK(train.image_entry(i).source_image_id == ids[i]);
  CHECK(train.image_entry(i).source == sources[i]);
  REQUIRE(train.image_labels(i).size() == 1);
  const auto& label = train.image_labels(i).front();
  CHECK(label.class_id == classes[i]);
  CHECK(label.bbox_x1 == 0.0F);
  CHECK(label.bbox_y1 == 0.0F);
  CHECK(label.bbox_x2 == 3.0F);
  CHECK(label.bbox_y2 == 3.0F);
  CHECK(label.original_area == 9);
  CHECK(label.source_ordinal == source_ordinals[i]);
  CHECK_FALSE(label.has_mask());
  for (unsigned pixel = 0; pixel < 27; ++pixel) CHECK(train.image_pixels(i)[pixel] == 1.0F);
 }
 CHECK(train.image_labels(0).front().annotation_id == 70);
 CHECK(train.image_labels(0).front().source_category_id == 3);
 CHECK(train.image_labels(0).front().is_crowd());
 CHECK(train.class_names()[2] == "car");
 CHECK(val.image_entry(0).source_image_id == 9);
 CHECK(val.image_entry(0).source == AnnotationSource::Coco);
 const auto manifest = read_json_file(output / "benchmark_manifest.json");
 CHECK(manifest["recipe"]["dataset"] == "coco-custom");
 CHECK(manifest["compiled_format_version"] == FORMAT_VERSION);
 CHECK(manifest["mapping_revision"] == kBenchmarkMappingRevision);
 REQUIRE(manifest["sources"].size() == 3);
 CHECK(manifest["sources"][0]["selected_images"] == 2);
 CHECK(manifest["sources"][0]["compiled_images"] == 1);
 CHECK(manifest["sources"][1]["indexed_images"] == 6);
 CHECK(manifest["sources"][2]["indexed_images"] == 6);
 CHECK(manifest["sources"][1]["selected_images"] == 1);
 CHECK(manifest["sources"][2]["selected_images"] == 1);
 CHECK(manifest["supplemental_sampling"]["target_images"] == 2);
 CHECK(manifest["supplemental_sampling"]["objects365_shards"] == Json::array({32}));
 REQUIRE(manifest["quarantined_images"].size() == 1);
 CHECK(manifest["quarantined_images"][0]["image_id"] == 10);
 CHECK(manifest["validation_source"]["compiled_images"] == 1);
 CHECK(manifest["mappings"].contains("objects365"));
 CHECK(manifest["mappings"].contains("open_images"));
}
}  // namespace
TEST_CASE("custom and COCONut production transactions preserve output and shared physical caches across switches", "[benchmark][coconut]") {
 ScopedTempDir root("both-native-recipes");
 LocalCoconutRecipe local(root.path());
 auto& physical = *std::ranges::find(local.catalog.images, CoconutImageNamespace::CocoTrain, &RecipeImageArchive::source);
 const auto archive = local.cache.source_downloads("coco") / physical.artifact.filename;
 const std::array<std::pair<std::string, std::string>, 2> members{{{coco(7).member, white_jpeg()}, {coco(10).member, "not a JPEG"}}};
 tar(archive, members);
 physical.artifact.expected_size = std::filesystem::file_size(archive);
 const auto bytes = file_bytes(archive);
 const std::vector<std::uint8_t> payload(bytes.begin(), bytes.end());
 mmltk::backend::data::testsupport::HttpServer server(payload);
 physical.artifact.url = server.url("shared-train");
 auto custom = local_custom_catalog(local);
 const auto coconut = local.selected(CoconutValidation::CoconutStock);
 auto config = local.compiler_config({}, true);
 const auto image_root = local.cache.source_images("coco") / "train2017";
 const auto jpeg = cached_image_path(image_root, 7);
 std::string custom_train, custom_val, coconut_train;
 ino_t inode = 0;
 std::filesystem::file_time_type mtime;
 for (unsigned step = 0; step < 5; ++step) {
  const bool enhanced = step % 2 == 0;
  config.num_workers = 1 + step % 2;
  config.selection = {enhanced ? BenchmarkDatasetVariant::Coconut : BenchmarkDatasetVariant::CocoCustom, CoconutValidation::CoconutStock};
  compile_benchmark_recipe(config, enhanced ? &coconut : nullptr, enhanced ? nullptr : &custom);
  CHECK(server.requests() == 0);
  struct stat status{};
  REQUIRE(::stat(jpeg.c_str(), &status) == 0);
  if (step == 0) {
   inode = status.st_ino;
   mtime = std::filesystem::last_write_time(jpeg);
  } else {
   CHECK(status.st_ino == inode);
   CHECK(std::filesystem::last_write_time(jpeg) == mtime);
  }
  if (enhanced) {
   const auto train = file_bytes(config.output_dir / "train.bin");
   if (coconut_train.empty())
    coconut_train = train;
   else
    CHECK(train == coconut_train);
   CHECK(read_json_file(config.output_dir / "benchmark_manifest.json")["recipe"]["dataset"] == "coconut");
  } else {
   check_custom_compilation(config.output_dir);
   CHECK(std::filesystem::is_regular_file(image_root / ".complete.json"));
   if (custom_train.empty()) {
    custom_train = file_bytes(config.output_dir / "train.bin");
    custom_val = file_bytes(config.output_dir / "val.bin");
   } else {
    CHECK(file_bytes(config.output_dir / "train.bin") == custom_train);
    CHECK(file_bytes(config.output_dir / "val.bin") == custom_val);
   }
  }
 }
 // Both proof families coexist after selection switches. Each recipe repairs
 // the same damaged member through the shared source lease and invalidates both.
 for (const bool enhanced : {false, true}) {
  const auto bad = std::string(std::filesystem::file_size(jpeg), 'x');
  mmltk::testsupport::write_text_file(jpeg, bad);
  config.selection = {enhanced ? BenchmarkDatasetVariant::Coconut : BenchmarkDatasetVariant::CocoCustom, CoconutValidation::CoconutStock};
  compile_benchmark_recipe(config, enhanced ? &coconut : nullptr, enhanced ? nullptr : &custom);
  CHECK(server.requests() == 0);
  if (enhanced) {
   CHECK_FALSE(std::filesystem::exists(image_root / ".complete.json"));
   CHECK(file_bytes(config.output_dir / "train.bin") == coconut_train);
  } else {
   CHECK(std::filesystem::is_empty(image_root / ".recipe-proofs"));
   check_custom_compilation(config.output_dir);
  }
 }
 config.selection = {BenchmarkDatasetVariant::CocoCustom, CoconutValidation::CoconutStock};
 compile_benchmark_recipe(config, nullptr, &custom);
 check_custom_compilation(config.output_dir);
 const auto published_manifest = file_bytes(config.output_dir / "benchmark_manifest.json");
 SECTION("cancellation retains the complete previous custom publication") {
  std::atomic<bool> stop{false};
  config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(stop);
  config.progress = [&](const auto& value) {
   if (value.phase == DatasetCompilePhase::Publishing) stop = true;
  };
  CHECK_THROWS(compile_benchmark_recipe(config, nullptr, &custom));
 }
 SECTION("required validation failure retains the complete previous custom publication") {
  // Force a required decode-probe failure with intact archive structure.
  auto& val = *std::ranges::find(local.catalog.images, CoconutImageNamespace::CocoValidation, &RecipeImageArchive::source);
  const auto val_archive = local.cache.source_downloads("coco") / val.artifact.filename;
  const std::array<std::pair<std::string, std::string>, 1> invalid{{{coco(9, CoconutImageNamespace::CocoValidation).member, "not a JPEG"}}};
  tar(val_archive, invalid);
  custom.coco_val_images.expected_size = std::filesystem::file_size(val_archive);
  mmltk::testsupport::write_text_file(cached_image_path(local.cache.source_images("coco") / "val2017", 9), "broken");
  CHECK_THROWS(compile_benchmark_recipe(config, nullptr, &custom));
 }
 CHECK(file_bytes(config.output_dir / "train.bin") == custom_train);
 CHECK(file_bytes(config.output_dir / "val.bin") == custom_val);
 CHECK(file_bytes(config.output_dir / "benchmark_manifest.json") == published_manifest);
 server.Check();
}
TEST_CASE("custom archive structural recovery preserves JPEGs from other selections", "[benchmark][coconut][download]") {
 ScopedTempDir root("custom-shared-archive-recovery");
 LocalCoconutRecipe local(root.path());
 auto custom = local_custom_catalog(local);
 auto& artifact = custom.objects_images.at(32);
 const auto archive = local.cache.source_downloads("objects365") / artifact.filename;
 const auto pristine = file_bytes(archive);
 const std::string malformed(pristine.size(), 'x');
 std::vector<std::uint8_t> payload(pristine.begin(), pristine.end());
 mmltk::backend::data::testsupport::HttpServer server(payload);
 artifact.url = server.url("custom-structural");
 auto coconut = local.selected(CoconutValidation::CoconutStock);
 std::ranges::find(coconut.images, CoconutImageNamespace::Objects365V2, &RecipeImageArchive::source)->artifact.url = artifact.url;
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 compile_benchmark_recipe(config, &coconut);
 config.selection.dataset = BenchmarkDatasetVariant::CocoCustom;
 compile_benchmark_recipe(config, nullptr, &custom);
 check_custom_compilation(config.output_dir);
 const auto train = file_bytes(config.output_dir / "train.bin"), val = file_bytes(config.output_dir / "val.bin");
 REQUIRE(server.requests() == 0);
 const auto manifest = file_bytes(config.output_dir / "benchmark_manifest.json");
 const auto images = local.cache.source_images("objects365") / "patch-32";
 const auto retained = cached_image_path(images, 2);
 const auto retained_bytes = file_bytes(retained);
 struct stat before{};
 REQUIRE(::stat(retained.c_str(), &before) == 0);
 const auto retained_time = std::filesystem::last_write_time(retained);
 REQUIRE_FALSE(std::filesystem::is_empty(images / ".recipe-proofs"));
 bool exhausted = false, cancelled = false;
 SECTION("successful retry retains unrelated bytes and allocation") {}
 SECTION("all three attempts fail without replacing publication") {
  exhausted = true;
  std::ranges::copy(malformed, payload.begin());
 }
 SECTION("cancellation after structural diagnosis retains publication") { cancelled = true; }
 // Remove only the selected JPEG/proof; the other recipe's proof and JPEG
 // remain until real archive failure invalidates shared completion evidence.
 std::filesystem::remove(cached_image_path(images, 1));
 std::filesystem::remove(images / ".complete.json");
 mmltk::testsupport::write_text_file(archive, malformed);
 std::atomic<bool> stop{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(stop);
 unsigned retries = 0, diagnoses = 0;
 config.trace = [&](std::string_view event, std::string_view fields) {
  const auto facts = Json::parse(fields);
  if (event == "benchmark.download.failure_sha256" && facts.value("artifact", std::string{}) == artifact.artifact_id) ++diagnoses;
  if (event == "benchmark.images.archive_retry" && facts.value("source", std::string{}) == "objects365") {
   ++retries;
   if (cancelled) stop = true;
  }
 };
 if (exhausted || cancelled)
  CHECK_THROWS(compile_benchmark_recipe(config, nullptr, &custom));
 else {
  compile_benchmark_recipe(config, nullptr, &custom);
  check_custom_compilation(config.output_dir);
 }
 CHECK(diagnoses == (exhausted ? 3U : 1U));
 CHECK(retries == (exhausted ? 2U : 1U));
 CHECK(server.requests() == (cancelled ? 0U : exhausted ? 2U : 1U));
 CHECK(std::filesystem::is_empty(images / ".recipe-proofs"));
 CHECK(file_bytes(retained) == retained_bytes);
 struct stat after{};
 REQUIRE(::stat(retained.c_str(), &after) == 0);
 CHECK(after.st_ino == before.st_ino);
 CHECK(std::filesystem::last_write_time(retained) == retained_time);
 CHECK(file_bytes(config.output_dir / "train.bin") == train);
 CHECK(file_bytes(config.output_dir / "val.bin") == val);
 if (exhausted || cancelled) CHECK(file_bytes(config.output_dir / "benchmark_manifest.json") == manifest);
 server.Check();
}
TEST_CASE("one physical admission budget governs membership extraction and writer recovery identities", "[coconut][benchmark][download]") {
 ScopedTempDir root("coconut-unified-recovery");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 ServedPhysicalArchive served(local.cache, catalog, CoconutImageNamespace::CocoTrain, "unified-recovery");
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 compile_benchmark_recipe(config, &catalog);
 REQUIRE(served.server.requests() == 0);
 const auto old_train = file_bytes(config.output_dir / "train.bin"), old_val = file_bytes(config.output_dir / "val.bin"), old_manifest = file_bytes(config.output_dir / "benchmark_manifest.json");
 const auto component_path = local.cache.source_indexes("coconut-fixture-base") / "coco-train2017.normalized.bin";
 const auto old_component = read_json_file(component_path.string() + ".complete.json");
 const auto old_input = old_component["coconut"]["input_identity"].get<std::string>();
 const auto unrelated = cached_image_path(local.cache.source_images("objects365") / "patch-32", 1);
 struct stat before{};
 REQUIRE(::stat(unrelated.c_str(), &before) == 0);
 const auto unrelated_time = std::filesystem::last_write_time(unrelated);
 const auto unrelated_proofs = local.cache.source_images("objects365") / "patch-32/.recipe-proofs";
 std::vector<std::pair<std::filesystem::path, std::string>> proofs;
 for (const auto& proof : std::filesystem::directory_iterator(unrelated_proofs)) proofs.emplace_back(proof.path(), file_bytes(proof.path()));
 const auto jpeg = cached_image_path(local.cache.source_images("coco") / "train2017", 7);
 const std::string corrupt_jpeg(white_jpeg().size(), 'x');
 const auto replace_archive = [&](unsigned id, const std::string& bytes) {
  const std::array<std::pair<std::string, std::string>, 1> members{{{coco(id).member, bytes}}};
  tar(served.path, members);
  REQUIRE(std::filesystem::file_size(served.path) == served.payload.size());
 };
 bool exhausted = false, writer_recovery = false;
 SECTION("selected JPEG extraction refreshes physical and dependent identities") {
  replace_archive(7, corrupt_jpeg);
  std::filesystem::remove(jpeg);
 }
 SECTION("writer repair shares the same owner and refreshes dependent identities") {
  writer_recovery = true;
  replace_archive(7, corrupt_jpeg);
  mmltk::testsupport::write_text_file(jpeg, corrupt_jpeg);
 }
 SECTION("membership repair leaves only one further physical recovery admission") {
  exhausted = true;
  replace_archive(7, corrupt_jpeg);
  const auto bad = file_bytes(served.path);
  std::ranges::copy(bad, served.payload.begin());
  replace_archive(6, white_jpeg());
  std::filesystem::remove(jpeg);
  std::filesystem::remove(component_path.string() + ".complete.json");
 }
 std::filesystem::remove(local.cache.source_indexes("coco") / (served.source.artifact.artifact_id + ".inventory.bin"));
 std::vector<BenchmarkCompileProgress> observations;
 config.progress = [&](const BenchmarkCompileProgress& value) { observations.push_back(value); };
 unsigned admissions = 0;
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event != "benchmark.download.cache_hit" && event != "benchmark.download.preseeded" && event != "benchmark.download.complete") return;
  if (Json::parse(fields).value("artifact", std::string{}) == served.source.artifact.artifact_id) ++admissions;
 };
 if (exhausted) {
  CHECK_THROWS_WITH(compile_benchmark_recipe(config, &catalog),
   "physical archive remains unavailable after three admissions: " + served.source.artifact.artifact_id + ": selected archive image 7 is not a complete JPEG or PNG");
  CHECK(admissions == 3);
  CHECK(served.server.requests() == 2);
  CHECK(file_bytes(config.output_dir / "benchmark_manifest.json") == old_manifest);
 } else {
  compile_benchmark_recipe(config, &catalog);
  CHECK(admissions == 2);
  REQUIRE_FALSE(observations.empty());
  CHECK(std::ranges::all_of(observations.back().sources, &BenchmarkSourceProgress::complete));
  if (writer_recovery) {
   const auto invalidated = std::ranges::find_if(observations, [](const auto& value) { return value.tracks.labels.invalidated != 0; });
   REQUIRE(invalidated != observations.end());
   REQUIRE(invalidated != observations.begin());
   const auto& previous_labels = (invalidated - 1)->tracks.labels;
   CHECK(invalidated->tracks.labels.completed + invalidated->tracks.labels.invalidated == previous_labels.completed);
   const auto replacement = std::ranges::find_if(invalidated, observations.end(), [&](const auto& value) { return value.tracks.labels.total == previous_labels.total && value.tracks.labels.active; });
   REQUIRE(replacement != observations.end());
   CHECK(replacement->tracks.labels.completed < replacement->tracks.labels.total);
   CHECK(observations.back().tracks.labels.completed == previous_labels.completed);
   const auto base_rows = std::ranges::find(catalog.releases, CoconutEdition::Base, &CoconutReleaseComponent::edition)->expected_rows;
   const auto rows_withdrawn =
    std::ranges::find_if(invalidated + 1, observations.end(), [&](const auto& value) { return value.tracks.labels.invalidated == invalidated->tracks.labels.invalidated + base_rows; });
   REQUIRE(rows_withdrawn != observations.end());
   const auto& retained = (rows_withdrawn - 1)->tracks.labels;
   CHECK(rows_withdrawn->tracks.labels.completed + base_rows == retained.completed);
   CHECK(rows_withdrawn->tracks.labels.total == retained.total);
   CHECK(rows_withdrawn->tracks.labels.active);
   CHECK(rows_withdrawn->tracks.labels.activity == DatasetCompileActivity::Normalizing);
   CHECK(rows_withdrawn->tracks.labels.completed > 0);
   CHECK(observations.back().tracks.labels.invalidated == invalidated->tracks.labels.invalidated + base_rows);
  }
  CHECK(served.server.requests() == 1);
  const auto completion = read_json_file(component_path.string() + ".complete.json");
  const auto input = completion["coconut"]["input_identity"].get<std::string>();
  CHECK(input != old_input);
  const auto component = load_coconut_component(component_path, CoconutEdition::Base, CoconutImageNamespace::CocoTrain, input);
  REQUIRE(component);
  REQUIRE(component->inventory().size() == 1);
  const auto manifest = read_json_file(config.output_dir / "benchmark_manifest.json");
  const auto physical = std::ranges::find_if(manifest["recipe"]["artifacts"], [&](const auto& item) { return item["artifact_id"] == served.source.artifact.artifact_id; });
  REQUIRE(physical != manifest["recipe"]["artifacts"].end());
  CHECK(component->inventory().front().physical.archive_identity == physical->at("identity").get<std::string>());
  const auto facts = std::ranges::find_if(manifest["recipe"]["components"], [](const auto& item) { return item["physical_source"] == "coco-train2017"; });
  REQUIRE(facts != manifest["recipe"]["components"].end());
  CHECK(facts->at("input_identity") == input);
  const auto train = CompiledDataset::open(config.output_dir / "train.bin");
  CHECK(train.image_entry(0).source_image_id == 7);
  for (unsigned pixel = 0; pixel < 27; ++pixel) CHECK(train.image_pixels(0)[pixel] == 1.0F);
 }
 CHECK(file_bytes(config.output_dir / "train.bin") == old_train);
 CHECK(file_bytes(config.output_dir / "val.bin") == old_val);
 struct stat after{};
 REQUIRE(::stat(unrelated.c_str(), &after) == 0);
 CHECK(after.st_ino == before.st_ino);
 CHECK(std::filesystem::last_write_time(unrelated) == unrelated_time);
 for (const auto& [path, bytes] : proofs) CHECK(file_bytes(path) == bytes);
 served.server.Check();
}
TEST_CASE("one admitted physical membership lookup serves every release without revalidation", "[coconut]") {
 ScopedTempDir root("coconut-shared-membership");
 LocalCoconutRecipe local(root.path());
 std::vector<CoconutPhysicalImage> physical;
 for (const auto& archive : local.catalog.images) {
  auto rows = coconut_image_archive_inventory(
   local.cache.source_downloads(benchmark_source_name(archive.artifact.source)) / archive.artifact.filename, {}, archive.source, archive.shard, archive.artifact.artifact_id);
  physical.insert(physical.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
 }
 PollCancellation construction;
 const CoconutPhysicalMembership membership(physical, mmltk::common::concurrency::CancellationObservation::Borrow(construction));
 REQUIRE(construction.polls == physical.size());
 const auto construction_polls = construction.polls;
 // Any repeated use of the admission's cancellation observation now fails.
 construction.stop_at = construction.polls;
 const auto* v1 = membership.find(CoconutImageNamespace::Objects365V1, 1);
 const auto* v2 = membership.find(CoconutImageNamespace::Objects365V2, 1);
 REQUIRE(v1);
 REQUIRE(v2);
 CHECK(v1 != v2);
 std::vector<CoconutComponent> components;
 for (const auto& release : local.catalog.releases) {
  CoconutImportRequest input;
  input.edition = release.edition;
  input.input_identity = "one-shared-generation";
  input.physical_membership = &membership;
  input.expected_rows = release.expected_rows;
  for (const auto& artifact : release.annotations) {
   const auto path = local.cache.source_downloads("coconut-" + std::string(release.name)) / artifact.filename;
   if (path.extension() == ".parquet")
    input.parquet_shards.push_back(path);
   else if (path.extension() == ".json")
    input.annotation_json = path;
   else
    input.mask_archive = path;
  }
  auto imported = import_coconut_annotations(input);
  components.insert(components.end(), std::make_move_iterator(imported.begin()), std::make_move_iterator(imported.end()));
  CHECK(construction.polls == construction_polls);
  CHECK(membership.find(CoconutImageNamespace::Objects365V1, 1) == v1);
  CHECK(membership.find(CoconutImageNamespace::Objects365V2, 1) == v2);
 }
 CHECK(reconcile_coconut_extensions(components) == 1);
 for (std::size_t cut = 0; cut < construction_polls; ++cut) {
  PollCancellation cancelled;
  cancelled.stop_at = cut;
  CHECK_THROWS(CoconutPhysicalMembership(physical, mmltk::common::concurrency::CancellationObservation::Borrow(cancelled)));
 }
 auto replacement = physical;
 replacement.front().archive_identity = "replacement-generation";
 PollCancellation rebuilt;
 const CoconutPhysicalMembership next(replacement, mmltk::common::concurrency::CancellationObservation::Borrow(rebuilt));
 CHECK(rebuilt.polls == construction_polls);
 CHECK(next.find(replacement.front().source, replacement.front().image_id) == &replacement.front());
 CHECK(membership.find(physical.front().source, physical.front().image_id) == &physical.front());
}
TEST_CASE("both stock recipes reuse normalized indexes without raw metadata or network", "[benchmark][coconut][cache]") {
 ScopedTempDir root("stock-normalized-only");
 LocalCoconutRecipe local(root.path());
 auto custom = local_custom_catalog(local);
 auto coconut = local.selected(CoconutValidation::Stock);
 const auto raw = local.cache.source_downloads("coco") / custom.coco_annotations.filename;
 remove_cache_path(raw);
 remove_cache_path(raw.string() + ".download.json");
 remove_cache_path(local.cache.source_indexes("coco") / "source-json");
 const auto index = local.cache.source_indexes("coco") / "val2017.normalized.bin";
 const auto original_index = file_bytes(index);
 const auto index_time = std::filesystem::last_write_time(index);
 {
  BenchmarkTraceSink trace;
  ProgressReporter progress({}, trace);
  CocoAnnotationCache stock(local.cache, coconut.stock_annotations, {CocoSplitAdmission::Unselected, CocoSplitAdmission::Required}, 0, 1, 1, {}, trace);
  stock.discover(progress);
  CHECK_FALSE(stock.pending_download());
  const auto admitted = stock.take_indexes();
  CHECK(admitted.cache_hit);
  CHECK(admitted.retained_storage_bytes == std::filesystem::file_size(index) + std::filesystem::file_size(index.string() + ".complete.json"));
 }
 auto config = local.compiler_config({}, true);
 std::array<std::string, 2> trains, validations;
 for (unsigned pass = 0; pass < 4; ++pass) {
  const auto enhanced = pass % 2;
  config.selection = {enhanced ? BenchmarkDatasetVariant::Coconut : BenchmarkDatasetVariant::CocoCustom, CoconutValidation::Stock};
  compile_benchmark_recipe(config, enhanced ? &coconut : nullptr, enhanced ? nullptr : &custom);
  const auto train = file_bytes(config.output_dir / "train.bin"), val = file_bytes(config.output_dir / "val.bin");
  if (pass < 2) {
   trains[enhanced] = train;
   validations[enhanced] = val;
  } else {
   CHECK(train == trains[enhanced]);
   CHECK(val == validations[enhanced]);
  }
  CHECK(file_bytes(index) == original_index);
  CHECK(std::filesystem::last_write_time(index) == index_time);
  CHECK_FALSE(std::filesystem::exists(raw));
  CHECK_FALSE(std::filesystem::exists(local.cache.source_indexes("coco") / "source-json"));
 }
}
TEST_CASE("stock annotation owner repairs only missing splits with bounded source attempts", "[benchmark][coconut][download]") {
 ScopedTempDir root("stock-source-recovery");
 LocalCoconutRecipe local(root.path());
 auto custom = local_custom_catalog(local);
 const auto train_path = local.cache.source_indexes("coco") / "train2017.normalized.bin";
 const auto train_bytes = file_bytes(train_path), train_metadata = file_bytes(train_path.string() + ".complete.json");
 const auto train_time = std::filesystem::last_write_time(train_path);
 const auto val_path = local.cache.source_indexes("coco") / "val2017.normalized.bin";
 remove_normalized_annotation_index(val_path);
 auto artifact = custom.coco_annotations;
 const auto raw = local.cache.source_downloads("coco") / artifact.filename;
 bool succeeds = true;
 SECTION("same-size malformed archive is replaced once") {
  // Local HTTP serves the original archive; the admitted cache copy is damaged below.
 }
 SECTION("missing required member exhausts the three-body budget") {
  const std::array<std::pair<std::string, std::string>, 1> rows{{{"annotations/unrelated.json", "{}"}}};
  tar(raw, rows);
  succeeds = false;
 }
 SECTION("malformed required JSON exhausts the three-body budget") {
  const std::array<std::pair<std::string, std::string>, 1> rows{{{"annotations/instances_val2017.json", "not JSON"}}};
  tar(raw, rows);
  succeeds = false;
 }
 const auto served = file_bytes(raw);
 mmltk::backend::data::testsupport::HttpServer server(served);
 artifact.url = server.url("stock-repair");
 artifact.expected_size = served.size();
 if (succeeds) mmltk::testsupport::write_text_file(raw, std::string(served.size(), 'x'));
 unsigned diagnoses = 0;
 BenchmarkTraceSink trace = [&](std::string_view event, const Json&) {
  if (event == "benchmark.download.failure_sha256") ++diagnoses;
 };
 ProgressReporter progress({}, trace);
 CocoAnnotationCache annotations(local.cache, artifact, {CocoSplitAdmission::Required, CocoSplitAdmission::Required}, 2, 1, 1, {}, trace);
 annotations.discover(progress);
 REQUIRE(annotations.pending_download());
 CHECK(annotations.completed_indexes() == 1);
 auto acquired = download_artifacts({*annotations.pending_download()}, 1, {}).front();
 CHECK(server.requests() == 0);
 std::uint64_t completed = 1;
 if (succeeds) {
  annotations.settle(std::move(acquired), progress, 1, completed, 2);
  auto indexes = annotations.take_indexes();
  REQUIRE(indexes.train);
  REQUIRE(indexes.validation);
  CHECK(indexes.train->images.size() == 2);
  CHECK(indexes.validation->images.size() == 1);
  CHECK_FALSE(indexes.cache_hit);
  CHECK(completed == 2);
  CHECK(server.requests() == 1);
  CHECK(diagnoses == 1);
 } else {
  CHECK_THROWS(annotations.settle(std::move(acquired), progress, 1, completed, 2));
  CHECK(server.requests() == 2);
  CHECK(diagnoses == 2);
  CHECK(completed == 1);
  CHECK_FALSE(std::filesystem::exists(val_path.string() + ".complete.json"));
 }
 CHECK(file_bytes(train_path) == train_bytes);
 CHECK(file_bytes(train_path.string() + ".complete.json") == train_metadata);
 CHECK(std::filesystem::last_write_time(train_path) == train_time);
 server.Check();
}
TEST_CASE("stock annotation cancellation preserves the previous recipe publication", "[benchmark][coconut][cache]") {
 ScopedTempDir root("stock-cancel-publication");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::Stock);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Stock}, true);
 compile_benchmark_recipe(config, &catalog);
 const auto train = file_bytes(config.output_dir / "train.bin"), val = file_bytes(config.output_dir / "val.bin");
 const auto manifest = file_bytes(config.output_dir / "benchmark_manifest.json");
 const auto index = local.cache.source_indexes("coco") / "val2017.normalized.bin";
 remove_normalized_annotation_index(index);
 std::atomic<bool> cancel{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancel);
 config.progress = [&](const auto& value) {
  if (value.activity == "Parsing and indexing COCO validation annotations") cancel = true;
 };
 CHECK_THROWS(compile_benchmark_recipe(config, &catalog));
 CHECK(cancel.load());
 CHECK_FALSE(std::filesystem::exists(index.string() + ".complete.json"));
 check_publication_bytes(config.output_dir, train, val, manifest);
}
TEST_CASE("both stock recipes repair malformed admitted archives through their production entry", "[benchmark][coconut][download]") {
 ScopedTempDir root("stock-recipe-repair");
 LocalCoconutRecipe local(root.path());
 auto custom = local_custom_catalog(local);
 auto coconut = local.selected(CoconutValidation::Stock);
 bool enhanced = false;
 SECTION("Coco custom") {}
 SECTION("Coconut Stock") { enhanced = true; }
 const auto val = local.cache.source_indexes("coco") / "val2017.normalized.bin";
 remove_normalized_annotation_index(val);
 const auto raw = local.cache.source_downloads("coco") / custom.coco_annotations.filename;
 const auto served = file_bytes(raw);
 mmltk::backend::data::testsupport::HttpServer server(served);
 custom.coco_annotations.url = server.url("stock-production");
 coconut.stock_annotations.url = custom.coco_annotations.url;
 mmltk::testsupport::write_text_file(raw, std::string(served.size(), 'x'));
 auto config = local.compiler_config({enhanced ? BenchmarkDatasetVariant::Coconut : BenchmarkDatasetVariant::CocoCustom, CoconutValidation::Stock}, false);
 compile_benchmark_recipe(config, enhanced ? &coconut : nullptr, enhanced ? nullptr : &custom);
 const auto compiled = CompiledDataset::open(config.output_dir / "val.bin");
 REQUIRE(compiled.image_entries().size() == 1);
 CHECK(compiled.image_entry(0).source_image_id == 9);
 CHECK(compiled.image_entry(0).source == AnnotationSource::Coco);
 CHECK(server.requests() == 1);
 CHECK(file_bytes(raw) == served);
 server.Check();
}
TEST_CASE("one stock request builds both splits and retains newly settled train during validation repair", "[benchmark][coconut][download]") {
 ScopedTempDir root("stock-both-splits");
 LocalCoconutRecipe local(root.path());
 auto custom = local_custom_catalog(local);
 const auto train = local.cache.source_indexes("coco") / "train2017.normalized.bin";
 const auto val = local.cache.source_indexes("coco") / "val2017.normalized.bin";
 remove_normalized_annotation_index(train);
 remove_normalized_annotation_index(val);
 const auto raw = local.cache.source_downloads("coco") / custom.coco_annotations.filename;
 const auto training_json = file_bytes(local.cache.source_indexes("coco") / "train2017.fixture.json");
 const auto validation_json = file_bytes(local.cache.source_indexes("coco") / "val2017.fixture.json");
 const std::array<std::pair<std::string, std::string>, 2> valid{{{"annotations/instances_train2017.json", training_json}, {"annotations/instances_val2017.json", validation_json}}};
 tar(raw, valid);
 const auto served = file_bytes(raw);
 mmltk::backend::data::testsupport::HttpServer server(served);
 auto malformed = valid;
 malformed[1].second = "not JSON";
 tar(raw, malformed);
 custom.coco_annotations.expected_size = 0;
 custom.coco_annotations.url = server.url("both-stock-splits");
 BenchmarkTraceSink trace;
 std::string settled_train, settled_metadata;
 ProgressReporter progress([&](const auto& update) {
  if (update.activity == "Redownloading annotation metadata after structural validation failure" && settled_train.empty()) {
   settled_train = file_bytes(train);
   settled_metadata = file_bytes(train.string() + ".complete.json");
  }
 }, trace);
 CocoAnnotationCache cache(local.cache, custom.coco_annotations, {CocoSplitAdmission::Required, CocoSplitAdmission::Required}, 2, 1, 1, {}, trace);
 cache.discover(progress);
 CHECK(cache.completed_indexes() == 0);
 REQUIRE(cache.pending_download());
 auto acquired = download_artifacts({*cache.pending_download()}, 1, {}).front();
 CHECK(server.requests() == 0);
 std::uint64_t completed = 0;
 cache.settle(std::move(acquired), progress, 1, completed, 2);
 auto indexes = cache.take_indexes();
 REQUIRE(indexes.train);
 REQUIRE(indexes.validation);
 REQUIRE_FALSE(settled_train.empty());
 CHECK(file_bytes(train) == settled_train);
 CHECK(file_bytes(train.string() + ".complete.json") == settled_metadata);
 CHECK(indexes.train->images.size() == 2);
 CHECK(indexes.validation->images.size() == 1);
 CHECK(completed == 2);
 CHECK(server.requests() == 1);
 server.Check();
}
TEST_CASE("a retained extracted stock member shares the remaining sibling walk", "[benchmark][coconut][cache]") {
 ScopedTempDir root("stock-retained-extraction");
 LocalCoconutRecipe local(root.path());
 auto catalog = local_custom_catalog(local);
 const auto indexes = local.cache.source_indexes("coco");
 remove_normalized_annotation_index(indexes / "train2017.normalized.bin");
 remove_normalized_annotation_index(indexes / "val2017.normalized.bin");
 const auto raw = local.cache.source_downloads("coco") / catalog.coco_annotations.filename;
 const std::array<std::pair<std::string, std::string>, 2> members{{
  {"annotations/instances_train2017.json", file_bytes(indexes / "train2017.fixture.json")},
  {"annotations/instances_val2017.json", file_bytes(indexes / "val2017.fixture.json")}}};
 tar(raw, members);
 catalog.coco_annotations.expected_size = std::filesystem::file_size(raw);
 BenchmarkTraceSink trace;
 ProgressReporter progress({}, trace);
 CocoAnnotationCache cache(local.cache, catalog.coco_annotations, {CocoSplitAdmission::Required, CocoSplitAdmission::Required}, 2, 1, 1, {}, trace);
 cache.discover(progress);
 REQUIRE(cache.pending_download());
 auto acquired = download_artifacts({*cache.pending_download()}, 1, {}).front();
 const auto extracted = indexes / "source-json/instances_train2017.json";
 (void)extract_archive_member(raw, members[0].first, extracted, acquired.identity, local.cache.locks / "fixture-extraction.lock", {}, trace);
 struct stat before{}, after{};
 REQUIRE(::stat(extracted.c_str(), &before) == 0);
 std::uint64_t completed = 0;
 cache.settle(std::move(acquired), progress, 1, completed, 2);
 const auto admitted = cache.take_indexes();
 REQUIRE(admitted.train); REQUIRE(admitted.validation);
 CHECK(completed == 2);
 REQUIRE(::stat(extracted.c_str(), &after) == 0);
 CHECK(before.st_ino == after.st_ino);
}
TEST_CASE("COCONut recovers the complete dropped dog candidate set and carves source supporters", "[coconut][benchmark]") {
 ScopedTempDir root("coconut-recovery");
 unsigned image_id = 2212;
 unsigned supporter_category = 63;
 bool enabled = true;
 bool empty_supporter = false;
 std::vector<RLEPair> masks{{0, 2}, {1, 2}};  // Independent originals intentionally overlap.
 SECTION("two dogs on couch") {}
 SECTION("dog on boat") {
  image_id = 400;
  supporter_category = 9;
  masks.resize(1);
 }
 SECTION("recovery off retains the previous omission and metadata") { enabled = false; }
 SECTION("an emptied supporter is omitted without recursive recovery") { empty_supporter = true; }
 auto originals = recovery_originals(image_id, masks);
 const auto indexed_originals_view = fixture_index(originals);
 CoconutRecoveryOriginals indexed_originals(&indexed_originals_view, nullptr);
 CoconutMaskRecovery recovery(indexed_originals);
 auto couch = segment(30, supporter_category);
 couch["area"] = 99;
 Json segments = Json::array({segment(20, 18), couch});
 if (masks.size() == 2) segments.push_back(segment(10, 18));
 std::array<std::uint32_t, 9> pixels{30, 30, 30, 30, 30, 30, 30, 30, 30};
 if (empty_supporter) pixels = {30, 30, 30, 0, 0, 0, 0, 0, 0};
 const std::array physical{coco(image_id)};
 auto input = request(physical);
 input.parquet_shards = {root.path() / "dogs.parquet"};
 input.recovery = enabled ? &recovery : nullptr;
 std::uint64_t rejected = 0;
 input.rejected_object = [&](const auto&, const auto&, const auto&, auto) { ++rejected; };
 parquet_file(input.parquet_shards[0], Json::array({hf_row(image_id, png(3, 3, pixels), segments)}));
 const auto result = import_coconut_annotations(input);
 REQUIRE(result.size() == 1);
 const auto& component = result.front();
 REQUIRE(component.index().image_count() == 1);
 CHECK(component.index().images()[0].source_image_id == image_id);
 if (!enabled) {
  REQUIRE(component.index().box_count() == 1);
  CHECK(component.index().storage().boxes[0].annotation_id == 30);
  CHECK(component.index().storage().boxes[0].original_area == 99);
  CHECK(rejected == masks.size());
  CHECK(component.recovery().empty());
  const std::array<RLEPair, 1> unchanged{{{0, 9}}};
  expect_runs(component, unchanged);
  return;
 }
 REQUIRE(component.recovery().size() == 1);
 const auto& facts = component.recovery().front();
 CHECK(component.recovery_policy() == kCoconutRecoveryPolicy);
 CHECK(component.original_annotation_identity() == originals.annotation_sha256);
 CHECK(facts.image_id == image_id);
 CHECK(facts.unresolved == (empty_supporter ? 1U : 0U));
 CHECK(rejected == facts.unresolved);
 REQUIRE(facts.objects.size() == masks.size());
 CHECK(facts.objects.front().annotation_id == (masks.size() == 2 ? 10 : 20));
 CHECK(facts.objects.front().original_annotation_id == (masks.size() == 2 ? 100 : 200));
 REQUIRE(component.index().box_count() == masks.size() + (empty_supporter ? 0 : 1));
 for (const auto& box : component.index().storage().boxes) {
  const auto runs = std::span(component.index().storage().mask_rle_pairs).subspan(box.mask_rle_offset, box.mask_rle_pairs);
  REQUIRE(runs.size() == 1);
  if (box.annotation_id == 30) {
   CHECK(box.original_area == (masks.size() == 2 ? 6 : 7));
   CHECK(runs[0].start == (masks.size() == 2 ? 3 : 2));
   CHECK(runs[0].start + runs[0].length == 9);
   CHECK(box.y1 == (masks.size() == 2 ? 1.0F / 3.0F : 0.0F));
  } else {
   CHECK(box.source_category_id == 18);
   CHECK(box.x1 == 0.1F);
   CHECK(box.y1 == 0.2F);
   CHECK(box.x2 == 0.8F);
   CHECK(box.y2 == 0.9F);
   CHECK(box.source_ordinal == (box.annotation_id == 20 ? 0 : 2));
   CHECK(box.original_area == (box.annotation_id == 10 ? 43 : 42));
   CHECK(runs[0].start == (box.annotation_id == 10 ? 1 : 0));
   CHECK(runs[0].length == 2);
  }
 }
 const auto cache = root.path() / "recovery.normalized.bin";
 store_coconut_component(cache, component);
 const auto cached = load_coconut_component(cache, component.edition(), component.source(), component.input_identity());
 REQUIRE(cached);
 CHECK(cached->recovery().front().unresolved == facts.unresolved);
 CHECK(cached->recovery().front().omissions.size() == facts.unresolved);
 CHECK(cached->index().annotation_sha256 == component.index().annotation_sha256);
 CHECK(std::ranges::equal(cached->inventory(), component.inventory()));
 const auto roundtrip = root.path() / "recovery-roundtrip.normalized.bin";
 store_coconut_component(roundtrip, *cached);
 for (const auto suffix : {"", ".inventory", ".complete.json"}) CHECK(file_bytes(cache.string() + suffix) == file_bytes(roundtrip.string() + suffix));
 const auto settled = file_bytes(cache);
 std::atomic<bool> cancelled{true};
 CHECK_THROWS(store_coconut_component(cache, component, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled)));
 CHECK(file_bytes(cache) == settled);
 const auto failed = root.path() / "failed.normalized.bin";
 const auto failed_completion = std::filesystem::path(failed.string() + ".complete.json");
 mmltk::testsupport::write_text_file(failed_completion / "blocker", "retain");
 CHECK_THROWS(store_coconut_component(failed, component));
 CHECK_FALSE(std::filesystem::is_regular_file(failed_completion));
 CHECK_FALSE(load_coconut_component(failed, component.edition(), component.source(), component.input_identity()));
 auto malformed = component_builder(component);
 malformed.recovery.front().objects.front().original_annotation_id += 1;
 CHECK_THROWS(std::move(malformed).finish({}, root.path()));
 auto invalid_join = component_builder(component);
 invalid_join.index.annotation_sha256.clear();
 ++invalid_join.recovery.front().objects.front().source_ordinal;
 CHECK_THROWS(std::move(invalid_join).finish({}, root.path()));
 CHECK(file_bytes(cache) == settled);
}
TEST_CASE("COCONut recovery requires a unique represented match and exact remaining candidate count", "[coconut][benchmark]") {
 const std::array<RLEPair, 2> masks{{{0, 2}, {3, 2}}};
 auto originals = recovery_originals(7, masks);
 CoconutRecord record;
 record.image_id = 7;
 record.first_segment_ordinal = 5;
 record.segments = {
  {.id = 20, .category_id = 18, .isthing = true}, {.id = 10, .category_id = 18, .isthing = true}, {.id = 30, .category_id = 63, .isthing = true, .bbox = std::array<double, 4>{-1, 0, 5, 3}}
 };
 const std::array<RLEPair, 1> whole{{{0, 9}}};
 std::vector<CoconutSegmentSupport> support{recovery_support(std::span(masks).first(1)), {}, recovery_support(whole)};
 auto source = CoconutImageNamespace::CocoTrain;
 bool accepted = false;
 bool unavailable = false;
 bool wrong_split = false;
 SECTION("represented dog excludes its original and boxed couch is carved") { accepted = true; }
 SECTION("one survivor intersects both originals") { support[0] = recovery_support(std::array<RLEPair, 1>{{{0, 5}}}); }
 SECTION("survivor has no positive match") { support[0] = recovery_support(std::array<RLEPair, 1>{{{6, 1}}}); }
 SECTION("survivor has bbox but no usable mask") {
  support[0] = {};
  record.segments[0].bbox = std::array<double, 4>{0, 0, 1, 1};
 }
 SECTION("two survivors claim one original") {
  record.segments.push_back({.id = 40, .category_id = 18, .isthing = true});
  support.push_back(support[0]);
  originals.boxes.push_back(originals.boxes.back());
  originals.boxes.back().annotation_id = 300;
  ++originals.images[0].box_count;
 }
 SECTION("unrelated extra original is not recovered") {
  originals.boxes.push_back(originals.boxes.back());
  originals.boxes.back().annotation_id = 300;
  ++originals.images[0].box_count;
 }
 SECTION("duplicate original identity") { originals.boxes[1].annotation_id = originals.boxes[0].annotation_id; }
 SECTION("invalid candidate mask") { originals.mask_rle_pairs[1].length = 100; }
 SECTION("candidate without mask presence") { originals.boxes[1].flags &= ~kAnnotationMask; }
 SECTION("invalid original box") { originals.boxes[1].x2 = originals.boxes[1].x1; }
 SECTION("mismatched dimensions") { originals.images[0].width = 4; }
 SECTION("mismatched physical identity") { originals.images[0].source_image_id = 8; }
 SECTION("duplicate physical identity") { originals.images.push_back(originals.images[0]); }
 SECTION("originals unavailable") { unavailable = true; }
 SECTION("unlabeled namespace cannot use train originals") { source = CoconutImageNamespace::CocoUnlabeled; }
 SECTION("crowd mismatch remains omitted") { originals.boxes[1].flags |= kAnnotationCrowd; }
 SECTION("ignore mismatch remains omitted") { originals.boxes[1].flags |= kAnnotationIgnore; }
 SECTION("matching crowd and ignore group recovers") {
  accepted = true;
  record.segments[1].crowd = record.segments[1].ignore = true;
  originals.boxes[1].flags |= kAnnotationCrowd | kAnnotationIgnore;
 }
 SECTION("an empty mask with an authoritative box is never a dropped slot") { record.segments[1].bbox = std::array<double, 4>{0, 0, 1, 1}; }
 SECTION("validation uses its own original index") {
  accepted = true;
  source = CoconutImageNamespace::CocoValidation;
  originals.split = "val2017";
 }
 SECTION("train rejects validation originals") {
  wrong_split = true;
  originals.split = "val2017";
 }
 SECTION("validation rejects train originals") {
  wrong_split = true;
  source = CoconutImageNamespace::CocoValidation;
 }
 SECTION("train rejects a noncanonical split") {
  wrong_split = true;
  originals.split = "train";
 }
 SECTION("validation rejects a noncanonical split") {
  wrong_split = true;
  source = CoconutImageNamespace::CocoValidation;
  originals.split = "validation";
 }
 const auto unchanged_support = support;
 const auto indexed_originals_view = fixture_index(originals);
 CoconutRecoveryOriginals indexed_originals(unavailable ? nullptr : &indexed_originals_view, &indexed_originals_view);
 CoconutMaskRecovery recovery(indexed_originals);
 CoconutRecoveryImage facts{7, 0, {}};
 recovery.apply(source, record, 3, 3, support, facts);
 CHECK(facts.objects.size() == (accepted ? 1 : 0));
 if (wrong_split) {
  CHECK(recovery.original_identity(source).empty());
  REQUIRE(support.size() == unchanged_support.size());
  for (std::size_t i = 0; i < support.size(); ++i) {
   CHECK(support[i].area == unchanged_support[i].area);
   CHECK(support[i].bounds.min_x == unchanged_support[i].bounds.min_x);
   CHECK(support[i].bounds.min_y == unchanged_support[i].bounds.min_y);
   CHECK(support[i].bounds.max_x == unchanged_support[i].bounds.max_x);
   CHECK(support[i].bounds.max_y == unchanged_support[i].bounds.max_y);
   CHECK_FALSE(support[i].recovered);
   CHECK_FALSE(support[i].carved);
   REQUIRE(support[i].runs.size() == unchanged_support[i].runs.size());
   for (std::size_t run = 0; run < support[i].runs.size(); ++run) {
    CHECK(support[i].runs[run].start == unchanged_support[i].runs[run].start);
    CHECK(support[i].runs[run].length == unchanged_support[i].runs[run].length);
   }
  }
 }
 CHECK(support[1].recovered.has_value() == accepted);
 CHECK(support[2].carved == accepted);
 CHECK(support[0].area == (support[0].runs.empty() ? 0 : support[0].runs[0].length));
 if (accepted) {
  CHECK(recovery.original_identity(source) == originals.annotation_sha256);
  CHECK(facts.objects[0].original_annotation_id == 100);
  CHECK(facts.objects[0].annotation_id == 10);
  CHECK(facts.objects[0].source_ordinal == 6);
  CHECK(support[1].area == 2);
  CHECK(support[2].area == 7);
  REQUIRE(record.segments[2].bbox);
  CHECK((*record.segments[2].bbox)[0] == -1);
 }
}
TEST_CASE("COCONut recovery propagates cancellation before mutating masks", "[coconut][benchmark]") {
 const std::array<RLEPair, 1> masks{{{0, 2}}};
 const auto originals = recovery_originals(7, masks);
 const auto indexed_originals_view = fixture_index(originals);
 CoconutRecoveryOriginals indexed_originals(&indexed_originals_view, nullptr);
 CoconutMaskRecovery recovery(indexed_originals);
 CoconutRecord record;
 record.image_id = 7;
 record.segments = {{.id = 10, .category_id = 18, .isthing = true}};
 std::vector<CoconutSegmentSupport> support(1);
 CoconutRecoveryImage facts{7, 0, {}};
 std::atomic<bool> cancelled{true};
 CHECK_THROWS(recovery.apply(CoconutImageNamespace::CocoTrain, record, 3, 3, support, facts, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled)));
 CHECK(support[0].runs.empty());
 CHECK(facts.objects.empty());
}
TEST_CASE("COCONut recovery reuses bounded image work across run geometry and cancellation", "[coconut][benchmark]") {
 struct Geometry {
  dataset::MaskDimensions dimensions;
  std::vector<RLEPair> supporter, cuts, expected;
  dataset::RowMajorMaskBounds bounds;
 };
 const std::vector<Geometry> cases{
  // One cut spans three separate supporter runs, including a row boundary.
  {{5, 4}, {{1, 3}, {6, 3}, {11, 3}}, {{2, 11}}, {{1, 1}, {13, 1}}, {1, 0, 4, 3, true}},
  // Adjacent recovered cuts coalesce; singleton output crosses neither row.
  {{3, 3}, {{0, 9}}, {{0, 2}, {2, 2}, {5, 1}, {8, 1}}, {{4, 1}, {6, 2}}, {0, 1, 2, 3, true}},
  // Disjoint cuts can touch supporter endpoints without changing its storage.
  {{4, 2}, {{2, 2}, {6, 1}}, {{0, 2}, {4, 2}, {7, 1}}, {{2, 2}, {6, 1}}, {2, 0, 4, 2, true}},
  {{2, 2}, {{0, 1}, {2, 2}}, {{0, 4}}, {}, {}},
  // Cross-row emission must widen x bounds to the complete row.
  {{4, 3}, {{0, 12}}, {{0, 2}, {10, 2}}, {{2, 8}}, {0, 0, 4, 3, true}},
  // Sparse supporter starts beyond an unrelated union prefix.
  {{5, 5}, {{21, 3}}, {{0, 1}, {4, 1}, {8, 1}, {22, 1}}, {{21, 1}, {23, 1}}, {1, 4, 4, 5, true}},
 };
 const auto prototype = recovery_originals(1, std::array<RLEPair, 1>{{{0, 1}}});
 NormalizedAnnotationBuilder originals;
 originals.annotation_sha256 = prototype.annotation_sha256;
 originals.split = prototype.split;
 std::vector<CoconutRecord> records;
 for (std::size_t i = 0; i < cases.size(); ++i) {
  const auto& geometry = cases[i];
  CoconutRecord record;
  record.image_id = i + 1;
  originals.images.push_back(
   {.source_image_id = record.image_id,
    .first_box = originals.boxes.size(),
    .box_count = static_cast<std::uint32_t>(geometry.cuts.size()),
    .width = geometry.dimensions.width,
    .height = geometry.dimensions.height});
  for (std::size_t j = 0; j < geometry.cuts.size(); ++j) {
   auto box = prototype.boxes.front();
   box.annotation_id = j + 100;
   box.source_category_id = j + 18;
   box.mask_rle_offset = originals.mask_rle_pairs.size();
   originals.boxes.push_back(box);
   originals.mask_rle_pairs.push_back(geometry.cuts[j]);
   record.segments.push_back({.id = static_cast<std::uint32_t>(j + 10), .category_id = box.source_category_id, .isthing = true});
  }
  record.segments.push_back({.id = 30, .category_id = 63, .isthing = true});
  records.push_back(std::move(record));
 }
 const auto indexed_originals_view = fixture_index(originals);
 CoconutRecoveryOriginals indexed_originals(&indexed_originals_view, nullptr);
 CoconutMaskRecovery recovery(indexed_originals);
 // Repeat in reverse order as well, exercising growth, shrinkage and key reuse.
 for (std::size_t pass = 0; pass < cases.size() * 2; ++pass) {
  const auto i = pass < cases.size() ? pass : cases.size() * 2 - pass - 1;
  CAPTURE(pass, i);
  const auto& geometry = cases[i];
  auto record = records[i];
  const auto fresh_support = [&] {
   std::vector<CoconutSegmentSupport> result(geometry.cuts.size());
   result.push_back(recovery_support(geometry.supporter, geometry.dimensions));
   return result;
  };
  std::vector<std::uint8_t> dense;
  dataset::RowMajorMaskBounds materialized;
  dataset::materialize_row_major_mask(geometry.expected, geometry.dimensions, &dense, &materialized);
  const auto encoded = dataset::encode_dense_row_major_mask(dense, geometry.dimensions);
  for (const auto& bounds : {materialized, encoded.bounds}) {
   CHECK(bounds.has_foreground == geometry.bounds.has_foreground);
   CHECK(bounds.min_x == geometry.bounds.min_x);
   CHECK(bounds.min_y == geometry.bounds.min_y);
   CHECK(bounds.max_x == geometry.bounds.max_x);
   CHECK(bounds.max_y == geometry.bounds.max_y);
  }
  // Optional bounds output and singleton/cross-row accumulation share validation.
  dataset::materialize_row_major_mask(geometry.expected, geometry.dimensions, &dense);
  auto support = fresh_support();
  CoconutRecoveryImage facts{record.image_id, 0, {}};
  // Exercise every cancellation boundary, including partially populated scratch.
  const auto* original_storage = support.back().runs.data();
  PollCancellation complete;
  recovery.apply(
   CoconutImageNamespace::CocoTrain, record, geometry.dimensions.width, geometry.dimensions.height, support, facts, mmltk::common::concurrency::CancellationObservation::Borrow(complete));
  if (i == 2) {
   CHECK_FALSE(support.back().carved);
   CHECK(support.back().runs.data() == original_storage);
  }
  for (std::size_t stop = 0; stop < complete.polls; ++stop) {
   CAPTURE(stop);
   auto interrupted = fresh_support();
   CoconutRecoveryImage partial{record.image_id, 0, {}};
   PollCancellation cancellation{.stop_at = stop};
   CHECK_THROWS(recovery.apply(
    CoconutImageNamespace::CocoTrain, record, geometry.dimensions.width, geometry.dimensions.height, interrupted, partial, mmltk::common::concurrency::CancellationObservation::Borrow(cancellation)));
   support = fresh_support();
   facts = {record.image_id, 0, {}};
   recovery.apply(CoconutImageNamespace::CocoTrain, record, geometry.dimensions.width, geometry.dimensions.height, support, facts);
   REQUIRE(facts.objects.size() == geometry.cuts.size());
   const auto& carved = support.back();
   REQUIRE(carved.runs.size() == geometry.expected.size());
   std::uint64_t expected_area = 0;
   for (std::size_t run = 0; run < geometry.expected.size(); ++run) {
    CHECK(carved.runs[run].start == geometry.expected[run].start);
    CHECK(carved.runs[run].length == geometry.expected[run].length);
    expected_area += geometry.expected[run].length;
   }
   CHECK(carved.area == expected_area);
   CHECK(carved.bounds.has_foreground == geometry.bounds.has_foreground);
   CHECK(carved.bounds.min_x == geometry.bounds.min_x);
   CHECK(carved.bounds.min_y == geometry.bounds.min_y);
   CHECK(carved.bounds.max_x == geometry.bounds.max_x);
   CHECK(carved.bounds.max_y == geometry.bounds.max_y);
  }
  // Once all thing masks are present, the next call needs no matching workspace.
  const auto* unchanged = support.back().runs.data();
  facts.objects.clear();
  for (auto& segment : record.segments) segment.bbox = std::array<double, 4>{0, 0, 1, 1};
  recovery.apply(CoconutImageNamespace::CocoTrain, record, geometry.dimensions.width, geometry.dimensions.height, support, facts);
  CHECK(facts.objects.empty());
  CHECK(support.back().runs.data() == unchanged);
 }
}
TEST_CASE("retained COCO source dogs recover exactly and remain disjoint after compiler mask projection", "[coconut][benchmark]") {
 ScopedTempDir root("coconut-source-recovery");
 const auto fixtures = std::filesystem::path(MMLTK_TEST_SOURCE_ROOT) / "src/backend/data/tests/fixtures/coconut_recovery";
 for (const unsigned id : {2212U, 400U}) {
  CAPTURE(id);
  const auto original = Json::parse(file_bytes(fixtures / (std::to_string(id) + ".originals.json")));
  const auto source = Json::parse(file_bytes(fixtures / (std::to_string(id) + ".segments.json")));
  NormalizedAnnotationBuilder index;
  index.split = "train2017";
  index.annotation_sha256 = original.at("annotation_sha256");
  const auto width = original.at("width").get<std::uint32_t>(), height = original.at("height").get<std::uint32_t>();
  for (const auto& object : original.at("objects")) {
   NormalizedBox box;
   const auto bounds = object.at("bbox").get<std::array<float, 4>>();
   box.x1 = bounds[0];
   box.y1 = bounds[1];
   box.x2 = bounds[2];
   box.y2 = bounds[3];
   box.annotation_id = object.at("annotation_id");
   box.source_category_id = object.at("source_category_id");
   box.source_ordinal = object.at("source_ordinal");
   box.original_area = object.at("original_area");
   box.flags = object.at("flags");
   box.class_id = object.at("class_id");
   box.mask_rle_offset = index.mask_rle_pairs.size();
   for (const auto& run : object.at("runs")) index.mask_rle_pairs.push_back({run[0].get<std::uint32_t>(), run[1].get<std::uint32_t>()});
   box.mask_rle_pairs = static_cast<std::uint32_t>(index.mask_rle_pairs.size() - box.mask_rle_offset);
   index.boxes.push_back(box);
  }
  index.images.push_back({.source_image_id = id, .box_count = static_cast<std::uint32_t>(index.boxes.size()), .width = width, .height = height});
  const auto indexed_originals_view = fixture_index(index);
  CoconutRecoveryOriginals indexed_originals(&indexed_originals_view, nullptr);
  CoconutMaskRecovery recovery(indexed_originals);
  const std::array physical{coco(id)};
  auto input = request(physical);
  input.recovery = &recovery;
  input.parquet_shards = {root.path() / "source.parquet"};
  parquet_file(input.parquet_shards[0], Json::array({hf_row(id, file_bytes(fixtures / (std::to_string(id) + ".mask.png")), source.at("segments"), width, height)}));
  const auto result = import_coconut_annotations(input);
  REQUIRE(result.size() == 1);
  const auto& component = result.front();
  REQUIRE(component.recovery().size() == 1);
  REQUIRE(component.recovery().front().objects.size() == (id == 2212 ? 2 : 1));
  CHECK(component.recovery().front().unresolved == 0);
  const auto supporter = std::ranges::find(component.index().storage().boxes, id == 2212 ? 63U : 9U, &NormalizedBox::source_category_id);
  REQUIRE(supporter != component.index().storage().boxes.end());
  CHECK(supporter->original_area < (id == 2212 ? 192166 : 259651));
  for (const auto& fact : component.recovery().front().objects) {
   const auto dog = std::ranges::find(component.index().storage().boxes, fact.annotation_id, &NormalizedBox::annotation_id);
   const auto original_dog = std::ranges::find(index.boxes, fact.original_annotation_id, &NormalizedBox::annotation_id);
   REQUIRE(dog != component.index().storage().boxes.end());
   REQUIRE(original_dog != index.boxes.end());
   CHECK(dog->source_category_id == 18);
   const auto dog_runs = std::span(component.index().storage().mask_rle_pairs).subspan(dog->mask_rle_offset, dog->mask_rle_pairs);
   const auto expected = std::span(index.mask_rle_pairs).subspan(original_dog->mask_rle_offset, original_dog->mask_rle_pairs);
   CHECK(std::ranges::equal(dog_runs, expected, [](auto a, auto b) { return a.start == b.start && a.length == b.length; }));
   const auto support_runs = std::span(component.index().storage().mask_rle_pairs).subspan(supporter->mask_rle_offset, supporter->mask_rle_pairs);
   for (unsigned projection = 0; projection < 3; ++projection) {
    using namespace mmltk::backend::imaging::resample;
    const dataset::MaskDimensions target = projection == 0 ? dataset::MaskDimensions{width, height} : dataset::MaskDimensions{384, 384};
    const auto geometry = compute_image_resize_geometry(width, height, target.width, target.height, projection == 2 ? ImageResizeMode::Letterbox : ImageResizeMode::Stretch);
    dataset::MaskResizeScratch scratch;
    const auto dogs = dataset::resize_row_major_mask(dog_runs, {width, height}, target, geometry, &scratch);
    const auto support = dataset::resize_row_major_mask(support_runs, {width, height}, target, geometry, &scratch);
    std::vector<std::uint8_t> foreground, background;
    dataset::materialize_row_major_mask(dogs.pairs, target, &foreground);
    dataset::materialize_row_major_mask(support.pairs, target, &background);
    std::size_t intersection = 0;
    for (std::size_t pixel = 0; pixel < foreground.size(); ++pixel) intersection += foreground[pixel] && background[pixel];
    CHECK(intersection == 0);
   }
  }
  const auto path = root.path() / "recovered.normalized.bin";
  store_coconut_component(path, component);
  const auto cached = load_coconut_component(path, component.edition(), component.source(), component.input_identity());
  REQUIRE(cached);
  CHECK(cached->index().annotation_sha256 == component.index().annotation_sha256);
  CHECK(cached->recovery().front().objects.size() == component.recovery().front().objects.size());
  auto changed = index;
  changed.annotation_sha256 = std::string(64, 'a');
  const auto changed_recovery_originals_view = fixture_index(changed);
  CoconutRecoveryOriginals changed_recovery_originals(&changed_recovery_originals_view, nullptr);
  CoconutMaskRecovery changed_recovery(changed_recovery_originals);
  CHECK_FALSE(load_coconut_component(path, component.edition(), component.source(), coconut_component_input_identity(input.input_identity, component.source(), &changed_recovery)));
  auto corrupt = read_json_file(path.string() + ".complete.json");
  corrupt["coconut"]["recovery_policy"] = 99;
  write_json_atomically(path.string() + ".complete.json", corrupt, {});
  CHECK_FALSE(load_coconut_component(path, component.edition(), component.source(), component.input_identity()));
 }
}
TEST_CASE("optional COCO split admission is independent and never conceals output failures", "[coconut][benchmark][cache]") {
 ScopedTempDir root("coco-optional-admission");
 LocalCoconutRecipe local(root.path());
 auto catalog = local_custom_catalog(local);
 const auto train = local.cache.source_indexes("coco") / "train2017.normalized.bin";
 const auto validation = local.cache.source_indexes("coco") / "val2017.normalized.bin";
 bool missing_train = true, initial_failure = false, publication_failure = false, required_validation = true, cold = false;
 bool local_parser_failure = false, local_archive_failure = false;
 unsigned unusable_document = 0;
 SECTION("missing optional train preserves discovered required validation") {}
 SECTION("missing optional validation preserves discovered train") {
  missing_train = false;
  required_validation = false;
 }
 SECTION("initial download failure returns independently admitted required validation") { initial_failure = true; }
 SECTION("initial download failure returns independently admitted optional train") {
  initial_failure = true;
  missing_train = false;
  required_validation = false;
 }
 SECTION("optional output publication failure remains fatal") { publication_failure = true; }
 SECTION("local parser file opening failure remains fatal") { local_parser_failure = true; }
 SECTION("local archive file opening failure remains fatal") { local_archive_failure = true; }
 SECTION("optional malformed document is unavailable") { unusable_document = 1; }
 SECTION("optional category metadata rejection is unavailable") { unusable_document = 2; }
 SECTION("optional mistyped image metadata is unavailable") { unusable_document = 3; }
 SECTION("optional integer exceeding 64 bits is unavailable") { unusable_document = 4; }
 SECTION("missing optional train cannot prevent cold required validation admission") { cold = true; }
 SECTION("a malformed optional member cannot prevent the other cold required member") { cold = true; unusable_document = 1; }
 SECTION("cold usable train survives unavailable optional validation") {
  cold = true;
  missing_train = false;
  required_validation = false;
 }
 const auto missing = missing_train ? train : validation;
 const auto retained = missing_train ? validation : train;
 const auto retained_bytes = file_bytes(retained);
 if (cold) { remove_normalized_annotation_index(retained); }
 remove_normalized_annotation_index(missing);
 auto artifact = catalog.coco_annotations;
 const auto archive_path = local.cache.source_downloads("coco") / artifact.filename;
 if (!publication_failure && !local_parser_failure && !local_archive_failure) {
  const auto split = missing_train ? "val2017" : "train2017";
  const std::array<std::pair<std::string, std::string>, 1> rows{
   {{cold ? "annotations/instances_" + std::string(split) + ".json" : "annotations/unrelated.json", cold ? file_bytes(local.cache.source_indexes("coco") / (std::string(split) + ".fixture.json")) : "{}"}}
  };
  tar(archive_path, rows);
 } else {
  const std::array<std::pair<std::string, std::string>, 1> rows{{
   {"annotations/instances_train2017.json", file_bytes(local.cache.source_indexes("coco") / "train2017.fixture.json")},
  }};
  tar(archive_path, rows);
 }
 if (unusable_document) {
  auto document = Json::parse(file_bytes(local.cache.source_indexes("coco") / "train2017.fixture.json"));
  if (unusable_document == 2) document["categories"][0]["name"] = "wrong source category name";
  if (unusable_document == 3) document["images"][0]["width"] = "not a numeric dimension";
  if (unusable_document == 4) document["images"][0]["width"] = "oversized-integer";
  auto payload = unusable_document == 1 ? std::string("not JSON") : document.dump();
  if (unusable_document == 4) {
   // Retain an exact JSON integer literal rather than a rounded double/string.
   const std::string marker = "\"oversized-integer\"";
   const auto position = payload.find(marker);
   REQUIRE(position != std::string::npos);
   payload.replace(position, marker.size(), "18446744073709551616");
  }
  std::vector<std::pair<std::string, std::string>> rows{{"annotations/instances_train2017.json", std::move(payload)}};
  if (cold) rows.emplace_back("annotations/instances_val2017.json", file_bytes(local.cache.source_indexes("coco") / "val2017.fixture.json"));
  tar(archive_path, rows);
 }
 const auto server_payload = file_bytes(archive_path);
 mmltk::backend::data::testsupport::HttpServer server(server_payload);
 artifact.url = server.url("optional-split");
 artifact.expected_size = std::filesystem::file_size(archive_path);
 if (publication_failure) mmltk::testsupport::write_text_file(std::filesystem::path(missing.string() + ".complete.json") / "blocker", "retain");
 BenchmarkTraceSink trace;
 bool parser_entered = false, extraction_entered = false;
 ProgressReporter progress([&](const BenchmarkCompileProgress& update) {
  if (local_archive_failure && update.activity == "Extracting COCO train annotations") {
   extraction_entered = true;
   REQUIRE(std::filesystem::remove(archive_path));
  }
  if (local_parser_failure && update.activity == "Parsing and indexing COCO train annotations") {
   parser_entered = true;
   // Remove the already-extracted input at the ordinary progress boundary.
   // The parser's open failure is local, not evidence of unusable source bytes.
   REQUIRE(std::filesystem::remove(local.cache.source_indexes("coco") / "source-json/instances_train2017.json"));
  }
 }, trace);
 CocoAnnotationCache cache(local.cache, artifact, {CocoSplitAdmission::Optional, required_validation ? CocoSplitAdmission::Required : CocoSplitAdmission::Optional}, 2, 1, 1, {}, trace);
 cache.discover(progress);
 REQUIRE(cache.pending_download());
 auto completed = cache.completed_indexes();
 REQUIRE(completed == (cold ? 0 : 1));
 if (initial_failure)
  cache.download_unavailable(BenchmarkDownloadUnavailable("fixture source unavailable"), progress);
 else {
  auto archive = download_artifacts({*cache.pending_download()}, 1, {}).front();
  if (publication_failure || local_parser_failure || local_archive_failure) {
   CHECK_THROWS(cache.settle(std::move(archive), progress, 1, completed, 2));
   CHECK(parser_entered == local_parser_failure);
   CHECK(extraction_entered == local_archive_failure);
   CHECK(server.requests() == 0);
   CHECK(completed == 1);
   CHECK_THROWS(cache.take_indexes());
   CHECK(file_bytes(retained) == retained_bytes);
   return;
  }
  cache.settle(std::move(archive), progress, 1, completed, 2);
 }
 // Probe the real source lease without sleeps or a potentially hanging waiter.
 const auto lease_probe = mmltk::common::io::FileHandle::open_readonly((local.cache.locks / "coco-annotations.lifecycle.lock").string());
 REQUIRE(::flock(lease_probe.get(), LOCK_EX | LOCK_NB) == -1);
 REQUIRE(errno == EWOULDBLOCK);
 const auto indexes = cache.take_indexes();
 REQUIRE(::flock(lease_probe.get(), LOCK_EX | LOCK_NB) == 0);
 CHECK(::flock(lease_probe.get(), LOCK_UN) == 0);
 CHECK(indexes.train.has_value() == !missing_train);
 CHECK(indexes.validation.has_value() == missing_train);
 if (!cold) CHECK(file_bytes(retained) == retained_bytes);
 CHECK(completed == 1);
 server.Check();
}
TEST_CASE("COCONut recovery caches preserve base and physical products across every validation choice", "[coconut][benchmark][cache]") {
 ScopedTempDir root("coconut-recovery-recipe");
 LocalCoconutRecipe local(root.path());
 for (auto& release : local.catalog.releases) {
  if (release.edition != CoconutEdition::Base && release.edition != CoconutEdition::RelabeledValidation) continue;
  auto& artifact = release.annotations.front();
  const auto path = local.cache.source_downloads("coconut-" + std::string(release.name)) / artifact.filename;
  const std::array<std::uint32_t, 9> pixels{1, 1, 1, 1, 1, 1, 1, 1, 1}, empty{};
  auto rows = Json::array({hf_row(release.edition == CoconutEdition::Base ? 7 : 9, png(3, 3, pixels), Json::array({segment(1, 63), segment(2, 18)}))});
  if (release.edition == CoconutEdition::Base) rows.push_back(hf_row(8, png(3, 3, empty), Json::array(), 3, 3, "unlabeled2017"));
  parquet_file(path, rows);
  artifact.expected_size = std::filesystem::file_size(path);
 }
 for (const bool training : {true, false}) {
  auto index = recovery_originals(training ? 7 : 9, std::array<RLEPair, 1>{{{0, 1}}});
  index.annotation_sha256 = std::string(64, training ? 'a' : 'b');
  index.split = training ? "train2017" : "val2017";
  store_normalized_annotation_index(local.cache.source_indexes("coco") / (index.split + ".normalized.bin"), fixture_index(index), {});
 }
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Coconut}, true);
 const auto base_path = local.cache.source_indexes("coconut-fixture-base") / "coco-train2017.normalized.bin";
 const auto unlabeled_path = local.cache.source_indexes("coconut-fixture-base") / "coco-unlabeled2017.normalized.bin";
 const auto image = cached_image_path(local.cache.source_images("coco") / "train2017", 7);
 std::string base_bytes, unlabeled_bytes, off_train, recovered_train;
 std::filesystem::file_time_type image_time{}, unlabeled_time{};
 for (const auto choice : {CoconutValidation::Coconut, CoconutValidation::Stock, CoconutValidation::CoconutStock}) {
  config.selection.validation = choice;
  const auto catalog = local.selected(choice);
  for (const bool enabled : {false, true, true, false, true}) {
   config.selection.recover_dropped_masks = enabled;
   compile_benchmark_recipe(config, &catalog);
   const auto manifest = Json::parse(file_bytes(config.output_dir / "benchmark_manifest.json")).at("recipe");
   CHECK(manifest.at("recovered_objects") == (enabled ? (choice == CoconutValidation::Stock ? 1 : 2) : 0));
   CHECK(manifest.at("unresolved_objects") == 0);
   const auto train_bytes = file_bytes(config.output_dir / "train.bin");
   auto& expected = enabled ? recovered_train : off_train;
   if (expected.empty()) expected = train_bytes;
   CHECK(train_bytes == expected);
   if (base_bytes.empty()) {
    base_bytes = file_bytes(base_path);
    unlabeled_bytes = file_bytes(unlabeled_path);
    image_time = std::filesystem::last_write_time(image);
    unlabeled_time = std::filesystem::last_write_time(unlabeled_path);
   }
   CHECK(file_bytes(base_path) == base_bytes);
   CHECK(file_bytes(unlabeled_path) == unlabeled_bytes);
   CHECK(std::filesystem::last_write_time(unlabeled_path) == unlabeled_time);
   CHECK(std::filesystem::last_write_time(image) == image_time);
   for (const auto& component : manifest.at("components"))
    if (component.at("physical_source") == "coco-train2017") {
     CHECK(component.at("recovery_policy") == (enabled ? kCoconutRecoveryPolicy : 0));
     CHECK(component.at("original_annotation_identity") == (enabled ? std::string(64, 'a') : std::string{}));
    }
  }
 }
 CHECK(off_train != recovered_train);
 const auto previous = file_bytes(config.output_dir / "train.bin");
 const auto prior_manifest = file_bytes(config.output_dir / "benchmark_manifest.json");
 auto changed = recovery_originals(7, std::array<RLEPair, 1>{{{0, 1}}});
 changed.annotation_sha256 = std::string(64, 'c');
 store_normalized_annotation_index(local.cache.source_indexes("coco") / "train2017.normalized.bin", fixture_index(changed), {});
 const auto catalog = local.selected(config.selection.validation);
 compile_benchmark_recipe(config, &catalog);
 CHECK(file_bytes(config.output_dir / "train.bin") == previous);
 CHECK(file_bytes(config.output_dir / "benchmark_manifest.json") != prior_manifest);
 // Both original splits remain independent through the recipe, including a
 // required Stock validation beside an unavailable optional train original.
 const auto archive_path = local.cache.source_downloads("coco") / local.catalog.stock_annotations.filename;
 const std::array<std::pair<std::string, std::string>, 1> unavailable{{{"annotations/unrelated.json", "{}"}}};
 tar(archive_path, unavailable);
 const auto server_payload = file_bytes(archive_path);
 mmltk::backend::data::testsupport::HttpServer server(server_payload);
 local.catalog.stock_annotations.url = server.url("unavailable-originals");
 local.catalog.stock_annotations.expected_size = std::filesystem::file_size(archive_path);
 for (const bool missing_train : {true, false}) {
  const auto split = missing_train ? "train2017" : "val2017";
  const auto path = local.cache.source_indexes("coco") / (std::string(split) + ".normalized.bin");
  remove_normalized_annotation_index(path);
  config.selection.validation = missing_train ? CoconutValidation::Stock : CoconutValidation::CoconutStock;
  const auto selected = local.selected(config.selection.validation);
  compile_benchmark_recipe(config, &selected);
  const auto manifest = Json::parse(file_bytes(config.output_dir / "benchmark_manifest.json")).at("recipe");
  CHECK(manifest.at("recovered_objects") == (missing_train ? 0 : 1));
  CHECK(manifest.at("unresolved_objects") == 1);
  CHECK(manifest.at("original_annotations").at(missing_train ? "train" : "validation").is_null());
  // A later admission retries the original and immediately reuses/rebuilds its
  // correctly bound derived cache rather than accepting unavailable as complete.
  auto restored = recovery_originals(missing_train ? 7 : 9, std::array<RLEPair, 1>{{{0, 1}}});
  restored.split = split;
  restored.annotation_sha256 = std::string(64, missing_train ? 'c' : 'b');
  store_normalized_annotation_index(path, fixture_index(restored), {});
  compile_benchmark_recipe(config, &selected);
  const auto retried = Json::parse(file_bytes(config.output_dir / "benchmark_manifest.json")).at("recipe");
  CHECK(retried.at("recovered_objects") == (missing_train ? 1 : 2));
  CHECK(retried.at("unresolved_objects") == 0);
  CHECK(file_bytes(base_path) == base_bytes);
  CHECK(std::filesystem::last_write_time(image) == image_time);
 }
 server.Check();
 std::atomic<bool> stop{true};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(stop);
 CHECK_THROWS(compile_benchmark_recipe(config, &catalog));
 CHECK(file_bytes(config.output_dir / "train.bin") == previous);
}
TEST_CASE("COCONut recovery facts follow physical images when import rows are reordered", "[coconut][benchmark][cache]") {
 ScopedTempDir root("coconut-recovery-order");
 const std::array physical{coco(3), coco(5), coco(7), coco(9)};
 auto originals = recovery_originals(7, std::array<RLEPair, 1>{{{0, 1}}});
 auto second = recovery_originals(3, std::array<RLEPair, 1>{{{4, 1}}});
 append_normalized_image_slice(originals, fixture_index(second), 0);
 const auto indexed_originals_view = fixture_index(originals);
 CoconutRecoveryOriginals indexed_originals(&indexed_originals_view, nullptr);
 CoconutMaskRecovery recovery(indexed_originals);
 const std::array<std::uint32_t, 9> pixels{1, 1, 1, 1, 1, 1, 1, 1, 1}, empty{};
 const auto mask = png(3, 3, pixels), empty_mask = png(3, 3, empty);
 const auto rows = Json::array(
  {hf_row(3, mask, Json::array({segment(1, 63), segment(2, 18), segment(3, 17)})), hf_row(5, empty_mask, Json::array()), hf_row(7, mask, Json::array({segment(1, 63), segment(2, 18)})),
   hf_row(9, empty_mask, Json::array({segment(3, 17)}))});
 std::array<std::size_t, 4> order{0, 1, 2, 3};
 SECTION("already ordered rows retain every kind of recovery fact") {}
 SECTION("permuted rows move every kind of recovery fact") { order = {2, 3, 1, 0}; }
 auto offered = Json::array();
 std::array<std::uint64_t, 4> image_ordinals{}, object_ordinals{};
 std::uint64_t ordinal = 0;
 for (std::size_t i = 0; i < order.size(); ++i) {
  const auto position = order[i];
  offered.push_back(rows[position]);
  image_ordinals[position] = i;
  object_ordinals[position] = ordinal;
  ordinal += position == 0 ? 3U : position == 2 ? 2U : position == 3 ? 1U : 0U;
 }
 auto input = request(physical);
 input.recovery = &recovery;
 input.parquet_shards = {root.path() / "source.parquet"};
 parquet_file(input.parquet_shards.front(), offered);
 const auto components = import_coconut_annotations(input);
 REQUIRE(components.size() == 1);
 const auto& component = components.front();
 const auto path = root.path() / "ordered.normalized.bin";
 store_coconut_component(path, component);
 const auto cached = load_coconut_component(path, component.edition(), component.source(), component.input_identity());
 REQUIRE(cached);
 for (const auto* product : {&component, &*cached}) {
  REQUIRE(product->recovery().size() == physical.size());
  REQUIRE(product->inventory().size() == physical.size());
  REQUIRE(product->index().image_count() == physical.size());
  CHECK(product->index().annotation_sha256 == component.index().annotation_sha256);
  CHECK(product->input_identity() == component.input_identity());
  CHECK(product->recovery_policy() == kCoconutRecoveryPolicy);
  CHECK(product->original_annotation_identity() == originals.annotation_sha256);
  for (std::size_t i = 0; i < physical.size(); ++i) {
   CAPTURE(i);
   const auto& facts = product->recovery()[i];
   const auto& inventory = product->inventory()[i];
   const auto& image = product->index().images()[i];
   CHECK(inventory.physical == physical[i]);
   CHECK(inventory.release_image_id == physical[i].image_id);
   CHECK(inventory.source_ordinal == image_ordinals[i]);
   CHECK(image.source_image_id == physical[i].image_id);
   CHECK(facts.image_id == physical[i].image_id);
   const bool recovered = i == 0 || i == 2, omitted = i == 0 || i == 3;
   REQUIRE(facts.objects.size() == (recovered ? 1 : 0));
   REQUIRE(facts.omissions.size() == (omitted ? 1 : 0));
   CHECK(facts.unresolved == facts.omissions.size());
   CHECK(image.box_count == (recovered ? 2 : 0));
   if (recovered) {
    const auto& object = facts.objects.front();
    CHECK(object.annotation_id == 2);
    CHECK(object.source_ordinal == object_ordinals[i] + 1);
    CHECK(object.source_category_id == 18);
    CHECK(object.original_annotation_id == 200);
    const auto& box = product->index().storage().boxes[image.first_box + 1];
    CHECK(box.annotation_id == object.annotation_id);
    CHECK(box.source_ordinal == object.source_ordinal);
    REQUIRE(box.mask_rle_pairs == 1);
    const auto run = product->index().storage().mask_rle_pairs[box.mask_rle_offset];
    CHECK(run.start == (i == 0 ? 4 : 0));
    CHECK(run.length == 1);
   }
   if (omitted) {
    const auto& object = facts.omissions.front();
    CHECK(object.annotation_id == 3);
    CHECK(object.source_ordinal == object_ordinals[i] + (i == 0 ? 2 : 0));
    CHECK(object.source_category_id == 17);
    CHECK(object.original_annotation_id == 0);
   }
  }
 }
 const auto selected = cached->select_images({0, 2});
 REQUIRE(selected.index().image_count() == 2);
 for (std::size_t i = 0; i < 2; ++i) {
  CHECK(&selected.inventory_image(i) == &cached->inventory_image(i * 2));
  CHECK(&selected.recovery_image(i) == &cached->recovery_image(i * 2));
  CHECK(selected.recovery_image(i).image_id == selected.index().image(i).source_image_id);
 }
 const auto membership = selected.membership();
 CHECK(membership.recovery().empty());
 CHECK(membership.index().annotation_sha256 == selected.index().annotation_sha256);
 CHECK(membership.completion() == selected.completion());
 const auto selected_path = root.path() / "selected-recovery.normalized.bin";
 store_coconut_component(selected_path, selected);
 const auto selected_cache = load_coconut_component(selected_path, selected.edition(), selected.source(), selected.input_identity());
 REQUIRE(selected_cache);
 CHECK(selected_cache->index().image_count() == 2);
 CHECK(selected_cache->recovery_image(1).objects.front().source_ordinal == selected.recovery_image(1).objects.front().source_ordinal);
 CHECK(selected.completion()->images == physical.size());
 const auto roundtrip = root.path() / "roundtrip.normalized.bin";
 store_coconut_component(roundtrip, *cached);
 for (const auto suffix : {"", ".inventory", ".complete.json"}) CHECK(file_bytes(path.string() + suffix) == file_bytes(roundtrip.string() + suffix));
 const auto rebuilt = import_coconut_annotations(input);
 REQUIRE(rebuilt.size() == 1);
 CHECK(rebuilt.front().index().annotation_sha256 == component.index().annotation_sha256);
 store_coconut_component(roundtrip, rebuilt.front());
 CHECK(file_bytes(path.string() + ".inventory") == file_bytes(roundtrip.string() + ".inventory"));
}
TEST_CASE("COCONut metadata membership precedes mask payload admission", "[coconut]") {
 ScopedTempDir root("coconut-membership-first");
 const auto path = root.path() / "membership.parquet";
 parquet_file(path, Json::array({hf_row(7, "not a PNG", Json::array()), hf_row(8, "also not a PNG", Json::array())}));
 const std::array physical{coco(7), coco(8, CoconutImageNamespace::CocoUnlabeled)};
 auto input = request(physical);
 input.parquet_shards = {path};
 input.expected_rows = 2;
 input.metadata_only = true;
 const auto membership = import_coconut_annotations(input);
 REQUIRE(membership.size() == 2);
 CHECK(membership[0].inventory()[0].physical.image_id == 7);
 CHECK(membership[1].inventory()[0].physical.image_id == 8);
 CHECK(membership[0].index().box_count() == 0);
 CHECK(membership[1].index().box_count() == 0);
 input.metadata_only = false;
 CHECK_THROWS(import_coconut_annotations(input));
}
TEST_CASE("cached training labels start before pixel drain and overlap subsequent pixels", "[benchmark][pipeline]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPU lanes");
 bool cancel_reader = false;
 SECTION("same training population labels overlap later pixel progress") {}
 SECTION("cancellation drains the held reader and retains the old generation") { cancel_reader = true; }
 ScopedTempDir root("custom-label-overlap");
 LocalCoconutRecipe local(root.path());
 std::vector<unsigned> ids;
 std::vector<std::pair<std::string, std::string>> members;
 // Seventy selected images exceed both the former 3P readiness limit and the
 // normal 64-image progress quantum. One pixel lane is assigned below.
 for (unsigned id = 100; id < 170; ++id) {
  ids.push_back(id);
  members.emplace_back(coco(id).member, white_jpeg());
 }
 replace_physical_images(local.cache, local.catalog, CoconutImageNamespace::CocoTrain, members);
 auto custom = local_custom_catalog(local, ids);
 auto config = local.compiler_config({BenchmarkDatasetVariant::CocoCustom, CoconutValidation::CoconutStock}, true);
 compile_benchmark_recipe(config, nullptr, &custom);
 compile_benchmark_recipe(config, nullptr, &custom);
 const auto train = file_bytes(local.output / "train.bin"), validation = file_bytes(local.output / "val.bin");
 const auto manifest = file_bytes(local.output / "benchmark_manifest.json");
 const auto image_root = local.cache.source_images("coco") / "train2017";
 mmltk::testsupport::TestGate reader("early COCO training reader"), labels("same COCO training labels");
 const auto reader_receipt = reader.receipt(), labels_receipt = labels.receipt();
 std::atomic<bool> labels_started{false}, reader_seen{false}, delivered{false}, cancelled{false};
 std::mutex observation_mutex;
 BenchmarkCompileProgress observed;
 config.progress = [&](const BenchmarkCompileProgress& update) {
  const std::lock_guard lock(observation_mutex);
  observed = update;
 };
 std::promise<void> subsequent_pixels;
 config.num_workers = 4;
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.pixel_compile.throughput" && labels_started.load() && Json::parse(fields).at("completed_images") >= 64 && !delivered.exchange(true)) subsequent_pixels.set_value();
 };
 auto compiling = std::async(std::launch::async, [&] {
  compile_benchmark_recipe(config, nullptr, &custom, [&](BenchmarkDatasetSource source, std::string_view split) {
   if (source == BenchmarkDatasetSource::kCoco2017 && split == "train2017") {
    if (!reader.WaitEntered(5s)) throw std::runtime_error("same-split reader did not reach its open-file boundary");
    labels_started.store(true);
    labels_receipt.ArriveAndWait();
   }
  }, [&](const std::filesystem::path& source, std::uint64_t id) {
   if (source == image_root && id == ids.front() && !reader_seen.exchange(true)) reader_receipt.ArriveAndWait();
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  reader.Release();
  labels.Release();
 });
 REQUIRE(reader.WaitEntered(5s));
 REQUIRE(labels.WaitEntered(5s));
 CHECK_FALSE(delivered.load());
 {
  const std::lock_guard lock(observation_mutex);
  CHECK(observed.tracks.labels.active);
  CHECK(observed.tracks.labels.activity == DatasetCompileActivity::Preparing);
  CHECK(observed.tracks.labels.completed < observed.tracks.labels.total);
 }
 const mmltk::common::io::ScopedFd lease(::open((local.cache.locks / "coco-train2017.images.lock").c_str(), O_RDWR | O_CLOEXEC));
 REQUIRE(lease.get() >= 0);
 CHECK(::flock(lease.get(), LOCK_EX | LOCK_NB) == -1);
 CHECK((errno == EWOULDBLOCK || errno == EAGAIN));
 if (cancel_reader) {
  cancelled.store(true);
  labels.Release();
  CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
  reader.Release();
  CHECK_THROWS(mmltk::testsupport::await_test_future(compiling, "cancelled reader custody drain", 5s));
 } else {
  reader.Release();
  mmltk::testsupport::await_test_promise(subsequent_pixels, "new pixels after same-split label entry", 5s);
  CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
  {
   const std::lock_guard lock(observation_mutex);
   CHECK(observed.tracks.labels.active);
   CHECK(observed.tracks.pixels.completed >= 64);
   CHECK(observed.tracks.labels.completed < observed.tracks.labels.total);
  }
  CHECK(file_bytes(local.output / "train.bin") == train);
  labels.Release();
  mmltk::testsupport::await_test_future(compiling, "production label settlement", 5s);
 }
 CHECK(::flock(lease.get(), LOCK_EX | LOCK_NB) == 0);
 check_publication_bytes(local.output, train, validation, manifest);
}
TEST_CASE("COCONut annotation transfer settles while physical inventory acquisition is blocked", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 bool cancel_pending_membership = false;
 SECTION("physical inventory and annotation transfer overlap") {}
 SECTION("cancellation drains releases waiting for physical membership") { cancel_pending_membership = true; }
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPU lanes");
 ScopedTempDir root("coconut-independent-inputs");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 const std::array<std::pair<std::string, std::string>, 1> members{{{coco(7).member, white_jpeg(3, 3, 1024 * 1024)}}};
 replace_physical_images(local.cache, catalog, CoconutImageNamespace::CocoTrain, members);
 ServedPhysicalArchive physical(local.cache, catalog, CoconutImageNamespace::CocoTrain, "physical");
 std::filesystem::remove(physical.path);
 auto& release = catalog.releases.front();
 auto& artifact = release.annotations.front();
 const auto annotation_path = local.cache.source_downloads("coconut-" + std::string(release.name)) / artifact.filename;
 const auto annotation_bytes = file_bytes(annotation_path);
 const std::vector<std::uint8_t> annotation_payload(annotation_bytes.begin(), annotation_bytes.end());
 mmltk::backend::data::testsupport::HttpServer annotation_server(annotation_payload);
 artifact.url = annotation_server.url("annotation");
 std::filesystem::remove(annotation_path);
 physical.server.GateNextTransfer();
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock});
 config.num_workers = 2;
 std::atomic<bool> cancelled{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 std::promise<void> annotation_ready;
 std::atomic<bool> delivered{false};
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.download.complete" && Json::parse(fields).at("artifact") == artifact.artifact_id && !delivered.exchange(true)) annotation_ready.set_value();
 };
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog); });
 const mmltk::testsupport::ScopedTestCleanup release_transfer([&] {
  cancelled.store(true);
  physical.server.ReleasePartial();
 });
 REQUIRE(physical.server.WaitPartial());
 mmltk::testsupport::await_test_promise(annotation_ready, "independent annotation transfer", 3s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 CHECK(std::filesystem::is_regular_file(annotation_path));
 if (cancel_pending_membership) {
  cancelled.store(true);
  CHECK_THROWS(mmltk::testsupport::await_test_future(compiling, "pending release cancellation", 5s));
  CHECK_FALSE(std::filesystem::is_regular_file(local.output / "train.bin"));
 } else {
  physical.server.ReleasePartial();
  mmltk::testsupport::await_test_future(compiling, "independent COCONut inputs", 5s);
  CHECK(CompiledDataset::open(local.output / "train.bin").image_entries().size() == 4);
 }
 physical.server.ReleasePartial();
 physical.server.Check();
 annotation_server.Check();
}
TEST_CASE("COCONut readable mismatched geometry survives a failed body and cache repair", "[benchmark][coconut][pipeline]") {
 ScopedTempDir root("coconut-header-repair");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 auto& physical = *std::ranges::find(catalog.images, CoconutImageNamespace::Objects365V2, &RecipeImageArchive::source);
 const auto archive = local.cache.source_downloads("objects365") / physical.artifact.filename;
 const std::array<std::pair<std::string, std::string>, 2> members{{{objects(1).member, white_jpeg(6, 4)}, {objects(2).member, white_jpeg(6, 4)}}};
 tar(archive, members);
 physical.artifact.expected_size = std::filesystem::file_size(archive);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 BenchmarkCompileProgress settled;
 config.progress = [&](const BenchmarkCompileProgress& value) { settled = value; };
 compile_benchmark_recipe(config, &catalog);
 CHECK(std::ranges::all_of(settled.sources, &BenchmarkSourceProgress::complete));
 const auto expected = file_bytes(config.output_dir / "train.bin");
 const std::array<std::uint32_t, 24> pixels{};
 auto bad_body = png(6, 4, pixels);
 bad_body.resize(41);  // Keep the IDAT header required by stbi_info, but no image data.
 const auto cached = cached_image_path(local.cache.source_images("objects365") / "patch-32", 1);
 mmltk::testsupport::write_text_file(cached, bad_body);
 unsigned repairs = 0;
 config.trace = [&](std::string_view event, std::string_view) {
  if (event == "benchmark.pixel_compile.cache_repair") ++repairs;
 };
 compile_benchmark_recipe(config, &catalog);
 CHECK(repairs == 1);
 CHECK(std::ranges::all_of(settled.sources, &BenchmarkSourceProgress::complete));
 CHECK(file_bytes(config.output_dir / "train.bin") == expected);
 const auto train = CompiledDataset::open(config.output_dir / "train.bin");
 CHECK(train.image_labels(2).empty());
 CHECK(train.image_entry(2).original_width == 6);
 CHECK(train.image_entry(2).original_height == 4);
}
TEST_CASE("production output admission counts only unsettled format-9 extents", "[benchmark][coconut][storage]") {
 ScopedTempDir root("output-additional-storage");
 LocalCoconutRecipe local(root.path());
 const auto catalog = local.selected(CoconutValidation::CoconutStock);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock});
 std::optional<std::uint64_t> additional;
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event != "benchmark.storage.preflight") return;
  const auto value = Json::parse(fields);
  if (value.at("target") == "additional benchmark compiled output staging") additional = value.at("required_bytes").get<std::uint64_t>();
 };
 compile_benchmark_recipe(config, &catalog);
 REQUIRE(additional.has_value());
 // All fixture labels fit the already allocated last filesystem block. Pixel
 // staging has already paid for both aligned format-9 index/pixel prefixes.
 CHECK(*additional == 0);
 CHECK(std::filesystem::file_size(config.output_dir / "train.bin") > 2 * 1024 * 1024);
}
TEST_CASE("COCONut cold partial and warm preparation admit each immutable product once", "[benchmark][coconut][pipeline]") {
 ScopedTempDir root("coconut-retained-admission");
 LocalCoconutRecipe local(root.path());
 const auto catalog = local.selected(CoconutValidation::CoconutStock);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 std::vector<std::string> annotation_ids;
 for (const auto& release : catalog.releases)
  for (const auto& artifact : release.annotations) annotation_ids.push_back(artifact.artifact_id);
 std::size_t component_admissions = 0, artifact_admissions = 0;
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.annotations.component_admitted") ++component_admissions;
  if (event == "benchmark.download.cache_hit" || event == "benchmark.download.preseeded")
   if (std::ranges::find(annotation_ids, Json::parse(fields).at("artifact").get<std::string>()) != annotation_ids.end()) ++artifact_admissions;
 };
 compile_benchmark_recipe(config, &catalog);
 CHECK(component_admissions == 0);
 CHECK(artifact_admissions == annotation_ids.size());
 const auto expected = file_bytes(local.output / "train.bin");
 const auto manifest = read_json_file(local.output / "benchmark_manifest.json");
 const auto components = manifest.at("recipe").at("components");
 const auto count = components.size();
 REQUIRE(count > 1);
 component_admissions = artifact_admissions = 0;
 config.num_workers = 4;
 compile_benchmark_recipe(config, &catalog);
 CHECK(component_admissions == count);
 CHECK(artifact_admissions == annotation_ids.size());
 CHECK(file_bytes(local.output / "train.bin") == expected);
 std::filesystem::remove(local.cache.root / components.front().at("imported_index").get<std::string>());
 component_admissions = artifact_admissions = 0;
 compile_benchmark_recipe(config, &catalog);
 CHECK(component_admissions == count - 1);
 CHECK(artifact_admissions == annotation_ids.size());
 CHECK(file_bytes(local.output / "train.bin") == expected);
}
TEST_CASE("custom production inspects a completed archive while an independent archive is blocked", "[benchmark][pipeline]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires three assigned CPU lanes");
 ScopedTempDir root("custom-independent-archives");
 LocalCoconutRecipe local(root.path());
 const std::array<std::pair<std::string, std::string>, 2> members{{{coco(7).member, white_jpeg(3, 3, 1024 * 1024)}, {coco(10).member, white_jpeg()}}};
 replace_physical_images(local.cache, local.catalog, CoconutImageNamespace::CocoTrain, members);
 ServedPhysicalArchive train(local.cache, local.catalog, CoconutImageNamespace::CocoTrain, "train");
 ServedPhysicalArchive validation(local.cache, local.catalog, CoconutImageNamespace::CocoValidation, "val");
 auto catalog = local_custom_catalog(local);
 std::filesystem::remove(train.path);
 std::filesystem::remove(validation.path);
 train.server.GateNextTransfer();
 auto config = local.compiler_config({BenchmarkDatasetVariant::CocoCustom, CoconutValidation::CoconutStock});
 config.num_workers = 3;
 std::promise<void> inspected;
 std::atomic<bool> delivered{false};
 auto compiling = std::async(std::launch::async, [&] {
  compile_benchmark_recipe(config, nullptr, &catalog, [&](BenchmarkDatasetSource source, std::string_view split) {
   if (source == BenchmarkDatasetSource::kCoco2017 && split == "val2017" && !delivered.exchange(true)) inspected.set_value();
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] { train.server.ReleasePartial(); });
 REQUIRE(train.server.WaitPartial());
 mmltk::testsupport::await_test_promise(inspected, "independent validation archive inspection", 3s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 CHECK(std::filesystem::is_regular_file(cached_image_path(local.cache.source_images("coco") / "val2017", 9)));
 train.server.ReleasePartial();
 mmltk::testsupport::await_test_future(compiling, "independent custom artifacts", 5s);
 CHECK(CompiledDataset::open(local.output / "train.bin").image_entries().size() == 4);
 train.server.Check();
 validation.server.Check();
}
TEST_CASE("COCONut consumes ready releases while an unrelated lifecycle lease is blocked", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires three assigned CPU lanes");
 bool fail_blocked_release = false, cold_ready_release = false;
 SECTION("ready releases settle in canonical output order") {}
 SECTION("a cold ready release normalizes before the blocked release settles") { cold_ready_release = true; }
 SECTION("a failed later release retains the published generation") { fail_blocked_release = true; }
 ScopedTempDir root("coconut-release-readiness");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 compile_benchmark_recipe(config, &catalog);
 compile_benchmark_recipe(config, &catalog);
 const auto train = file_bytes(local.output / "train.bin");
 const auto validation = file_bytes(local.output / "val.bin");
 const auto manifest = file_bytes(local.output / "benchmark_manifest.json");
 if (cold_ready_release) {
  const auto facts = Json::parse(manifest);
  for (const auto& component : facts.at("recipe").at("components"))
   if (component.at("edition") == CoconutEdition::RelabeledValidation) std::filesystem::remove(local.cache.root / component.at("imported_index").get<std::string>());
 }
 REQUIRE(catalog.releases.size() > 2);  // More releases than the two acquisition/parser lanes.
 const auto& blocked_release = catalog.releases.front();
 const auto lock_path = local.cache.locks / (std::string(blocked_release.name) + ".annotations.lifecycle.lock");
 std::optional<ArtifactLease> lease(ArtifactLease::acquire(lock_path, {}));
 if (fail_blocked_release) {
  auto& artifact = catalog.releases.front().annotations.front();
  const auto path = local.cache.source_downloads("coconut-" + std::string(blocked_release.name)) / artifact.filename;
  mmltk::testsupport::write_text_file(path, "invalid required Parquet release");
  artifact.expected_size = std::filesystem::file_size(path);
  artifact.expected_sha256 = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(path));
  // The poisoned catalog-matching artifact fails structurally without retrying
  // an unrelated network endpoint. Its former normalized products are stale.
 }
 std::promise<void> ready_releases;
 unsigned admitted = 0;
 config.num_workers = 3;
 std::atomic<bool> cancelled{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event != (cold_ready_release ? "benchmark.annotations.release_complete" : "benchmark.annotations.release_metadata")) return;
  const auto value = Json::parse(fields);
  if (value.at("edition") != blocked_release.edition && ++admitted == catalog.releases.size() - 1) ready_releases.set_value();
 };
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  lease.reset();
 });
 mmltk::testsupport::await_test_promise(ready_releases, "all independent release admissions", 5s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 check_publication_bytes(local.output, train, validation, manifest);
 lease.reset();
 if (fail_blocked_release)
  CHECK_THROWS(mmltk::testsupport::await_test_future(compiling, "failed release drain", 5s));
 else
  mmltk::testsupport::await_test_future(compiling, "canonical ready release merge", 5s);
 check_publication_bytes(local.output, train, validation, manifest);
 // No completed or failed release retains lifecycle custody after compilation.
 for (const auto& component : catalog.releases) {
  const auto path = local.cache.locks / (std::string(component.name) + ".annotations.lifecycle.lock");
  const mmltk::common::io::ScopedFd descriptor(::open(path.c_str(), O_RDWR | O_CLOEXEC));
  REQUIRE(descriptor.get() >= 0);
  CHECK(::flock(descriptor.get(), LOCK_EX | LOCK_NB) == 0);
 }
}
TEST_CASE("concurrent recovery workspaces share immutable physical lookups", "[benchmark][coconut][recovery]") {
 using namespace std::chrono_literals;
 const std::array<RLEPair, 1> mask{{{0, 2}}}, whole{{{0, 9}}};
 const auto originals = recovery_originals(7, mask);
 const auto indexed_view = fixture_index(originals);
 const CoconutRecoveryOriginals indexed(&indexed_view, nullptr);
 CoconutMaskRecovery first(indexed), second(indexed);
 CHECK(first.original_identity(CoconutImageNamespace::CocoTrain).data() == second.original_identity(CoconutImageNamespace::CocoTrain).data());
 CoconutRecord record;
 record.image_id = 7;
 record.segments = {{.id = 10, .category_id = 18, .isthing = true}, {.id = 30, .category_id = 63, .isthing = true}};
 mmltk::testsupport::TestGate readers("shared originals readers");
 const auto receipt = readers.receipt();
 const auto recover = [&](CoconutMaskRecovery& workspace) {
  std::vector<CoconutSegmentSupport> support{{}, recovery_support(whole)};
  CoconutRecoveryImage facts{7, 0, {}};
  receipt.ArriveAndWait();
  workspace.apply(CoconutImageNamespace::CocoTrain, record, 3, 3, support, facts);
  return std::pair{std::move(support), std::move(facts)};
 };
 auto left = std::async(std::launch::async, [&] { return recover(first); });
 auto right = std::async(std::launch::async, [&] { return recover(second); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { readers.Release(); });
 REQUIRE(readers.WaitEntered(2s, 2));
 readers.Release();
 const auto a = mmltk::testsupport::await_test_future(left, "first recovery workspace");
 const auto b = mmltk::testsupport::await_test_future(right, "second recovery workspace");
 REQUIRE(a.second.objects.size() == 1);
 REQUIRE(b.second.objects.size() == 1);
 CHECK(a.second.objects[0].original_annotation_id == b.second.objects[0].original_annotation_id);
 CHECK(a.first[0].area == 2);
 CHECK(b.first[0].area == 2);
 CHECK(a.first[1].area == 7);
 CHECK(b.first[1].area == 7);
 CHECK(originals.mask_rle_pairs[0].start == 0);
 CHECK(originals.mask_rle_pairs[0].length == 2);
}
TEST_CASE("cancellation retires release work while original annotations are locked", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires three assigned CPU lanes");
 ScopedTempDir root("coconut-originals-cancellation");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 auto catalog = local.selected(config.selection.validation);
 compile_benchmark_recipe(config, &catalog);
 const auto train = file_bytes(local.output / "train.bin"), validation = file_bytes(local.output / "val.bin");
 const auto manifest = file_bytes(local.output / "benchmark_manifest.json");
 auto originals = ArtifactLease::acquire(local.cache.locks / "coco-annotations.lifecycle.lock", {});
 config.selection.validation = CoconutValidation::Stock;
 catalog = local.selected(config.selection.validation);
 config.num_workers = 3;
 std::atomic<bool> cancelled{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 std::promise<void> waiting;
 config.trace = [&](std::string_view event, std::string_view) {
  if (event == "benchmark.annotations.originals_wait") waiting.set_value();
 };
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog); });
 const mmltk::testsupport::ScopedTestCleanup cancel([&] { cancelled.store(true); });
 mmltk::testsupport::await_test_promise(waiting, "original-index dependency", 5s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 cancelled.store(true);
 CHECK_THROWS(mmltk::testsupport::await_test_future(compiling, "original-index cancellation drain", 5s));
 check_publication_bytes(local.output, train, validation, manifest);
}
TEST_CASE("COCONut receives metadata while a managed release lane is importing masks", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires three assigned CPU lanes");
 ScopedTempDir root("coconut-metadata-receiver");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 compile_benchmark_recipe(config, &catalog);
 compile_benchmark_recipe(config, &catalog);
 const auto train = file_bytes(local.output / "train.bin"), validation = file_bytes(local.output / "val.bin");
 const auto manifest = file_bytes(local.output / "benchmark_manifest.json");
 const auto facts = Json::parse(manifest);
 for (const auto& component : facts.at("recipe").at("components")) std::filesystem::remove(local.cache.root / component.at("imported_index").get<std::string>());
 REQUIRE(catalog.releases.size() > 2);
 const auto blocked = catalog.releases.front().edition;
 const auto& delayed = catalog.releases.at(1);
 std::optional<ArtifactLease> acquisition(ArtifactLease::acquire(local.cache.locks / (std::string(delayed.name) + ".annotations.lifecycle.lock"), {}));
 mmltk::testsupport::TestGate masks("managed release full-mask boundary");
 const auto receipt = masks.receipt();
 std::promise<void> received;
 std::size_t consumed = 0;
 catalog.release_observer = [&](CoconutEdition edition, CoconutReleaseBoundary boundary) {
  if (edition == blocked && boundary == CoconutReleaseBoundary::MasksStarted) receipt.ArriveAndWait();
  if (edition != blocked && boundary == CoconutReleaseBoundary::MetadataConsumed && ++consumed == catalog.releases.size() - 1) received.set_value();
 };
 config.num_workers = 3;
 SECTION("one shared CPU") { config.num_workers = 1; }
 SECTION("three shared CPUs") {}
 std::atomic<bool> cancelled{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  acquisition.reset();
  masks.Release();
 });
 REQUIRE(masks.WaitEntered(5s));
 acquisition.reset();
 mmltk::testsupport::await_test_promise(received, "metadata consumption during independent full-mask work", 5s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 masks.Release();
 mmltk::testsupport::await_test_future(compiling, "metadata receiver completion", 5s);
 check_publication_bytes(local.output, train, validation, manifest);
}
TEST_CASE("cold COCONut releases retain one aggregate indexing denominator", "[benchmark][coconut][progress]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires three assigned CPU lanes");
 ScopedTempDir root("coconut-indexing-interleave");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 bool repair_annotations = false;
 SECTION("independent releases settle while extraction remains foreground") {}
 SECTION("partial annotation replacement withdraws only its release rows") { repair_annotations = true; }
 std::string annotation_payload;
 std::unique_ptr<mmltk::backend::data::testsupport::HttpServer> annotation_server;
 if (repair_annotations) {
  auto& release = *std::ranges::find(catalog.releases, CoconutEdition::Base, &CoconutReleaseComponent::edition);
  auto& physical = *std::ranges::find(catalog.images, CoconutImageNamespace::CocoTrain, &RecipeImageArchive::source);
  const auto archive = local.cache.source_downloads("coco") / physical.artifact.filename;
  const std::array<std::uint32_t, 9> pixels{1, 1, 1, 1, 1, 1, 1, 1, 1};
  const auto mask = png(3, 3, pixels);
  Json rows = Json::array();
  std::vector<std::pair<std::string, std::string>> members;
  for (unsigned id = 100; id < 165; ++id) {
   rows.push_back(hf_row(id, mask, Json::array({segment()})));
   members.emplace_back(coco(id).member, white_jpeg());
  }
  tar(archive, members);
  physical.artifact.expected_size = std::filesystem::file_size(archive);
  release.expected_rows = rows.size();
  auto& artifact = release.annotations.front();
  const auto path = local.cache.source_downloads("coconut-" + std::string(release.name)) / artifact.filename;
  parquet_file(path, rows, hf_schema(), parquet::Compression::UNCOMPRESSED);
  annotation_payload = file_bytes(path);
  annotation_server = std::make_unique<mmltk::backend::data::testsupport::HttpServer>(annotation_payload);
  artifact.url = annotation_server->url("release-repair");
  artifact.expected_size = annotation_payload.size();
  auto corrupt = mask;
  corrupt.front() = 'x';
  rows.back()["mask"]["bytes"] = corrupt;
  parquet_file(path, rows, hf_schema(), parquet::Compression::UNCOMPRESSED);
  REQUIRE(std::filesystem::file_size(path) == artifact.expected_size);
 }
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock});
 config.num_workers = 3;
 std::uint64_t total = 0;
 for (const auto& release : catalog.releases) total += release.expected_rows;
 REQUIRE(catalog.releases[0].expected_rows != catalog.releases[1].expected_rows);
 const auto base_rows = catalog.releases[0].expected_rows;
 mmltk::testsupport::TestGate base("base cold mask import"), last_release("last independent cold mask import"), receiver("metadata receiver before pixel phase");
 const auto base_receipt = base.receipt(), last_receipt = last_release.receipt(), receiver_receipt = receiver.receipt();
 catalog.release_observer = [&](CoconutEdition edition, CoconutReleaseBoundary boundary) {
  if (edition == CoconutEdition::Base) {
   if (boundary == CoconutReleaseBoundary::MasksStarted)
    base_receipt.ArriveAndWait();
   else
    receiver_receipt.ArriveAndWait();
  } else if (edition == catalog.releases.back().edition && boundary == CoconutReleaseBoundary::MasksStarted)
   last_receipt.ArriveAndWait();
 };
 std::vector<BenchmarkCompileProgress> indexing;
 std::promise<void> independent_settled, all_settled, extracting;
 bool extracting_reported = false;
 bool independent_reported = false, all_reported = false;
 bool foreground_preserved = true, background_observed = false;
 std::optional<BenchmarkCompileProgress> previous_update;
 config.progress = [&](const BenchmarkCompileProgress& value) {
  if (previous_update && previous_update->phase == DatasetCompilePhase::Extracting && value.tracks.labels.completed != previous_update->tracks.labels.completed) {
   background_observed = true;
   foreground_preserved = foreground_preserved && value.phase == previous_update->phase && value.activity == previous_update->activity && value.current_source == previous_update->current_source &&
                          value.completed == previous_update->completed && value.total == previous_update->total && value.activity_elapsed_seconds >= previous_update->activity_elapsed_seconds;
  }
  previous_update = value;
  if (value.phase == DatasetCompilePhase::Extracting && !extracting_reported) {
   extracting_reported = true;
   extracting.set_value();
  }
  if (value.tracks.labels.total < total || value.tracks.labels.activity == DatasetCompileActivity::Preparing || value.tracks.labels.completed > total) return;
  indexing.push_back(value);
  if (value.tracks.labels.completed == total - base_rows && !independent_reported) {
   independent_reported = true;
   independent_settled.set_value();
  }
  if (value.tracks.labels.completed == total && !all_reported) {
   all_reported = true;
   all_settled.set_value();
  }
 };
 std::atomic<bool> cancelled{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  base.Release();
  last_release.Release();
  receiver.Release();
 });
 REQUIRE(base.WaitEntered(5s));
 REQUIRE(last_release.WaitEntered(5s));
 REQUIRE(receiver.WaitEntered(5s));
 receiver.Release();
 mmltk::testsupport::await_test_promise(extracting, "foreground extraction while masks remain active", 5s);
 last_release.Release();
 mmltk::testsupport::await_test_promise(independent_settled, "independent cold releases indexed", 5s);
 base.Release();
 mmltk::testsupport::await_test_promise(all_settled, "aggregate indexing settlement", 5s);
 receiver.Release();
 mmltk::testsupport::await_test_future(compiling, "interleaved indexing compile", 5s);
 REQUIRE_FALSE(indexing.empty());
 CHECK(background_observed);
 CHECK(foreground_preserved);
 CHECK(indexing.front().tracks.labels.completed == 0);
 CHECK(indexing.back().tracks.labels.completed == total);
 const auto background = std::ranges::find_if(indexing, [&](const auto& value) { return value.phase == DatasetCompilePhase::Extracting && value.tracks.labels.completed == total; });
 REQUIRE(background != indexing.end());
 std::uint64_t previous = 0;
 for (const auto& value : indexing) {
  CHECK(value.tracks.labels.total >= total);
  if (value.tracks.labels.completed < previous) {
   CHECK(repair_annotations);
   CHECK(previous - value.tracks.labels.completed == 64);
   CHECK(value.tracks.labels.invalidated == 64);
   CHECK(value.tracks.labels.completed == total - base_rows);
   CHECK(value.tracks.labels.active);
   CHECK(value.tracks.labels.activity == DatasetCompileActivity::Normalizing);
  }
  CHECK(value.tracks.labels.completed <= total);
  previous = value.tracks.labels.completed;
 }
 CHECK(indexing.back().tracks.labels.invalidated == (repair_annotations ? 64 : 0));
 if (annotation_server) {
  CHECK(annotation_server->requests() == 1);
  annotation_server->Check();
 }
 CHECK(CompiledDataset::open(local.output / "val.bin").image_entries().size() == 1);
}

TEST_CASE("cold benchmark readers finish oversized grants with retained release control on one CPU", "[benchmark][coconut][pipeline]") {
 ScopedTempDir root("cold-shared-readers");
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 65536, .descriptors = 32}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 auto release = execution.reserve(BenchmarkResources::handles(1, true, 4));
 const std::array<std::uint32_t, 1> ids{1};
 const auto mask = png(1, 1, ids);
 const std::array physical{coco(7), objects(1)};
 auto input = request(physical);
 input.execution = &execution;
 input.parent_allowance = release;
 SECTION("Parquet retains its complete batch and consumer grant") {
  const auto path = root.path() / "input.parquet";
  parquet_file(path, Json::array({hf_row(7, mask, Json::array({segment()}), 1, 1)}));
  input.parquet_shards = {path};
 }
 SECTION("raw masks retain their archive and consumer grant") {
  input.edition = CoconutEdition::XLarge;
  input.mask_archive = root.path() / "input.tar";
  const std::array<std::pair<std::string, std::string>, 2> entries{{
   {"coconuts_xlarge/panseg_info/objects365_v2_00000001.json", Json::array({segment()}).dump()},
   {"coconuts_xlarge/panseg/objects365_v2_00000001.png", mask}}};
  tar(input.mask_archive, entries);
 }
 SECTION("gzip extraction retains source and directory control") {
  const auto path = root.path() / "images.tar.gz";
  const std::array<std::pair<std::string, std::string>, 1> entries{{{"train2017/000000000007.jpg", mask}}};
  tar(path, entries);
  const std::array<std::uint64_t, 1> selected{7};
  auto work = std::async(std::launch::async, [&] {
   return extract_selected_archive_images({.archive_path = path, .source_identity = "cold-gzip", .output_root = root.path() / "images", .source = "coco", .shard = "train2017",
    .selected_image_ids = selected, .image_id_parser = [](std::string_view) { return std::optional<std::uint64_t>{7}; }, .execution = &execution, .parent_allowance = release});
  });
  const mmltk::testsupport::ScopedTestCleanup cancel([&] { cancelled.store(true); });
  CHECK(mmltk::testsupport::await_test_future(work, "cold gzip shared grant").image_count == 1);
  return;
 }
 auto work = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup cancel([&] { cancelled.store(true); });
 const auto result = mmltk::testsupport::await_test_future(work, "cold annotation shared grant");
 REQUIRE(result.size() == 1);
 CHECK(result.front().index().image_count() == 1);
 CHECK(execution.try_reserve({65537, 0, true}).has_value());
}

TEST_CASE("one archive controller selects independent or prefetched work from real capacity", "[benchmark][pipeline]") {
 using namespace std::chrono_literals;
 bool pressure = false, release_capacity = false, cancel_wait = false, fail_wait = false;
 SECTION("admitted prefetch progresses when another source cannot fit") { pressure = true; }
 SECTION("independent extraction progresses while prefetch remains held") {}
 SECTION("last pixel credit wakes independent extraction with both prefetches held") { release_capacity = true; }
 SECTION("external cancellation releases an admission waiter with both prefetches held") { release_capacity = cancel_wait = true; }
 SECTION("source failure releases an admission waiter with both prefetches held") { release_capacity = fail_wait = true; }
 ScopedTempDir root("custom-controller-descriptor-dependency");
 LocalCoconutRecipe local(root.path());
 auto catalog = local_custom_catalog(local);
 auto config = local.compiler_config({BenchmarkDatasetVariant::CocoCustom, CoconutValidation::Stock});
 config.num_workers = 1;
 std::atomic<bool> cancelled{false}, first_selection{true};
 mmltk::testsupport::TestGate selection("metadata finished before archive selection"), prefetch("prefetch owns its source envelope"),
  denied("independent source has no admission"), independent("Objects365 pixel before COCO prefetch settlement"),
  archive_selection("archive controller before source choice"), pixel("last Open Images reader owns group custody"),
  source_returned("Open Images acquisition has returned"), waiting("archive controller registered its admission wait"), stopped("archive admission wait stopped");
 struct AdmissionObservation {
  const std::atomic<bool>& cancelled_flag;
  mmltk::testsupport::TestGate& waiting;
  struct Local { mmltk::testsupport::TestGate* next = nullptr; bool armed = false; unsigned polls = 0; };
  static Local& local() { thread_local Local value; return value; }
  bool cancelled() const noexcept {
   auto& state = local();
   // Trace delivery is serialized. Hold the source at its next cancellation
   // boundary after that callback returns, leaving other sources observable.
   if (auto* gate = std::exchange(state.next, nullptr)) gate->receipt().ArriveAndWait();
   // The first observation checks the admission predicate. The second follows
   // an actual condition wait. Selection resets this on an intervening retry.
   if (state.armed && ++state.polls == 2) waiting.receipt().ArriveAndWait();
   return cancelled_flag.load();
  }
 } observation{cancelled, waiting};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Borrow(observation);
 config.progress = [&](const BenchmarkCompileProgress& value) {
  if (value.phase == DatasetCompilePhase::Extracting && first_selection.exchange(false)) selection.receipt().ArriveAndWait();
 };
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.storage.reserved") {
   const auto path = std::filesystem::path(Json::parse(fields).at("path").get<std::string>());
   if (path == local.cache.source_downloads("coco") / catalog.coco_train_images.filename || path == local.cache.source_downloads("coco") / catalog.coco_val_images.filename)
    AdmissionObservation::local().next = &prefetch;
  }
  if (event == "benchmark.archive.selection") {
   AdmissionObservation::local() = {};
   if (release_capacity) AdmissionObservation::local().next = &archive_selection;
  }
  if ((pressure || release_capacity) && event == "benchmark.archive.admission_wait") {
   AdmissionObservation::local() = {.next = &denied, .armed = release_capacity};
  }
  if (release_capacity && event == "benchmark.archive.admission_stopped") {
   AdmissionObservation::local() = {};
   stopped.receipt().ArriveAndWait();
  }
 };
 const auto objects_root = local.cache.source_images("objects365") / "patch-32";
 const auto open_images_root = local.cache.source_images("open-images") / "train";
 auto work = std::async(std::launch::async, [&] {
  compile_benchmark_recipe(config, nullptr, &catalog, [&](BenchmarkDatasetSource source, std::string_view) {
   if (release_capacity && source == BenchmarkDatasetSource::kOpenImagesV7) {
    source_returned.receipt().ArriveAndWait();
    if (fail_wait) throw std::runtime_error("Open Images label owner failed");
   }
  }, [&](const std::filesystem::path& source, std::uint64_t) {
   if (!pressure && source == objects_root) independent.receipt().ArriveAndWait();
   if (release_capacity && source == open_images_root) pixel.receipt().ArriveAndWait();
  }, {.descriptors = release_capacity ? 26U : pressure ? 24U : 64U});
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  selection.Release(); prefetch.Release(); denied.Release(); independent.Release();
  archive_selection.Release(); pixel.Release(); source_returned.Release(); waiting.Release(); stopped.Release();
 });
 REQUIRE(selection.WaitEntered(5s));
 REQUIRE(prefetch.WaitEntered(5s, release_capacity ? 2 : 1));
 selection.Release();
 if (release_capacity) {
  REQUIRE(archive_selection.WaitEntered(5s));
  REQUIRE(pixel.WaitEntered(5s));
  REQUIRE(source_returned.WaitEntered(5s));
  // Acquisition has dropped its own handles. Only the pixel retains this
  // group's promise; both COCO downloads continue to hold their envelopes.
  archive_selection.Release();
  REQUIRE(denied.WaitEntered(5s));
  denied.Release();
  REQUIRE(waiting.WaitEntered(5s));
  if (cancel_wait) cancelled.store(true);
  if (fail_wait) source_returned.Release();
  waiting.Release();
  if (cancel_wait || fail_wait) REQUIRE(stopped.WaitEntered(5s));
  else {
   pixel.Release();
   // No prefetch/readiness publisher runs between the denial and this open.
   // The last reader's returned credit alone must make selection progress.
   REQUIRE(independent.WaitEntered(5s));
  }
 } else if (pressure) REQUIRE(denied.WaitEntered(5s));
 else REQUIRE(independent.WaitEntered(5s));
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 denied.Release();
 independent.Release();
 pixel.Release();
 source_returned.Release();
 stopped.Release();
 prefetch.Release();
 if (cancel_wait) CHECK_THROWS(mmltk::testsupport::await_test_future(work, "cancelled source admission", 5s));
 else if (fail_wait) CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(work, "failed source admission", 5s), "Open Images label owner failed");
 else {
  mmltk::testsupport::await_test_future(work, "one-controller source admission", 5s);
  check_custom_compilation(local.output);
 }
}

TEST_CASE("custom preparation retains its prerequisite beside prefetched archive leases", "[benchmark][pipeline]") {
 ScopedTempDir root("custom-prefetch-descriptor-dependency");
 LocalCoconutRecipe local(root.path());
 auto catalog = local_custom_catalog(local);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = 64}, cancellation);
 auto preparation = execution.reserve(BenchmarkResources::handles(0, true, custom_annotation_resources().descriptors + custom_annotation_resources().continuation_descriptors));
 auto train = execution.reserve(BenchmarkResources::handles(1, true, 4));
 auto validation = execution.reserve(BenchmarkResources::handles(1, true, 4));
 auto train_lease = ArtifactLease::acquire_charged(local.cache.locks / "coco-train2017.images.lock", cancellation, train);
 auto validation_lease = ArtifactLease::acquire_charged(local.cache.locks / "coco-val2017.images.lock", cancellation, validation);
 auto competing = execution.reserve({0, 42, true});
 CHECK_FALSE(execution.try_reserve(custom_annotation_resources()).has_value());
 BenchmarkTraceSink trace;
 CHECK(download_artifacts({make_download_request(local.cache, "coco", catalog.coco_train_images)}, 1, cancellation, {}, trace, {}, &execution, train).size() == 1);
 CHECK(download_artifacts({make_download_request(local.cache, "coco", catalog.coco_val_images)}, 1, cancellation, {}, trace, {}, &execution, validation).size() == 1);
 ProgressReporter progress({}, trace);
 const auto config = local.compiler_config({BenchmarkDatasetVariant::CocoCustom, CoconutValidation::Stock});
 auto work = std::async(std::launch::async, [&] {
  return prepare_custom_recipe(config, local.cache, catalog, progress, 1, cancellation, trace, {}, &execution, std::move(preparation));
 });
 const mmltk::testsupport::ScopedTestCleanup cancel([&] { cancelled.store(true); });
 const auto result = mmltk::testsupport::await_test_future(work, "metadata beside retained prefetch commitments");
 REQUIRE(result.coco_train);
 REQUIRE(result.coco_val);
 CHECK(result.coco_train->images.size() == 2);
 CHECK(result.coco_val->images.size() == 1);
 CHECK(execution.try_reserve({0, custom_annotation_resources().descriptors + custom_annotation_resources().continuation_descriptors, true}).has_value());
 // Both source owners still hold their lease and can use their own promise.
 CHECK(execution.try_reserve({0, 4, true}, train).has_value());
 CHECK(execution.try_reserve({0, 4, true}, validation).has_value());
}

TEST_CASE("COCONut inventory and originals retain activation capacity beside a release", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("coconut-activation-descriptor-dependency");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::Stock);
 catalog.releases.resize(1);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = 40}, cancellation);
 auto preparation = execution.reserve(BenchmarkResources::handles(0, true, coco_annotation_resources().descriptors + coco_annotation_resources().continuation_descriptors));
 std::vector<CoconutPhysicalImage> physical;
 std::vector<AdmittedRecipeArchive> admitted;
 for (const auto& archive : catalog.images) {
  const auto path = local.cache.source_downloads(benchmark_source_name(archive.artifact.source)) / archive.artifact.filename;
  auto inventory = execution.reserve(BenchmarkResources::handles(1, true, 3), preparation);
  auto rows = coconut_image_archive_inventory(path, {}, archive.source, archive.shard, archive.artifact.artifact_id, cancellation, &execution.storage(), &execution, inventory);
  physical.insert(physical.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
  admitted.push_back({archive, {.path = path, .size = archive.artifact.expected_size, .identity = archive.artifact.artifact_id}});
 }
 const CoconutPhysicalMembership membership(physical);
 mmltk::testsupport::TestGate release_acquired("release holds its source allowance before activation");
 std::promise<void> originals_admitted;
 BenchmarkTraceSink trace = [&](std::string_view event, const Json& fields) {
  if (event == "benchmark.download.preseeded" && fields.at("artifact") == catalog.releases.front().annotations.front().artifact_id)
   release_acquired.receipt().ArriveAndWait();
  if (event == "benchmark.annotations.originals_begin") originals_admitted.set_value();
 };
 ProgressReporter progress({}, trace);
 CoconutFailureReport failures(local.cache.root, progress);
 std::vector<BenchmarkAllowance> competing;
 auto inputs = acquire_coconut_recipe_inputs(local.cache, catalog, progress, 1, cancellation, trace, 1, {}, {}, &execution);
 const mmltk::testsupport::ScopedTestCleanup release([&] { cancelled.store(true); release_acquired.Release(); });
 REQUIRE(release_acquired.WaitEntered(2s));
 // Fill only uncommitted producer capacity after the real release owns its
 // complete continuation. Originals must use the preparation promise already
 // retained by the caller, regardless of the release's descriptor demand.
 while (auto available = execution.try_reserve(BenchmarkResources::handles(1, true))) competing.push_back(std::move(*available));
 CHECK_FALSE(execution.try_reserve(coco_annotation_resources()).has_value());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Stock});
 auto work = std::async(std::launch::async, [&] {
  return prepare_coconut_recipe(config, local.cache, catalog, admitted, membership, progress, failures, 1, cancellation, trace, {}, false, inputs, &execution, std::move(preparation));
 });
 const mmltk::testsupport::ScopedTestCleanup release_before_join([&] { cancelled.store(true); release_acquired.Release(); });
 mmltk::testsupport::await_test_promise(originals_admitted, "originals draw their explicit preparation commitment");
 release_acquired.Release();
 const auto result = mmltk::testsupport::await_test_future(work, "release activation after prerequisite settlement", 5s);
 CHECK(result.components.size() == 2);
 REQUIRE(result.stock_validation);
 CHECK(result.stock_validation->images.size() == 1);
 competing.clear();
 CHECK(execution.try_reserve({0, 32, true}).has_value());
}

TEST_CASE("COCONut JSON source rejection leaves shared CPU admission usable", "[benchmark][coconut][pipeline]") {
 ScopedTempDir root("shared-json-source-errors");
 BenchmarkCompilePipeline execution(1);
 const std::array physical{objects(1)};
 auto input = request(physical, CoconutEdition::Large);
 input.execution = &execution;
 input.metadata_only = true;
 input.annotation_json = root.path() / "annotations.json";
 Json image{{"id", 1}, {"file_name", "objects365_v2_00000001.png"}, {"width", 1}, {"height", 1}};
 Json annotation{{"image_id", 1}, {"file_name", "objects365_v2_00000001.png"}, {"segments_info", Json::array({segment()})}};
 json_file(input.annotation_json, {{"images", Json::array({image, image})}, {"annotations", Json::array({annotation})}});
 CHECK_THROWS(import_coconut_annotations(input));
 json_file(input.annotation_json, {{"images", Json::array({image})}, {"annotations", Json::array({annotation})}});
 const auto result = import_coconut_annotations(input);
 REQUIRE(result.size() == 1);
 CHECK(result.front().index().image_count() == 1);
}

TEST_CASE("COCONut JSON parsing yields one shared CPU before its source pass finishes", "[benchmark][coconut][pipeline]") {
 ScopedTempDir root("shared-json-fairness");
 BenchmarkCompilePipeline execution(1);
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 const std::array<std::uint32_t, 1> mask_ids{1};
 const auto encoded = png(1, 1, mask_ids);
 BenchmarkEncodedImage::publish(cached_image_path(images, 7), {reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()}, {});
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{images}};
 split.images = {{7, 1, 1, 0, 0, 0}};
 BenchmarkWriteRequest pixels{.split = split, .output_path = root.path() / "result.bin", .resolution = 1, .num_workers = 1, .execution = &execution};
 BenchmarkSplitWriter writer(pixels);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 execution.register_split(writer, split);
 std::vector<CoconutPhysicalImage> physical;
 Json rows = Json::array(), annotations = Json::array();
 for (unsigned id = 1; id <= 256; ++id) {
  physical.push_back(objects(id));
  const auto name = std::filesystem::path(physical.back().member).stem().string() + ".png";
  rows.push_back({{"id", id}, {"file_name", name}, {"width", 1}, {"height", 1}});
  annotations.push_back({{"image_id", id}, {"file_name", name}, {"segments_info", Json::array({segment()})}});
 }
 auto input = request(physical, CoconutEdition::Large);
 input.metadata_only = true;
 input.execution = &execution;
 input.annotation_json = root.path() / "annotations.json";
 json_file(input.annotation_json, {{"images", std::move(rows)}, {"annotations", std::move(annotations)}});
 mmltk::testsupport::TestGate entered("JSON parser shared lane"), later("JSON parser remains in its source pass");
 struct ObserveParser {
  BenchmarkCompilePipeline& execution;
  mmltk::testsupport::TestGate& entered;
  mmltk::testsupport::TestGate& later;
  mutable std::size_t polls = 0;
  bool cancelled() const noexcept {
   try { (void)execution.current_lane(); } catch (...) { return false; }
   if (++polls == 1) entered.receipt().ArriveAndWait();
   if (polls == 128) later.receipt().ArriveAndWait();
   return false;
  }
 } observed{execution, entered, later};
 input.cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(observed);
 auto work = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { entered.Release(); later.Release(); });
 REQUIRE(entered.WaitEntered(std::chrono::seconds(2)));
 execution.source_publication(images, {})({7, {}, true});
 entered.Release();
 REQUIRE(later.WaitEntered(std::chrono::seconds(2)));
 CHECK(writer.image_complete(0));
 later.Release();
 const auto result = mmltk::testsupport::await_test_future(work, "shared JSON source pass");
 REQUIRE(result.size() == 1);
 CHECK(result.front().index().image_count() == 256);
}

TEST_CASE("custom annotation sources acquire lifecycle custody independently on one CPU", "[benchmark][pipeline]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("custom-independent-lifecycle");
 LocalCoconutRecipe local(root.path());
 const auto catalog = local_custom_catalog(local);
 auto blocked = ArtifactLease::acquire(local.cache.locks / "coco-annotations.lifecycle.lock", {});
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = 24}, cancellation);
 std::promise<void> independent;
 std::atomic<unsigned> completed{0};
 const BenchmarkTraceSink trace = [&](std::string_view event, const Json& fields) {
  if (event == "benchmark.annotations.cache_hit" && fields.at("source") != "coco" && completed.fetch_add(1) == 1) independent.set_value();
 };
 ProgressReporter progress({}, trace);
 const auto config = local.compiler_config({BenchmarkDatasetVariant::CocoCustom, CoconutValidation::Stock});
 auto work = std::async(std::launch::async, [&] { return prepare_custom_recipe(config, local.cache, catalog, progress, 1, cancellation, trace, {}, &execution); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { cancelled.store(true); blocked = {}; });
 mmltk::testsupport::await_test_promise(independent, "supplemental metadata while COCO lifecycle is locked");
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 blocked = {};
 const auto result = mmltk::testsupport::await_test_future(work, "independent custom metadata settlement");
 REQUIRE(result.coco_train);
 REQUIRE(result.coco_val);
 REQUIRE(result.objects);
 REQUIRE(result.open_images);
 CHECK(execution.try_reserve({0, 24}).has_value());
}
TEST_CASE("sixteen-descriptor annotation admission protects HTTP before independent lifecycle locks", "[benchmark][pipeline]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("custom-fixed-transport-before-lifecycles");
 LocalCoconutRecipe local(root.path());
 auto catalog = local_custom_catalog(local);
 const auto raw = local.cache.source_downloads("coco") / catalog.coco_annotations.filename;
 const auto payload = file_bytes(raw);
 mmltk::backend::data::testsupport::HttpServer server(payload);
 catalog.coco_annotations.url = server.url("annotations");
 catalog.coco_annotations.expected_size = payload.size();
 std::filesystem::remove(raw);
 std::filesystem::remove(raw.string() + ".download.json");
 remove_normalized_annotation_index(local.cache.source_indexes("coco") / "val2017.normalized.bin");
 auto blocked = ArtifactLease::acquire(local.cache.locks / "objects365-annotations.lifecycle.lock", {});
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = 16}, cancellation);
 std::promise<void> downloaded;
 const BenchmarkTraceSink trace = [&](std::string_view event, const Json& fields) {
  if (event == "benchmark.download.complete" && fields.at("artifact") == catalog.coco_annotations.artifact_id) downloaded.set_value();
 };
 ProgressReporter progress({}, trace);
 const auto config = local.compiler_config({BenchmarkDatasetVariant::CocoCustom, CoconutValidation::Stock});
 auto work = std::async(std::launch::async, [&] { return prepare_custom_recipe(config, local.cache, catalog, progress, 1, cancellation, trace, {}, &execution); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { cancelled.store(true); blocked = {}; });
 mmltk::testsupport::await_test_promise(downloaded, "COCO HTTP while independent Objects365 lifecycle is contended", 5s);
 CHECK(file_bytes(raw) == payload);
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 blocked = {};
 const auto result = mmltk::testsupport::await_test_future(work, "sixteen-descriptor annotation settlement", 5s);
 REQUIRE(result.coco_train);
 REQUIRE(result.coco_val);
 REQUIRE(result.objects);
 REQUIRE(result.open_images);
 CHECK(result.coco_val->images.size() == 1);
 CHECK(read_json_file((local.cache.source_indexes("coco") / "val2017.normalized.bin").string() + ".complete.json").at("complete") == true);
 CHECK(execution.try_reserve({0, 16}).has_value());
 const auto warm = prepare_custom_recipe(config, local.cache, catalog, progress, 1, cancellation, {}, {}, &execution);
 CHECK(warm.coco_indexes_cache_hit);
 CHECK(server.requests() == 1);
 CHECK(execution.try_reserve({0, 16}).has_value());
 server.Check();
}

TEST_CASE("COCONut annotation artifacts enter independently inside one release lifecycle", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 std::size_t descriptors = 32;
 SECTION("existing source budget") {}
 SECTION("thirteen descriptors") { descriptors = 13; }
 SECTION("sixteen descriptors") { descriptors = 16; }
 SECTION("twenty-four descriptors") { descriptors = 24; }
 SECTION("twenty-six descriptors") { descriptors = 26; }
 ScopedTempDir root("coconut-independent-annotation-artifacts");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 catalog.releases.resize(1);
 auto& release = catalog.releases.front();
 release.annotations.resize(1);
 auto artifact = release.annotations.front();
 artifact.artifact_id += "-independent";
 artifact.filename = "independent.parquet";
 const std::string payload = "independently acquired annotation bytes";
 mmltk::backend::data::testsupport::HttpServer server(payload);
 artifact.url = server.url("annotation");
 artifact.expected_size = payload.size();
 artifact.expected_sha256.clear();
 release.annotations.push_back(artifact);
 auto blocked = ArtifactLease::acquire(local.cache.locks / (release.annotations.front().artifact_id + ".lock"), {});
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = descriptors}, cancellation);
 std::promise<void> independent;
 const BenchmarkTraceSink trace = [&](std::string_view event, const Json& fields) {
  if (event == "benchmark.download.complete" && fields.at("artifact") == artifact.artifact_id) independent.set_value();
 };
 ProgressReporter progress({}, trace);
 auto inputs = acquire_coconut_recipe_inputs(local.cache, catalog, progress, 1, cancellation, trace, 1, {}, {}, &execution);
 const mmltk::testsupport::ScopedTestCleanup settle([&] { cancelled.store(true); blocked = {}; inputs.reset(); });
 mmltk::testsupport::await_test_promise(independent, "second annotation while first artifact is locked");
 CHECK(file_bytes(local.cache.source_downloads("coconut-" + std::string(release.name)) / artifact.filename) == payload);
 cancelled.store(true);
 blocked = {};
 inputs.reset();
 cancelled.store(false);
 CHECK(execution.try_reserve({0, descriptors}).has_value());
 server.Check();
}

TEST_CASE("a contended archive lock leaves the sole controller available to ready prefetched sources", "[benchmark][pipeline]") {
 using namespace std::chrono_literals;
 bool cancel_wait = false;
 SECTION("ready archives and independent image work finish before unlock") {}
 SECTION("cancellation retires prefetched leases while the unrelated lock stays held") { cancel_wait = true; }
 ScopedTempDir root("contended-archive-selector");
 LocalCoconutRecipe local(root.path());
 auto catalog = local_custom_catalog(local);
 auto config = local.compiler_config({BenchmarkDatasetVariant::CocoCustom, CoconutValidation::Stock});
 config.num_workers = 1;
 auto blocked = ArtifactLease::acquire(local.cache.locks / "objects365-patch-32.images.lock", {});
 mmltk::testsupport::TestGate train("train prefetch before cache admission"), validation("validation prefetch before cache admission"), waiting("selector returned failed lock custody");
 std::atomic<bool> cancelled{false}, observed_wait{false};
 struct Observation {
  const std::atomic<bool>& cancelled_flag;
  static mmltk::testsupport::TestGate*& next() { thread_local mmltk::testsupport::TestGate* value = nullptr; return value; }
  bool cancelled() const noexcept {
   if (auto* gate = std::exchange(next(), nullptr)) gate->receipt().ArriveAndWait();
   return cancelled_flag.load();
  }
 } observation{cancelled};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Borrow(observation);
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.storage.reserved") {
   const auto path = std::filesystem::path(Json::parse(fields).at("path").get<std::string>());
   if (path == local.cache.source_downloads("coco") / catalog.coco_train_images.filename) Observation::next() = &train;
   if (path == local.cache.source_downloads("coco") / catalog.coco_val_images.filename) Observation::next() = &validation;
  }
  if (event == "benchmark.archive.admission_wait" && !observed_wait.exchange(true)) Observation::next() = &waiting;
 };
 std::promise<void> independent_ready, validation_ready, train_ready;
 std::atomic<bool> reported_validation{false}, reported_train{false};
 const auto descriptors = [&] {
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
   std::error_code error;
   const auto path = std::filesystem::read_symlink(entry.path(), error);
   if (!error && path.native().starts_with(root.path().native() + "/")) ++count;
  }
  return count;
 };
 const auto before = descriptors();
 auto work = std::async(std::launch::async, [&] {
  compile_benchmark_recipe(config, nullptr, &catalog, [&](BenchmarkDatasetSource source, std::string_view) {
   if (source == BenchmarkDatasetSource::kOpenImagesV7) independent_ready.set_value();
  }, [&](const std::filesystem::path& source, std::uint64_t) {
   if (source == local.cache.source_images("coco") / "val2017" && !reported_validation.exchange(true)) validation_ready.set_value();
   if (source == local.cache.source_images("coco") / "train2017" && !reported_train.exchange(true)) train_ready.set_value();
  }, {.descriptors = 52});
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] { cancelled.store(true); train.Release(); validation.Release(); waiting.Release(); blocked = {}; });
 REQUIRE(train.WaitEntered(5s));
 REQUIRE(validation.WaitEntered(5s));
 REQUIRE(waiting.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(independent_ready, "independent Open Images beside contended archive", 5s);
 // A formerly pregranted blocked Objects365 source could occupy this sole
 // controller. The ready validation archive must now receive its turn.
 validation.Release(); waiting.Release();
 mmltk::testsupport::await_test_promise(validation_ready, "ready validation archive while Objects365 remains locked", 5s);
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 if (cancel_wait) cancelled.store(true);
 train.Release();
 if (cancel_wait) CHECK_THROWS(mmltk::testsupport::await_test_future(work, "cancelled archive selection custody", 5s));
 else {
  mmltk::testsupport::await_test_promise(train_ready, "second prefetched archive while Objects365 remains locked", 5s);
  CHECK(work.wait_for(0ms) == std::future_status::timeout);
  blocked = {};
  mmltk::testsupport::await_test_future(work, "unlocked independent archive", 5s);
  check_custom_compilation(local.output);
 }
 // All compile file descriptors and captured source leases have retired. Keep the
 // external blocking descriptor in both snapshots for the cancellation case.
 CHECK(descriptors() == before - (cancel_wait ? 0 : 1));
 for (const auto& name : {"coco-train2017.images.lock", "coco-val2017.images.lock"})
  CHECK(ArtifactLease::try_acquire_charged(local.cache.locks / name, {}, nullptr));
}

TEST_CASE("archive transport seeks only consumed generations across supported formats", "[benchmark][coconut][archive]") {
 ScopedTempDir root("archive-consumption");
 int format = ARCHIVE_FORMAT_TAR_USTAR;
 bool gzip = false;
 std::string name = "payload/first.bin";
 SECTION("raw tar") {}
 SECTION("gzip tar") { gzip = true; }
 SECTION("ZIP") { format = ARCHIVE_FORMAT_ZIP; }
 SECTION("PAX extended names") { format = ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE; name = "payload/" + std::string(140, 'p') + ".bin"; }
 SECTION("GNU extended names") { format = ARCHIVE_FORMAT_TAR_GNUTAR; name = "payload/" + std::string(140, 'g') + ".bin"; }
 const auto path = root.path() / (gzip ? "source.tar.gz" : "source.archive");
 const std::array<std::pair<std::string, std::string>, 2> members{{{name, "first generation"}, {"payload/second.bin", "second member"}}};
 {
  std::unique_ptr<archive, decltype(&archive_write_free)> output(archive_write_new(), archive_write_free);
  REQUIRE(archive_write_set_format(output.get(), format) == ARCHIVE_OK);
  if (gzip) REQUIRE(archive_write_add_filter_gzip(output.get()) == ARCHIVE_OK);
  REQUIRE(archive_write_open_filename(output.get(), path.c_str()) == ARCHIVE_OK);
  for (const auto& [member, bytes] : members) {
   std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
   archive_entry_set_pathname(entry.get(), member.c_str()); archive_entry_set_filetype(entry.get(), AE_IFREG);
   archive_entry_set_perm(entry.get(), 0600); archive_entry_set_size(entry.get(), bytes.size());
   REQUIRE(archive_write_header(output.get(), entry.get()) == ARCHIVE_OK);
   REQUIRE(archive_write_data(output.get(), bytes.data(), bytes.size()) == static_cast<la_ssize_t>(bytes.size()));
  }
  REQUIRE(archive_write_close(output.get()) == ARCHIVE_OK);
 }
 BenchmarkArchive input(path);
 REQUIRE(input.next()); CHECK(input.member() == name);
 auto first = input.read(1024);
 CHECK(std::string(first.begin(), first.end()) == members[0].second);
 const auto first_position = input.member_position();
 {
  BenchmarkArchive resumed(path);
  REQUIRE(resumed.seek(first_position));
  const auto reopened = resumed.read(1024);
  CHECK(std::string(reopened.begin(), reopened.end()) == members[0].second);
 }
 REQUIRE(input.next()); CHECK(input.member() == members[1].first);
 const auto second = input.read(1024);
 CHECK(std::string(second.begin(), second.end()) == members[1].second);
 const auto replacement = root.path() / "replacement.tar";
 const std::array<std::pair<std::string, std::string>, 1> changed{{{name, "different generation"}}};
 tar(replacement, changed);
 std::filesystem::rename(replacement, path);
 {
  BenchmarkArchive changed_generation(path);
  CHECK_THROWS_AS(changed_generation.seek(first_position), BenchmarkArchiveError);
 }
 REQUIRE(input.seek(first_position));
 first = input.read(1024);
 CHECK(std::string(first.begin(), first.end()) == members[0].second);
 CHECK_FALSE(input.seek("missing.bin"));
 input.pause();
 const std::array required{members[1].first, members[0].first};
 std::vector<std::size_t> order;
 input.visit_known(required, [&](std::size_t index) {
  order.push_back(index);
  const auto actual = input.read(1024);
  CHECK(std::string(actual.begin(), actual.end()) == members[1 - index].second);
 });
 CHECK(order == std::vector<std::size_t>{1, 0});
}
TEST_CASE("requested archive extraction closes before unsafe unused members", "[benchmark][coconut][archive]") {
 ScopedTempDir root("archive-unused-tail");
 const auto path = root.path() / "images.tar";
 const std::array<std::uint32_t, 1> pixels{0x001122};
 const auto encoded = png(1, 1, pixels);
 const std::array<std::pair<std::string, std::string>, 2> members{{{"images/1.jpg", encoded}, {"../unused.jpg", "unsafe unused bytes"}}};
 tar(path, members);
 const std::array<std::uint64_t, 1> ids{1};
 const auto result = extract_selected_archive_images({.archive_path = path, .source_identity = "consumed-only", .output_root = root.path() / "images", .source = "coco", .shard = "train2017",
  .selected_image_ids = ids, .image_id_parser = [](std::string_view name) -> std::optional<std::uint64_t> { return name == "images/1.jpg" ? std::optional<std::uint64_t>{1} : std::nullopt; },
  .cache_write_workers = 0});
 CHECK(result.image_count == 1);
 BenchmarkArchive reader(path);
 REQUIRE(reader.next()); (void)reader.read(1024);
 REQUIRE(reader.next());
 CHECK_THROWS(reader.read(1024));
}
TEST_CASE("Parquet namespace hints preserve recognized routes and unknown input", "[benchmark][coconut][parquet]") {
 ScopedTempDir root("parquet-hints");
 const std::array<std::uint32_t, 1> pixels{0};
 const auto encoded = png(1, 1, pixels);
 const auto path = root.path() / "hints.parquet";
 parquet_file(path, Json::array({hf_row(1, encoded, Json::array(), 1, 1, "unlabeled2017"), hf_row(2, encoded, Json::array(), 1, 1, "unknown"), hf_row(3, encoded, Json::array(), 1, 1, "val2017")}));
 std::vector<std::optional<CoconutImageNamespace>> hints;
 const std::array paths{path};
 read_coconut_parquet(paths, {}, {}, [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput&) { hints.push_back(record.namespace_hint); }, true);
 REQUIRE(hints.size() == 3);
 CHECK(hints[0] == CoconutImageNamespace::CocoUnlabeled); CHECK_FALSE(hints[1]); CHECK(hints[2] == CoconutImageNamespace::CocoValidation);
}

TEST_CASE("archive consumed-member checks retain sparse payloads and reject required conflicts", "[benchmark][coconut][archive]") {
 ScopedTempDir root("archive-member-boundaries");
 const auto path = root.path() / "members.tar";
 SECTION("PAX sparse payloads retain logical holes through a late reread") {
  std::string bytes(8192, '\0'); bytes.replace(0, 4, "head"); bytes.replace(8188, 4, "tail");
  std::unique_ptr<archive, decltype(&archive_write_free)> output(archive_write_new(), archive_write_free);
  REQUIRE(archive_write_set_format_pax(output.get()) == ARCHIVE_OK);
  REQUIRE(archive_write_open_filename(output.get(), path.c_str()) == ARCHIVE_OK);
  std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
  archive_entry_set_pathname(entry.get(), "sparse.bin"); archive_entry_set_filetype(entry.get(), AE_IFREG);
  archive_entry_set_perm(entry.get(), 0600); archive_entry_set_size(entry.get(), bytes.size());
  archive_entry_sparse_add_entry(entry.get(), 0, 4); archive_entry_sparse_add_entry(entry.get(), 8188, 4);
  REQUIRE(archive_write_header(output.get(), entry.get()) == ARCHIVE_OK);
  REQUIRE(archive_write_data(output.get(), bytes.data(), bytes.size()) == static_cast<la_ssize_t>(bytes.size()));
  REQUIRE(archive_write_close(output.get()) == ARCHIVE_OK);
  BenchmarkArchive input(path);
  REQUIRE(input.next());
  auto actual = input.read(bytes.size()); CHECK(std::string(actual.begin(), actual.end()) == bytes);
  input.pause(); input.resume(bytes.size());
  REQUIRE(input.seek("sparse.bin")); actual = input.read(bytes.size());
  CHECK(std::string(actual.begin(), actual.end()) == bytes);
  input.pause();
  const std::array required{std::string("sparse.bin")};
  input.visit_known(required, [&](std::size_t index) {
   CHECK(index == 0);
   const auto data = input.read(bytes.size());
   CHECK(std::string(data.begin(), data.end()) == bytes);
  });
 }
 SECTION("conflicting required names fail when encountered and consumed") {
  const std::array<std::pair<std::string, std::string>, 2> members{{{"same.bin", "first"}, {"same.bin", "second"}}};
  tar(path, members); BenchmarkArchive input(path);
  REQUIRE(input.next()); (void)input.read(1024);
  REQUIRE(input.next()); CHECK_THROWS_AS(input.read(1024), BenchmarkArchiveError);
  CHECK_THROWS_AS(input.seek("same.bin"), BenchmarkArchiveError);
  const std::array required{std::string("same.bin")};
  CHECK_THROWS_AS(input.visit_known(required, [](std::size_t) {}), BenchmarkArchiveError);
 }
 SECTION("missing requests resolve only after the eligible stream ends") {
  const std::array<std::pair<std::string, std::string>, 1> members{{{"other.bin", "other"}}};
  tar(path, members); BenchmarkArchive input(path);
  REQUIRE(input.next()); CHECK_FALSE(input.seek("missing.bin"));
  CHECK_FALSE(input.next()); CHECK_FALSE(input.seek("missing.bin"));
  const std::array required{std::string("missing.bin")};
  CHECK_THROWS_AS(input.visit_known(required, [](std::size_t) {}), BenchmarkArchiveError);
 }
 SECTION("a consumed symbolic link is rejected") {
  std::unique_ptr<archive, decltype(&archive_write_free)> output(archive_write_new(), archive_write_free);
  REQUIRE(archive_write_set_format_pax(output.get()) == ARCHIVE_OK);
  REQUIRE(archive_write_open_filename(output.get(), path.c_str()) == ARCHIVE_OK);
  std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
  archive_entry_set_pathname(entry.get(), "image.jpg"); archive_entry_set_filetype(entry.get(), AE_IFLNK);
  archive_entry_set_symlink(entry.get(), "../outside.jpg"); archive_entry_set_size(entry.get(), 0);
  REQUIRE(archive_write_header(output.get(), entry.get()) == ARCHIVE_OK);
  REQUIRE(archive_write_close(output.get()) == ARCHIVE_OK);
  BenchmarkArchive input(path); REQUIRE(input.next()); CHECK_THROWS_AS(input.read(1024), BenchmarkArchiveError);
 }
}
TEST_CASE("COCONut physical routing consumes requests without an eager archive census", "[benchmark][coconut][archive]") {
 ScopedTempDir root("physical-request-routing");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::Coconut);
 auto& physical = *std::ranges::find(catalog.images, CoconutImageNamespace::Objects365V2, &RecipeImageArchive::source);
 const auto path = local.cache.source_downloads("objects365") / physical.artifact.filename;
 // Both real requests precede a malformed unused member. A census or a tail
 // validation walk would reject this source before useful image admission.
 const std::array<std::pair<std::string, std::string>, 3> members{{{objects(2).member, white_jpeg()}, {objects(1).member, white_jpeg()}, {"../unused.jpg", "unused"}}};
 tar(path, members); physical.artifact.expected_size = std::filesystem::file_size(path);
 auto& base = *std::ranges::find(catalog.releases, CoconutEdition::Base, &CoconutReleaseComponent::edition);
 auto& artifact = base.annotations.front();
 const auto parquet = local.cache.source_downloads("coconut-" + std::string(base.name)) / artifact.filename;
 const std::array<std::uint32_t, 9> empty{};
 // An incorrect recognized hint and an unknown hint both retain identity until
 // a real eligible source supplies the requested member.
 parquet_file(parquet, Json::array({hf_row(7, png(3, 3, empty), Json::array(), 3, 3, "unlabeled2017"), hf_row(8, png(3, 3, empty), Json::array(), 3, 3, "unknown")}));
 artifact.expected_size = std::filesystem::file_size(parquet);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Coconut}, true);
 compile_benchmark_recipe(config, &catalog);
 CHECK(CompiledDataset::open(config.output_dir / "train.bin").image_entries().size() == 4);
 // Warm component locators and individually cached images continue to use the
 // same physical generation without walking its unused suffix.
 const auto image = cached_image_path(local.cache.source_images("objects365") / "patch-32", 1);
 struct stat before{}, after{}; REQUIRE(::stat(image.c_str(), &before) == 0);
 compile_benchmark_recipe(config, &catalog);
 REQUIRE(::stat(image.c_str(), &after) == 0); CHECK(before.st_ino == after.st_ino);
}
TEST_CASE("local physical cache publication failure bypasses source repair", "[benchmark][coconut][archive]") {
 ScopedTempDir root("physical-local-failure");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::Coconut);
 const auto blocked = local.cache.source_images("coco") / "train2017";
 std::filesystem::remove_all(blocked);
 mmltk::testsupport::write_text_file(blocked, "not a cache directory");
 std::atomic<std::size_t> repairs{0};
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Coconut}, true);
 config.trace = [&](std::string_view event, std::string_view) { if (event == "benchmark.download.failure_sha256") ++repairs; };
 CHECK_THROWS_AS(compile_benchmark_recipe(config, &catalog), std::filesystem::filesystem_error);
 CHECK(repairs.load() == 0);
}
TEST_CASE("archive image payloads outlive acquisition and exceptional pool retirement", "[benchmark][coconut][archive]") {
 ScopedTempDir root("archive-payload-lease");
 bool fail = false;
 SECTION("completed producer") {}
 SECTION("failed readiness callback") { fail = true; }
 const auto path = root.path() / "images.tar";
 const auto encoded = white_jpeg();
 const std::array<std::pair<std::string, std::string>, 1> members{{{"images/1.jpg", encoded}}};
 tar(path, members);
 const std::array<std::uint64_t, 1> ids{1};
 std::shared_ptr<const BenchmarkEncodedImage> retained;
 ArchiveExtractionRequest request{.archive_path = path, .source_identity = "payload-generation", .output_root = root.path() / "images", .source = "coco", .shard = "train2017",
  .selected_image_ids = ids, .image_id_parser = [](std::string_view name) -> std::optional<std::uint64_t> { return name == "images/1.jpg" ? std::optional<std::uint64_t>{1} : std::nullopt; }, .cache_write_workers = 0};
 request.image_ready = [&](const CachedImageReady& ready) { retained = ready.payload; if (fail) throw std::runtime_error("retire acquisition"); };
 if (fail) CHECK_THROWS(extract_selected_archive_images(request)); else (void)extract_selected_archive_images(request);
 REQUIRE(retained); REQUIRE(retained->backing());
 CHECK(std::string(retained->encoded().begin(), retained->encoded().end()) == encoded);
 std::weak_ptr<const void> backing = retained->backing();
 retained.reset(); CHECK(backing.expired());
}
TEST_CASE("complete physical continuations progress beside Curl at low descriptor limits", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 std::size_t descriptors = 13;
 SECTION("thirteen descriptors") {}
 SECTION("sixteen descriptors") { descriptors = 16; }
 SECTION("twenty-four descriptors and two admitted batches") { descriptors = 24; }
 SECTION("twenty-six descriptors and two admitted batches") { descriptors = 26; }
 const bool two_admitted = descriptors >= 24;
 ScopedTempDir root("physical-consumer-envelope");
 const std::array<std::uint32_t, 1> pixels{0};
 const auto parquet = root.path() / "source.parquet", archive = root.path() / "source.tar.gz";
 parquet_file(parquet, Json::array({hf_row(1, png(1, 1, pixels), Json::array(), 1, 1)}));
 const std::array<std::pair<std::string, std::string>, 1> members{{{"member.bin", "consumed bytes"}}};
 tar(archive, members);
 const auto workspace = BenchmarkArchive::workspace_bytes(archive, 1024);
 const auto target = (two_admitted ? 2 : 1) * ((256ULL << 20) + (128ULL << 10) + workspace);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = target, .descriptors = descriptors}, cancellation);
 auto channel = execution.curl().channel(BenchmarkCurl::Class::Artifact, cancellation, coco_annotation_resources());
 auto release = ArtifactLease::acquire_charged(root.path() / "release.lock", cancellation, &execution, coco_annotation_resources());
 auto independent = two_admitted ? ArtifactLease::acquire_charged(root.path() / "independent.lock", cancellation, &execution, coco_annotation_resources()) : nullptr;
 StorageReservationPool storage(root.path(), {});
 mmltk::testsupport::TestGate held("first batch owns the shared reader and opened archive");
 std::promise<void> second_admitted;
 std::mutex reader_mutex;
 std::future<void> first, second;
 const mmltk::testsupport::ScopedTestCleanup settle([&] { cancelled.store(true); held.Release(); });
 const std::array paths{parquet};
 const auto consumer = CoconutPhysicalInputRequirement::archive(workspace);
 const auto consume = [&](bool initial) {
  read_coconut_parquet(paths, {}, cancellation, [&](std::size_t, const CoconutRecord&, const CoconutAnnotationInput& batch) {
   const auto& allowance = batch.allowance;
   if (!initial) second_admitted.set_value();
   const std::lock_guard lock(reader_mutex);
   CHECK(allowance.bytes() >= (256ULL << 20) + consumer.workspace_bytes());
   CHECK(allowance.descriptors() == 1);
   CHECK(batch.live_bytes >= consumer.workspace_bytes());
   auto physical = ArtifactLease::acquire_charged(root.path() / "physical.lock", cancellation, &execution, consumer.lease_controls(), allowance);
   BenchmarkArchive input(archive, &execution, 1024, physical->allowance(), 1, true, allowance);
   if (initial) held.receipt().ArriveAndWait();
   auto output_handles = execution.reserve(BenchmarkResources::handles(2, true), physical->allowance());
   mmltk::common::io::FileHandle directory(::open(root.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
   if (directory.get() < 0) throw std::runtime_error("cannot open test output directory");
   const auto destination = root.path() / (initial ? "first.bin" : "second.bin");
   auto output = BenchmarkStagedArtifact::create(storage, destination, 1024, "admitted physical output", ".tmp.XXXXXX", 0644, directory.get());
   if (!input.next(cancellation)) throw std::runtime_error("missing admitted archive member");
   input.consume(1024, [&](std::span<const std::uint8_t> bytes, std::uint64_t offset) { output.file().pwrite_all(bytes.data(), bytes.size(), offset); }, cancellation);
   output.publish(destination, cancellation, BenchmarkStagedArtifact::Publication::Rename);
  }, true, &execution, {}, release->allowance(), consumer);
 };
 first = std::async(std::launch::async, [&] { consume(true); });
 REQUIRE(held.WaitEntered(5s));
 second = std::async(std::launch::async, [&] { consume(false); });
 if (two_admitted) mmltk::testsupport::await_test_promise(second_admitted, "second live Arrow batch before the shared reader lock", 5s);
 CHECK_FALSE(execution.try_reserve({1, 0}).has_value());
 {
  auto opportunity = execution.try_reserve(BenchmarkResources::handles(1, false));
  REQUIRE(opportunity);
  mmltk::common::io::FileHandle socket(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  REQUIRE(socket.get() >= 0);
  if (independent) {
   auto descriptor = execution.reserve(BenchmarkResources::handles(1, true), independent->allowance());
   auto input = mmltk::common::io::FileHandle::open_readonly(parquet.string());
   bool ran = false;
   execution.run(BenchmarkStage::Metadata, {}, [&](std::size_t) { ran = input.size() > 0; }, descriptor);
   CHECK(ran);
  }
 }
 held.Release();
 mmltk::testsupport::await_test_future(first, "first archive consumer uses its existing envelope", 5s);
 mmltk::testsupport::await_test_future(second, "second archive consumer uses its existing envelope", 5s);
 CHECK(file_bytes(root.path() / "first.bin") == members.front().second);
 CHECK(file_bytes(root.path() / "second.bin") == members.front().second);
 independent.reset(); release.reset(); channel.reset();
 CHECK(execution.try_reserve({target, descriptors}).has_value());
}
TEST_CASE("consumed ZIP checksums and truncated gzip filters remain source failures", "[benchmark][coconut][archive]") {
 ScopedTempDir root("archive-filter-errors");
 bool gzip = false;
 SECTION("consumed ZIP CRC") {}
 SECTION("truncated gzip payload") { gzip = true; }
 const auto path = root.path() / "source.archive";
 std::string payload(65536, '\0');
 for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>((i * 31) ^ (i >> 8));
 {
  std::unique_ptr<archive, decltype(&archive_write_free)> output(archive_write_new(), archive_write_free);
  REQUIRE(archive_write_set_format(output.get(), gzip ? ARCHIVE_FORMAT_TAR_USTAR : ARCHIVE_FORMAT_ZIP) == ARCHIVE_OK);
  if (gzip) REQUIRE(archive_write_add_filter_gzip(output.get()) == ARCHIVE_OK);
  else REQUIRE(archive_write_set_options(output.get(), "zip:compression=store") == ARCHIVE_OK);
  REQUIRE(archive_write_open_filename(output.get(), path.c_str()) == ARCHIVE_OK);
  std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
  archive_entry_set_pathname(entry.get(), "payload.bin"); archive_entry_set_filetype(entry.get(), AE_IFREG);
  archive_entry_set_perm(entry.get(), 0600); archive_entry_set_size(entry.get(), payload.size());
  REQUIRE(archive_write_header(output.get(), entry.get()) == ARCHIVE_OK);
  REQUIRE(archive_write_data(output.get(), payload.data(), payload.size()) == static_cast<la_ssize_t>(payload.size()));
  REQUIRE(archive_write_close(output.get()) == ARCHIVE_OK);
 }
 auto encoded = file_bytes(path);
 if (gzip) encoded.resize(encoded.size() / 2);
 else { const auto position = encoded.find(payload); REQUIRE(position != std::string::npos); encoded[position + payload.size() / 2] ^= 1; }
 mmltk::testsupport::write_text_file(path, encoded);
 const auto consume = [&] { BenchmarkArchive input(path); if (!input.next()) throw BenchmarkArchiveError("missing consumed member"); (void)input.read(payload.size()); };
 CHECK_THROWS_AS(consume(), BenchmarkArchiveError);
}

TEST_CASE("high-expansion gzip keeps a charged bounded dictionary working set", "[benchmark][coconut][archive]") {
 ScopedTempDir root("archive-gzip-expansion");
 const auto path = root.path() / "expanded.tar.gz";
 constexpr std::size_t expanded_bytes = 32U << 20;
 {
  const std::array<std::pair<std::string, std::string>, 3> members{{{"first.bin", "first"}, {"expanded.bin", std::string(expanded_bytes, 'z')}, {"last.bin", "last"}}};
  const auto raw = root.path() / "one-block.tar";
  tar(raw, members);
  mmltk::testsupport::write_text_file(path, gzip_fixture(file_bytes(raw), true));
 }
 REQUIRE(std::filesystem::file_size(path) < expanded_bytes / 100);
 const auto workspace = BenchmarkArchive::workspace_bytes(path, 1024);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = workspace, .descriptors = 16});
 {
  BenchmarkArchive input(path, &execution, 1024);
  // The single block is valid; retiring its oversized parallel representation
  // must preserve the callback cursor and every subsequently consumed byte.
  CHECK(input.allowance().bytes() == workspace);
  CHECK_FALSE(execution.try_reserve({1, 0}).has_value());
  REQUIRE(input.next()); (void)input.read(1024);
  REQUIRE(input.next()); REQUIRE(input.member() == "expanded.bin");
  std::uint64_t consumed = 0;
  input.consume(expanded_bytes, [&](std::span<const std::uint8_t> bytes, std::uint64_t offset) {
   CHECK(offset == consumed);
   REQUIRE(std::ranges::all_of(bytes, [](std::uint8_t byte) { return byte == 'z'; }));
   consumed += bytes.size();
  });
  CHECK(consumed == expanded_bytes);
  CHECK_FALSE(input.gzip_seek_state().retained);
  CHECK(input.gzip_seek_state().streaming);
  CHECK_FALSE(input.gzip_seek_state().control_capacity_reached);
  // Each dictionary is at most a deflate window plus compressed/allocation
  // overhead. In-flight chunk dictionaries share the charged decoder bound.
  CHECK(input.gzip_seek_state().dictionaries * (64ULL << 10) <= workspace);
  REQUIRE(input.seek("first.bin"));
  const auto first = input.read(1024);
  CHECK(std::string(first.begin(), first.end()) == "first");
  input.pause();
  CHECK(input.gzip_seek_state().dictionaries == 0);
  CHECK(execution.try_reserve({workspace, 0}).has_value());
 }
 CHECK(execution.try_reserve({workspace, 16}).has_value());
}
TEST_CASE("gzip dictionary eviction preserves parallel forward work and bounded controls", "[benchmark][coconut][archive]") {
 bool concatenated = false, control_limit = false, cancel_replay = false, corrupt_crc = false, bgzf = false;
 SECTION("ordinary rolling windows and late exact bytes") {}
 SECTION("concatenated gzip members") { concatenated = true; }
 SECTION("control ceiling independently retires the reader") { control_limit = true; }
 SECTION("BGZF finder prefetch obeys its control ceiling") { control_limit = true; bgzf = true; }
 SECTION("cancellation during a late serial prefix replay") { cancel_replay = true; }
 SECTION("consumed concatenated member CRC remains required") { concatenated = true; corrupt_crc = true; }
 ScopedTempDir root("archive-gzip-rolling");
 const auto raw = root.path() / "ordinary.tar", path = root.path() / "ordinary.tar.gz";
 constexpr std::size_t payload_bytes = 20U << 20;
 {
  const std::array<std::pair<std::string, std::string>, 3> members{{{"first.bin", "first"}, {"payload.bin", std::string(payload_bytes, 'q')}, {"last.bin", "last"}}};
  tar(raw, members);
 }
 {
  const auto bytes = file_bytes(raw);
  std::string encoded;
  if (bgzf) {
   for (std::size_t offset = 0; offset < bytes.size(); offset += 32768) {
    auto block = gzip_fixture(std::string_view(bytes).substr(offset, std::min<std::size_t>(32768, bytes.size() - offset)));
    block[3] = 4;
    const auto size = block.size() + 8 - 1;
    REQUIRE(size <= 65535);
    const std::array<char, 8> extra{6, 0, 'B', 'C', 2, 0, static_cast<char>(size), static_cast<char>(size >> 8)};
    block.insert(10, extra.data(), extra.size());
    encoded += block;
   }
   const std::array<unsigned char, 28> end{0x1f, 0x8b, 8, 4, 0, 0, 0, 0, 0, 255, 6, 0, 'B', 'C', 2, 0, 27, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   encoded.append(reinterpret_cast<const char*>(end.data()), end.size());
  } else {
   encoded = gzip_fixture(concatenated ? std::string_view(bytes).substr(0, bytes.size() / 2) : std::string_view(bytes));
   if (corrupt_crc) encoded[encoded.size() - 8] ^= 1;
   if (concatenated) encoded += gzip_fixture(std::string_view(bytes).substr(bytes.size() / 2));
  }
  mmltk::testsupport::write_text_file(path, encoded);
 }
 const auto workspace = BenchmarkArchive::workspace_bytes(path, 1024, 2);
 BenchmarkCompilePipeline execution(4, {}, {.transient_bytes = workspace, .descriptors = 16});
 {
  BenchmarkArchive input(path, &execution, 1024, {}, 2, true, {}, control_limit ? 1024 : 2, 0, control_limit ? 1 : 32768);
  REQUIRE(input.next());
  const auto first = input.member_position();
  const auto initial = input.read(1024);
  CHECK(std::string(initial.begin(), initial.end()) == "first");
  REQUIRE(input.next()); REQUIRE(input.member() == "payload.bin");
  std::uint64_t consumed = 0;
  const auto consume = [&] {
   input.consume(payload_bytes, [&](std::span<const std::uint8_t> bytes, std::uint64_t offset) {
    CHECK(offset == consumed);
    REQUIRE(std::ranges::all_of(bytes, [](std::uint8_t value) { return value == 'q'; }));
    consumed += bytes.size();
    const auto state = input.gzip_seek_state();
    CHECK(state.index_entries <= (control_limit ? 2 : 32769));
    CHECK(state.index_bytes < (8U << 20));
    CHECK(state.dictionaries <= 2 + 64 + 4);
   });
  };
  if (corrupt_crc) {
   CHECK_THROWS_AS(consume(), BenchmarkArchiveError);
  } else {
   consume();
   CHECK(consumed == payload_bytes);
   REQUIRE(input.next()); REQUIRE(input.member() == "last.bin");
   const auto last = input.member_position();
   const auto bytes = input.read(1024);
   CHECK(std::string(bytes.begin(), bytes.end()) == "last");
   CHECK_FALSE(input.next());
   const auto state = input.gzip_seek_state();
   CHECK(state.control_capacity_reached == control_limit);
   CHECK(state.retained == !control_limit);
   CHECK(state.streaming == control_limit);
   if (!control_limit) CHECK(state.rolling);
   if (cancel_replay) {
    PollCancellation observation{.stop_at = 32};
    CHECK_THROWS(input.seek(last, mmltk::common::concurrency::CancellationObservation::Borrow(observation)));
    CHECK(observation.polls >= observation.stop_at);
   } else {
    REQUIRE(input.seek(first));
    const auto replayed = input.read(1024);
    CHECK(std::string(replayed.begin(), replayed.end()) == "first");
   }
   CHECK(input.gzip_seek_state().streaming);
   CHECK_FALSE(input.gzip_seek_state().retained);
   CHECK(input.gzip_seek_state().index_entries == 0);
   CHECK(input.gzip_seek_state().index_bytes == 0);
   if (!cancel_replay) {
    if (control_limit) input.pause();
    const std::array required{std::string("last.bin"), std::string("first.bin")};
    std::vector<std::size_t> order;
    input.visit_known(required, [&](std::size_t index) {
     order.push_back(index);
     const auto value = input.read(1024);
     CHECK(std::string(value.begin(), value.end()) == (index == 0 ? "last" : "first"));
    });
    CHECK(order == std::vector<std::size_t>{1, 0});
   }
  }
  input.pause();
  CHECK(execution.try_reserve({workspace, 0}).has_value());
 }
 CHECK(execution.try_reserve({workspace, 16}).has_value());
}
TEST_CASE("physical metadata and pixels progress while stock originals are held", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("coconut-independent-originals");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Coconut, true});
 auto catalog = local.selected(config.selection.validation);
 std::atomic<bool> cancelled{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 std::optional<ArtifactLease> originals(ArtifactLease::acquire(local.cache.locks / "coco-annotations.lifecycle.lock", {}));
 std::promise<void> metadata, pixels;
 std::size_t releases = 0;
 std::atomic<bool> pixel_reported{false};
 catalog.release_observer = [&](CoconutEdition, CoconutReleaseBoundary boundary) {
  if (boundary == CoconutReleaseBoundary::MetadataConsumed && ++releases == catalog.releases.size()) metadata.set_value();
 };
 auto compiling = std::async(std::launch::async, [&] {
  compile_benchmark_recipe(config, &catalog, nullptr, {}, [&](const std::filesystem::path&, std::uint64_t) {
   if (!pixel_reported.exchange(true)) pixels.set_value();
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] { cancelled.store(true); originals.reset(); });
 mmltk::testsupport::await_test_promise(metadata, "all canonical metadata while original parsing is held", 5s);
 mmltk::testsupport::await_test_promise(pixels, "known pixel input while original parsing is held", 5s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 originals.reset();
 mmltk::testsupport::await_test_future(compiling, "originals and independent pixels settle", 5s);
 CHECK(CompiledDataset::open(config.output_dir / "train.bin").image_entries().size() == 4);
}
TEST_CASE("independent physical archives resolve while one image source is held", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("coconut-independent-physical-archives");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Coconut});
 auto catalog = local.selected(config.selection.validation);
 std::optional<ArtifactLease> source(ArtifactLease::acquire(local.cache.locks / "coco-train2017.images.lock", {}));
 std::atomic<bool> cancelled{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 std::promise<void> independent;
 std::size_t received = 0;
 catalog.release_observer = [&](CoconutEdition edition, CoconutReleaseBoundary boundary) {
  if (edition != CoconutEdition::Base && boundary == CoconutReleaseBoundary::MetadataConsumed && ++received == catalog.releases.size() - 1) independent.set_value();
 };
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { cancelled.store(true); source.reset(); });
 mmltk::testsupport::await_test_promise(independent, "independent physical source resolution with one shared CPU", 5s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 source.reset();
 mmltk::testsupport::await_test_future(compiling, "held source resolves after release", 5s);
}

TEST_CASE("XL discovery resolves ready physical requests before later JSON parsing", "[benchmark][coconut][archive]") {
 ScopedTempDir root("xl-incremental-discovery");
 const auto path = root.path() / "xl.tar";
 const std::array<std::pair<std::string, std::string>, 2> members{{
  {"coconuts_xlarge/panseg_info/objects365_v2_00000003.json", Json::array({segment()}).dump()},
  {"coconuts_xlarge/panseg_info/objects365_v2_00000002.json", "invalid later JSON"}
 }};
 tar(path, members);
 const auto cache = BenchmarkCacheLayout::create(root.path() / "cache");
 const auto images = cache.source_downloads("objects365") / "physical.tar";
 const auto jpeg = white_jpeg();
 const std::array<std::pair<std::string, std::string>, 1> image_members{{{objects(3).member, jpeg}}};
 tar(images, image_members);
 CoconutRecipeCatalog catalog;
 catalog.releases.push_back({CoconutEdition::XLarge, "fixture-xl", "fixture-v1", 1, {}});
 catalog.images.push_back({CoconutImageNamespace::Objects365V2, 32, "patch-32",
  {"physical", "unused", images.filename().string(), std::filesystem::file_size(images), "", BenchmarkDatasetSource::kObjects365V2}});
 auto admitted = local_physical_archives(cache, catalog);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1, .descriptors = 16});
 CoconutPhysicalMembership physical(admitted, catalog, true, cache, execution.storage(), execution);
 CoconutImportRequest input;
 input.execution = &execution;
 input.edition = CoconutEdition::XLarge; input.input_identity = "xl-discovery";
 input.metadata_only = true; input.mask_archive = path; input.physical_membership = &physical;
 CHECK_THROWS(import_coconut_annotations(input));
 const auto image_root = cache.source_images("objects365") / "patch-32";
 CHECK(file_bytes(cached_image_path(image_root, 3)) == jpeg);
 const auto geometry = execution.geometry(image_root, 3);
 REQUIRE(geometry);
 CHECK(geometry->width == 3);
 CHECK(geometry->height == 3);
 CHECK(execution.try_reserve({execution.transient_target(), 16}).has_value());
}

TEST_CASE("failed XL generations retire oversized backing before source repair", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 bool invalid_png = false, pinned = false;
 SECTION("valid JSON followed by malformed JSON") {}
 SECTION("malformed consumed mask after successful discovery") { invalid_png = true; }
 SECTION("matching pinned malformed input keeps its source failure") { pinned = true; }
 ScopedTempDir root("xl-repair-retirement");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::Coconut);
 auto& release = *std::ranges::find(catalog.releases, CoconutEdition::XLarge, &CoconutReleaseComponent::edition);
 auto& artifact = release.annotations.front();
 const auto path = local.cache.source_downloads("coconut-" + std::string(release.name)) / artifact.filename;
 const auto payload = file_bytes(path);
 const std::array<std::uint32_t, 9> support{1, 1, 1, 1, 1, 1, 1, 1, 1};
 const std::array<std::pair<std::string, std::string>, 4> malformed{{
  {"coconuts_xlarge/panseg/objects365_v2_00000001.png", invalid_png ? "malformed PNG" : png(3, 3, support)},
  {"coconuts_xlarge/panseg_info/objects365_v2_00000001.json", Json::array({segment()}).dump()},
  {"coconuts_xlarge/panseg/objects365_v2_00000002.png", png(3, 3, support)},
  {"coconuts_xlarge/panseg_info/objects365_v2_00000002.json", invalid_png ? Json::array({segment()}).dump() : "malformed later JSON"}
 }};
 tar(path, malformed);
 artifact.expected_size = 0;
 artifact.expected_sha256 = pinned ? mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(path)) : "";
 mmltk::backend::data::testsupport::HttpServer server(payload);
 artifact.url = server.url("xl-repair");
 server.GateNextRequest();
 std::atomic<bool> cancelled{false};
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Coconut});
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog, nullptr, {}, {}, {.transient_bytes = 1, .descriptors = 26}); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] { cancelled.store(true); server.ReleaseRequest(); });
 if (pinned) {
  REQUIRE(compiling.wait_for(10s) == std::future_status::ready);
  CHECK_THROWS(compiling.get());
  CHECK(server.requests() == 0);
 } else {
  REQUIRE(server.WaitRequest());
  CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
  server.ReleaseRequest();
  mmltk::testsupport::await_test_future(compiling, "XL repair reuses the unchanged one-byte scheduling target", 10s);
  CHECK(server.requests() == 1);
  CHECK(file_bytes(path) == payload);
  CHECK(CompiledDataset::open(config.output_dir / "train.bin").image_entries().size() == 4);
 }
 server.Check();
}

TEST_CASE("known XL masks preserve canonical ordinals across physical archive order and formats", "[benchmark][coconut][archive]") {
 int format = ARCHIVE_FORMAT_TAR_USTAR;
 bool gzip = false;
 SECTION("raw tar") {}
 SECTION("gzip tar") { gzip = true; }
 SECTION("ZIP") { format = ARCHIVE_FORMAT_ZIP; }
 SECTION("PAX extended records") { format = ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE; }
 SECTION("GNU records") { format = ARCHIVE_FORMAT_TAR_GNUTAR; }
 ScopedTempDir root("xl-ordered-masks");
 const std::array physical{objects(2), objects(3)};
 const std::array<std::uint32_t, 2> pixels{1, 1};
 const auto mask = png(2, 1, pixels);
 const std::string info_prefix = "coconuts_xlarge/panseg_info/", mask_prefix = "coconuts_xlarge/panseg/";
 const std::string lower = "objects365_v2_00000002", upper = "objects365_v2_00000003";
 const auto info = Json::array({segment(9, 200, false), segment()}).dump();
 std::optional<CoconutComponent> expected;
 for (const bool reverse : {false, true}) {
  auto input = request(physical, CoconutEdition::XLarge);
  input.mask_archive = root.path() / (gzip ? "xl.tar.gz" : "xl.archive");
  input.records = std::make_shared<CoconutAnnotationRecords>();
  const auto& first = reverse ? upper : lower;
  const auto& second = reverse ? lower : upper;
  const std::array<std::pair<std::string, std::string>, 6> members{{
   {mask_prefix + first + ".png", mask},
   {info_prefix + second + ".json", info},
   {"unused/" + std::string(150, 'x'), std::string(256U << 10, 'x')},
   {info_prefix + first + ".json", info},
   {mask_prefix + second + ".png", mask},
   // XL has always ignored masks with no offered JSON record.
   {mask_prefix + "objects365_v2_00000099.png", "unused invalid PNG"}
  }};
  // USTAR cannot encode an oversized single path component. A long prefix is
  // representable there, while PAX/GNU retain genuine extension records.
  auto contents = std::vector(members.begin(), members.end());
  if (format == ARCHIVE_FORMAT_TAR_USTAR) contents[2].first = "unused/payload.bin";
  tar(input.mask_archive, contents, false, format);
  input.metadata_only = true;
  const auto metadata = import_coconut_annotations(input);
  REQUIRE(metadata.size() == 1);
  REQUIRE(input.records->records.size() == 2);
  CHECK(input.records->records[0].image_id == 2);
  CHECK(input.records->records[0].first_segment_ordinal == 0);
  CHECK(input.records->records[1].first_segment_ordinal == 2);
  input.metadata_only = false;
  auto result = import_coconut_annotations(input);
  REQUIRE(result.size() == 1);
  const auto& component = result.front();
  REQUIRE(component.index().image_count() == 2);
  REQUIRE(component.index().box_count() == 2);
  CHECK(component.index().images()[0].source_image_id == 2);
  CHECK(component.index().images()[1].source_image_id == 3);
  CHECK(component.index().storage().boxes[0].source_ordinal == 1);
  CHECK(component.index().storage().boxes[1].source_ordinal == 3);
  CHECK(component.inventory()[0].source_ordinal == 0);
  CHECK(component.inventory()[1].source_ordinal == 1);
  if (!expected) expected = component;
  else {
   CHECK(std::ranges::equal(component.inventory(), expected->inventory()));
   CHECK(component.index().annotation_sha256 == expected->index().annotation_sha256);
   CHECK(component.index().run_count() == expected->index().run_count());
   CHECK(std::memcmp(component.index().storage().mask_rle_pairs.data(), expected->index().storage().mask_rle_pairs.data(), component.index().run_count() * sizeof(RLEPair)) == 0);
  }
  for (const auto failure : {"missing", "duplicate", "invalid PNG", "actual geometry"}) {
   INFO(failure);
   // Retire the borrowed inode before replacing the fixture generation.
   input.records->discard();
   auto rejected = contents;
   if (std::string_view(failure) == "missing") rejected.erase(rejected.begin());
   else if (std::string_view(failure) == "duplicate") rejected.push_back(rejected.front());
   else if (std::string_view(failure) == "invalid PNG") rejected.front().second = "invalid required PNG";
   input.limits.max_pixels = std::string_view(failure) == "actual geometry" ? 1 : 64U * 1024U * 1024U;
   tar(input.mask_archive, rejected, false, format);
   CHECK_THROWS(import_coconut_annotations(input));
  }
 }
}

TEST_CASE("physical owner withdraws only affected routes and refreshes their consumer envelope", "[benchmark][coconut][archive]") {
 ScopedTempDir root("physical-owner-withdrawal");
 LocalCoconutRecipe local(root.path());
 auto admitted = local_physical_archives(local.cache, local.catalog);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1, .descriptors = 16});
 CoconutPhysicalMembership physical(admitted, local.catalog, true, local.cache, execution.storage(), execution);
 auto& train = *std::ranges::find(admitted, CoconutImageNamespace::CocoTrain, [](const auto& row) { return row.origin.source; });
 auto& objects_archive = *std::ranges::find(admitted, CoconutImageNamespace::Objects365V2, [](const auto& row) { return row.origin.source; });
 CHECK(physical.eligible(CoconutEdition::Base, train));
 CHECK_FALSE(physical.eligible(CoconutEdition::Large, train));
 CHECK(physical.eligible(CoconutEdition::Large, objects_archive));
 CoconutRecord base;
 base.image_id = 7; base.file_name = "000000000007.jpg";
 CoconutRecord large;
 large.image_id = 601; large.physical_stem = "objects365_v2_00000001";
 const auto before = physical.input_requirement(CoconutEdition::Base);
 const auto other = physical.input_requirement(CoconutEdition::Large);
 CHECK(before.lease_controls().retained_handles);
 CHECK(before.lease_controls().bytes == archive_image_resources().bytes);
 CHECK(before.continuation_descriptors() == 5);
 const auto original = physical.resolve(CoconutEdition::Base, "base", base, {});
 physical.release_readers(CoconutEdition::Base);
 const auto independent = physical.resolve(CoconutEdition::Large, "large", large, {});
 physical.release_readers(CoconutEdition::Large);
 const auto other_path = cached_image_path(local.cache.source_images("objects365") / objects_archive.origin.cache_shard, 1);
 struct stat first{}, second{};
 REQUIRE(::stat(other_path.c_str(), &first) == 0);
 const auto train_root = local.cache.source_images("coco") / train.origin.cache_shard;
 const auto admitted_image = execution.image_input(train_root, 7);
 REQUIRE(admitted_image);
 CHECK(admitted_image->storage() == BenchmarkEncodedImage::Storage::HeaderOnly);
 {
  mmltk::common::io::FileHandle directory(::open(train_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  REQUIRE(directory.get() >= 0);
  const auto image = BenchmarkEncodedImage::open(directory.get(), 7,
   [](std::uint64_t, std::span<const std::uint8_t>) -> BenchmarkImageHeader { throw std::runtime_error("physical publication header must be reused"); }, {}, nullptr, {}, admitted_image);
  REQUIRE(image);
  CHECK(image->header().width == admitted_image->header().width);
 }
 execution.retire_source(train_root);
 CHECK_FALSE(execution.image_input(train_root, 7));
 physical.withdraw(train);
 const auto replacement = root.path() / "replacement.tar.gz";
 const std::array<std::uint32_t, 1> pixel{0x001234};
 const auto encoded = png(1, 1, pixel);
 const std::array<std::pair<std::string, std::string>, 1> members{{{coco(7).member, encoded}}};
 tar(replacement, members);
 std::filesystem::rename(replacement, train.download.path);
 train.download.identity = "changed-physical-generation";
 train.resolution_workspace = BenchmarkArchive::workspace_bytes(train.download.path, 64ULL << 20);
 CHECK(physical.input_requirement(CoconutEdition::Base).workspace_bytes() > before.workspace_bytes());
 CHECK(physical.input_requirement(CoconutEdition::Large).workspace_bytes() == other.workspace_bytes());
 CHECK(physical.resolve(CoconutEdition::Large, "large", large, {}) == independent);
 REQUIRE(::stat(other_path.c_str(), &second) == 0);
 CHECK(first.st_ino == second.st_ino);
 const auto changed = physical.resolve(CoconutEdition::Base, "base", base, {});
 CHECK(changed.archive_identity != original.archive_identity);
 CHECK(file_bytes(cached_image_path(train_root, 7)) == encoded);
 const auto geometry = execution.geometry(train_root, 7);
 REQUIRE(geometry);
 CHECK(geometry->width == 1);
 CHECK(geometry->height == 1);
 physical.release_readers(CoconutEdition::Base);
 CHECK(execution.try_reserve({execution.transient_target(), 16}).has_value());
}

TEST_CASE("retained archive images keep storage after decoder CPUs return", "[benchmark][coconut][archive][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPU lanes");
 bool failure = false, live_cpus_charged = false;
 SECTION("successful stream") {}
 SECTION("readiness callback failure") { failure = true; }
 ScopedTempDir root("archive-execution-storage-split");
 const auto path = root.path() / "images.tar.gz";
 const auto encoded = white_jpeg();
 const std::array<std::pair<std::string, std::string>, 1> members{{{"image.jpg", encoded}}};
 tar(path, members);
 BenchmarkCompilePipeline execution(4, {}, {.descriptors = 16});
 std::shared_ptr<const BenchmarkEncodedImage> retained;
 const std::array<std::uint64_t, 1> ids{1};
 ArchiveExtractionRequest request{.archive_path = path, .source_identity = "retained-gzip", .output_root = root.path() / "images", .source = "coco", .shard = "train2017",
  .selected_image_ids = ids, .image_id_parser = [](std::string_view) { return std::optional<std::uint64_t>{1}; }, .decompression_workers = 2, .cache_write_workers = 0,
  .image_ready = [&](const CachedImageReady& ready) {
   retained = ready.payload;
   live_cpus_charged = !execution.try_reserve({0, 0, false, 3});
   if (failure) throw std::runtime_error("readiness callback failure");
  }, .execution = &execution};
 if (failure) CHECK_THROWS(extract_selected_archive_images(request));
 else CHECK(extract_selected_archive_images(request).image_count == 1);
 CHECK(live_cpus_charged);
 REQUIRE(retained);
 CHECK(retained->storage() == BenchmarkEncodedImage::Storage::Pooled);
 CHECK(std::string(retained->encoded().begin(), retained->encoded().end()) == encoded);
 CHECK(retained->allowance().bytes() == (64ULL << 20));
 CHECK(execution.try_reserve({0, 0, false, 3}).has_value());
 CHECK_FALSE(execution.try_reserve({execution.transient_target(), 0}).has_value());
 const std::weak_ptr<const void> backing = retained->backing();
 retained.reset();
 CHECK(backing.expired());
 CHECK(execution.try_reserve({execution.transient_target(), 16}).has_value());
}

TEST_CASE("paused archive custody cannot retain retired external execution", "[benchmark][coconut][archive][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPU lanes");
 ScopedTempDir root("paused-archive-cpu-custody");
 const auto path = root.path() / "archive.tar.gz";
 const std::array<std::pair<std::string, std::string>, 1> members{{{"member", "bytes"}}};
 tar(path, members);
 BenchmarkCompilePipeline execution(4);
 BenchmarkArchive archive(path, &execution, 1024, {}, 2);
 REQUIRE(archive.next());
 auto retained = archive.allowance();
 CHECK_FALSE(execution.try_reserve({0, 0, false, 3}).has_value());
 archive.pause();
 CHECK(execution.try_reserve({0, 0, false, 3}).has_value());
 CHECK(execution.try_reserve({execution.transient_target(), 0}).has_value());
 retained = {};
 CHECK(execution.try_reserve({execution.transient_target(), 0}).has_value());
}

TEST_CASE("oversized gzip storage admits its synchronous pixel continuation", "[benchmark][coconut][archive][pipeline]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("gzip-pixel-continuation");
 const auto path = root.path() / "images.tar.gz", images = root.path() / "images";
 const auto encoded = white_jpeg();
 const std::array<std::pair<std::string, std::string>, 2> members{{{"1.jpg", encoded}, {"2.jpg", encoded}}};
 tar(path, members);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1, .descriptors = 13}, cancellation);
 PreparedBenchmarkSplit split;
 split.name = "train"; split.class_names = {"person"}; split.sources = {{images}};
 split.images = {{1, 3, 3, 0, 0, 0}, {2, 3, 3, 0, 0, 0}};
 BenchmarkWriteRequest pixels{.split = split, .output_path = root.path() / "result.bin", .resolution = 1, .num_workers = 1, .cancel_requested = cancellation, .execution = &execution};
 BenchmarkSplitWriter writer(pixels, true);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 execution.register_split(writer, split);
 const auto publication = execution.source_publication(images, {});
 std::atomic<unsigned> consumed{0};
 const std::array<std::uint64_t, 2> selected{1, 2};
 auto extraction = std::async(std::launch::async, [&] {
  return extract_selected_archive_images({.archive_path = path, .source_identity = "oversized-continuation", .output_root = images, .source = "coco", .shard = "train2017",
   .selected_image_ids = selected, .image_id_parser = [](std::string_view name) { return std::optional<std::uint64_t>{name == "1.jpg" ? 1U : 2U}; }, .cancel_requested = cancellation,
   .image_ready = [&](const CachedImageReady& ready) { if (!publication.consume(ready)) throw std::runtime_error("missing synchronous pixel consumer"); ++consumed; }, .execution = &execution});
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] { cancelled.store(true); });
 CHECK(mmltk::testsupport::await_test_future(extraction, "oversized producer with required pixel continuation").image_count == 2);
 CHECK(consumed.load() == 2);
 CHECK(writer.completed() == 2);
 execution.drain();
 writer.finish(pixels);
 CHECK(CompiledDataset::open(pixels.output_path).header().num_images == 2);
}

TEST_CASE("retained Parquet metadata joins group ordinals and owns batches after reader retirement", "[benchmark][coconut][parquet][pipeline]") {
 bool fail = false, cancel = false, allocation_failure = false;
 SECTION("successful reader retirement") {}
 SECTION("consumer failure with a retained batch") { fail = true; }
 SECTION("cancellation with a retained batch") { cancel = true; }
 SECTION("partial consumer allocation with a retained batch") { allocation_failure = true; }
 ScopedTempDir root("parquet-batch-lifetime");
 const std::array<std::uint32_t, 1> pixels{1};
 const auto encoded = png(1, 1, pixels);
 const std::array paths{root.path() / "first.parquet", root.path() / "second.parquet"};
 parquet_file(paths[0], Json::array({hf_row(7, encoded, Json::array({segment(9, 200, false), segment()}), 1, 1), hf_row(8, encoded, Json::array({segment()}), 1, 1)}));
 parquet_file(paths[1], Json::array({hf_row(9, encoded, Json::array({segment(3), segment(1)}), 1, 1)}), hf_schema(true));
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256ULL << 20, .descriptors = 13});
 CoconutAnnotationRecords records;
 std::vector<std::string> names;
 read_coconut_parquet(paths, {}, {}, [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput& input) {
  CHECK(input.membership); names.push_back(record.file_name);
 }, true, &execution, {}, {}, {}, &records);
 REQUIRE(records.records.size() == 3);
 REQUIRE(records.groups.size() == 3);
 CoconutAnnotationInput retained;
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 const auto consume = [&] {
  read_coconut_parquet(paths, {}, cancellation, [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput& input) {
   CHECK_FALSE(input.membership);
   CHECK(record.file_name == names[record.source_ordinal]);
   if (!retained.backing && (fail || cancel || allocation_failure || record.source_ordinal == 2)) {
    retained = input;
    if (cancel) cancelled.store(true);
    if (allocation_failure) throw std::bad_alloc{};
    if (fail) throw std::runtime_error("retained PNG consumer failed");
   }
  }, false, &execution, {}, {}, {}, &records);
 };
 if (allocation_failure) CHECK_THROWS_AS(consume(), std::bad_alloc);
 else if (fail || cancel) CHECK_THROWS(consume()); else consume();
 REQUIRE(retained.backing);
 CHECK(std::string(retained.png.begin(), retained.png.end()) == encoded);
 CHECK(retained.allowance.descriptors() == 0);
 if (!fail && !cancel && !allocation_failure) {
  CHECK(records.records[0].first_segment_ordinal == 0);
  CHECK(records.records[1].first_segment_ordinal == 2);
  CHECK(records.records[2].first_segment_ordinal == 3);
  CHECK(records.groups[2].first_segment == 3);
 }
 records.discard();
 CHECK(std::string(retained.png.begin(), retained.png.end()) == encoded);
 // The batch keeps its whole heap pool, including array/offset storage beyond
 // the borrowed PNG extent, after every reader/footer descriptor has retired.
 CHECK_FALSE(execution.try_reserve({(256ULL << 20) - retained.png.size(), 13}));
 for (const auto& descriptor : std::filesystem::directory_iterator("/proc/self/fd")) {
  std::error_code error;
  const auto path = std::filesystem::read_symlink(descriptor.path(), error);
  if (!error) for (const auto& input : paths) CHECK(path != input);
 }
 const std::weak_ptr<const void> backing = retained.backing;
 retained = {};
 CHECK(backing.expired());
 CHECK(execution.try_reserve({256ULL << 20, 13}));
}

TEST_CASE("panoptic JSON keeps decoded metadata and one unchanged inventory seal", "[benchmark][coconut][annotations][cache]") {
 ScopedTempDir root("panoptic-semantic-rows");
 const std::array physical{objects(7)};
 auto input = request(physical, CoconutEdition::Large);
 input.records = std::make_shared<CoconutAnnotationRecords>();
 input.annotation_json = root.path() / "annotations.json";
 input.mask_archive = root.path() / "masks.tar";
 const std::string image = R"({"id":[],"\u0069d":7,"width":"bad","width":2,"height":1,"file_name":"objects365_v2_00000007.jpg"})";
 const std::string annotation = R"({"image_id":7,"file_name":"objects365_v2_00000007.png","segments_info":{},"segments_info":[{"id":false,"id":1,"category_id":1,"isthing":1,"bbox":[0,0,2,1],"area":18446744073709551616}]})";
 const auto document = std::string{"{\"annotations\":["} + annotation + "],\"\\u0069mages\":[" + image + "],\"categories\":" + category_catalog().dump() + "}";
 mmltk::testsupport::write_text_file(input.annotation_json, document);
 const std::array<std::uint32_t, 2> pixels{1, 1};
 const std::array<std::pair<std::string, std::string>, 1> masks{{{"panoptic_object365/objects365_v2_00000007.png", png(2, 1, pixels)}}};
 tar(input.mask_archive, masks);
 input.metadata_only = true;
 const auto metadata = import_coconut_annotations(input);
 REQUIRE(metadata.size() == 1);
 REQUIRE(input.records->records.size() == 1);
 CHECK(input.records->records[0].file_name == "objects365_v2_00000007.png");
 CHECK(input.records->records[0].width == 2);
 bool changed = false;
 SECTION("identical admitted inventory is shared") {}
 SECTION("changed release-row facts require a new inventory generation") {
  input.records->records[0].source_ordinal = 7;
  changed = true;
 }
 std::filesystem::remove(input.annotation_json);
 input.metadata_only = false;
 auto full = import_coconut_annotations(input);
 REQUIRE(full.size() == 1); REQUIRE(full[0].index().box_count() == 1);
 CHECK((&full[0].inventory_image(0) == &metadata[0].inventory_image(0)) == !changed);
 CHECK((full[0].index().annotation_sha256 == metadata[0].index().annotation_sha256) == !changed);
 CHECK(metadata[0].inventory_image(0).source_ordinal == 0);
 CHECK(full[0].inventory_image(0).source_ordinal == (changed ? 7 : 0));
 CHECK(full[0].index().storage().boxes[0].source_ordinal == 0);
 const auto path = root.path() / "component.bin";
 store_coconut_component(path, full[0]);
 REQUIRE(full[0].completion());
 CHECK_FALSE(metadata[0].completion());
 CHECK(coconut_component_storage_bytes(full[0]) == std::filesystem::file_size(path) + std::filesystem::file_size(path.string() + ".inventory") + std::filesystem::file_size(path.string() + ".complete.json"));
 const auto proof = read_json_file(path.string() + ".complete.json");
 CHECK(proof.contains("coconut"));
 for (const auto& field : {"edition", "source", "input_identity", "normalization", "inventory_identity", "inventory_count"}) {
  auto incomplete = proof; incomplete["coconut"].erase(field);
  write_json_atomically(path.string() + ".complete.json", incomplete, {});
  CHECK_FALSE(load_coconut_component(path, full[0].edition(), full[0].source(), input.input_identity));
 }
 write_json_atomically(path.string() + ".complete.json", proof, {});
 CHECK(load_coconut_component(path, full[0].edition(), full[0].source(), input.input_identity));
}

TEST_CASE("a live Parquet normalizer lends actual workspace to pixels on one CPU", "[benchmark][coconut][parquet][pipeline]") {
 ScopedTempDir root("parquet-normalizer-pixels");
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256ULL << 20, .descriptors = 13});
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 const std::array<std::uint32_t, 1> pixels{1};
 const auto encoded = png(1, 1, pixels);
 BenchmarkEncodedImage::publish(cached_image_path(images, 7), {reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()}, {});
 PreparedBenchmarkSplit split;
 split.name = "train"; split.class_names = {"person"}; split.sources = {{images}}; split.images = {{7, 1, 1, 0, 0, 0}};
 BenchmarkWriteRequest output{.split = split, .output_path = root.path() / "pixels.bin", .resolution = 1, .num_workers = 1, .execution = &execution};
 BenchmarkSplitWriter writer(output);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 execution.register_split(writer, split);
 const std::array physical{coco(7)};
 auto input = request(physical);
 input.execution = &execution;
 input.parquet_shards = {root.path() / "annotations.parquet"};
 parquet_file(input.parquet_shards.front(), Json::array({hf_row(7, encoded, Json::array({segment(), segment(2)}), 1, 1)}));
 mmltk::testsupport::TestGate normalizing("normalizer retains its live Arrow batch and support");
 bool completed_inside_normalizer = false;
 input.rejected_object = [&](const CoconutPhysicalImage&, const CoconutRecord&, const CoconutSegment&, std::string_view) {
  normalizing.receipt().ArriveAndWait();
  execution.cooperate(); execution.cooperate();
  completed_inside_normalizer = writer.image_complete(0);
 };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] { normalizing.Release(); });
 REQUIRE(normalizing.WaitEntered(5s));
 CHECK_FALSE(execution.try_reserve({1, 0, true}));
 execution.source_publication(images, {})({7, {}, true});
 normalizing.Release();
 const auto result = mmltk::testsupport::await_test_future(importing, "Arrow reader resumes after pixel loan retirement", 10s);
 CHECK(completed_inside_normalizer);
 REQUIRE(result.size() == 1); REQUIRE(result[0].index().box_count() == 1);
 CHECK(result[0].index().rejected.degenerate_boxes == 1);
 CHECK(execution.try_reserve({256ULL << 20, 13}));
}

TEST_CASE("panoptic projection preserves document and consumed-row admission limits", "[benchmark][coconut][annotations]") {
 ScopedTempDir root("panoptic-row-limits");
 const std::array physical{objects(7)};
 auto input = request(physical, CoconutEdition::Large);
 input.metadata_only = true; input.annotation_json = root.path() / "annotations.json";
 input.limits.max_segments = 1;
 Json annotation{{"image_id", 7}, {"file_name", "objects365_v2_00000007.png"}, {"segments_info", Json::array({segment()})}};
 Json document{{"categories", category_catalog()}, {"images", Json::array({{{"id", 7}, {"width", 1}, {"height", 1}, {"file_name", "objects365_v2_00000007.jpg"}}})}, {"annotations", Json::array({annotation})}};
 bool accepted = false;
 std::optional<std::string> raw;
 SECTION("UTF-8 document marker") { raw = std::string("\xef\xbb\xbf") + document.dump(); accepted = true; }
 SECTION("bounded ignored row text") { document["annotations"][0]["extra"] = std::string(4096, 'x'); accepted = true; }
 SECTION("oversized ignored row text") { document["annotations"][0]["extra"] = std::string(4097, 'x'); }
 SECTION("events include ignored row values") { document["annotations"][0]["extra"] = std::vector<unsigned>(161, 0); }
 SECTION("depth applies to ignored envelope values") {
  Json nested = 0;
  for (int depth = 0; depth < 17; ++depth) nested = Json::array({std::move(nested)});
  document["extra"] = std::move(nested);
 }
 SECTION("fractional representation is not an exact ID") { document["annotations"][0]["image_id"] = 7.0; }
 SECTION("overflowing JSON number") {
  auto text = document.dump(); text.insert(1, "\"extra\":1e9999,"); raw = std::move(text);
 }
 SECTION("ignored envelope syntax remains required") {
  auto text = document.dump(); text.insert(1, "\"extra\":[true false],"); raw = std::move(text);
 }
 SECTION("escaped duplicate selected envelope name") {
  auto text = document.dump(); text.insert(1, "\"\\u0069mages\":[],"); raw = std::move(text);
 }
 mmltk::testsupport::write_text_file(input.annotation_json, raw.value_or(document.dump()));
 if (accepted) CHECK(import_coconut_annotations(input).front().index().image_count() == 1);
 else CHECK_THROWS(import_coconut_annotations(input));
}

TEST_CASE("Parquet binding admits optional fields and rejects required schema during membership", "[benchmark][coconut][parquet]") {
 ScopedTempDir root("parquet-bound-fields");
 const std::array<std::uint32_t, 1> pixels{1};
 auto row = hf_row(7, png(1, 1, pixels), Json::array({segment()}), 1, 1);
 auto fields = hf_schema()->fields();
 bool valid = false;
 SECTION("absent crowd and ignore default to false") {
  auto segments = static_cast<const arrow::StructType&>(*segment_type(false)).fields();
  std::erase_if(segments, [](const auto& field) { return field->name() == "iscrowd"; });
  auto annotation = static_cast<const arrow::StructType&>(*fields[1]->type()).fields();
  annotation[2] = arrow::field("segments_info", arrow::list(arrow::struct_(segments)));
  fields[1] = arrow::field("segments_info", arrow::struct_(annotation));
  valid = true;
 }
 SECTION("required unused date is absent") {
  auto images = static_cast<const arrow::StructType&>(*fields[2]->type()).fields();
  std::erase_if(images, [](const auto& field) { return field->name() == "date_captured"; });
  fields[2] = arrow::field("image_info", arrow::struct_(images));
 }
 SECTION("unconsumed mask has the wrong schema") { fields[0] = arrow::field("mask", arrow::utf8()); row["mask"] = "wrong"; }
 const auto path = root.path() / "records.parquet";
 parquet_file(path, Json::array({row}), arrow::schema(fields));
 const std::array paths{path};
 if (valid) {
  read_coconut_parquet(paths, {}, {}, [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput& input) {
   if (input.membership) return;
   REQUIRE(record.segments.size() == 1); CHECK_FALSE(record.segments[0].crowd); CHECK_FALSE(record.segments[0].ignore);
  });
 } else CHECK_THROWS(read_coconut_parquet(paths, {}, {}, [](std::size_t, const CoconutRecord&, const CoconutAnnotationInput&) {}, true));
}

TEST_CASE("independent Parquet groups seal identical artifacts across shared CPU counts", "[benchmark][coconut][parquet][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPU lanes");
 ScopedTempDir root("parquet-canonical-groups");
 const std::array<std::uint32_t, 1> pixels{1};
 const auto encoded = png(1, 1, pixels);
 const std::array physical{coco(7), coco(8), coco(9)};
 const auto path = root.path() / "records.parquet";
 const auto segments = Json::array({segment(9, 200, false), segment(), segment(2)});
 parquet_file(path, Json::array({hf_row(9, encoded, segments, 1, 1), hf_row(7, encoded, segments, 1, 1), hf_row(8, encoded, segments, 1, 1)}));
 for (const auto workers : {1U, 2U}) {
  BenchmarkCompilePipeline execution(workers, {}, {.transient_bytes = 4ULL << 30, .descriptors = 24});
  auto input = request(physical); input.execution = &execution; input.parquet_shards = {path};
  auto result = import_coconut_annotations(input);
  REQUIRE(result.size() == 1);
  CHECK(result[0].index().storage().boxes[0].source_ordinal == 4);
  CHECK(result[0].index().storage().boxes[1].source_ordinal == 7);
  CHECK(result[0].index().storage().boxes[2].source_ordinal == 1);
  CHECK(result[0].index().rejected.raw_records == 9);
  CHECK(result[0].index().rejected.degenerate_boxes == 3);
  store_coconut_component(root.path() / (std::to_string(workers) + ".bin"), result[0]);
 }
 for (const auto suffix : {"", ".inventory", ".complete.json"})
  CHECK(file_bytes(root.path() / (std::string("1.bin") + suffix)) == file_bytes(root.path() / (std::string("2.bin") + suffix)));
}

TEST_CASE("Parquet segment prefixes include declared groups and reject unrepresentable ordinals", "[benchmark][coconut][parquet]") {
 CoconutAnnotationRecords records;
 records.records.resize(2);
 records.records[0].first_segment_ordinal = 2;
 records.records[1].first_segment_ordinal = 1;
 records.groups = {{0, 1, 0, 3}, {1, 1, 0, 2}};
 bool valid = false;
 SECTION("declared stuff and omitted segment positions remain in the prefix") { valid = true; }
 SECTION("group total overflow") { records.groups[0].segments = UINT64_MAX; records.records[1].first_segment_ordinal = 0; }
 SECTION("record offset overflow") { records.groups[0].segments = UINT64_MAX; }
 SECTION("row range overflow") { records.groups[0].first_row = UINT64_MAX; }
 if (valid) {
  records.rebase_parquet_segments();
  CHECK(records.groups[1].first_segment == 3);
  CHECK(records.records[0].first_segment_ordinal == 2);
  CHECK(records.records[1].first_segment_ordinal == 4);
 } else CHECK_THROWS(records.rebase_parquet_segments());
}

TEST_CASE("panoptic JSON publishes physical facts before unrelated envelope tails", "[benchmark][coconut][annotations][pipeline]") {
 bool reversed = false, malformed = false, duplicate = false, cancel = false;
 SECTION("ready arrays precede valid ignored tail") {}
 SECTION("reversed arrays and escaped selected field") { reversed = true; }
 SECTION("malformed ignored tail follows ready facts") { malformed = true; }
 SECTION("duplicate selected field follows ready facts") { duplicate = true; }
 SECTION("pixel completion cancels unread tail") { cancel = true; }
 ScopedTempDir root("json-incremental-physical");
 const auto cache = BenchmarkCacheLayout::create(root.path() / "cache");
 const auto archive = cache.source_downloads("objects365") / "physical.tar";
 const auto jpeg = white_jpeg();
 const std::array<std::pair<std::string, std::string>, 1> members{{{objects(3).member, jpeg}}};
 tar(archive, members);
 CoconutRecipeCatalog catalog;
 catalog.releases.push_back({CoconutEdition::Large, "fixture-large", "fixture-v1", 1, {}});
 catalog.images.push_back({CoconutImageNamespace::Objects365V2, 32, "patch-32",
  {"physical", "unused", archive.filename().string(), std::filesystem::file_size(archive), "", BenchmarkDatasetSource::kObjects365V2}});
 auto admitted = local_physical_archives(cache, catalog);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256ULL << 20, .descriptors = 13}, cancellation);
 CoconutPhysicalMembership physical(admitted, catalog, true, cache, execution.storage(), execution, cancellation);
 const auto image_root = cache.source_images("objects365") / "patch-32";
 PreparedBenchmarkSplit split;
 split.name = "train"; split.class_names = {"person"}; split.sources = {{image_root}}; split.images = {{3, 3, 3, 0, 0, 0}};
 BenchmarkWriteRequest output{.split = split, .output_path = root.path() / "pixels.bin", .resolution = 3, .num_workers = 1, .execution = &execution};
 if (cancel) output.progress = {.context = &cancelled, .image_completed = [](void* state) { static_cast<std::atomic<bool>*>(state)->store(true); }};
 BenchmarkSplitWriter writer(output);
 BenchmarkCompilePipeline::Attempt attempt(execution); execution.register_split(writer, split);
 CoconutImportRequest input;
 input.execution = &execution; input.cancellation = cancellation; input.edition = CoconutEdition::Large; input.input_identity = "json-discovery";
 input.metadata_only = true; input.annotation_json = root.path() / "annotations.json"; input.physical_membership = &physical;
 const std::string categories = "\"categories\":" + category_catalog().dump();
 const std::string images = R"("\u0069mages":[{"id":3,"width":3,"height":3,"file_name":"objects365_v2_00000003.jpg"}])";
 const std::string annotations = R"("annotations":[{"image_id":3,"file_name":"objects365_v2_00000003.png","segments_info":[]}])";
 const auto prefix = "{" + (reversed ? annotations + "," + images + "," + categories : categories + "," + images + "," + annotations);
 const auto tail = ",\"ignored\":\"" + std::string(2U << 20, 'x') + (malformed ? "" : "\"") + (duplicate ? ",\"images\":[]}" : malformed ? "" : "}");
 mmltk::testsupport::write_text_file(input.annotation_json, prefix + tail);
 if (malformed || duplicate || cancel) CHECK_THROWS(import_coconut_annotations(input));
 else {
  const auto result = import_coconut_annotations(input);
  REQUIRE(result.size() == 1); REQUIRE(result[0].inventory().size() == 1);
  CHECK(result[0].inventory()[0].source_ordinal == 0);
 }
 CHECK(file_bytes(cached_image_path(image_root, 3)) == jpeg);
 REQUIRE(execution.geometry(image_root, 3));
 if (cancel) CHECK(cancelled.load()); else { execution.drain(); CHECK(writer.image_complete(0)); }
}

TEST_CASE("consecutive Parquet groups retain compressed physical progress with bounded continuation", "[benchmark][coconut][parquet][pipeline]") {
 bool cancel = false, fail = false, pressure = false, warm = false;
 SECTION("one CPU thirteen descriptors and an opened gzip generation") {}
 SECTION("warm metadata followed by a full forward gzip sequence") { warm = true; }
 SECTION("cancellation retires the consecutive reader") { cancel = true; }
 SECTION("consumer failure retires the consecutive reader") { fail = true; }
 SECTION("actual growth pressure releases before reacquiring") { pressure = true; }
 ScopedTempDir root("parquet-forward-physical");
 const auto cache = BenchmarkCacheLayout::create(root.path() / "cache");
 const auto archive = cache.source_downloads("coco") / "physical.tar.gz";
 const auto jpeg = white_jpeg();
 std::vector<std::pair<std::string, std::string>> members;
 Json rows = Json::array();
 const std::array<std::uint32_t, 9> ids{1, 1, 1, 1, 1, 1, 1, 1, 1};
 const auto encoded = png(3, 3, ids);
 for (unsigned id = 1; id <= 4; ++id) {
  members.emplace_back(coco(id).member, jpeg);
  rows.push_back(hf_row(id, encoded, Json::array({segment(9, 200, false), segment()})));
 }
 tar(archive, members);
 CoconutRecipeCatalog catalog;
 catalog.releases.push_back({CoconutEdition::Base, "fixture-base", "fixture-v1", 4, {}});
 catalog.images.push_back({CoconutImageNamespace::CocoTrain, 0, "train2017",
  {"physical", "unused", archive.filename().string(), std::filesystem::file_size(archive), "", BenchmarkDatasetSource::kCoco2017}});
 auto admitted = local_physical_archives(cache, catalog);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256ULL << 20, .descriptors = 13}, cancellation);
 CoconutPhysicalMembership physical(admitted, catalog, true, cache, execution.storage(), execution, cancellation);
 const auto images = cache.source_images("coco") / "train2017";
 PreparedBenchmarkSplit split;
 split.name = "train"; split.class_names = {"person"}; split.sources = {{images}};
 for (unsigned id = 1; id <= 4; ++id) split.images.push_back({id, 3, 3, 0, 0, 0});
 BenchmarkWriteRequest output{.split = split, .output_path = root.path() / "pixels.bin", .resolution = 3, .num_workers = 1, .execution = &execution};
 BenchmarkSplitWriter writer(output);
 BenchmarkCompilePipeline::Attempt attempt(execution); execution.register_split(writer, split);
 const std::array paths{root.path() / "records.parquet"};
 parquet_file(paths[0], rows);
 CoconutAnnotationRecords retained;
 if (warm) read_coconut_parquet(paths, {}, cancellation, [&](std::size_t, const CoconutRecord&, const CoconutAnnotationInput& input) {
  CHECK(input.membership);
 }, true, &execution, {}, {}, {}, &retained);
 BenchmarkAllowance first, blocker;
 std::size_t metadata_rows = 0, payload_rows = 0, retired_inputs = 0, retired_groups = 0;
 const auto requirement = physical.input_requirement(CoconutEdition::Base);
 const auto consume = [&] {
  read_coconut_parquet(paths, {}, cancellation, [&](std::size_t group, const CoconutRecord& record, const CoconutAnnotationInput& input) {
   CHECK(input.allowance.bytes() >= (256ULL << 20) + requirement.workspace_bytes());
   CHECK(input.allowance.descriptors() == 1);
   CHECK(input.live_bytes > requirement.workspace_bytes());
   CHECK(record.source_ordinal == group);
   CHECK(record.image_id == group + 1);
   const auto membership = physical.resolve(CoconutEdition::Base, "forward-gzip", record, input.allowance);
   CHECK(membership.member == coco(static_cast<unsigned>(record.image_id)).member);
   if ((input.membership || warm) && record.image_id == 1) {
     first = input.allowance;
     const auto unrelated = execution.reserve({0, 0});
     physical.release_readers(CoconutEdition::Base, unrelated);
     if (pressure) blocker = first.split_storage(64U << 10);
     else {
      // The next groups must consume this already-open generation. Reopening
      // the pathname would encounter the corrupt replacement, exposing a reset.
      std::filesystem::rename(archive, archive.string() + ".opened");
      mmltk::testsupport::write_text_file(archive, "replacement is not an archive");
     }
   }
   if (input.membership) {
    ++metadata_rows;
    if (cancel && metadata_rows == 2) cancelled.store(true);
   } else {
    ++payload_rows;
    CHECK(std::string(input.png.begin(), input.png.end()) == encoded);
    CHECK(record.segments.size() == 2);
    if (fail && payload_rows == 2) throw std::runtime_error("consecutive group consumer failed");
   }
   if (!cancelled.load()) execution.with_unused_workspace(input.allowance, input.live_bytes, [&] {
    execution.run(BenchmarkStage::Metadata, {}, [&](std::size_t) { execution.cooperate(); }, input.allowance);
   });
  }, false, &execution, [&](std::size_t) { ++retired_groups; }, {}, requirement, &retained,
  [](const CoconutRecord& record) { return std::uint64_t{record.width} * record.height * 1024; },
  [&](const BenchmarkAllowance& producer) {
   ++retired_inputs;
   physical.release_readers(CoconutEdition::Base, producer);
   blocker = {};
  });
 };
 if (cancel || fail) CHECK_THROWS(consume()); else consume();
 CHECK(first.descriptors() == 0);
 CHECK(retired_inputs == (pressure ? 2 : 1));
 CHECK(retired_groups >= 2);
 if (!cancel && !fail) {
  CHECK(metadata_rows == (warm ? 0 : 4)); CHECK(payload_rows == 4);
  REQUIRE(retained.groups.size() == 4);
  for (std::size_t group = 0; group < 4; ++group) {
   CHECK(retained.groups[group].first_segment == group * 2);
   CHECK(retained.records[group].first_segment_ordinal == group * 2);
   CHECK(file_bytes(cached_image_path(images, group + 1)) == jpeg);
  }
  execution.drain(); writer.finish(output);
  const auto compiled = CompiledDataset::open(output.output_path);
  CHECK(compiled.header().num_images == 4);
  for (std::size_t i = 1; i < 4; ++i)
   CHECK(std::memcmp(compiled.image_pixels(0), compiled.image_pixels(static_cast<std::uint32_t>(i)), 27 * sizeof(float)) == 0);
 }
}

TEST_CASE("a smaller Parquet group yields unused high water to a blocked independent reader", "[benchmark][coconut][parquet][pipeline]") {
 ScopedTempDir root("parquet-workspace-high-water");
 const std::array<std::uint32_t, 1> ids{1};
 const auto encoded = png(1, 1, ids);
 const std::array main_paths{root.path() / "sequence.parquet"}, other_paths{root.path() / "independent.parquet"};
 parquet_file(main_paths[0], Json::array({hf_row(1, encoded, Json::array({segment()}), 1, 1), hf_row(2, encoded, Json::array({segment()}), 1, 1)}));
 parquet_file(other_paths[0], Json::array({hf_row(3, encoded, Json::array({segment()}), 1, 1)}));
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 768ULL << 20, .descriptors = 13});
 CoconutAnnotationRecords metadata;
 read_coconut_parquet(main_paths, {}, {}, [](std::size_t, const CoconutRecord&, const CoconutAnnotationInput& input) { CHECK(input.membership); },
  true, &execution, {}, {}, {}, &metadata);
 CoconutAnnotationInput retained_batch;
 std::future<void> other;
 std::promise<void> blocked;
 bool overlapped = false;
 std::size_t sequence_retirements = 0;
 read_coconut_parquet(main_paths, {}, {}, [&](std::size_t group, const CoconutRecord&, const CoconutAnnotationInput& input) {
  CHECK_FALSE(input.membership);
  if (group == 0) {
   retained_batch = input;
   other = std::async(std::launch::async, [&] {
    if (execution.try_reserve({256ULL << 20, 1})) throw std::runtime_error("independent reader was not blocked by retained high water");
    auto pressure = execution.defer_resources();
    blocked.set_value();
    read_coconut_parquet(other_paths, {}, {}, [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput&) { CHECK(record.image_id == 3); }, true, &execution);
   });
   mmltk::testsupport::await_test_promise(blocked, "blocked smaller reader");
   CHECK(input.allowance.bytes() > 512ULL << 20);
  } else {
   mmltk::testsupport::await_test_future(other, "independent reader before sequence retirement");
   overlapped = true;
   CHECK(sequence_retirements == 0);
   CHECK(input.allowance.bytes() < 257ULL << 20);
   CHECK(input.allowance.bytes() >= 256ULL << 20);
   CHECK(input.allowance.aliases(retained_batch.allowance));
   CHECK(input.live_bytes > 0);
   CHECK(std::string(retained_batch.png.begin(), retained_batch.png.end()) == encoded);
  }
 }, false, &execution, {}, {}, {}, &metadata,
 [](const CoconutRecord& record) { return record.image_id == 1 ? 256ULL << 20 : 65536ULL; },
 [&](const BenchmarkAllowance&) { ++sequence_retirements; });
 CHECK(overlapped);
 CHECK(sequence_retirements == 1);
 CHECK(retained_batch.allowance.descriptors() == 0);
 CHECK(std::string(retained_batch.png.begin(), retained_batch.png.end()) == encoded);
 retained_batch = {};
 metadata.discard();
 CHECK(execution.try_reserve({768ULL << 20, 13}));
}

TEST_CASE("repeated warm Parquet membership owns transient physical workspace without Arrow reads", "[benchmark][coconut][parquet][pipeline]") {
 bool cancel = false, fail = false, lend = true;
 SECTION("membership lending and production metadata completion") {}
 SECTION("consecutive groups without an offer reuse the same physical promise") { lend = false; }
 SECTION("cancellation reclaims the offered physical promise") { cancel = true; }
 SECTION("consumer exception reclaims the offered physical promise") { fail = true; }
 ScopedTempDir root("parquet-warm-membership");
 const auto cache = BenchmarkCacheLayout::create(root.path() / "cache");
 const auto archive = cache.source_downloads("coco") / "physical.tar.gz";
 const auto jpeg = white_jpeg();
 const std::array<std::uint32_t, 9> mask{1, 1, 1, 1, 1, 1, 1, 1, 1};
 const auto encoded = png(3, 3, mask);
 Json rows = Json::array();
 std::vector<std::pair<std::string, std::string>> members;
 for (unsigned id = 1; id <= 4; ++id) {
  rows.push_back(hf_row(id, encoded, Json::array({segment()})));
  members.emplace_back(coco(id).member, jpeg);
 }
 tar(archive, members);
 CoconutRecipeCatalog catalog;
 catalog.releases.push_back({CoconutEdition::Base, "fixture-base", "fixture-v1", 4, {}});
 catalog.images.push_back({CoconutImageNamespace::CocoTrain, 0, "train2017",
  {"physical", "unused", archive.filename().string(), std::filesystem::file_size(archive), "", BenchmarkDatasetSource::kCoco2017}});
 auto admitted = local_physical_archives(cache, catalog);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256ULL << 20, .descriptors = 13}, cancellation);
 CoconutPhysicalMembership physical(admitted, catalog, true, cache, execution.storage(), execution, cancellation);
 const auto resources = physical.input_requirement(CoconutEdition::Base);
 REQUIRE(resources.lease_controls().retained_handles);
 const std::array paths{root.path() / "records.parquet"};
 parquet_file(paths[0], rows);
 auto retained = std::make_shared<CoconutAnnotationRecords>();
 std::size_t cold_rows = 0;
 read_coconut_parquet(paths, {}, cancellation, [&](std::size_t, const CoconutRecord&, const CoconutAnnotationInput& input) {
  CHECK(input.membership); ++cold_rows;
 }, true, &execution, {}, {}, {}, retained.get());
 REQUIRE(cold_rows == 4);
 // A warm call must use its retained typed rows without opening any projection.
 std::filesystem::remove(paths[0]);
 BenchmarkAllowance first;
 std::unique_ptr<BenchmarkResourceWait> pressure;
 std::size_t published = 0, retired = 0;
 const auto consume = [&] {
  read_coconut_parquet(paths, {}, cancellation, [&](std::size_t group, const CoconutRecord& record, const CoconutAnnotationInput& input) {
   CHECK(input.membership); CHECK(input.png.empty()); CHECK_FALSE(input.backing);
   CHECK(record.source_ordinal == group); CHECK(record.image_id == group + 1);
   CHECK(input.allowance.bytes() == resources.workspace_bytes()); CHECK(input.live_bytes == resources.workspace_bytes());
   CHECK(input.allowance.descriptors() == 0); // Every physical descriptor is a continuation, with no Arrow reader.
   if (!first) first = input.allowance; else CHECK(first.aliases(input.allowance));
   CHECK_FALSE(execution.try_reserve({execution.transient_target(), 0, true}));
   const auto member = physical.resolve(CoconutEdition::Base, "warm-membership", record, input.allowance);
   CHECK(member.member == coco(static_cast<unsigned>(record.image_id)).member);
   ++published;
   if (published == 2) pressure.reset();
   if (published == 1) {
    pressure = execution.defer_resources();
    // Consecutive groups must keep the already-open compressed generation.
    std::filesystem::rename(archive, archive.string() + ".opened");
    mmltk::testsupport::write_text_file(archive, "replacement is not an archive");
   }
   const auto work = [&] {
    CHECK_FALSE(execution.try_reserve({execution.transient_target(), 0, true}));
    execution.run(BenchmarkStage::Metadata, {}, [&](std::size_t) {
     if (published == 2 && fail) throw std::runtime_error("warm membership consumer failed");
     if (published == 2 && cancel) cancelled.store(true);
    }, input.allowance);
   };
   if (lend) execution.with_unused_workspace(input.allowance, input.live_bytes, work); else work();
  }, true, &execution, {}, {}, resources, retained.get(), {}, [&](const BenchmarkAllowance& producer) {
   ++retired; physical.release_readers(CoconutEdition::Base, producer);
  });
 };
 if (cancel || fail) CHECK_THROWS(consume()); else consume();
 CHECK(published == (cancel || fail ? 2 : 4));
 CHECK(retired == 1); CHECK(first.bytes() == 0); CHECK(first.descriptors() == 0);
 cancelled.store(false);
 const auto images = cache.source_images("coco") / "train2017";
 for (std::size_t i = 1; i <= published; ++i) CHECK(file_bytes(cached_image_path(images, i)) == jpeg);
 if (!cancel && !fail) {
  // Reach the product Importer::consume path with the same already-ready rows.
  // Physical membership remains cached after its actual reader retired above.
  retained->identity = "warm-membership";
  CoconutImportRequest input;
  input.execution = &execution; input.edition = CoconutEdition::Base; input.input_identity = retained->identity;
  input.metadata_only = true; input.records = retained; input.parquet_shards.assign(paths.begin(), paths.end());
  input.physical_membership = &physical; input.expected_rows = 4; input.cancellation = cancellation;
  const auto components = import_coconut_annotations(input);
  REQUIRE(components.size() == 1); CHECK(components[0].index().image_count() == 4);
  REQUIRE(components[0].inventory().size() == 4);
  for (std::size_t i = 0; i < 4; ++i) {
   CHECK(components[0].inventory()[i].source_ordinal == i);
   CHECK(components[0].inventory()[i].physical.image_id == i + 1);
  }
 }
 retained->discard(); execution.retire_source(images);
 CHECK(execution.try_reserve({execution.transient_target(), 13}));
}
