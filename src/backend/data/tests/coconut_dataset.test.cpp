#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_annotation_identity.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_import.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_parquet.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_parquet_pool.h"
#include "src/backend/data/detail/compile_workspace.h"
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/detail/file_work_queue.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_catalog.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory_io.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_input.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_native_image.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/backend/data/benchmark/detail/benchmark_labels.h"
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
#include "src/backend/data/benchmark/detail/benchmark_image_publication.h"
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
#include <parquet/file_reader.h>
#include <parquet/properties.h>
#include <archive.h>
#include <archive_entry.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <stb_image_write.h>
#include <zlib.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <latch>
#include <thread>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <system_error>
#include <string>
#include <utility>
#include <vector>
using mmltk::backend::data::detail::CompileAllowance;
using mmltk::backend::data::detail::CompileChange;
using mmltk::backend::data::detail::CompileResources;
using mmltk::backend::data::detail::CompileResourceWait;
using mmltk::backend::data::detail::CompileTaskKind;
using namespace mmltk::backend::data;
using namespace mmltk::backend::data::benchmark_internal;
namespace {
using mmltk::backend::data::testsupport::arrow_ok;
using mmltk::backend::data::testsupport::benchmark_entry;
using mmltk::backend::data::testsupport::benchmark_semantics;
using mmltk::backend::data::testsupport::bind_benchmark_slots;
using mmltk::backend::data::testsupport::coconut_parquet_schema;
using mmltk::backend::data::testsupport::coconut_parquet_segment_type;
using mmltk::backend::data::testsupport::write_benchmark_fixture;
using mmltk::backend::data::testsupport::write_coconut_parquet;
using mmltk::testsupport::require_condition;
std::uint32_t compiled_slot(const CompiledDataset& dataset, std::uint64_t identity, AnnotationSource source) {
 auto slots = dataset.eligible_slots();
 const auto found = std::ranges::find_if(slots, [&](auto slot) {
  const auto& entry = dataset.image_entry(slot);
  return entry.has_source_image_id && entry.source_image_id == identity && entry.source == source;
 });
 if (found == slots.end()) throw std::runtime_error("expected source identity is absent from compiled output");
 return *found;
}
NormalizedAnnotationIndex fixture_index(const NormalizedAnnotationBuilder& builder) { return seal_normalized_annotation_metadata(NormalizedAnnotationBuilder(builder)); }
const NormalizedAnnotationIndex& fixture_index(const NormalizedAnnotationIndex& index) { return index; }
using Json = nlohmann::json;
using mmltk::testsupport::ScopedTempDir;
struct DeferredCancellationGate {
 const std::atomic<bool>& cancelled_flag;
 static mmltk::testsupport::TestGate*& next() {
  thread_local mmltk::testsupport::TestGate* value = nullptr;
  return value;
 }
 bool cancelled() const noexcept {
  if (auto* gate = std::exchange(next(), nullptr)) gate->receipt().ArriveAndWait();
  return cancelled_flag.load();
 }
};
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
Json hf_row(unsigned id, std::string encoded, Json segments, int width = 3, int height = 3, std::string_view physical_source = "train2017") {
 const auto digits = std::to_string(id);
 const auto stem = std::string(12U - digits.size(), '0') + digits;
 return {
  {"mask", {{"bytes", std::move(encoded)}, {"path", nullptr}}}, {"segments_info", {{"file_name", stem + ".png"}, {"image_id", id}, {"segments_info", std::move(segments)}}},
  {"image_info", {{"coco_url", "http://images.cocodataset.org/" + std::string(physical_source) + "/" + stem + ".jpg"}, {"date_captured", "2017"}, {"file_name", stem + ".jpg"}, {"height", height},
                  {"width", width}, {"id", id}, {"license", 1}}}
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
 void prepare_partial_staging(BenchmarkCompilePipeline& pipeline, const std::filesystem::path& directory) {
  execution = &pipeline;
  inventory_directory = directory;
  records = std::make_shared<CoconutAnnotationRecords>();
  records->identity = input_identity;
  // The two parsed segments exhaust this block before the single valid mask
  // is staged, forcing a new range while normalization retains its scratch.
  records->staging = std::make_shared<CoconutStaging>(directory, &pipeline.storage(), 1);
  parquet_shards = {directory.parent_path() / "row.parquet"};
  const std::array<std::uint32_t, 1> pixel{1};
  write_coconut_parquet(parquet_shards.front(), Json::array({hf_row(7, png(1, 1, pixel), Json::array({segment(), segment(2)}), 1, 1)}));
 }
};
LocalCoconutImportRequest request(std::span<const CoconutPhysicalImage> physical, CoconutEdition edition = CoconutEdition::Base) { return LocalCoconutImportRequest(physical, edition); }
void check_rejected_coconut_labels(CoconutImportRequest& input) {
 input.accept_partial = true;
 const auto result = import_coconut_annotations(input);
 REQUIRE(result.size() == 1);
 CHECK_FALSE(result.front().required_labels_valid(0));
}
void reversed_coco_parquet(const std::filesystem::path& path) {
 const std::array<std::uint32_t, 1> ids{1};
 const auto mask = png(1, 1, ids);
 write_coconut_parquet(path, Json::array({hf_row(8, mask, Json::array({segment()}), 1, 1), hf_row(7, mask, Json::array({segment()}), 1, 1)}));
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
  struct End {
   z_stream& stream;
   ~End() { deflateEnd(&stream); }
  } end{stream};
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
  pending |= value << bits;
  bits += count;
  while (bits >= 8) {
   encoded.push_back(static_cast<char>(pending));
   pending >>= 8;
   bits -= 8;
  }
 };
 const auto symbol = [&](unsigned value) {
  const unsigned count = value < 144 ? 8 : value < 256 ? 9 : value < 280 ? 7 : 8;
  const unsigned code = value < 144 ? value + 0x30 : value < 256 ? value - 144 + 0x190 : value < 280 ? value - 256 : value - 280 + 0xc0;
  unsigned reversed = 0;
  for (unsigned bit = 0; bit < count; ++bit) reversed = (reversed << 1) | ((code >> bit) & 1);
  append(reversed, count);
 };
 append(3, 3);  // BFINAL=1, BTYPE=01: exactly one fixed-Huffman block.
 for (std::size_t offset = 0; offset < input.size();) {
  const auto value = static_cast<unsigned char>(input[offset]);
  std::size_t end = offset + 1;
  while (end < input.size() && input[end] == input[offset]) ++end;
  symbol(value);
  ++offset;
  while (end - offset >= 258) {
   symbol(285);
   append(0, 5);
   offset += 258;
  }  // length=258, distance=1.
  while (offset < end) {
   symbol(value);
   ++offset;
  }
 }
 symbol(256);
 if (bits) encoded.push_back(static_cast<char>(pending));
 const auto checksum = static_cast<std::uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(input.data()), static_cast<uInt>(input.size())));
 for (const auto value : {checksum, static_cast<std::uint32_t>(input.size())})
  for (unsigned shift = 0; shift < 32; shift += 8) encoded.push_back(static_cast<char>(value >> shift));
 return encoded;
}
std::array<std::string, 3> capture_publication_bytes(const std::filesystem::path& output) {
 return {file_bytes(output / "train.bin"), file_bytes(output / "val.bin"), file_bytes(output / "benchmark_manifest.json")};
}
void check_publication_bytes(const std::filesystem::path& output, std::string_view train, std::string_view validation, std::string_view manifest) {
 CHECK((file_bytes(output / "train.bin") == train));
 CHECK((file_bytes(output / "val.bin") == validation));
 CHECK((file_bytes(output / "benchmark_manifest.json") == manifest));
}
std::vector<CoconutPhysicalImage> fixture_physical_inventory(const std::filesystem::path& archive_path, const std::filesystem::path& cache_path, CoconutImageNamespace source, std::uint16_t shard,
 std::string identity, mmltk::common::concurrency::CancellationObservation cancelled = {}, detail::StorageReservationPool* storage = nullptr, BenchmarkCompilePipeline* execution = nullptr,
 const CompileAllowance& parent = {}) {
 if (!cache_path.empty() && std::filesystem::exists(cache_path)) {
  try {
   return load_coconut_physical_inventory(cache_path, source, shard, identity, cancelled, {});
  } catch (const CoconutInputError&) {}
 }
 BenchmarkArchive input(archive_path, (execution ? &execution->execution() : nullptr), 0, parent);
 std::vector<CoconutPhysicalImage> images;
 while (input.next(cancelled)) {
  if (!input.regular() || !input.member().ends_with(".jpg")) continue;
  const auto id =
   source == CoconutImageNamespace::Objects365V1 || source == CoconutImageNamespace::Objects365V2 ? parse_coconut_objects_member(input.member()).id : parse_coconut_coco_member(input.member());
  CoconutPhysicalImage image{source, id, shard, input.member(), identity};
  validate_coconut_physical_image(image);
  images.push_back(std::move(image));
 }
 std::ranges::sort(images, {}, &CoconutPhysicalImage::image_id);
 if (std::ranges::adjacent_find(images, {}, &CoconutPhysicalImage::image_id) != images.end()) throw CoconutInputError("duplicate fixture image");
 if (!cache_path.empty()) store_coconut_physical_inventory(cache_path, images, source, shard, identity, cancelled, storage);
 return images;
}
CoconutComponentBuilder component_builder(const CoconutComponent& component) {
 CoconutComponentBuilder builder;
 builder.edition = component.edition();
 builder.source = component.source();
 builder.input_identity = component.input_identity();
 builder.recovery_policy = component.recovery_policy();
 builder.original_annotation_identity = component.original_annotation_identity();
 builder.original_generation = component.original_generation();
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
 write_coconut_parquet(root.path() / "a.parquet", Json::array({row, empty}));
 write_coconut_parquet(root.path() / "b.parquet", Json::array({hf_row(9, png(3, 3, filled), Json::array({nonthing}))}), coconut_parquet_schema(true), 1, {},
  parquet::WriterProperties::Builder().compression(parquet::Compression::ZSTD)->build());
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
 auto loaded = load_coconut_component(cache, train.edition(), train.source(), train.input_identity());
 REQUIRE(loaded);
 CHECK(std::ranges::equal(loaded->inventory(), train.inventory()));
 expect_runs(*loaded, runs);
 CHECK_FALSE(load_coconut_component(cache, train.edition(), train.source(), "another-edition"));
 auto manifest = read_json_file(cache.string() + ".complete.json");
 manifest["coconut"]["inventory_identity"] = std::string(64, '0');
 json_file(cache.string() + ".complete.json", manifest);
 CHECK_FALSE(load_coconut_component(cache, train.edition(), train.source(), train.input_identity()));
}
TEST_CASE("COCONut Parquet rejects malformed nested records missing membership and bounded overflows", "[coconut]") {
 ScopedTempDir root("coconut-malformed");
 const std::array<std::uint32_t, 9> ids{1, 1, 1, 1, 1, 1, 1, 1, 1};
 auto row = hf_row(7, png(3, 3, ids), Json::array({segment()}));
 const std::array physical{coco(7)};
 auto input = request(physical);
 input.parquet_shards = {root.path() / "a.parquet"};
 input.expected_rows = 1;
 bool rejected_labels = false;
 SECTION("null image metadata") { row["image_info"] = nullptr; }
 SECTION("null required unused image license") { row["image_info"]["license"] = nullptr; }
 SECTION("null required unused image date") { row["image_info"]["date_captured"] = nullptr; }
 SECTION("null required segment ID") {
  rejected_labels = true;
  row["segments_info"]["segments_info"][0]["id"] = nullptr;
 }
 SECTION("null optional present flag") {
  rejected_labels = true;
  row["segments_info"]["segments_info"][0]["iscrowd"] = nullptr;
 }
 SECTION("null segment struct") {
  rejected_labels = true;
  row["segments_info"]["segments_info"][0] = nullptr;
 }
 SECTION("null segment list") { row["segments_info"]["segments_info"] = nullptr; }
 SECTION("null annotation struct") { row["segments_info"] = nullptr; }
 SECTION("null mask struct") {
  rejected_labels = true;
  row["mask"] = nullptr;
 }
 SECTION("mismatched release IDs") { row["segments_info"]["image_id"] = 8; }
 SECTION("unknown thing category") {
  rejected_labels = true;
  row["segments_info"]["segments_info"][0]["category_id"] = 12;
 }
 SECTION("contradictory dimensions") {
  rejected_labels = true;
  row["image_info"]["width"] = 4;
 }
 SECTION("missing embedded bytes") {
  rejected_labels = true;
  row["mask"]["bytes"] = nullptr;
 }
 SECTION("PNG admission") {
  rejected_labels = true;
  input.limits.max_png_bytes = 1;
 }
 SECTION("pixel admission") { input.limits.max_pixels = 8; }
 SECTION("segment admission") { input.limits.max_segments = 0; }
 SECTION("release completeness") { input.expected_rows = 2; }
 SECTION("missing physical member") {
  input.membership = std::make_unique<CoconutPhysicalMembership>(std::span<const CoconutPhysicalImage>{});
  input.physical_membership = input.membership.get();
 }
 SECTION("undeclared void-independent RGB ID") {
  rejected_labels = true;
  row["segments_info"]["segments_info"][0]["id"] = 2;
 }
 SECTION("duplicate segments") {
  rejected_labels = true;
  row["segments_info"]["segments_info"].push_back(segment());
 }
 write_coconut_parquet(input.parquet_shards[0], Json::array({row}));
 if (rejected_labels) {
  check_rejected_coconut_labels(input);
 } else
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
  auto schema = coconut_parquet_schema();
  auto fields = schema->fields();
  fields[0] = arrow::field("mask", arrow::utf8());
  row["mask"] = "wrong";
  write_coconut_parquet(input.parquet_shards[0], Json::array({row}), arrow::schema(fields));
  CHECK_THROWS(import_coconut_annotations(input));
 }
 SECTION("between records") {
  write_coconut_parquet(input.parquet_shards[0], Json::array({row, hf_row(8, png(1, 1, ids), Json::array({segment()}), 1, 1)}));
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
 auto loaded = load_coconut_component(cache, input.edition, CoconutImageNamespace::Objects365V1, components[0].input_identity());
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
 covering.index.split = coconut_component_split(covering.edition, covering.source);
 covering.index.annotation_sha256.clear();
 covered[0] = std::move(covering).finish({}, root.path());
 CHECK(reconcile_coconut_extensions(covered) == 2);
 CHECK(covered[1].inventory().empty());
 CHECK(covered[1].index().image_count() == 0);
 CHECK(covered[1].index().box_count() == 0);
 CHECK(covered[1].index().run_count() == 0);
 CHECK(covered[0].inventory().size() == 2);
}
TEST_CASE("COCONut archives reject invalid consumed members and ignore unused tails", "[coconut]") {
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
 bool link = false, unused_tail = false, rejected_labels = false;
 SECTION("missing mask") {
  rejected_labels = true;
  members.clear();
 }
 SECTION("unused extra mask") {
  members.emplace_back("panoptic_object365/objects365_v2_00000002.png", mask);
  unused_tail = true;
 }
 SECTION("unused duplicate mask") {
  members.push_back(members.front());
  unused_tail = true;
 }
 SECTION("unused traversal") {
  members.emplace_back("../outside", "x");
  unused_tail = true;
 }
 SECTION("symlink") { link = true; }
 SECTION("duplicate offered annotation") { document["annotations"].push_back(annotation); }
 SECTION("unknown physical namespace") { document["annotations"][0]["file_name"] = "00000001.png"; }
 SECTION("contradictory physical names") { document["annotations"][0]["object365_file_name"] = "objects365_v2_00000002"; }
 SECTION("unknown PNG segment") {
  rejected_labels = true;
  const std::array<std::uint32_t, 1> unknown{2};
  members[0].second = png(1, 1, unknown);
 }
 json_file(input.annotation_json, document);
 tar(input.mask_archive, members, link);
 if (unused_tail)
  CHECK_NOTHROW(import_coconut_annotations(input));
 else if (rejected_labels) {
  check_rejected_coconut_labels(input);
 } else
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
 CHECK(fixture_physical_inventory(archive, cache, CoconutImageNamespace::Objects365V1, 0x1234, "a") == wanted);
 CHECK_FALSE(std::filesystem::exists(archive));
 const std::array<std::pair<std::string, std::string>, 3> members{{{".", ""}, {"./", ""}, {"./image//./objects365_v1_00091105.jpg", "jpeg"}}};
 tar(archive, members);
 const auto emitted = root.path() / "emitted.bin";
 detail::StorageReservationPool storage(root.path(), {});
 CHECK(fixture_physical_inventory(archive, emitted, CoconutImageNamespace::Objects365V1, 0x1234, "a", {}, &storage) == wanted);
 CHECK(storage.outstanding() == 0);
 struct stat status{};
 REQUIRE(::stat(emitted.c_str(), &status) == 0);
 CHECK((status.st_mode & 0777) == 0644);
 CHECK(file_bytes(emitted) == expected);
 CHECK(expected.size() == 114 + 32);
 CHECK(file_bytes(cache) == expected);
 // Reading, hashing and durable publication share the existing v1 encoding.
 const auto retained = [&] {
  InventoryInput input(emitted, {});
  InventoryHeader header;
  input.value(header);
  CoconutPhysicalImage image;
  input.value(image);
  CHECK(image == wanted.front());
  auto seal = input.seal();
  seal->identity = input.finish();
  CHECK(seal->identity == "93e47afa75da695b47e8c5401fb230eade7266f798bcea057b528fbd0b945e17");
  InventoryOutput hash(nullptr, {});
  CHECK(encode_inventory(hash, header, wanted, {}) == seal->identity);
  CHECK(hash.byte_size() == expected.size());
  return seal;
 }();
 std::filesystem::remove(emitted);
 mmltk::testsupport::write_text_file(emitted, std::string(expected.size(), 'x'));
 CHECK(std::string(reinterpret_cast<const char*>(retained->data().data()), retained->data().size()) == expected);
 {
  const std::lock_guard lock(retained->mutex);
  retained->publish(emitted, {}, &storage);
 }
 CHECK(file_bytes(emitted) == expected);
 const auto snapshot = mmltk::common::io::FileSnapshot::Read(emitted);
 {
  const std::lock_guard lock(retained->mutex);
  retained->publish(emitted, {}, &storage);
 }
 CHECK(mmltk::common::io::FileSnapshot::Read(emitted) == snapshot);
 CHECK(storage.outstanding() == 0);
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
 detail::StorageReservationPool storage(root.path(), {});
 store_coconut_component(path, component, {}, &storage);
 CHECK(storage.outstanding() == 0);
 const auto index_bytes = file_bytes(path);
 const auto completion_bytes = file_bytes(path.string() + ".complete.json");
 const auto completion = Json::parse(completion_bytes);
 CHECK(completion.at("index_version") == 3);
 const Json expected_completion{
  {"edition", 4}, {"source", 3}, {"input_identity", "i"}, {"normalization", "coconut-exact-rgb-rle-v1"}, {"inventory_identity", "c5304ca7a16f7e76d6c21bfd7bda2b39a025cc268dde2336d95a2157eb9d4632"},
  {"inventory_count", 1}, {"recovery_policy", 0}, {"original_annotation_identity", ""}, {"recovery_images", 0}, {"rejected_images", Json::array()}, {"source_rows", 0}, {"source_segments", 0},
  {"source_extent_complete", false}, {"incomplete_ordinal_images", Json::array()}
 };
 CHECK(completion.at("coconut") == expected_completion);
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
 CHECK(file_bytes(path.string() + ".complete.json") == completion_bytes);
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
 auto result = fixture_physical_inventory(archive, cache, CoconutImageNamespace::CocoUnlabeled, 0, "pinned-archive");
 REQUIRE(result.size() == 2);
 CHECK(result[0].image_id == 7);
 CHECK(result[1].image_id == 8);
 std::filesystem::remove(archive);
 CHECK(fixture_physical_inventory(archive, cache, CoconutImageNamespace::CocoUnlabeled, 0, "pinned-archive") == result);
 CHECK_THROWS(fixture_physical_inventory(archive, cache, CoconutImageNamespace::CocoUnlabeled, 0, "changed-archive"));
 SECTION("ready physical rows precede terminal digest rejection") {
  auto bytes = file_bytes(cache);
  bytes.back() ^= 1;
  mmltk::testsupport::write_text_file(cache, bytes);
  std::vector<CoconutPhysicalImage> ready;
  CHECK_THROWS_AS(
   load_coconut_physical_inventory(cache, CoconutImageNamespace::CocoUnlabeled, 0, "pinned-archive", {}, [&](const CoconutPhysicalImage& image) { ready.push_back(image); }), CoconutInputError);
  CHECK(ready == result);
 }
 SECTION("allocation failure in a physical consumer propagates") {
  CHECK_THROWS_AS(load_coconut_physical_inventory(cache, CoconutImageNamespace::CocoUnlabeled, 0, "pinned-archive", {}, [](const CoconutPhysicalImage&) { throw std::bad_alloc(); }), std::bad_alloc);
 }
 SECTION("storage failure in a physical consumer propagates") {
  CHECK_THROWS_AS(load_coconut_physical_inventory(
                   cache, CoconutImageNamespace::CocoUnlabeled, 0, "pinned-archive", {}, [](const CoconutPhysicalImage&) { throw std::system_error(std::make_error_code(std::errc::io_error)); }),
   std::system_error);
 }
 SECTION("internal failure in a physical consumer propagates") {
  CHECK_THROWS_AS(
   load_coconut_physical_inventory(cache, CoconutImageNamespace::CocoUnlabeled, 0, "pinned-archive", {}, [](const CoconutPhysicalImage&) { throw std::logic_error("physical consumer invariant"); }),
   std::logic_error);
 }
 SECTION("cancellation after a ready row remains fatal") {
  std::atomic<bool> stopped{false};
  std::size_t ready = 0;
  CHECK_THROWS(load_coconut_physical_inventory(
   cache, CoconutImageNamespace::CocoUnlabeled, 0, "pinned-archive", mmltk::common::concurrency::CancellationObservation::Atomic(stopped), [&](const CoconutPhysicalImage&) {
   ++ready;
   stopped = true;
  }));
  CHECK(ready == 1);
 }
 const std::array<std::pair<std::string, std::string>, 2> duplicate{{members[0], {"./unlabeled2017//./000000000007.jpg", "jpeg7"}}};
 tar(archive, duplicate);
 CHECK_THROWS(fixture_physical_inventory(archive, {}, CoconutImageNamespace::CocoUnlabeled, 0, "duplicate-archive"));
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
 write_coconut_parquet(input.parquet_shards[0], rows, coconut_parquet_schema(true), 1, {}, parquet::WriterProperties::Builder().compression(parquet::Compression::GZIP)->build());
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
 write_coconut_parquet(input.parquet_shards[0], rows);
 CHECK_THROWS(import_coconut_annotations(input));
}
TEST_CASE("physical readiness and resolution share final identity decisions", "[benchmark][coconut][recovery]") {
 const auto id = GENERATE(0U, 7U);
 const bool ambiguous = GENERATE(false, true);
 std::vector<CoconutPhysicalImage> images{coco(id)};
 if (ambiguous) images.push_back(coco(id, CoconutImageNamespace::CocoUnlabeled));
 CoconutPhysicalMembership physical(images);
 CoconutRecord record;
 record.image_id = id;
 record.file_name = std::filesystem::path(images.front().member).filename().string();
 record.namespace_hint = CoconutImageNamespace::CocoUnlabeled;
 const auto ready = physical.readiness(CoconutEdition::Base, record);
 CHECK(ready.terminal);
 if (ambiguous) {
  CHECK_FALSE(ready.image);
  CHECK_THROWS_WITH(physical.resolve(CoconutEdition::Base, record), "ambiguous physical image identity: " + record.file_name);
  CHECK_THROWS_WITH(physical.identify(CoconutEdition::Base, record), "ambiguous physical image identity: " + record.file_name);
 } else {
  REQUIRE(ready.image);
  CHECK(*ready.image == images.front());
  CHECK(physical.resolve(CoconutEdition::Base, record) == *ready.image);
  CHECK(physical.identify(CoconutEdition::Base, record) == *ready.image);
 }
 record.image_id = 8;
 record.file_name = "000000000008.jpg";
 const auto missing = physical.readiness(CoconutEdition::Base, record);
 CHECK(missing.terminal);
 CHECK_FALSE(missing.image);
 CHECK_THROWS_WITH(physical.resolve(CoconutEdition::Base, record), "missing physical image: " + record.file_name);
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
  check_rejected_coconut_labels(input);
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
 const auto loaded = load_coconut_component(path, first[0].edition(), first[0].source(), first[0].input_identity());
 REQUIRE(loaded);
 CHECK(std::ranges::equal(loaded->inventory(), first[0].inventory()));
 SECTION("truncation") {
  std::filesystem::resize_file(path.string() + ".inventory", original.size() - 1);
  CHECK_FALSE(load_coconut_component(path, first[0].edition(), first[0].source(), first[0].input_identity()));
 }
 SECTION("corruption") {
  auto corrupt = original;
  corrupt[corrupt.size() / 2] ^= 1;
  mmltk::testsupport::write_text_file(path.string() + ".inventory", corrupt);
  CHECK_FALSE(load_coconut_component(path, first[0].edition(), first[0].source(), first[0].input_identity()));
 }
 SECTION("reordered inventory cannot replace an admitted index") {
  auto reordered = component_builder(first[0]);
  std::swap(reordered.inventory[0], reordered.inventory[1]);
  CHECK_THROWS(std::move(reordered).finish({}, root.path()));
  CHECK(file_bytes(path.string() + ".inventory") == original);
 }
 SECTION("shared aligned views outlive the opening component") {
  const auto selected = [&] {
   auto opened = load_coconut_component(path, first[0].edition(), first[0].source(), first[0].input_identity(), {}, true);
   REQUIRE(opened);
   const auto* retained_image = &opened->inventory_image(1);
   auto view = opened->select_images({1});
   CHECK(&view.inventory_image(0) == retained_image);
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
  const auto deferred = load_coconut_component(path, first[0].edition(), first[0].source(), first[0].input_identity(), {}, true);
  REQUIRE(deferred);
  const auto membership = deferred->membership();
  CHECK(std::ranges::equal(membership.inventory(), first[0].inventory()));
  CHECK_THROWS(admit_coconut_component(*deferred));
  CHECK_THROWS(store_coconut_component(root.path() / "rejected.bin", *deferred));
  CHECK_FALSE(load_coconut_component(path, first[0].edition(), first[0].source(), first[0].input_identity()));
  CHECK(loaded->index().storage().mask_rle_pairs.front().length != 0);
 }
 SECTION("every store cancellation point prevents fresh admission") {
  for (std::size_t cut = 0; cut < baseline.polls; ++cut) {
   const auto cancelled_path = root.path() / ("cancelled-" + std::to_string(cut) + ".bin");
   PollCancellation stop;
   stop.stop_at = cut;
   CHECK_THROWS(store_coconut_component(cancelled_path, first[0], mmltk::common::concurrency::CancellationObservation::Borrow(stop)));
   CHECK_FALSE(load_coconut_component(cancelled_path, first[0].edition(), first[0].source(), first[0].input_identity()));
  }
 }
 SECTION("load cancellation propagates instead of becoming a cache miss") {
  PollCancellation observed;
  REQUIRE(load_coconut_component(path, first[0].edition(), first[0].source(), first[0].input_identity(), mmltk::common::concurrency::CancellationObservation::Borrow(observed)));
  for (std::size_t cut = 0; cut < observed.polls; ++cut) {
   PollCancellation stop;
   stop.stop_at = cut;
   CHECK_THROWS(load_coconut_component(path, first[0].edition(), first[0].source(), first[0].input_identity(), mmltk::common::concurrency::CancellationObservation::Borrow(stop)));
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
  annotation(base, "base.parquet",
   [&](const auto& path) { write_coconut_parquet(path, Json::array({hf_row(7, mask, Json::array({first, second})), hf_row(8, png(3, 3, empty), Json::array(), 3, 3, "unlabeled2017")})); });
  auto& validation = release(CoconutEdition::RelabeledValidation, "fixture-val", 1);
  annotation(validation, "val.parquet", [&](const auto& path) { write_coconut_parquet(path, Json::array({hf_row(9, mask, Json::array({first, second}), 3, 3, "val2017")})); });
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
  if (auto trace = mmltk::backend::data::testsupport::benchmark_trace())
   config.trace = {trace.selection(), [trace](std::string_view event, std::string_view fields) { trace(event, fields.empty() ? Json{} : Json::parse(fields)); }};
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
void add_ready_parquet_part(LocalCoconutRecipe& local, CoconutRecipeCatalog& catalog) {
 auto& release = catalog.releases.front();
 const std::array<std::uint32_t, 9> support{1, 1, 1, 1, 1, 1, 1, 1, 1};
 auto artifact = release.annotations.front();
 artifact.artifact_id += "-ready";
 artifact.filename = "ready.parquet";
 artifact.url += "/ready";
 const auto input = local.cache.source_downloads("coconut-" + std::string(release.name)) / artifact.filename;
 write_coconut_parquet(input, Json::array({hf_row(11, png(3, 3, support), Json::array({segment()}))}));
 artifact.expected_size = std::filesystem::file_size(input);
 artifact.expected_sha256.clear();
 release.annotations.push_back(artifact);
 ++release.expected_rows;
 const std::array<std::pair<std::string, std::string>, 2> images{{{coco(7).member, white_jpeg()}, {coco(11).member, white_jpeg()}}};
 replace_physical_images(local.cache, catalog, CoconutImageNamespace::CocoTrain, images);
}
void omit_optional_coconut_facts(const std::filesystem::path& path) {
 auto completion = Json::parse(file_bytes(path));
 auto& facts = completion.at("coconut");
 for (const auto* field : {"rejected_images", "source_rows", "source_segments", "source_extent_complete", "incomplete_ordinal_images"}) facts.erase(field);
 json_file(path, completion);
}
std::vector<AdmittedRecipeArchive> local_physical_archives(const BenchmarkCacheLayout& cache, const CoconutRecipeCatalog& catalog) {
 std::vector<AdmittedRecipeArchive> admitted;
 for (const auto& archive : catalog.images) {
  const auto path = cache.source_downloads(benchmark_source_name(archive.artifact.source)) / archive.artifact.filename;
  admitted.push_back({archive, {.path = path, .size = std::filesystem::file_size(path), .identity = archive.artifact.artifact_id}});
 }
 return admitted;
}
struct PhysicalMembershipFixture {
 std::vector<AdmittedRecipeArchive> admitted;
 std::atomic<bool> cancelled{false};
 mmltk::common::concurrency::CancellationObservation cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution;
 CoconutPhysicalMembership physical;
 PhysicalMembershipFixture(const BenchmarkCacheLayout& cache, const CoconutRecipeCatalog& catalog, std::uint64_t transient_bytes = 256ULL << 20, bool consume = true)
     : admitted(local_physical_archives(cache, catalog)),
       execution(1, {}, {.transient_bytes = transient_bytes, .descriptors = 13}, cancellation, mmltk::backend::data::testsupport::benchmark_trace()),
       physical(admitted, catalog, true, cache, execution.storage(), execution, cancellation) {
  if (consume)
   for (auto& source : admitted) {
    physical.admitted(source, source.download);
    physical.consume(source);
   }
 }
};
CoconutRecipeCatalog base_physical_catalog(const std::filesystem::path& archive, std::uint32_t images, const std::filesystem::path& unlabeled = {}) {
 CoconutRecipeCatalog catalog;
 catalog.releases.push_back({CoconutEdition::Base, "fixture-base", "fixture-v1", images, {}});
 catalog.images.push_back(
  {CoconutImageNamespace::CocoTrain, 0, "train2017", {"physical", "unused", archive.filename().string(), std::filesystem::file_size(archive), "", BenchmarkDatasetSource::kCoco2017}});
 if (!unlabeled.empty())
  catalog.images.push_back(
   {CoconutImageNamespace::CocoUnlabeled, 0, "unlabeled2017", {"unlabeled", "unused", unlabeled.filename().string(), std::filesystem::file_size(unlabeled), "", BenchmarkDatasetSource::kCoco2017}});
 return catalog;
}
std::pair<PreparedBenchmarkSplit, std::string> cached_mask_pixel_fixture(const std::filesystem::path& images) {
 prepare_cached_image_directory(images);
 const std::array<std::uint32_t, 1> foreground{1};
 auto encoded = png(1, 1, foreground);
 BenchmarkEncodedImage::publish(cached_image_path(images, 7), {reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()}, {});
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{images}};
 split.images = {{7, 1, 1, 0, 0}};
 return {std::move(split), std::move(encoded)};
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
 SECTION("mismatched mask geometry excludes contradictory samples") { mismatched_geometry = true; }
 SECTION("mismatched mask geometry also excludes contradictory letterbox samples") {
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
  std::atomic<bool> stopped{false};
  config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(stopped);
  std::string admission;
  config.trace = [&](std::string_view event, std::string_view fields) {
   if (event == "benchmark.archive.admission_wait") admission = fields;
  };
  auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog, nullptr, {}, {}, {.descriptors = descriptors}); });
  const mmltk::testsupport::ScopedTestCleanup stop([&] { stopped.store(true); });
  if (compiling.wait_for(std::chrono::seconds{15}) != std::future_status::ready) {
   stopped.store(true);
   compiling.wait();
   FAIL("recipe settlement deadline expired: " << admission);
  }
  compiling.get();
  config.cancel_requested = {};
  config.trace = {};
  const auto train = CompiledDataset::open(config.output_dir / "train.bin");
  const auto val = CompiledDataset::open(config.output_dir / "val.bin");
  REQUIRE(train.header().num_images == (mismatched_geometry ? 2U : 4U));
  const std::array<std::uint64_t, 4> expected{7, 8, 1, 2};
  std::array<std::uint32_t, 4> slots{};
  for (std::uint32_t i = 0; i < (mismatched_geometry ? 2U : expected.size()); ++i)
   slots[i] = compiled_slot(train, expected[i], i < 2 ? AnnotationSource::CoconutCoco : AnnotationSource::CoconutObjects365V2);
  CHECK(train.image_entry(slots[0]).source == AnnotationSource::CoconutCoco);
  if (!mismatched_geometry) CHECK(train.image_entry(slots[2]).source == AnnotationSource::CoconutObjects365V2);
  if (mismatched_geometry) {
   std::ifstream report(local.cache.root / "failed.txt");
   std::size_t failures = 0;
   for (std::string line; std::getline(report, line);) {
    const auto failed = Json::parse(line);
    if (failed["reason"] == "annotation canvas contradicts the image header") ++failures;
   }
   CHECK(failures >= 2U * static_cast<unsigned>(rebuild));
  }
  CHECK(dropped_instances == 0U);
  CHECK(train.image_labels(slots[1]).empty());
  REQUIRE(train.image_labels(slots[0]).size() == 2);
  CHECK(train.image_labels(slots[0])[0].is_crowd());
  CHECK(train.image_labels(slots[0])[0].original_area == 7);
  CHECK(train.image_labels(slots[0])[1].original_area == 17);
  CHECK(train.image_labels(slots[0])[1].source_category_id == 2);
  CHECK(train.image_labels(slots[0])[0].source_ordinal == 0);
  CHECK(train.image_labels(slots[0])[1].source_ordinal == 1);
  const auto runs = train.instance_rle(slots[0], train.image_labels(slots[0])[0]);
  REQUIRE(runs.size() == 3);
  CHECK(runs[0].start == 0);
  CHECK(runs[0].length == 4);
  CHECK(runs[1].start == 5);
  CHECK(runs[1].length == 2);
  CHECK(runs[2].start == 8);
  CHECK(runs[2].length == 1);
  for (std::size_t i = 0; i < 27; ++i) CHECK(train.image_pixels(slots[0])[i] == 1.0F);
  REQUIRE(train.class_names().size() == 80);
  CHECK(train.class_names()[0] == "person");
  CHECK(train.class_names()[79] == "toothbrush");
  CHECK(val.header().num_images == (choice == CoconutValidation::Coconut ? 2 : 1));
  const auto val_slot = compiled_slot(val, 9, choice == CoconutValidation::Stock ? AnnotationSource::Coco : AnnotationSource::CoconutCoco);
  CHECK(val.image_entry(val_slot).source_image_id == 9);
  CHECK(val.image_entry(val_slot).source == (choice == CoconutValidation::Stock ? AnnotationSource::Coco : AnnotationSource::CoconutCoco));
  CHECK(val.image_labels(val_slot).size() == (choice == CoconutValidation::Stock ? 1 : 2));
  if (choice == CoconutValidation::Coconut) {
   const auto objects_slot = compiled_slot(val, 1, AnnotationSource::CoconutObjects365V1);
   CHECK(val.image_entry(objects_slot).source_image_id == 1);
   CHECK(val.image_entry(objects_slot).source == AnnotationSource::CoconutObjects365V1);
  }
  const auto manifest = read_json_file(config.output_dir / "benchmark_manifest.json");
  CHECK_FALSE(manifest.contains("supplemental_sampling"));
  CHECK_FALSE(manifest["mappings"].contains("objects365"));
  CHECK(manifest["recipe"]["duplicate_xl_images"] == 1);
  CHECK(manifest["train"]["images"] == (mismatched_geometry ? 2U : 4U));
  struct stat status{};
  REQUIRE(::stat(jpeg_path.c_str(), &status) == 0);
  if (original_train.empty()) {
   original_train = benchmark_semantics(config.output_dir / "train.bin");
   jpeg_time = std::filesystem::last_write_time(jpeg_path);
   jpeg_inode = status.st_ino;
  } else {
   CHECK(benchmark_semantics(config.output_dir / "train.bin") == original_train);
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
 const auto validation_bytes = benchmark_semantics(config.output_dir / "val.bin");
 mmltk::testsupport::write_text_file(jpeg_path, std::string(std::filesystem::file_size(jpeg_path), 'x'));
 const auto repair_catalog = local.selected(config.selection.validation);
 unsigned replacement_downloads = 0;
 config.trace = [&](std::string_view event, std::string_view) {
  if (event == "benchmark.download.complete") ++replacement_downloads;
 };
 compile_benchmark_recipe(config, &repair_catalog);
 CHECK(replacement_downloads == 0);
 CHECK(benchmark_semantics(config.output_dir / "train.bin") == original_train);
 CHECK(benchmark_semantics(config.output_dir / "val.bin") == validation_bytes);
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
 CHECK(CompiledDataset::open(config.output_dir / "train.bin").header().num_images == 3);
 const auto previous = benchmark_semantics(config.output_dir / "train.bin");
 compile_benchmark_recipe(config, &catalog);
 CHECK(benchmark_semantics(config.output_dir / "train.bin") == previous);
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
 const auto proof = images / ".recipe-proofs/physical.inventory.complete.json";
 REQUIRE(std::filesystem::exists(proof));
 const auto original = read_json_file(proof);
 {
  auto lease = ArtifactLease::acquire(local.cache.locks / "objects365-patch-32.images.lock", {});
  const auto image_bytes = std::filesystem::file_size(cached_image_path(images, 1)) + std::filesystem::file_size(cached_image_path(images, 2));
  complete_cached_image_group(images, images / ".complete.json", original["identity"].get<std::string>(), ids, image_bytes, {});
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
  const auto extracted = extract_selected_archive_images({
   .archive_path = local.cache.source_downloads("objects365") / archive->artifact.filename,
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
  });
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
 const auto val = benchmark_semantics(config.output_dir / "val.bin");
 auto& large = *std::ranges::find(catalog.releases, CoconutEdition::Large, &CoconutReleaseComponent::edition);
 auto& masks = *std::ranges::find(large.annotations, std::string("large.tar"), &CatalogArtifact::filename);
 const auto path = local.cache.source_downloads("coconut-fixture-large") / masks.filename;
 const std::array<std::pair<std::string, std::string>, 1> unrelated{{{"unrelated", "missing required mask"}}};
 tar(path, unrelated);
 masks.expected_size = std::filesystem::file_size(path);
 masks.url += "?missing-mask";
 compile_benchmark_recipe(config, &catalog);
 CHECK(CompiledDataset::open(config.output_dir / "train.bin").header().num_images == 3);
 CHECK(benchmark_semantics(config.output_dir / "val.bin") == val);
}
TEST_CASE("COCONut disjoint XL membership remains complete", "[coconut][benchmark]") {
 ScopedTempDir root("coconut-disjoint");
 LocalCoconutRecipe local(root.path(), false, true);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, false);
 const auto catalog = local.selected(CoconutValidation::CoconutStock);
 compile_benchmark_recipe(config, &catalog);
 CHECK(CompiledDataset::open(config.output_dir / "train.bin").header().num_images == 4);
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
 write_coconut_parquet(path, Json::array({hf_row(7, png(3, 3, ids), Json::array({segment()}))}));
 artifact.expected_size = std::filesystem::file_size(path);
 artifact.url += "?overlap";
 CHECK_THROWS_WITH(compile_benchmark_recipe(config, &catalog), "COCONut train and validation reuse a physical member");
 CHECK(file_bytes(config.output_dir / "train.bin") == previous);
}
TEST_CASE("physical archive structural repair retains one admitted acquisition through extraction", "[coconut][benchmark][download]") {
 bool diagnostics = false;
 SECTION("disabled diagnostics preserve structural repair") {}
 SECTION("enabled diagnostics observe retained generations") { diagnostics = true; }
 ScopedTempDir root("coconut-physical-repair");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 ServedPhysicalArchive served(local.cache, catalog, CoconutImageNamespace::CocoTrain, "physical");
 compile_benchmark_recipe(config, &catalog);
 REQUIRE(served.server.requests() == 0);
 const auto before = benchmark_semantics(config.output_dir / "train.bin");
 const auto unaffected = cached_image_path(local.cache.source_images("objects365") / "patch-32", 1);
 struct stat inode{};
 REQUIRE(::stat(unaffected.c_str(), &inode) == 0);
 const auto proof = local.cache.source_images("objects365") / "patch-32/.recipe-proofs";
 const auto proof_count = std::distance(std::filesystem::directory_iterator(proof), std::filesystem::directory_iterator{});
 invalidate_cached_image_proofs(local.cache.source_images("coco") / "train2017");
 std::filesystem::remove(cached_image_path(local.cache.source_images("coco") / "train2017", 7));
 mmltk::testsupport::write_text_file(served.path, std::string(served.payload.size(), 'x'));
 unsigned physical_settlements = 0, failure_hashes = 0, cached_admissions = 0;
 if (diagnostics)
  config.trace = [&](std::string_view event, std::string_view fields) {
   const auto facts = Json::parse(fields);
   if (facts.value("artifact", std::string{}) != served.source.artifact.artifact_id) return;
   if (event == "benchmark.download.complete") ++physical_settlements;
   if (event == "benchmark.download.cache_hit" || event == "benchmark.download.preseeded") ++cached_admissions;
   if (event == "benchmark.download.failure_sha256") ++failure_hashes;
  };
 compile_benchmark_recipe(config, &catalog);
 CHECK(served.server.requests() == 1);
 CHECK(physical_settlements == (diagnostics ? 1 : 0));
 CHECK(failure_hashes == (diagnostics ? 1 : 0));
 CHECK(benchmark_semantics(config.output_dir / "train.bin") == before);
 struct stat after{};
 REQUIRE(::stat(unaffected.c_str(), &after) == 0);
 CHECK(after.st_ino == inode.st_ino);
 CHECK(std::distance(std::filesystem::directory_iterator(proof), std::filesystem::directory_iterator{}) == proof_count);
 const auto admissions_before = cached_admissions;
 compile_benchmark_recipe(config, &catalog);
 CHECK(served.server.requests() == 1);
 CHECK(physical_settlements == (diagnostics ? 1 : 0));
 CHECK(cached_admissions == admissions_before);
 std::ranges::fill(served.payload, 'x');
 mmltk::testsupport::write_text_file(served.path, std::string(served.payload.size(), 'x'));
 invalidate_cached_image_proofs(local.cache.source_images("coco") / "train2017");
 std::filesystem::remove(cached_image_path(local.cache.source_images("coco") / "train2017", 7));
 compile_benchmark_recipe(config, &catalog);
 CHECK(CompiledDataset::open(config.output_dir / "train.bin").header().num_images == 3);
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
 const std::string_view checksum = GENERATE("unavailable", "matching damaged source", "matching repaired source");
 const bool diagnostics = GENERATE(false, true);
 CAPTURE(checksum, diagnostics);
 ScopedTempDir root("coconut-membership-repair");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 ServedPhysicalArchive served(local.cache, catalog, CoconutImageNamespace::CocoTrain, "membership");
 if (checksum == "matching repaired source") served.source.artifact.expected_sha256 = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(served.path));
 const std::array<std::pair<std::string, std::string>, 1> wrong{{{coco(6).member, white_jpeg()}}};
 tar(served.path, wrong);
 REQUIRE(std::filesystem::file_size(served.path) == served.payload.size());
 const bool verified = checksum == "matching damaged source";
 if (verified) served.source.artifact.expected_sha256 = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(served.path));
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, false);
 std::vector<std::string> diagnosed;
 if (diagnostics)
  config.trace = [&](std::string_view event, std::string_view fields) {
   if (event == "benchmark.download.failure_sha256") diagnosed.push_back(Json::parse(fields).at("artifact").get<std::string>());
  };
 compile_benchmark_recipe(config, &catalog);
 CHECK(served.server.requests() == (verified ? 0 : 1));
 CHECK(CompiledDataset::open(config.output_dir / "train.bin").header().num_images == (verified ? 3 : 4));
 CHECK(diagnosed == (diagnostics ? std::vector<std::string>{served.source.artifact.artifact_id} : std::vector<std::string>{}));
 served.server.Check();
}
TEST_CASE("late shared archive repair rebinds completed Coconut releases", "[benchmark][coconut][cache][publication]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("coconut-late-shared-repair");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 ServedPhysicalArchive served(local.cache, catalog, CoconutImageNamespace::Objects365V2, "late-shared-repair");
 const std::array<std::pair<std::string, std::string>, 2> incomplete{{{objects(1).member, white_jpeg()}, {objects(3).member, white_jpeg()}}};
 tar(served.path, incomplete);
 REQUIRE(std::filesystem::file_size(served.path) == served.payload.size());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, false);
 mmltk::testsupport::TestGate xl("XL masks wait for the completed Large release");
 std::promise<void> large;
 BenchmarkCompileProgress settled;
 config.progress = [&](const BenchmarkCompileProgress& value) { settled = value; };
 std::atomic<bool> cancelled{false}, completed{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.annotations.release_complete" && Json::parse(fields).at("edition") == CoconutEdition::Large && !completed.exchange(true)) large.set_value();
 };
 catalog.release_observer = [&](CoconutEdition edition, CoconutReleaseBoundary boundary) {
  if (edition == CoconutEdition::XLarge && boundary == CoconutReleaseBoundary::MasksStarted) xl.receipt().ArriveAndWait();
 };
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled = true;
  xl.Release();
 });
 REQUIRE(xl.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(large, "Large completes before the shared archive is repaired", 5s);
 CHECK(served.server.requests() == 0);
 xl.Release();
 mmltk::testsupport::await_test_future(compiling, "shared physical generation repair settles all releases", 15s);
 CHECK(served.server.requests() == 1);
 const auto train = CompiledDataset::open(local.output / "train.bin");
 CHECK(train.header().num_images == 4);
 CHECK(train.image_labels(compiled_slot(train, 1, AnnotationSource::CoconutObjects365V2)).size() == 2);
 CHECK(train.image_labels(compiled_slot(train, 2, AnnotationSource::CoconutObjects365V2)).size() == 2);
 CHECK(CompiledDataset::open(local.output / "val.bin").header().num_images == 1);
 CHECK(settled.tracks.labels.complete);
 CHECK(settled.tracks.labels.completed == settled.tracks.labels.total);
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
 write_coconut_parquet(input.parquet_shards[0], rows);
 std::vector<std::uint64_t> observed;
 input.progress = [&](auto value) { observed.push_back(value); };
 std::map<CoconutPreparationStage, std::vector<std::uint64_t>> preparation;
 input.preparation_progress = [&](auto stage, auto count) { preparation[stage].push_back(count); };
 unsigned report_terminals = 0;
 input.image_terminal = [&] { ++report_terminals; };
 REQUIRE(import_coconut_annotations(input).front().index().image_count() == 130);
 CHECK(observed == std::vector<std::uint64_t>{0, 1, 64, 128, 130});
 CHECK(report_terminals == 0);
 SECTION("healthy metadata rows have no report settlement") {
  preparation.clear();
  input.metadata_only = true;
  REQUIRE(import_coconut_annotations(input).front().index().image_count() == 130);
  CHECK(preparation.at(CoconutPreparationStage::Annotations) == std::vector<std::uint64_t>{1, 64, 128, 130});
  CHECK(preparation.at(CoconutPreparationStage::Membership) == std::vector<std::uint64_t>{1, 64, 128, 130});
  CHECK(report_terminals == 0);
 }
 SECTION("disabled observation installs no callback") {
  const BenchmarkTraceSink silent;
  ProgressReporter reporter({}, silent);
  CHECK_FALSE(reporter.normalization_observer_enabled());
  input.progress = {};
  input.preparation_progress = {};
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
  CHECK(observed == std::vector<std::uint64_t>{0, 1, 64});
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
std::pair<CustomRecipeCatalog, std::vector<unsigned>> custom_training_window(LocalCoconutRecipe& local) {
 // Seventy images cross both ready-window and pixel-progress boundaries.
 std::vector<unsigned> ids;
 std::vector<std::pair<std::string, std::string>> members;
 for (unsigned id = 100; id < 170; ++id) {
  ids.push_back(id);
  members.emplace_back(coco(id).member, white_jpeg());
 }
 replace_physical_images(local.cache, local.catalog, CoconutImageNamespace::CocoTrain, members);
 return {local_custom_catalog(local, ids), std::move(ids)};
}
// Keep the constrained recipe's asynchronous admission and shared resource
// limits in one fixture driver. Callers release held inputs before joining.
std::future<void> compile_constrained_coconut(const BenchmarkCompilerConfig& config, const CoconutRecipeCatalog& catalog) {
 return std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog, nullptr, {}, {}, {.transient_bytes = 1, .descriptors = 13}); });
}
void remove_coconut_part_caches(const std::filesystem::path& directory) {
 for (const auto& entry : std::filesystem::directory_iterator(directory))
  if (entry.path().filename().string().find(".part-") != std::string::npos) std::filesystem::remove(entry.path());
}
void check_custom_compilation(const std::filesystem::path& output) {
 const auto train = CompiledDataset::open(output / "train.bin");
 const auto val = CompiledDataset::open(output / "val.bin");
 REQUIRE(train.header().num_images == 3);
 REQUIRE(val.header().num_images == 1);
 const std::array<std::uint64_t, 3> ids{7, 1, 17};
 const std::array<AnnotationSource, 3> sources{AnnotationSource::Coco, AnnotationSource::Objects365, AnnotationSource::OpenImages};
 const std::array<unsigned, 3> classes{2, 0, 1};
 // COCO preserves the annotation object's byte offset after {"annotations":[.
 const std::array<std::uint64_t, 3> source_ordinals{16, 0, 0};
 for (std::uint32_t i = 0; i < 3; ++i) {
  const auto slot = compiled_slot(train, ids[i], sources[i]);
  CHECK(train.image_entry(slot).source_image_id == ids[i]);
  CHECK(train.image_entry(slot).source == sources[i]);
  REQUIRE(train.image_labels(slot).size() == 1);
  const auto& label = train.image_labels(slot).front();
  CHECK(label.class_id == classes[i]);
  CHECK(label.bbox_x1 == 0.0F);
  CHECK(label.bbox_y1 == 0.0F);
  CHECK(label.bbox_x2 == 3.0F);
  CHECK(label.bbox_y2 == 3.0F);
  CHECK(label.original_area == 9);
  CHECK(label.source_ordinal == source_ordinals[i]);
  CHECK_FALSE(label.has_mask());
  for (unsigned pixel = 0; pixel < 27; ++pixel) CHECK(train.image_pixels(slot)[pixel] == 1.0F);
 }
 const auto coco_slot = compiled_slot(train, 7, AnnotationSource::Coco);
 CHECK(train.image_labels(coco_slot).front().annotation_id == 70);
 CHECK(train.image_labels(coco_slot).front().source_category_id == 3);
 CHECK(train.image_labels(coco_slot).front().is_crowd());
 CHECK(train.class_names()[2] == "car");
 const auto validation_slot = compiled_slot(val, 9, AnnotationSource::Coco);
 CHECK(val.image_entry(validation_slot).source_image_id == 9);
 CHECK(val.image_entry(validation_slot).source == AnnotationSource::Coco);
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
 std::string custom_train, custom_val, coconut_train, custom_train_semantics, custom_val_semantics;
 ino_t inode = 0;
 std::filesystem::file_time_type mtime;
 for (unsigned step = 0; step < 5; ++step) {
  const bool enhanced = step % 2 == 0;
  CAPTURE(step, enhanced);
  config.num_workers = 1 + step % 2;
  config.selection = {enhanced ? BenchmarkDatasetVariant::Coconut : BenchmarkDatasetVariant::CocoCustom, CoconutValidation::CoconutStock};
  REQUIRE_NOTHROW(compile_benchmark_recipe(config, enhanced ? &coconut : nullptr, enhanced ? nullptr : &custom));
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
   const auto train = benchmark_semantics(config.output_dir / "train.bin");
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
    custom_train_semantics = benchmark_semantics(config.output_dir / "train.bin");
    custom_val_semantics = benchmark_semantics(config.output_dir / "val.bin");
   } else {
    CHECK(benchmark_semantics(config.output_dir / "train.bin") == custom_train_semantics);
    CHECK(benchmark_semantics(config.output_dir / "val.bin") == custom_val_semantics);
   }
  }
 }
 // Both proof families coexist after selection switches. Each recipe repairs
 // the same damaged member through the shared source lease and invalidates both.
 for (const bool enhanced : {false, true}) {
  CAPTURE(enhanced);
  const auto bad = std::string(std::filesystem::file_size(jpeg), 'x');
  mmltk::testsupport::write_text_file(jpeg, bad);
  config.selection = {enhanced ? BenchmarkDatasetVariant::Coconut : BenchmarkDatasetVariant::CocoCustom, CoconutValidation::CoconutStock};
  REQUIRE_NOTHROW(compile_benchmark_recipe(config, enhanced ? &coconut : nullptr, enhanced ? nullptr : &custom));
  CHECK(server.requests() == 0);
  if (enhanced) {
   CHECK_FALSE(std::filesystem::exists(image_root / ".complete.json"));
   CHECK(benchmark_semantics(config.output_dir / "train.bin") == coconut_train);
  } else {
   CHECK((!std::filesystem::exists(image_root / ".recipe-proofs") || std::filesystem::is_empty(image_root / ".recipe-proofs")));
   check_custom_compilation(config.output_dir);
  }
 }
 config.selection = {BenchmarkDatasetVariant::CocoCustom, CoconutValidation::CoconutStock};
 REQUIRE_NOTHROW(compile_benchmark_recipe(config, nullptr, &custom));
 check_custom_compilation(config.output_dir);
 custom_train = file_bytes(config.output_dir / "train.bin");
 custom_val = file_bytes(config.output_dir / "val.bin");
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
 const auto train_semantics = benchmark_semantics(config.output_dir / "train.bin"), val_semantics = benchmark_semantics(config.output_dir / "val.bin");
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
 if (exhausted || cancelled) {
  CHECK(file_bytes(config.output_dir / "train.bin") == train);
  CHECK(file_bytes(config.output_dir / "val.bin") == val);
  CHECK(file_bytes(config.output_dir / "benchmark_manifest.json") == manifest);
 } else {
  CHECK(benchmark_semantics(config.output_dir / "train.bin") == train_semantics);
  CHECK(benchmark_semantics(config.output_dir / "val.bin") == val_semantics);
 }
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
 const auto old_train = benchmark_semantics(config.output_dir / "train.bin"), old_val = benchmark_semantics(config.output_dir / "val.bin");
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
 std::filesystem::remove(local.cache.source_indexes("coco") / "train2017.images.inventory");
 invalidate_cached_image_proofs(local.cache.source_images("coco") / "train2017");
 std::vector<BenchmarkCompileProgress> observations;
 config.progress = [&](const BenchmarkCompileProgress& value) { observations.push_back(value); };
 unsigned admissions = 0;
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event != "benchmark.download.cache_hit" && event != "benchmark.download.preseeded" && event != "benchmark.download.complete") return;
  if (Json::parse(fields).value("artifact", std::string{}) == served.source.artifact.artifact_id) ++admissions;
 };
 if (exhausted) {
  compile_benchmark_recipe(config, &catalog);
  CHECK(CompiledDataset::open(config.output_dir / "train.bin").header().num_images == 3);
  CHECK(admissions <= 3);
  CHECK(served.server.requests() <= 2);
 } else {
  compile_benchmark_recipe(config, &catalog);
  CHECK(admissions == 2);
  REQUIRE_FALSE(observations.empty());
  CHECK(std::ranges::all_of(observations.back().sources, &BenchmarkSourceProgress::complete));
  if (writer_recovery) CHECK(observations.back().tracks.labels.complete);
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
  const auto slot = compiled_slot(train, 7, AnnotationSource::CoconutCoco);
  for (unsigned pixel = 0; pixel < 27; ++pixel) CHECK(train.image_pixels(slot)[pixel] == 1.0F);
 }
 if (!exhausted) CHECK(benchmark_semantics(config.output_dir / "train.bin") == old_train);
 CHECK(benchmark_semantics(config.output_dir / "val.bin") == old_val);
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
  auto rows = fixture_physical_inventory(
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
  const auto admitted = stock.finish();
  CHECK(admitted.cache_hit);
  CHECK(admitted.retained_storage_bytes == std::filesystem::file_size(index) + std::filesystem::file_size(index.string() + ".complete.json"));
 }
 auto config = local.compiler_config({}, true);
 std::array<std::string, 2> trains, validations;
 for (unsigned pass = 0; pass < 4; ++pass) {
  const auto enhanced = pass % 2;
  config.selection = {enhanced ? BenchmarkDatasetVariant::Coconut : BenchmarkDatasetVariant::CocoCustom, CoconutValidation::Stock};
  compile_benchmark_recipe(config, enhanced ? &coconut : nullptr, enhanced ? nullptr : &custom);
  const auto train = benchmark_semantics(config.output_dir / "train.bin"), val = benchmark_semantics(config.output_dir / "val.bin");
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
  auto indexes = annotations.finish();
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
 const auto [train, val, manifest] = capture_publication_bytes(config.output_dir);
 const auto index = local.cache.source_indexes("coco") / "val2017.normalized.bin";
 remove_normalized_annotation_index(index);
 std::atomic<bool> cancel{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancel);
 const CocoAnnotationInputObserver cancel_parse = [&](bool training, CocoAnnotationInputBoundary boundary) {
  if (!training && boundary == CocoAnnotationInputBoundary::Parsing) cancel = true;
 };
 CHECK_THROWS(compile_benchmark_recipe(config, &catalog, nullptr, {}, {}, {}, cancel_parse));
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
 REQUIRE(compiled.header().num_images == 1);
 CHECK(compiled.image_entry(0).source_image_id == 9);
 CHECK(compiled.image_entry(0).source == AnnotationSource::Coco);
 CHECK(server.requests() == 1);
 CHECK(file_bytes(raw) == served);
 server.Check();
}
TEST_CASE("one stock request builds both splits and rebinds newly settled train after validation repair", "[benchmark][coconut][download]") {
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
 std::optional<NormalizedAnnotationIndex> settled_train;
 ProgressReporter progress({}, trace);
 CocoAnnotationCache cache(local.cache, custom.coco_annotations, {CocoSplitAdmission::Required, CocoSplitAdmission::Required}, 2, 1, 1, {}, trace);
 cache.observe_splits([&](CocoAnnotationSplit split) {
  if (split.training && split.index && !settled_train) settled_train = *split.index;
 });
 cache.discover(progress);
 CHECK(cache.completed_indexes() == 0);
 REQUIRE(cache.pending_download());
 auto acquired = download_artifacts({*cache.pending_download()}, 1, {}).front();
 CHECK(server.requests() == 0);
 std::uint64_t completed = 0;
 cache.settle(std::move(acquired), progress, 1, completed, 2);
 auto indexes = cache.finish();
 REQUIRE(indexes.train);
 REQUIRE(indexes.validation);
 REQUIRE(settled_train);
 CHECK(indexes.train->annotation_sha256 != settled_train->annotation_sha256);
 CHECK(std::ranges::equal(std::as_bytes(indexes.train->images), std::as_bytes(settled_train->images)));
 CHECK(std::ranges::equal(std::as_bytes(indexes.train->boxes), std::as_bytes(settled_train->boxes)));
 CHECK(std::ranges::equal(std::as_bytes(indexes.train->mask_rle_pairs), std::as_bytes(settled_train->mask_rle_pairs)));
 CHECK(indexes.train->images.size() == 2);
 CHECK(indexes.validation->images.size() == 1);
 CHECK(completed == 2);
 CHECK(server.requests() == 1);
 server.Check();
}
TEST_CASE("late original repair settles completed Coconut recovery without retaining its publication lock", "[benchmark][coconut][recovery][publication]") {
 using namespace std::chrono_literals;
 const bool warm = GENERATE(false, true);
 CAPTURE(warm);
 ScopedTempDir root("coconut-late-original-repair");
 LocalCoconutRecipe local(root.path());
 auto originals = local_custom_catalog(local).coco_annotations;
 const auto indexes = local.cache.source_indexes("coco");
 for (const auto split : {"train2017", "val2017"}) remove_normalized_annotation_index(indexes / (std::string(split) + ".normalized.bin"));
 const auto path = local.cache.source_downloads("coco") / originals.filename;
 const auto first_train = file_bytes(indexes / "train2017.fixture.json");
 auto replacement_train = Json::parse(first_train);
 replacement_train["annotations"][0]["id"] = 71;
 std::array<std::pair<std::string, std::string>, 2> members{
  {{"annotations/instances_train2017.json", replacement_train.dump()}, {"annotations/instances_val2017.json", file_bytes(indexes / "val2017.fixture.json")}}
 };
 tar(path, members);
 const auto payload = file_bytes(path);
 mmltk::backend::data::testsupport::HttpServer server(payload);
 originals.url = server.url("late-original-repair");
 originals.expected_size = payload.size();
 members[0].second = first_train;
 tar(path, members);
 REQUIRE(std::filesystem::file_size(path) == originals.expected_size);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Stock, true}, true);
 config.num_workers = 4;
 auto catalog = local.selected(config.selection.validation);
 catalog.stock_annotations = originals;
 const auto& base_release = *std::ranges::find(catalog.releases, CoconutEdition::Base, &CoconutReleaseComponent::edition);
 std::string seed_identity;
 if (warm) {
  compile_benchmark_recipe(config, &catalog);
  seed_identity = read_json_file(local.output / "benchmark_manifest.json").at("recipe").at("original_annotations").at("train").get<std::string>();
  std::size_t part_completions = 0;
  for (const auto& entry : std::filesystem::directory_iterator(local.cache.source_indexes("coconut-" + std::string(base_release.name))))
   if (entry.path().string().ends_with(".part-0.normalized.bin.complete.json")) {
    const auto index_path = entry.path().string().substr(0, entry.path().string().size() - std::string_view(".complete.json").size());
    CHECK(std::filesystem::is_regular_file(index_path));
    CHECK(std::filesystem::is_regular_file(index_path + ".inventory"));
    ++part_completions;
   }
  REQUIRE(part_completions == 2);
 }
 // Preserve the seed's archive identity while forcing both splits through
 // parsing again; the Base part triplets and all physical inputs stay retained.
 members[1].second.assign(members[1].second.size(), ' ');
 members[1].second.front() = '{';
 tar(path, members);
 REQUIRE(std::filesystem::file_size(path) == originals.expected_size);
 for (const auto split : {"train2017", "val2017"}) remove_normalized_annotation_index(indexes / (std::string(split) + ".normalized.bin"));
 std::filesystem::remove_all(indexes / "source-json");
 auto artifact = warm ? ArtifactLease::acquire(local.cache.locks / (base_release.annotations.front().artifact_id + ".lock"), {}) : ArtifactLease{};
 mmltk::testsupport::TestGate validation("validation parse waits for completed Base recovery");
 std::promise<void> base, train_ready;
 BenchmarkCompileProgress settled;
 config.progress = [&](const BenchmarkCompileProgress& value) { settled = value; };
 std::atomic<bool> cancelled{false}, held{false}, completed{false}, original_ready{false}, base_cached{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 const auto diagnostics = mmltk::backend::data::testsupport::benchmark_trace();
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (diagnostics) diagnostics(event, Json::parse(fields));
  if (event == "benchmark.annotations.original_index_completed" && Json::parse(fields).at("split") == "train2017" && !original_ready.exchange(true)) train_ready.set_value();
  if (event == "benchmark.annotations.release_complete") {
   const auto value = Json::parse(fields);
   if (value.at("edition") == CoconutEdition::Base && !completed.exchange(true)) {
    base_cached = value.at("cache_hit").get<bool>();
    base.set_value();
   }
  }
 };
 auto compiling = std::async(std::launch::async, [&] {
  compile_benchmark_recipe(config, &catalog, nullptr, {}, {}, {.transient_bytes = 1ULL << 30}, [&](bool training, CocoAnnotationInputBoundary boundary) {
   if (!training && boundary == CocoAnnotationInputBoundary::Parsing && !held.exchange(true)) validation.receipt().ArriveAndWait();
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled = true;
  artifact = {};
  validation.Release();
 });
 REQUIRE(validation.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(train_ready, "first train original publication precedes Base part admission", 5s);
 artifact = {};
 mmltk::testsupport::await_test_promise(base, "Base recovery completes against the first original generation", 5s);
 CHECK(base_cached.load() == warm);
 CHECK(server.requests() == 0);
 validation.Release();
 mmltk::testsupport::await_test_future(compiling, "late original generation replacement releases all lifecycle leases", 15s);
 CHECK(server.requests() == 1);
 const auto train = CompiledDataset::open(local.output / "train.bin");
 CHECK(train.header().num_images == 4);
 CHECK(train.image_labels(compiled_slot(train, 7, AnnotationSource::CoconutCoco)).size() == 2);
 CHECK(train.image_labels(compiled_slot(train, 8, AnnotationSource::CoconutCoco)).empty());
 CHECK(CompiledDataset::open(local.output / "val.bin").header().num_images == 1);
 CHECK(settled.tracks.labels.complete);
 CHECK(settled.tracks.labels.completed == settled.tracks.labels.total);
 CHECK(settled.tracks.labels.invalidated == 1);
 const auto manifest = read_json_file(local.output / "benchmark_manifest.json").at("recipe");
 if (warm) CHECK(manifest.at("original_annotations").at("train") != seed_identity);
 std::size_t train_components = 0;
 for (const auto& component : manifest.at("components"))
  if (component.at("physical_source") == coconut_namespace_name(CoconutImageNamespace::CocoTrain)) {
   CHECK(component.at("original_annotation_identity") == manifest.at("original_annotations").at("train"));
   ++train_components;
  }
 CHECK(train_components == 1);
 for (const auto& source_release : catalog.releases) CHECK(ArtifactLease::try_acquire_charged(local.cache.locks / (std::string(source_release.name) + ".annotations.lifecycle.lock"), {}, nullptr));
 CHECK(ArtifactLease::try_acquire_charged(local.cache.locks / "coco-annotations.lifecycle.lock", {}, nullptr));
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
 const std::array<std::pair<std::string, std::string>, 2> members{
  {{"annotations/instances_train2017.json", file_bytes(indexes / "train2017.fixture.json")}, {"annotations/instances_val2017.json", file_bytes(indexes / "val2017.fixture.json")}}
 };
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
 const auto admitted = cache.finish();
 REQUIRE(admitted.train);
 REQUIRE(admitted.validation);
 CHECK(completed == 2);
 REQUIRE(::stat(extracted.c_str(), &after) == 0);
 CHECK(before.st_ino == after.st_ino);
}
TEST_CASE("COCONut recovers the complete dropped dog candidate set and carves source supporters", "[coconut][benchmark]") {
 ScopedTempDir root("coconut-recovery");
 unsigned image_id = 2212;
 unsigned supporter_category = 63;
 bool enabled = true;
 bool empty_supporter = false, replace_original = false;
 std::vector<RLEPair> masks{{0, 2}, {1, 2}};  // Independent originals intentionally overlap.
 SECTION("two dogs on couch") {}
 SECTION("dog on boat") {
  image_id = 400;
  supporter_category = 9;
  masks.resize(1);
 }
 SECTION("recovery off retains the previous omission and metadata") { enabled = false; }
 SECTION("original replacement changes identity without changing recovered geometry") { replace_original = true; }
 SECTION("an emptied supporter is omitted without recursive recovery") { empty_supporter = true; }
 auto originals = recovery_originals(image_id, masks);
 const auto indexed_originals_view = fixture_index(originals);
 auto indexed_originals = std::make_shared<CoconutRecoveryOriginals>(&indexed_originals_view, nullptr);
 CoconutMaskRecovery recovery(*indexed_originals);
 auto couch = segment(30, supporter_category);
 couch["area"] = 99;
 Json segments = Json::array({segment(20, 18), couch});
 if (masks.size() == 2) segments.push_back(segment(10, 18));
 std::array<std::uint32_t, 9> pixels{30, 30, 30, 30, 30, 30, 30, 30, 30};
 if (empty_supporter) pixels = {30, 30, 30, 0, 0, 0, 0, 0, 0};
 const std::array physical{coco(image_id)};
 auto input = request(physical);
 input.records = std::make_shared<CoconutAnnotationRecords>();
 std::shared_ptr<const CoconutNativeImage> native;
 input.progress = [&](std::uint64_t complete) {
  if (complete == 1) native = input.records->row(0).native();
 };
 input.parquet_shards = {root.path() / "dogs.parquet"};
 input.recovery = enabled ? &recovery : nullptr;
 if (enabled) input.originals = [&](CoconutImageNamespace, bool, std::stop_token) { return CoconutOriginalReadiness{indexed_originals, true, 11}; };
 std::uint64_t rejected = 0;
 unsigned report_terminals = 0;
 input.rejected_object = [&](const auto&, const auto&, const auto&, auto, auto) { ++rejected; };
 input.image_terminal = [&] { ++report_terminals; };
 write_coconut_parquet(input.parquet_shards[0], Json::array({hf_row(image_id, png(3, 3, pixels), segments)}));
 const auto result = import_coconut_annotations(input);
 CHECK(report_terminals == (enabled ? 0U : 1U));
 REQUIRE(result.size() == 1);
 const auto& component = result.front();
 REQUIRE(component.index().image_count() == 1);
 CHECK(component.index().images()[0].source_image_id == image_id);
 REQUIRE(native);
 CHECK(native->input_identity() == component.image_input_identity(0));
 CHECK(native->lineage().index.source == component.index().source);
 CHECK(native->lineage().index.split == component.index().split);
 CHECK(native->lineage().component.recovery_policy == (enabled ? kCoconutRecoveryPolicy : 0));
 const auto cache = root.path() / "recovery.normalized.bin";
 store_coconut_component(cache, component);
 const auto cached = load_coconut_component(cache, component.edition(), component.source(), input.input_identity, {}, false, input.physical_membership, enabled ? &recovery : nullptr);
 REQUIRE(cached);
 CHECK(cached->image_input_identity(0) == native->input_identity());
 if (replace_original) {
  const auto previous = native;
  auto replacement_source = originals;
  replacement_source.annotation_sha256 = std::string(64, 'b');
  const auto replacement_index = fixture_index(replacement_source);
  auto replacement_originals = std::make_shared<CoconutRecoveryOriginals>(&replacement_index, nullptr);
  CoconutMaskRecovery replacement_recovery(*replacement_originals);
  input.recovery = &replacement_recovery;
  input.originals = [replacement_originals](CoconutImageNamespace, bool, std::stop_token) { return CoconutOriginalReadiness{replacement_originals, true, 12}; };
  const auto replacement = import_coconut_annotations(input);
  REQUIRE(replacement.size() == 1);
  REQUIRE(native);
  CHECK(native->input_identity() != previous->input_identity());
  CHECK(previous->input_identity() == "293304b221de8f6d6984ad2998e7ec88701087a77e3eb3efb34520a1fbaf51d4");
  CHECK(native->input_identity() == "fc84daad7aac93ac6c70eda8a20d27c043b4589c833056f7afccdaef86059e51");
  CHECK(native->input_identity() == replacement.front().image_input_identity(0));
  CHECK(native->lineage().component.original_generation == 12);
  CHECK(previous->lineage().component.original_generation == 11);
  CHECK(native->lineage().component.original_annotation_identity == replacement_source.annotation_sha256);
  REQUIRE(native->runs().size() == previous->runs().size());
  CHECK(std::memcmp(native->runs().data(), previous->runs().data(), native->runs().size_bytes()) == 0);
  const auto replacement_path = root.path() / "replacement.bin";
  store_coconut_component(replacement_path, replacement.front());
  const auto reloaded = load_coconut_component(replacement_path, component.edition(), component.source(), input.input_identity, {}, false, input.physical_membership, &replacement_recovery);
  REQUIRE(reloaded);
  CHECK(reloaded->image_input_identity(0) == native->input_identity());
  CHECK_FALSE(load_coconut_component(cache, component.edition(), component.source(), input.input_identity, {}, false, input.physical_membership, &replacement_recovery));
  CHECK(cached->image_input_identity(0) == previous->input_identity());
  input.recovery = &recovery;
 }
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
 CHECK(component.original_generation() == 11);
 CHECK(component.select_images({0}).original_generation() == 11);
 const auto& facts = component.recovery_image(0);
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
 CHECK(cached->original_generation() == 0);
 const auto admitted = cached->with_original({indexed_originals, true, 11});
 CHECK(admitted.original_generation() == 11);
 CHECK(admitted.membership().original_generation() == 11);
 CHECK(admitted.select_images({0}).original_generation() == 11);
 CHECK_THROWS(cached->with_original({{}, true, 11}));
 CHECK_THROWS(cached->with_original({indexed_originals, false, 11}));
 CHECK(cached->recovery().front().unresolved == facts.unresolved);
 CHECK(cached->recovery().front().omissions.size() == facts.unresolved);
 CHECK(cached->index().annotation_sha256 == component.index().annotation_sha256);
 CHECK(std::ranges::equal(cached->inventory(), component.inventory()));
 const auto roundtrip = root.path() / "recovery-roundtrip.normalized.bin";
 store_coconut_component(roundtrip, *cached);
 for (const auto suffix : {"", ".inventory", ".complete.json"}) CHECK(file_bytes(cache.string() + suffix) == file_bytes(roundtrip.string() + suffix));
 for (const auto& previous : {component, *cached}) {
  const auto generation = previous.original_generation();
  CAPTURE(generation);
  const auto bound = previous.with_original({indexed_originals, true, 12});
  std::array parts{CoconutComponentPart{{bound.select_images({0})}, 0, 0}};
  const auto composed = merge_coconut_components(parts, input.input_identity, *input.physical_membership, root.path());
  REQUIRE(composed.size() == 1);
  CHECK(composed.front().original_generation() == 12);
  CHECK(composed.front().select_images({0}).original_generation() == 12);
  CHECK(previous.original_generation() == generation);
  CHECK(previous.select_images({0}).original_generation() == generation);
  CHECK(admitted.original_generation() == 11);
  CHECK(component.original_generation() == 11);
  CHECK(cached->original_generation() == 0);
  parts.front().components = composed;
  const auto repeated = merge_coconut_components(parts, input.input_identity, *input.physical_membership, root.path());
  REQUIRE(repeated.size() == 1);
  CHECK(repeated.front().original_generation() == 12);
  CHECK(repeated.front().image_input_identity(0) == component.image_input_identity(0));
  CHECK(repeated.front().matches_inputs(input.input_identity, *input.physical_membership, &recovery));
  CHECK(repeated.front().compiled_ordinal_base(0) == component.compiled_ordinal_base(0));
  CHECK(repeated.front().source_ordinals_complete(0) == component.source_ordinals_complete(0));
  CHECK(repeated.front().required_labels_valid(0) == component.required_labels_valid(0));
  unsigned publication = 0;
  for (const auto& value : {bound, composed.front(), repeated.front()}) {
   const auto path = root.path() / ("bound-" + std::to_string(generation) + "-" + std::to_string(publication++) + ".normalized.bin");
   store_coconut_component(path, value);
   for (const auto suffix : {"", ".inventory", ".complete.json"}) CHECK(file_bytes(cache.string() + suffix) == file_bytes(path.string() + suffix));
   const auto loaded = load_coconut_component(path, component.edition(), component.source(), input.input_identity, {}, false, input.physical_membership, &recovery);
   REQUIRE(loaded);
   CHECK(loaded->original_generation() == 0);
  }
  // Empty older views contribute no rows or generation to the composition.
  parts.front().components = {previous.select_images({}), bound};
  const auto with_empty = merge_coconut_components(parts, input.input_identity, *input.physical_membership, root.path());
  REQUIRE(with_empty.size() == 1);
  CHECK(with_empty.front().original_generation() == 12);
  CHECK(with_empty.front().index().image_count() == 1);
 }
 const std::array unbound{CoconutComponentPart{{*cached}, 0, 0}};
 const auto merged_unbound = merge_coconut_components(unbound, input.input_identity, *input.physical_membership, root.path());
 REQUIRE(merged_unbound.size() == 1);
 CHECK(merged_unbound.front().original_generation() == 0);
 CHECK(merge_coconut_components({}, input.input_identity, *input.physical_membership, root.path()).empty());
 const std::array empty{CoconutComponentPart{{admitted.select_images({})}, 0, 0}};
 CHECK(merge_coconut_components(empty, input.input_identity, *input.physical_membership, root.path()).empty());
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
  for (std::size_t segment = 0; segment < record.segments.size(); ++segment) record.segments[segment].bbox = std::array<double, 4>{0, 0, 1, 1};
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
  write_coconut_parquet(input.parquet_shards[0], Json::array({hf_row(id, file_bytes(fixtures / (std::to_string(id) + ".mask.png")), source.at("segments"), width, height)}));
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
    std::vector<RLEPair> dogs, support;
    (void)dataset::append_resized_row_major_mask(dog_runs, {width, height}, target, geometry, &scratch, dogs);
    (void)dataset::append_resized_row_major_mask(support_runs, {width, height}, target, geometry, &scratch, support);
    std::vector<std::uint8_t> foreground, background;
    dataset::materialize_row_major_mask(dogs, target, &foreground);
    dataset::materialize_row_major_mask(support, target, &background);
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
  const std::array parts{CoconutComponentPart{{component}, 0, 0}};
  const auto merged = merge_coconut_components(parts, input.input_identity, *input.physical_membership, root.path());
  REQUIRE(merged.size() == 1);
  CHECK(merged.front().input_identity() == component.input_identity());
  CHECK(merged.front().index().annotation_sha256 == component.index().annotation_sha256);
  const auto merged_path = root.path() / "recovered-merged.normalized.bin";
  store_coconut_component(merged_path, merged.front());
  CHECK(file_bytes(merged_path.string() + ".inventory") == file_bytes(path.string() + ".inventory"));
  CHECK(file_bytes(merged_path.string() + ".complete.json") == file_bytes(path.string() + ".complete.json"));
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
 SECTION("a malformed optional member cannot prevent the other cold required member") {
  cold = true;
  unusable_document = 1;
 }
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
 ProgressReporter progress({}, trace);
 CocoAnnotationCache cache(local.cache, artifact, {CocoSplitAdmission::Optional, required_validation ? CocoSplitAdmission::Required : CocoSplitAdmission::Optional}, 2, 1, 1, {}, trace);
 cache.observe_input([&](bool training, CocoAnnotationInputBoundary boundary) {
  if (!training) return;
  if (local_archive_failure && boundary == CocoAnnotationInputBoundary::Extracting) {
   extraction_entered = true;
   REQUIRE(std::filesystem::remove(archive_path));
  }
  if (local_parser_failure && boundary == CocoAnnotationInputBoundary::Parsing) {
   parser_entered = true;
   REQUIRE(std::filesystem::remove(local.cache.source_indexes("coco") / "source-json/instances_train2017.json"));
  }
 });
 std::mutex splits_mutex;
 std::vector<CocoAnnotationSplit> splits;
 cache.observe_splits([&](CocoAnnotationSplit split) {
  const std::lock_guard lock(splits_mutex);
  splits.push_back(std::move(split));
 });
 cache.discover(progress);
 {
  const std::lock_guard lock(splits_mutex);
  REQUIRE(splits.size() == (cold ? 0 : 1));
  if (!cold) {
   CHECK(splits.front().training != missing_train);
   CHECK(splits.front().terminal);
   REQUIRE(splits.front().index);
  }
 }
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
   CHECK_THROWS(cache.finish());
   CHECK(file_bytes(retained) == retained_bytes);
   return;
  }
  cache.settle(std::move(archive), progress, 1, completed, 2);
 }
 // Probe the real source lease without sleeps or a potentially hanging waiter.
 const auto lease_probe = mmltk::common::io::FileHandle::open_readonly((local.cache.locks / "coco-annotations.lifecycle.lock").string());
 REQUIRE(::flock(lease_probe.get(), LOCK_EX | LOCK_NB) == -1);
 REQUIRE(errno == EWOULDBLOCK);
 {
  const std::lock_guard lock(splits_mutex);
  for (const bool training : {true, false}) {
   const auto last = std::ranges::find_if(splits | std::views::reverse, [&](const auto& split) { return split.training == training; });
   REQUIRE(last != (splits | std::views::reverse).end());
   CHECK(last->terminal);
   CHECK(last->index.has_value() == (training != missing_train));
  }
 }
 const auto indexes = cache.finish();
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
  write_coconut_parquet(path, rows);
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
   const auto train_bytes = benchmark_semantics(config.output_dir / "train.bin");
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
 const auto previous = benchmark_semantics(config.output_dir / "train.bin");
 const auto prior_manifest = file_bytes(config.output_dir / "benchmark_manifest.json");
 auto changed = recovery_originals(7, std::array<RLEPair, 1>{{{0, 1}}});
 changed.annotation_sha256 = std::string(64, 'c');
 store_normalized_annotation_index(local.cache.source_indexes("coco") / "train2017.normalized.bin", fixture_index(changed), {});
 const auto catalog = local.selected(config.selection.validation);
 compile_benchmark_recipe(config, &catalog);
 CHECK(benchmark_semantics(config.output_dir / "train.bin") == previous);
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
 const auto published_before_cancel = file_bytes(config.output_dir / "train.bin");
 std::atomic<bool> stop{true};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(stop);
 CHECK_THROWS(compile_benchmark_recipe(config, &catalog));
 CHECK(file_bytes(config.output_dir / "train.bin") == published_before_cancel);
}
TEST_CASE("COCONut recovery facts follow physical images when import rows are reordered", "[coconut][benchmark][cache]") {
 const bool incomplete = GENERATE(false, true);
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
 write_coconut_parquet(input.parquet_shards.front(), offered);
 auto components = import_coconut_annotations(input);
 if (incomplete) {
  const std::array parts{CoconutComponentPart{components, std::nullopt, std::nullopt}};
  components = merge_coconut_components(parts, input.input_identity, *input.physical_membership, root.path());
 }
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
   const auto& facts = product->recovery_image(i);
   const auto& inventory = product->inventory_image(i);
   const auto& image = product->index().image(i);
   CHECK(product->source_ordinals_complete(i) == !incomplete);
   for (const auto& box : product->index().storage().boxes.subspan(image.first_box, image.box_count)) CHECK(((box.flags & kAnnotationOrdinalIncomplete) != 0) == incomplete);
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
 CHECK(rebuilt.front().source_ordinals_complete(0));
 CHECK((rebuilt.front().index().storage().boxes.front().flags & kAnnotationOrdinalIncomplete) == 0);
 CHECK(rebuilt.front().index().annotation_sha256 == component.index().annotation_sha256);
 store_coconut_component(roundtrip, rebuilt.front());
 CHECK(file_bytes(path.string() + ".inventory") == file_bytes(roundtrip.string() + ".inventory"));
}
TEST_CASE("COCONut metadata membership precedes mask payload admission", "[coconut]") {
 ScopedTempDir root("coconut-membership-first");
 const auto path = root.path() / "membership.parquet";
 write_coconut_parquet(path, Json::array({hf_row(7, "not a PNG", Json::array()), hf_row(8, "also not a PNG", Json::array())}));
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
 const auto background = import_coconut_annotations(input);
 REQUIRE(background.size() == 2);
 for (const auto& component : background) {
  CHECK(component.required_labels_valid(0));
  CHECK(component.index().box_count() == 0);
 }
}
TEST_CASE("cached training labels start before pixel drain and overlap subsequent pixels", "[benchmark][pipeline]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPU lanes");
 bool cancel_reader = false;
 SECTION("same training population labels overlap later pixel progress") {}
 SECTION("cancellation drains the held reader and retains the old generation") { cancel_reader = true; }
 ScopedTempDir root("custom-label-overlap");
 LocalCoconutRecipe local(root.path());
 auto [custom, ids] = custom_training_window(local);
 auto config = local.compiler_config({BenchmarkDatasetVariant::CocoCustom, CoconutValidation::CoconutStock}, true);
 compile_benchmark_recipe(config, nullptr, &custom);
 compile_benchmark_recipe(config, nullptr, &custom);
 const auto [train, validation, manifest] = capture_publication_bytes(local.output);
 const auto train_semantics = benchmark_semantics(local.output / "train.bin");
 const auto validation_semantics = benchmark_semantics(local.output / "val.bin");
 const auto image_root = local.cache.source_images("coco") / "train2017";
 mmltk::testsupport::TestGate reader("early COCO training reader"), labels("same COCO training labels");
 const auto reader_receipt = reader.receipt(), labels_receipt = labels.receipt();
 std::atomic<bool> reader_seen{false}, cancelled{false};
 std::mutex observation_mutex;
 BenchmarkCompileProgress observed;
 std::uint64_t pixels_before_release = 0;
 bool observe_subsequent_pixels = false, delivered = false, preparing_delivered = false;
 std::promise<void> subsequent_pixels, validation_placed, labels_preparing;
 config.progress = [&](const BenchmarkCompileProgress& update) {
  const std::lock_guard lock(observation_mutex);
  observed = update;
  if (update.tracks.labels.activity == DatasetCompileActivity::Preparing && !std::exchange(preparing_delivered, true)) labels_preparing.set_value();
  if (observe_subsequent_pixels && update.tracks.pixels.completed > pixels_before_release && !std::exchange(delivered, true)) subsequent_pixels.set_value();
 };
 std::atomic<bool> placed{false};
 config.num_workers = 4;
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.labels.slot_written" && Json::parse(fields).at("root") == (local.cache.source_images("coco") / "val2017").string() && !placed.exchange(true)) validation_placed.set_value();
 };
 auto compiling = std::async(std::launch::async, [&] {
  compile_benchmark_recipe(config, nullptr, &custom, [&](BenchmarkDatasetSource source, std::string_view split) {
   if (source == BenchmarkDatasetSource::kCoco2017 && split == "train2017") {
    if (!reader.WaitEntered(5s)) throw std::runtime_error("same-split reader did not reach its open-file boundary");
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
 mmltk::testsupport::await_test_promise(validation_placed, "validation final metadata while training acquisition and reader remain held", 5s);
 mmltk::testsupport::await_test_promise(labels_preparing, "coalesced active label preparation notification", 5s);
 {
  const std::lock_guard lock(observation_mutex);
  CHECK_FALSE(observed.tracks.pixels.complete);
  CHECK((!observed.tracks.pixels.total_known || observed.tracks.pixels.completed < observed.tracks.pixels.total));
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
  {
   const std::lock_guard lock(observation_mutex);
   pixels_before_release = observed.tracks.pixels.completed;
   observe_subsequent_pixels = true;
  }
  reader.Release();
  mmltk::testsupport::await_test_promise(subsequent_pixels, "new pixels after same-split label entry", 5s);
  CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
  {
   const std::lock_guard lock(observation_mutex);
   CHECK(observed.tracks.labels.active);
   CHECK(observed.tracks.pixels.completed > pixels_before_release);
   CHECK(observed.tracks.labels.completed < observed.tracks.labels.total);
  }
  CHECK(file_bytes(local.output / "train.bin") == train);
  labels.Release();
  mmltk::testsupport::await_test_future(compiling, "production label settlement", 5s);
 }
 CHECK(::flock(lease.get(), LOCK_EX | LOCK_NB) == 0);
 if (cancel_reader)
  check_publication_bytes(local.output, train, validation, manifest);
 else {
  CHECK(benchmark_semantics(local.output / "train.bin") == train_semantics);
  CHECK(benchmark_semantics(local.output / "val.bin") == validation_semantics);
 }
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
  CHECK(CompiledDataset::open(local.output / "train.bin").header().num_images == 4);
 }
 physical.server.ReleasePartial();
 physical.server.Check();
 annotation_server.Check();
}
TEST_CASE("COCONut contradictory annotation geometry remains excluded across pixel cache repair", "[benchmark][coconut][pipeline]") {
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
 const auto expected = benchmark_semantics(config.output_dir / "train.bin");
 const std::array<std::uint32_t, 24> pixels{};
 auto bad_body = png(6, 4, pixels);
 bad_body.resize(41);  // Keep the IDAT header required by stbi_info, but no image data.
 const auto cached = cached_image_path(local.cache.source_images("objects365") / "patch-32", 1);
 mmltk::testsupport::write_text_file(cached, bad_body);
 unsigned repairs = 0;
 config.trace = [&](std::string_view event, std::string_view) {
  if (event == "benchmark.archive.image_ready") ++repairs;
 };
 compile_benchmark_recipe(config, &catalog);
 CHECK(repairs > 0);
 CHECK(std::ranges::all_of(settled.sources, &BenchmarkSourceProgress::complete));
 CHECK(benchmark_semantics(config.output_dir / "train.bin") == expected);
 const auto train = CompiledDataset::open(config.output_dir / "train.bin");
 CHECK(train.header().num_images == 2);
 CHECK_THROWS(compiled_slot(train, 1, AnnotationSource::CoconutObjects365V2));
 const auto manifest = read_json_file(config.output_dir / "benchmark_manifest.json");
 CHECK(std::ranges::any_of(manifest.at("quarantined_images"), [](const auto& image) { return image.at("image_id") == 1 && image.at("reason") == "annotation canvas contradicts the image header"; }));
}
TEST_CASE("production output admission counts only unsettled occupied extents", "[benchmark][coconut][storage]") {
 ScopedTempDir root("output-additional-storage");
 LocalCoconutRecipe local(root.path());
 const auto catalog = local.selected(CoconutValidation::CoconutStock);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock});
 compile_benchmark_recipe(config, &catalog);
 struct stat train{}, validation{};
 REQUIRE(::stat((config.output_dir / "train.bin").c_str(), &train) == 0);
 REQUIRE(::stat((config.output_dir / "val.bin").c_str(), &validation) == 0);
 CHECK(train.st_size > 2 * 1024 * 1024);
 CHECK(static_cast<std::uint64_t>(train.st_blocks + validation.st_blocks) * 512 < 128 * 1024);
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
 const auto expected = benchmark_semantics(local.output / "train.bin");
 const auto manifest = read_json_file(local.output / "benchmark_manifest.json");
 const auto components = manifest.at("recipe").at("components");
 const auto count = components.size();
 REQUIRE(count > 1);
 component_admissions = artifact_admissions = 0;
 config.num_workers = 4;
 compile_benchmark_recipe(config, &catalog);
 CHECK(component_admissions >= count);
 CHECK(artifact_admissions == annotation_ids.size());
 CHECK(benchmark_semantics(local.output / "train.bin") == expected);
 std::filesystem::remove(local.cache.root / components.front().at("imported_index").get<std::string>());
 component_admissions = artifact_admissions = 0;
 compile_benchmark_recipe(config, &catalog);
 CHECK(component_admissions >= count - 1);
 CHECK(artifact_admissions == annotation_ids.size());
 CHECK(benchmark_semantics(local.output / "train.bin") == expected);
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
  compile_benchmark_recipe(config, nullptr, &catalog, {}, {}, {}, {}, {}, [&](BenchmarkDatasetSource source, std::string_view split) {
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
 CHECK(CompiledDataset::open(local.output / "train.bin").header().num_images == 4);
 train.server.Check();
 validation.server.Check();
}
TEST_CASE("COCONut consumes ready releases while an unrelated lifecycle lease is blocked", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires three assigned CPU lanes");
 bool fail_blocked_release = false, cold_ready_release = false;
 SECTION("ready releases retain their source identities") {}
 SECTION("a cold ready release normalizes before the blocked release settles") { cold_ready_release = true; }
 SECTION("an unreadable later release retains healthy independent images") { fail_blocked_release = true; }
 ScopedTempDir root("coconut-release-readiness");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 compile_benchmark_recipe(config, &catalog);
 compile_benchmark_recipe(config, &catalog);
 const auto train = file_bytes(local.output / "train.bin");
 const auto validation = file_bytes(local.output / "val.bin");
 const auto manifest = file_bytes(local.output / "benchmark_manifest.json");
 const auto train_semantics = benchmark_semantics(local.output / "train.bin");
 const auto validation_semantics = benchmark_semantics(local.output / "val.bin");
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
 std::atomic<unsigned> admitted{0};
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
 mmltk::testsupport::await_test_future(compiling, "healthy ready release merge", 5s);
 if (fail_blocked_release) {
  const auto healthy = CompiledDataset::open(local.output / "train.bin");
  CHECK(healthy.header().num_images == 2);
  CHECK_THROWS(compiled_slot(healthy, 7, AnnotationSource::CoconutCoco));
 } else
  CHECK(benchmark_semantics(local.output / "train.bin") == train_semantics);
 CHECK(benchmark_semantics(local.output / "val.bin") == validation_semantics);
 // No completed or failed release retains lifecycle custody after compilation.
 for (const auto& component : catalog.releases) {
  const auto path = local.cache.locks / (std::string(component.name) + ".annotations.lifecycle.lock");
  CHECK(mmltk::testsupport::probe_file_lock(path).available);
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
 const auto [train, validation, manifest] = capture_publication_bytes(local.output);
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
 ScopedTempDir root("coconut-metadata-receiver");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 compile_benchmark_recipe(config, &catalog);
 compile_benchmark_recipe(config, &catalog);
 const auto [train, validation, manifest] = capture_publication_bytes(local.output);
 const auto train_semantics = benchmark_semantics(local.output / "train.bin");
 const auto validation_semantics = benchmark_semantics(local.output / "val.bin");
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
 bool hold_two = false;
 std::size_t descriptors = 0;
 config.num_workers = 3;
 SECTION("one shared CPU") { config.num_workers = 1; }
 SECTION("three shared CPUs") {
  if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires three assigned CPU lanes");
 }
 SECTION("two held mask starts leave later metadata capacity at thirteen descriptors") {
  hold_two = true;
  config.num_workers = 1;
  descriptors = 13;
 }
 CAPTURE(hold_two, descriptors, config.num_workers);
 std::mutex trace_mutex;
 Json release_events = Json::array();
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (!event.starts_with("benchmark.annotations.") && event != "benchmark.archive.image_ready") return;
  const std::lock_guard lock(trace_mutex);
  release_events.push_back({{"event", event}, {"fields", Json::parse(fields)}});
 };
 const auto held_release = [&](CoconutEdition edition) { return edition == blocked || (hold_two && edition == delayed.edition); };
 catalog.release_observer = [&](CoconutEdition edition, CoconutReleaseBoundary boundary) {
  if (held_release(edition) && boundary == CoconutReleaseBoundary::MasksStarted) receipt.ArriveAndWait();
  if (!held_release(edition) && boundary == CoconutReleaseBoundary::MetadataConsumed && ++consumed == catalog.releases.size() - (hold_two ? 2 : 1)) received.set_value();
 };
 std::atomic<bool> cancelled{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog, nullptr, {}, {}, {.descriptors = descriptors}); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  acquisition.reset();
  masks.Release();
 });
 REQUIRE(masks.WaitEntered(5s));
 acquisition.reset();
 if (hold_two) REQUIRE(masks.WaitEntered(5s, 2));
 try {
  mmltk::testsupport::await_test_promise(received, "metadata consumption during independent full-mask work", 5s);
 } catch (const std::exception& error) {
  if (compiling.wait_for(0ms) == std::future_status::ready) compiling.get();
  const std::lock_guard lock(trace_mutex);
  throw std::runtime_error(std::string(error.what()) + ": " + release_events.dump());
 }
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 check_publication_bytes(local.output, train, validation, manifest);
 masks.Release();
 mmltk::testsupport::await_test_future(compiling, "metadata receiver completion", 5s);
 CHECK(benchmark_semantics(local.output / "train.bin") == train_semantics);
 CHECK(benchmark_semantics(local.output / "val.bin") == validation_semantics);
}
TEST_CASE("cold COCONut releases retain one aggregate indexing denominator", "[benchmark][coconut][progress]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires three assigned CPU lanes");
 ScopedTempDir root("coconut-indexing-interleave");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 bool repair_annotations = false;
 SECTION("independent releases normalize before complete placement membership") {}
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
  write_coconut_parquet(path, rows, coconut_parquet_schema(), 1, {}, parquet::WriterProperties::Builder().compression(parquet::Compression::UNCOMPRESSED)->build());
  annotation_payload = file_bytes(path);
  annotation_server = std::make_unique<mmltk::backend::data::testsupport::HttpServer>(annotation_payload);
  artifact.url = annotation_server->url("release-repair");
  artifact.expected_size = annotation_payload.size();
  auto corrupt = mask;
  corrupt.front() = 'x';
  rows.back()["mask"]["bytes"] = corrupt;
  write_coconut_parquet(path, rows, coconut_parquet_schema(), 1, {}, parquet::WriterProperties::Builder().compression(parquet::Compression::UNCOMPRESSED)->build());
  REQUIRE(std::filesystem::file_size(path) == artifact.expected_size);
 }
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock});
 config.num_workers = 3;
 std::uint64_t total = 0;
 for (const auto& release : catalog.releases) total += release.expected_rows;
 REQUIRE(catalog.releases[0].expected_rows != catalog.releases[1].expected_rows);
 const auto base_units = catalog.releases[0].expected_rows;
 mmltk::testsupport::TestGate base("base cold mask import"), last_release("last independent cold mask import");
 const auto base_receipt = base.receipt(), last_receipt = last_release.receipt();
 catalog.release_observer = [&](CoconutEdition edition, CoconutReleaseBoundary boundary) {
  if (edition == CoconutEdition::Base) {
   if (boundary == CoconutReleaseBoundary::MasksStarted) base_receipt.ArriveAndWait();
  } else if (edition == catalog.releases.back().edition && boundary == CoconutReleaseBoundary::MasksStarted)
   last_receipt.ArriveAndWait();
 };
 std::vector<BenchmarkCompileProgress> indexing;
 std::promise<void> independent_settled, all_settled;
 bool independent_reported = false, all_reported = false;
 bool foreground_preserved = true;
 std::optional<BenchmarkCompileProgress> previous_update;
 config.progress = [&](const BenchmarkCompileProgress& value) {
  if (previous_update && previous_update->phase == DatasetCompilePhase::Extracting && value.tracks.labels.completed != previous_update->tracks.labels.completed) {
   foreground_preserved = foreground_preserved && value.phase == previous_update->phase && value.activity == previous_update->activity && value.current_source == previous_update->current_source &&
                          value.completed == previous_update->completed && value.total == previous_update->total && value.activity_elapsed_seconds >= previous_update->activity_elapsed_seconds;
  }
  previous_update = value;
  if (value.tracks.labels.unit != DatasetCompileUnit::Rows || !value.tracks.labels.total_known) return;
  indexing.push_back(value);
  if (value.tracks.labels.completed == total - base_units && !independent_reported) {
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
 });
 REQUIRE(base.WaitEntered(5s));
 REQUIRE(last_release.WaitEntered(5s));
 last_release.Release();
 mmltk::testsupport::await_test_promise(independent_settled, "independent cold releases indexed", 5s);
 CHECK_FALSE(std::filesystem::exists(local.output / "train.bin"));
 base.Release();
 mmltk::testsupport::await_test_promise(all_settled, "aggregate indexing settlement", 5s);
 mmltk::testsupport::await_test_future(compiling, "interleaved indexing compile", 5s);
 REQUIRE_FALSE(indexing.empty());
 CHECK(foreground_preserved);
 CHECK(indexing.front().tracks.labels.completed == 0);
 CHECK(indexing.back().tracks.labels.completed == total);
 std::uint64_t previous = 0;
 for (const auto& value : indexing) {
  CHECK(value.tracks.labels.total == total);
  CHECK(value.tracks.valid());
  if (value.tracks.labels.completed < previous) {
   CHECK(repair_annotations);
   CHECK(previous - value.tracks.labels.completed == 64);
   CHECK(value.tracks.labels.invalidated == 64);
   // Other releases retain their rows across this withdrawal. Component jobs
   // settle the track lifecycle independently of the measured row count.
   CHECK(value.tracks.labels.completed >= total - base_units);
   CHECK(value.tracks.labels.completed <= value.tracks.labels.total - base_units);
   CHECK(value.tracks.labels.active);
   CHECK(value.tracks.labels.activity == DatasetCompileActivity::Preparing);
  }
  CHECK(value.tracks.labels.completed <= total);
  previous = value.tracks.labels.completed;
 }
 CHECK(indexing.back().tracks.labels.invalidated == (repair_annotations ? 64 : 0));
 if (annotation_server) {
  CHECK(annotation_server->requests() == 1);
  annotation_server->Check();
 }
 CHECK(CompiledDataset::open(local.output / "val.bin").header().num_images == 1);
}
TEST_CASE("cold benchmark readers finish oversized grants with retained release control on one CPU", "[benchmark][coconut][pipeline]") {
 ScopedTempDir root("cold-shared-readers");
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline execution(
  1, {}, {.transient_bytes = 65536, .descriptors = 32}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), mmltk::backend::data::testsupport::benchmark_trace());
 auto release = execution.execution().reserve(CompileResources::handles(1, true, 4));
 const std::array<std::uint32_t, 1> ids{1};
 const auto mask = png(1, 1, ids);
 const std::array physical{coco(7), objects(1)};
 auto input = request(physical);
 input.execution = &execution;
 input.trace = mmltk::backend::data::testsupport::benchmark_trace();
 input.parent_allowance = release;
 SECTION("Parquet retains its complete batch and consumer grant") {
  const auto path = root.path() / "input.parquet";
  write_coconut_parquet(path, Json::array({hf_row(7, mask, Json::array({segment()}), 1, 1)}));
  input.parquet_shards = {path};
 }
 SECTION("raw masks retain their archive and consumer grant") {
  input.edition = CoconutEdition::XLarge;
  input.mask_archive = root.path() / "input.tar";
  const std::array<std::pair<std::string, std::string>, 2> entries{
   {{"coconuts_xlarge/panseg_info/objects365_v2_00000001.json", Json::array({segment()}).dump()}, {"coconuts_xlarge/panseg/objects365_v2_00000001.png", mask}}
  };
  tar(input.mask_archive, entries);
 }
 SECTION("gzip extraction retains source and directory control") {
  const auto path = root.path() / "images.tar.gz";
  const std::array<std::pair<std::string, std::string>, 1> entries{{{"train2017/000000000007.jpg", mask}}};
  tar(path, entries);
  const std::array<std::uint64_t, 1> selected{7};
  auto work = std::async(std::launch::async, [&] {
   return extract_selected_archive_images(
    {.archive_path = path,
     .source_identity = "cold-gzip",
     .output_root = root.path() / "images",
     .source = "coco",
     .shard = "train2017",
     .selected_image_ids = selected,
     .image_id_parser = [](std::string_view) { return std::optional<std::uint64_t>{7}; },
     .execution = &execution,
     .parent_allowance = release});
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
 CHECK(execution.execution().try_reserve({65537, 0, true}).has_value());
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
 mmltk::testsupport::TestGate selection("metadata finished before archive selection"), prefetch("prefetch owns its source envelope"), denied("independent source has no admission"),
  independent("Objects365 pixel before COCO prefetch settlement"), archive_selection("archive controller before source choice"), pixel("last Open Images reader owns group custody"),
  source_returned("Open Images acquisition has returned"), waiting("archive controller registered its admission wait"), stopped("archive admission wait stopped");
 struct AdmissionObservation {
  const std::atomic<bool>& cancelled_flag;
  mmltk::testsupport::TestGate& waiting;
  struct Local {
   mmltk::testsupport::TestGate* next = nullptr;
   bool armed = false;
   unsigned polls = 0;
  };
  static Local& local() {
   thread_local Local value;
   return value;
  }
  bool cancelled() const noexcept {
   auto& state = local();
   // Trace delivery is serialized. Hold the source at its next cancellation
   // boundary after that callback returns, leaving other sources observable.
   if (auto* gate = std::exchange(state.next, nullptr)) gate->receipt().ArriveAndWait();
   // The controller checks cancellation before entering admission, then its
   // predicate checks again. The third follows an actual condition wait.
   // Selection resets this on an intervening retry.
   if (state.armed && ++state.polls == 3) waiting.receipt().ArriveAndWait();
   return cancelled_flag.load();
  }
 } observation{cancelled, waiting};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Borrow(observation);
 const auto selecting = [&] {
  if (first_selection.exchange(false)) selection.receipt().ArriveAndWait();
 };
 std::mutex admission_mutex;
 std::string admission_snapshot;
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.archive.prefetched") {
   const auto path = std::filesystem::path(Json::parse(fields).at("path").get<std::string>());
   if (path == local.cache.source_downloads("coco") / catalog.coco_train_images.filename || path == local.cache.source_downloads("coco") / catalog.coco_val_images.filename)
    AdmissionObservation::local().next = &prefetch;
  }
  if (event == "benchmark.archive.selection") {
   AdmissionObservation::local() = {};
   if (release_capacity) AdmissionObservation::local().next = &archive_selection;
  }
  if ((pressure || release_capacity) && event == "benchmark.archive.admission_wait") {
   {
    const std::lock_guard lock(admission_mutex);
    admission_snapshot = fields;
   }
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
  compile_benchmark_recipe(config, nullptr, &catalog, {},
   [&](const std::filesystem::path& source, std::uint64_t) {
   if (!pressure && source == objects_root) independent.receipt().ArriveAndWait();
   if (release_capacity && source == open_images_root) pixel.receipt().ArriveAndWait();
  },
   // The Open Images directory and two-descriptor group need a producer
   // ceiling of 20 to start beside Curl, two prefetched archives and outputs.
   // At limit 25 its retained group prevents another five-descriptor archive
   // fitting the archive ceiling of 23 until the last pixel retires the group.
   {.descriptors = release_capacity ? 25U
                   : pressure       ? 24U
                                    : 64U},
   {}, selecting, [&](BenchmarkDatasetSource source, std::string_view) {
   if (release_capacity && source == BenchmarkDatasetSource::kOpenImagesV7) {
    source_returned.receipt().ArriveAndWait();
    if (fail_wait) throw std::runtime_error("Open Images acquisition owner failed");
   }
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  selection.Release();
  prefetch.Release();
  denied.Release();
  independent.Release();
  archive_selection.Release();
  pixel.Release();
  source_returned.Release();
  waiting.Release();
  stopped.Release();
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
  if (cancel_wait || fail_wait)
   REQUIRE(stopped.WaitEntered(5s));
  else {
   pixel.Release();
   // No prefetch/readiness publisher runs between the denial and this open.
   // The last reader's returned credit alone must make selection progress.
   const bool entered = independent.WaitEntered(5s);
   std::string admission;
   {
    const std::lock_guard lock(admission_mutex);
    admission = admission_snapshot;
   }
   CAPTURE(admission);
   REQUIRE(entered);
  }
 } else if (pressure)
  REQUIRE(denied.WaitEntered(5s));
 else
  REQUIRE(independent.WaitEntered(5s));
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 denied.Release();
 independent.Release();
 pixel.Release();
 source_returned.Release();
 stopped.Release();
 prefetch.Release();
 if (cancel_wait)
  CHECK_THROWS(mmltk::testsupport::await_test_future(work, "cancelled source admission", 5s));
 else if (fail_wait)
  CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(work, "failed source admission", 5s), "Open Images acquisition owner failed");
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
 auto preparation = execution.execution().reserve(CompileResources::handles(0, true, custom_annotation_resources().descriptors + custom_annotation_resources().continuation_descriptors));
 auto train = execution.execution().reserve(CompileResources::handles(1, true, 4));
 auto validation = execution.execution().reserve(CompileResources::handles(1, true, 4));
 auto train_lease = ArtifactLease::acquire_charged(local.cache.locks / "coco-train2017.images.lock", cancellation, train);
 auto validation_lease = ArtifactLease::acquire_charged(local.cache.locks / "coco-val2017.images.lock", cancellation, validation);
 const auto source = custom_annotation_resources();
 const auto held = execution.execution().resource_usage();
 const auto competing_descriptors = execution.execution().descriptor_ceiling(source) - held.descriptors - held.continuation_descriptors - source.descriptors - source.continuation_descriptors + 1;
 // Leave one descriptor less than a new metadata continuation needs, without
 // borrowing either the prerequisite's promise or either archive's promise.
 auto competing = execution.execution().reserve(CompileResources::handles(competing_descriptors));
 CHECK_FALSE(execution.execution().try_reserve(source).has_value());
 BenchmarkTraceSink trace;
 CHECK(download_artifacts({make_download_request(local.cache, "coco", catalog.coco_train_images)}, 1, cancellation, {}, trace, {}, &execution, train).size() == 1);
 CHECK(download_artifacts({make_download_request(local.cache, "coco", catalog.coco_val_images)}, 1, cancellation, {}, trace, {}, &execution, validation).size() == 1);
 ProgressReporter progress({}, trace);
 const auto config = local.compiler_config({BenchmarkDatasetVariant::CocoCustom, CoconutValidation::Stock});
 auto work = std::async(std::launch::async, [&] { return prepare_custom_recipe(config, local.cache, catalog, progress, 1, cancellation, trace, {}, &execution, std::move(preparation)); });
 const mmltk::testsupport::ScopedTestCleanup cancel([&] { cancelled.store(true); });
 const auto result = mmltk::testsupport::await_test_future(work, "metadata beside retained prefetch commitments");
 REQUIRE(result.coco_train);
 REQUIRE(result.coco_val);
 CHECK(result.coco_train->image_count() == 2);
 CHECK(result.coco_val->image_count() == 1);
 CHECK(execution.execution().try_reserve(source).has_value());
 // Both source owners still hold their lease and can use their own promise.
 CHECK(execution.execution().try_reserve({0, 4, true}, train).has_value());
 CHECK(execution.execution().try_reserve({0, 4, true}, validation).has_value());
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
 auto preparation = execution.execution().reserve(CompileResources::handles(0, true, coco_annotation_resources().descriptors + coco_annotation_resources().continuation_descriptors));
 std::vector<CoconutPhysicalImage> physical;
 std::vector<AdmittedRecipeArchive> admitted;
 for (const auto& archive : catalog.images) {
  const auto path = local.cache.source_downloads(benchmark_source_name(archive.artifact.source)) / archive.artifact.filename;
  auto inventory = execution.execution().reserve(CompileResources::handles(1, true, 3), preparation);
  auto rows = fixture_physical_inventory(path, {}, archive.source, archive.shard, archive.artifact.artifact_id, cancellation, &execution.storage(), &execution, inventory);
  physical.insert(physical.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
  admitted.push_back({archive, {.path = path, .size = archive.artifact.expected_size, .identity = archive.artifact.artifact_id}});
 }
 const CoconutPhysicalMembership membership(physical);
 mmltk::testsupport::TestGate release_acquired("release holds its source allowance before activation");
 std::promise<void> originals_admitted;
 BenchmarkTraceSink trace = [&](std::string_view event, const Json& fields) {
  if (event == "benchmark.download.preseeded" && fields.at("artifact") == catalog.releases.front().annotations.front().artifact_id) release_acquired.receipt().ArriveAndWait();
  if (event == "benchmark.annotations.originals_begin") originals_admitted.set_value();
 };
 ProgressReporter progress({}, trace);
 CoconutFailureReport failures(local.cache.root, progress);
 std::vector<CompileAllowance> competing;
 auto inputs = acquire_coconut_recipe_inputs(local.cache, catalog, progress, 1, cancellation, trace, 1, {}, {}, &execution);
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  release_acquired.Release();
 });
 REQUIRE(release_acquired.WaitEntered(2s));
 // Fill only uncommitted producer capacity after the real release owns its
 // complete continuation. Originals must use the preparation promise already
 // retained by the caller, regardless of the release's descriptor demand.
 while (auto available = execution.execution().try_reserve(CompileResources::handles(1, true))) competing.push_back(std::move(*available));
 CHECK_FALSE(execution.execution().try_reserve(coco_annotation_resources()).has_value());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Stock});
 auto work = std::async(std::launch::async,
  [&] { return prepare_coconut_recipe(config, local.cache, catalog, admitted, membership, progress, failures, 1, cancellation, trace, inputs, &execution, std::move(preparation)); });
 const mmltk::testsupport::ScopedTestCleanup release_before_join([&] {
  cancelled.store(true);
  release_acquired.Release();
 });
 mmltk::testsupport::await_test_promise(originals_admitted, "originals draw their explicit preparation commitment");
 release_acquired.Release();
 const auto result = mmltk::testsupport::await_test_future(work, "release activation after prerequisite settlement", 5s);
 CHECK(result.components.size() == 2);
 REQUIRE(result.stock_validation);
 CHECK(result.stock_validation->images.size() == 1);
 competing.clear();
 CHECK(execution.execution().try_reserve({0, 32, true}).has_value());
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
 auto [split, encoded] = cached_mask_pixel_fixture(images);
 BenchmarkWriteRequest pixels{.split = split, .output_path = root.path() / "result.bin", .resolution = 1, .num_workers = 1, .execution = &execution};
 BenchmarkSplitWriter writer(pixels);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 // Measure yielding to ready CPU work; asynchronous input/output preparation
 // has its own scheduling and must not race the parser's bounded checkpoint.
 auto pixel = mmltk::backend::data::testsupport::prepare_benchmark_pixel(execution, writer);
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
   try {
    (void)execution.execution().current_lane();
   } catch (...) { return false; }
   if (++polls == 1) entered.receipt().ArriveAndWait();
   if (polls == 128) later.receipt().ArriveAndWait();
   return false;
  }
 } observed{execution, entered, later};
 input.cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(observed);
 auto work = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  entered.Release();
  later.Release();
 });
 REQUIRE(entered.WaitEntered(std::chrono::seconds(2)));
 auto pixel_completion = execution.execution().submit(std::move(pixel));
 entered.Release();
 REQUIRE(later.WaitEntered(std::chrono::seconds(2)));
 CHECK(writer.image_complete(0));
 later.Release();
 const auto result = mmltk::testsupport::await_test_future(work, "shared JSON source pass");
 pixel_completion.wait();
 REQUIRE(result.size() == 1);
 CHECK(result.front().index().image_count() == 256);
}
TEST_CASE("custom annotation sources preserve independent HTTP and lifecycle admission", "[benchmark][pipeline]") {
 using namespace std::chrono_literals;
 bool download = false;
 SECTION("custom annotation sources acquire lifecycle custody independently on one CPU") {}
 SECTION("sixteen-descriptor annotation admission protects HTTP before independent lifecycle locks") { download = true; }
 ScopedTempDir root("custom-independent-lifecycle");
 LocalCoconutRecipe local(root.path());
 auto catalog = local_custom_catalog(local);
 const auto raw = local.cache.source_downloads("coco") / catalog.coco_annotations.filename;
 std::string payload;
 std::optional<mmltk::backend::data::testsupport::HttpServer> server;
 if (download) {
  payload = file_bytes(raw);
  server.emplace(payload);
  catalog.coco_annotations.url = server->url("annotations");
  catalog.coco_annotations.expected_size = payload.size();
  std::filesystem::remove(raw);
  std::filesystem::remove(raw.string() + ".download.json");
  remove_normalized_annotation_index(local.cache.source_indexes("coco") / "val2017.normalized.bin");
 }
 auto blocked = ArtifactLease::acquire(local.cache.locks / (download ? "objects365-annotations.lifecycle.lock" : "coco-annotations.lifecycle.lock"), {});
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 const std::size_t descriptors = download ? 16 : 24;
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = descriptors}, cancellation);
 std::promise<void> independent;
 std::atomic<unsigned> completed{0};
 const BenchmarkTraceSink trace = [&](std::string_view event, const Json& fields) {
  if (download) {
   if (event == "benchmark.download.complete" && fields.at("artifact") == catalog.coco_annotations.artifact_id) independent.set_value();
  } else if (event == "benchmark.annotations.cache_hit" && fields.at("source") != "coco" && completed.fetch_add(1) == 1)
   independent.set_value();
 };
 ProgressReporter progress({}, trace);
 const auto config = local.compiler_config({BenchmarkDatasetVariant::CocoCustom, CoconutValidation::Stock});
 auto work = std::async(std::launch::async, [&] { return prepare_custom_recipe(config, local.cache, catalog, progress, 1, cancellation, trace, {}, &execution); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  blocked = {};
 });
 const auto deadline = download ? 5s : 2s;
 mmltk::testsupport::await_test_promise(independent, download ? "COCO HTTP while independent Objects365 lifecycle is contended" : "supplemental metadata while COCO lifecycle is locked", deadline);
 if (download) CHECK(file_bytes(raw) == payload);
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 blocked = {};
 const auto result = mmltk::testsupport::await_test_future(work, "independent custom metadata settlement", deadline);
 REQUIRE(result.coco_train);
 REQUIRE(result.coco_val);
 REQUIRE(result.objects);
 REQUIRE(result.open_images);
 CHECK(execution.execution().try_reserve({0, descriptors}).has_value());
 if (download) {
  CHECK(result.coco_val->image_count() == 1);
  CHECK(read_json_file((local.cache.source_indexes("coco") / "val2017.normalized.bin").string() + ".complete.json").at("complete") == true);
  const auto warm = prepare_custom_recipe(config, local.cache, catalog, progress, 1, cancellation, {}, {}, &execution);
  CHECK(warm.coco_indexes_cache_hit);
  CHECK(server->requests() == 1);
  CHECK(execution.execution().try_reserve({0, descriptors}).has_value());
  server->Check();
 }
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
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  blocked = {};
  inputs.reset();
 });
 mmltk::testsupport::await_test_promise(independent, "second annotation while first artifact is locked");
 CHECK(file_bytes(local.cache.source_downloads("coconut-" + std::string(release.name)) / artifact.filename) == payload);
 cancelled.store(true);
 blocked = {};
 inputs.reset();
 cancelled.store(false);
 CHECK(execution.execution().try_reserve({0, descriptors}).has_value());
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
 DeferredCancellationGate observation{cancelled};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Borrow(observation);
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.archive.prefetched") {
   const auto path = std::filesystem::path(Json::parse(fields).at("path").get<std::string>());
   if (path == local.cache.source_downloads("coco") / catalog.coco_train_images.filename) DeferredCancellationGate::next() = &train;
   if (path == local.cache.source_downloads("coco") / catalog.coco_val_images.filename) DeferredCancellationGate::next() = &validation;
  }
  if (event == "benchmark.archive.admission_wait" && !observed_wait.exchange(true)) DeferredCancellationGate::next() = &waiting;
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
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  train.Release();
  validation.Release();
  waiting.Release();
  blocked = {};
 });
 REQUIRE(train.WaitEntered(5s));
 REQUIRE(validation.WaitEntered(5s));
 REQUIRE(waiting.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(independent_ready, "independent Open Images beside contended archive", 5s);
 // A formerly pregranted blocked Objects365 source could occupy this sole
 // controller. The ready validation archive must now receive its turn.
 validation.Release();
 waiting.Release();
 mmltk::testsupport::await_test_promise(validation_ready, "ready validation archive while Objects365 remains locked", 5s);
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 if (cancel_wait) cancelled.store(true);
 train.Release();
 if (cancel_wait)
  CHECK_THROWS(mmltk::testsupport::await_test_future(work, "cancelled archive selection custody", 5s));
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
 for (const auto& name : {"coco-train2017.images.lock", "coco-val2017.images.lock"}) CHECK(ArtifactLease::try_acquire_charged(local.cache.locks / name, {}, nullptr));
}
TEST_CASE("archive transport seeks only consumed generations across supported formats", "[benchmark][coconut][archive]") {
 ScopedTempDir root("archive-consumption");
 int format = ARCHIVE_FORMAT_TAR_USTAR;
 bool gzip = false;
 std::string name = "payload/first.bin";
 SECTION("raw tar") {}
 SECTION("gzip tar") { gzip = true; }
 SECTION("ZIP") { format = ARCHIVE_FORMAT_ZIP; }
 SECTION("PAX extended names") {
  format = ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE;
  name = "payload/" + std::string(140, 'p') + ".bin";
 }
 SECTION("GNU extended names") {
  format = ARCHIVE_FORMAT_TAR_GNUTAR;
  name = "payload/" + std::string(140, 'g') + ".bin";
 }
 const auto path = root.path() / (gzip ? "source.tar.gz" : "source.archive");
 const std::array<std::pair<std::string, std::string>, 2> members{{{name, "first generation"}, {"payload/second.bin", "second member"}}};
 {
  std::unique_ptr<archive, decltype(&archive_write_free)> output(archive_write_new(), archive_write_free);
  REQUIRE(archive_write_set_format(output.get(), format) == ARCHIVE_OK);
  if (gzip) REQUIRE(archive_write_add_filter_gzip(output.get()) == ARCHIVE_OK);
  REQUIRE(archive_write_open_filename(output.get(), path.c_str()) == ARCHIVE_OK);
  for (const auto& [member, bytes] : members) {
   std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
   archive_entry_set_pathname(entry.get(), member.c_str());
   archive_entry_set_filetype(entry.get(), AE_IFREG);
   archive_entry_set_perm(entry.get(), 0600);
   archive_entry_set_size(entry.get(), bytes.size());
   REQUIRE(archive_write_header(output.get(), entry.get()) == ARCHIVE_OK);
   REQUIRE(archive_write_data(output.get(), bytes.data(), bytes.size()) == static_cast<la_ssize_t>(bytes.size()));
  }
  REQUIRE(archive_write_close(output.get()) == ARCHIVE_OK);
 }
 BenchmarkArchive input(path);
 const auto suspend_readmitted_input = [&] {
  input.suspend();
  input.suspend();
  CHECK(input.retained_workspace_bytes() == 0);
  REQUIRE(input.try_resume(0));
 };
 // Before-first-header suspension is distinct from a saved member. Repeated
 // teardown and decoder admission must retain that initial forward position.
 suspend_readmitted_input();
 input.suspend();
 REQUIRE(input.next());
 CHECK(input.member() == name);
 auto first = input.read(1024);
 CHECK(std::string(first.begin(), first.end()) == members[0].second);
 CHECK(input.observation().read_turns == 0);
 CHECK(input.observation().body_cpu_ns == 0);
 const auto first_position = input.member_position();
 {
  BenchmarkArchive resumed(path);
  REQUIRE(resumed.seek(first_position));
  const auto reopened = resumed.read(1024);
  CHECK(std::string(reopened.begin(), reopened.end()) == members[0].second);
 }
 REQUIRE(input.next());
 CHECK(input.member() == members[1].first);
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
 suspend_readmitted_input();
 // Admission itself does not consume the saved member; resuspending before a
 // read must keep its metadata even though the fresh decoder starts at zero.
 input.suspend();
 const auto resumed_second = input.read(1024);
 CHECK(std::string(resumed_second.begin(), resumed_second.end()) == members[1].second);
 REQUIRE(input.seek(first_position));
 input.suspend();
 input.suspend();
 first = input.read(1024);
 CHECK(std::string(first.begin(), first.end()) == members[0].second);
 CHECK_FALSE(input.seek("missing.bin"));
 input.suspend();
 REQUIRE(input.next());
 CHECK(input.member() == members[1].first);
 CHECK(input.position() == 1);
 input.pause();
 const std::array required{members[1].first, members[0].first};
 std::vector<std::size_t> order;
 input.visit_known(required, [&](std::size_t index) {
  order.push_back(index);
  const auto actual = input.read(1024);
  CHECK(std::string(actual.begin(), actual.end()) == members[1 - index].second);
 });
 CHECK(order == std::vector<std::size_t>{1, 0});
 input.suspend();
 input.suspend();
 CHECK_FALSE(input.next());
}
TEST_CASE("archive replay amortizes skipped headers across bounded CPU work", "[benchmark][coconut][archive][resources]") {
 using namespace mmltk::backend::data::detail;
 ScopedTempDir root("archive-replay-work");
 int format = ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE;
 bool gzip = false;
 SECTION("extended tar") {}
 SECTION("GNU extended names") { format = ARCHIVE_FORMAT_TAR_GNUTAR; }
 SECTION("ZIP") { format = ARCHIVE_FORMAT_ZIP; }
 SECTION("compressed extended tar") { gzip = true; }
 constexpr std::size_t count = 256;
 std::vector<std::pair<std::string, std::string>> members;
 for (std::size_t i = 0; i < count; ++i) members.emplace_back(std::string(140, 'p') + "/" + std::to_string(i) + ".bin", "payload " + std::to_string(i));
 const auto path = root.path() / (gzip ? "source.tar.gz" : "source.archive");
 tar(path, members, false, format);
 CompileExecution execution(1, {}, {.transient_bytes = 8U << 20}, {}, [](auto, const auto&) {});
 BenchmarkArchive input(path, &execution);
 std::size_t discovered = 0;
 while (input.next()) ++discovered;
 REQUIRE(discovered == count);
 const auto calls = [&] { return execution.snapshot().task_completed[static_cast<std::size_t>(CompileTaskKind::Archive)]; };
 input.pause();
 const auto before = calls();
 REQUIRE(input.seek(members.back().first));
 const auto last = input.read(1024);
 CHECK(std::string(last.begin(), last.end()) == members.back().second);
 input.suspend();
 CHECK_FALSE(input.next());
 const auto replay_calls = calls() - before;
 CAPTURE(format, gzip, replay_calls);
 CHECK(replay_calls < count / 4);
 input.pause();
 const std::array required{members.back().first, members[count / 2].first};
 const auto batch_start = calls();
 std::vector<std::size_t> order;
 input.visit_known(required, [&](std::size_t index) {
  order.push_back(index);
  const auto actual = input.read(1024);
  CHECK(std::string(actual.begin(), actual.end()) == members[index ? count / 2 : count - 1].second);
  input.suspend();
 });
 CHECK(order == std::vector<std::size_t>{1, 0});
 CHECK(calls() - batch_start < count / 4);
}
TEST_CASE("requested archive extraction closes before unsafe unused members", "[benchmark][coconut][archive]") {
 ScopedTempDir root("archive-unused-tail");
 const auto path = root.path() / "images.tar";
 const std::array<std::uint32_t, 1> pixels{0x001122};
 const auto encoded = png(1, 1, pixels);
 const std::array<std::pair<std::string, std::string>, 2> members{{{"images/1.jpg", encoded}, {"../unused.jpg", "unsafe unused bytes"}}};
 tar(path, members);
 const std::array<std::uint64_t, 1> ids{1};
 const auto result = extract_selected_archive_images({
  .archive_path = path,
  .source_identity = "consumed-only",
  .output_root = root.path() / "images",
  .source = "coco",
  .shard = "train2017",
  .selected_image_ids = ids,
  .image_id_parser = [](std::string_view name) -> std::optional<std::uint64_t> { return name == "images/1.jpg" ? std::optional<std::uint64_t>{1} : std::nullopt; },
 });
 CHECK(result.image_count == 1);
 BenchmarkArchive reader(path);
 REQUIRE(reader.next());
 (void)reader.read(1024);
 REQUIRE(reader.next());
 CHECK_THROWS(reader.read(1024));
}
TEST_CASE("Parquet namespace hints preserve recognized routes and unknown input", "[benchmark][coconut][parquet]") {
 ScopedTempDir root("parquet-hints");
 const std::array<std::uint32_t, 1> pixels{0};
 const auto encoded = png(1, 1, pixels);
 const auto path = root.path() / "hints.parquet";
 write_coconut_parquet(
  path, Json::array({hf_row(1, encoded, Json::array(), 1, 1, "unlabeled2017"), hf_row(2, encoded, Json::array(), 1, 1, "unknown"), hf_row(3, encoded, Json::array(), 1, 1, "val2017")}));
 std::vector<std::optional<CoconutImageNamespace>> hints;
 const std::array paths{path};
 read_coconut_parquet(paths, {}, {}, [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput&) { hints.push_back(record.namespace_hint); }, true);
 REQUIRE(hints.size() == 3);
 CHECK(hints[0] == CoconutImageNamespace::CocoUnlabeled);
 CHECK_FALSE(hints[1]);
 CHECK(hints[2] == CoconutImageNamespace::CocoValidation);
}
TEST_CASE("archive consumed-member checks retain sparse payloads and reject required conflicts", "[benchmark][coconut][archive]") {
 ScopedTempDir root("archive-member-boundaries");
 const auto path = root.path() / "members.tar";
 SECTION("PAX sparse payloads retain logical holes through a late reread") {
  std::string bytes((2U << 20) + 8192, '\0');
  bytes.replace(0, 4, "head");
  bytes.replace(bytes.size() - 4, 4, "tail");
  std::unique_ptr<archive, decltype(&archive_write_free)> output(archive_write_new(), archive_write_free);
  REQUIRE(archive_write_set_format_pax(output.get()) == ARCHIVE_OK);
  REQUIRE(archive_write_open_filename(output.get(), path.c_str()) == ARCHIVE_OK);
  std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
  archive_entry_set_pathname(entry.get(), "sparse.bin");
  archive_entry_set_filetype(entry.get(), AE_IFREG);
  archive_entry_set_perm(entry.get(), 0600);
  archive_entry_set_size(entry.get(), bytes.size());
  archive_entry_sparse_add_entry(entry.get(), 0, 4);
  archive_entry_sparse_add_entry(entry.get(), bytes.size() - 4, 4);
  REQUIRE(archive_write_header(output.get(), entry.get()) == ARCHIVE_OK);
  REQUIRE(archive_write_data(output.get(), bytes.data(), bytes.size()) == static_cast<la_ssize_t>(bytes.size()));
  REQUIRE(archive_write_close(output.get()) == ARCHIVE_OK);
  BenchmarkArchive input(path);
  REQUIRE(input.next());
  auto actual = input.read(bytes.size());
  CHECK(std::string(actual.begin(), actual.end()) == bytes);
  input.pause();
  input.resume(bytes.size());
  REQUIRE(input.seek("sparse.bin"));
  actual = input.read(bytes.size());
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
  tar(path, members);
  BenchmarkArchive input(path);
  REQUIRE(input.next());
  (void)input.read(1024);
  REQUIRE(input.next());
  CHECK_THROWS_AS(input.read(1024), BenchmarkArchiveError);
  CHECK_THROWS_AS(input.seek("same.bin"), BenchmarkArchiveError);
  const std::array required{std::string("same.bin")};
  CHECK_THROWS_AS(input.visit_known(required, [](std::size_t) {}), BenchmarkArchiveError);
 }
 SECTION("missing requests resolve only after the eligible stream ends") {
  const std::array<std::pair<std::string, std::string>, 1> members{{{"other.bin", "other"}}};
  tar(path, members);
  BenchmarkArchive input(path);
  REQUIRE(input.next());
  CHECK_FALSE(input.seek("missing.bin"));
  CHECK_FALSE(input.next());
  CHECK_FALSE(input.seek("missing.bin"));
  const std::array required{std::string("missing.bin")};
  CHECK_THROWS_AS(input.visit_known(required, [](std::size_t) {}), BenchmarkArchiveError);
 }
 SECTION("a consumed symbolic link is rejected") {
  std::unique_ptr<archive, decltype(&archive_write_free)> output(archive_write_new(), archive_write_free);
  REQUIRE(archive_write_set_format_pax(output.get()) == ARCHIVE_OK);
  REQUIRE(archive_write_open_filename(output.get(), path.c_str()) == ARCHIVE_OK);
  std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
  archive_entry_set_pathname(entry.get(), "image.jpg");
  archive_entry_set_filetype(entry.get(), AE_IFLNK);
  archive_entry_set_symlink(entry.get(), "../outside.jpg");
  archive_entry_set_size(entry.get(), 0);
  REQUIRE(archive_write_header(output.get(), entry.get()) == ARCHIVE_OK);
  REQUIRE(archive_write_close(output.get()) == ARCHIVE_OK);
  BenchmarkArchive input(path);
  REQUIRE(input.next());
  CHECK_THROWS_AS(input.read(1024), BenchmarkArchiveError);
 }
}
TEST_CASE("COCONut physical consumers keep healthy members around malformed archive entries", "[benchmark][coconut][archive]") {
 ScopedTempDir root("physical-request-routing");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::Coconut);
 auto& physical = *std::ranges::find(catalog.images, CoconutImageNamespace::Objects365V2, &RecipeImageArchive::source);
 const auto path = local.cache.source_downloads("objects365") / physical.artifact.filename;
 // Healthy members become useful before the malformed tail is rejected by
 // the independent physical consumer.
 const std::array<std::pair<std::string, std::string>, 3> members{{{objects(2).member, white_jpeg()}, {objects(1).member, white_jpeg()}, {"unused/000000000099.jpg", "unused"}}};
 tar(path, members);
 physical.artifact.expected_size = std::filesystem::file_size(path);
 auto& base = *std::ranges::find(catalog.releases, CoconutEdition::Base, &CoconutReleaseComponent::edition);
 auto& artifact = base.annotations.front();
 const auto parquet = local.cache.source_downloads("coconut-" + std::string(base.name)) / artifact.filename;
 const std::array<std::uint32_t, 9> empty{};
 // An incorrect recognized hint and an unknown hint both retain identity until
 // a real eligible source supplies the requested member.
 write_coconut_parquet(parquet, Json::array({hf_row(7, png(3, 3, empty), Json::array(), 3, 3, "unlabeled2017"), hf_row(8, png(3, 3, empty), Json::array(), 3, 3, "unknown")}));
 artifact.expected_size = std::filesystem::file_size(parquet);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Coconut}, true);
 compile_benchmark_recipe(config, &catalog);
 CHECK(CompiledDataset::open(config.output_dir / "train.bin").header().num_images == 4);
 // The sealed forward inventory and cached image payloads retain their
 // physical generation during warm admission.
 const auto image = cached_image_path(local.cache.source_images("objects365") / "patch-32", 1);
 struct stat before{}, after{};
 REQUIRE(::stat(image.c_str(), &before) == 0);
 compile_benchmark_recipe(config, &catalog);
 REQUIRE(::stat(image.c_str(), &after) == 0);
 CHECK(before.st_ino == after.st_ino);
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
 config.trace = [&](std::string_view event, std::string_view) {
  if (event == "benchmark.download.failure_sha256") ++repairs;
 };
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
 ArchiveExtractionRequest request{
  .archive_path = path,
  .source_identity = "payload-generation",
  .output_root = root.path() / "images",
  .source = "coco",
  .shard = "train2017",
  .selected_image_ids = ids,
  .image_id_parser = [](std::string_view name) -> std::optional<std::uint64_t> { return name == "images/1.jpg" ? std::optional<std::uint64_t>{1} : std::nullopt; },
 };
 request.image_ready = [&](const CachedImageReady& ready) {
  retained = ready.payload;
  if (fail) throw std::runtime_error("retire acquisition");
 };
 if (fail)
  CHECK_THROWS(extract_selected_archive_images(request));
 else
  (void)extract_selected_archive_images(request);
 REQUIRE(retained);
 REQUIRE(retained->backing());
 CHECK(std::string(retained->encoded().begin(), retained->encoded().end()) == encoded);
 std::weak_ptr<const void> backing = retained->backing();
 retained.reset();
 CHECK(backing.expired());
}
TEST_CASE("independent physical consumers and labels progress beside Curl at low descriptor limits", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 // Two BIN writers coexist with Curl's five-descriptor first request, the
 // download's lock/partial/checkpoint trio, and a five-descriptor image source.
 const auto descriptors = GENERATE(std::size_t{15}, std::size_t{16}, std::size_t{24}, std::size_t{26});
 CAPTURE(descriptors);
 ScopedTempDir root("physical-consumer-envelope");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Stock}, true);
 auto catalog = local.selected(CoconutValidation::Stock);
 ServedPhysicalArchive served(local.cache, catalog, CoconutImageNamespace::CocoTrain, "held-train-images");
 std::filesystem::remove(served.path);
 served.server.GateNextRequest();
 std::atomic<bool> cancelled{false}, pixels_done{false}, labels_done{false};
 std::promise<void> pixels, labels;
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (std::getenv("MMLTK_TEST_BENCHMARK_TRACE") && event == "benchmark.resource.wait") std::fprintf(stderr, "%s\n", Json{{"event", event}, {"payload", Json::parse(fields)}}.dump().c_str());
  if (event == "benchmark.archive.image_ready" && Json::parse(fields).at("image_id") == 8 && !pixels_done.exchange(true)) pixels.set_value();
  if (event == "benchmark.labels.placed" && Json::parse(fields).at("image_id") == 7 && !labels_done.exchange(true)) labels.set_value();
 };
 // This case constrains descriptors while retaining enough memory for a
 // stalled transfer and an independent reader. One-byte oversized-operation
 // serialization is exercised separately by compile_constrained_coconut.
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog, nullptr, {}, {}, {.transient_bytes = 1ULL << 30, .descriptors = descriptors}); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  served.server.ReleaseRequest();
 });
 REQUIRE(served.server.WaitRequest());
 REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(pixels, "independent cached artifact pixels while Curl owns a pending transfer", 5s));
 REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(labels, "annotation canvas labels while their JPEG transfer is held", 5s));
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 served.server.ReleaseRequest();
 mmltk::testsupport::await_test_future(compiling, "independent sources drain their descriptor and workspace custody", 15s);
 CHECK(CompiledDataset::open(local.output / "train.bin").header().num_images == 4);
 CHECK(served.server.requests() == 1);
 served.server.Check();
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
  if (gzip)
   REQUIRE(archive_write_add_filter_gzip(output.get()) == ARCHIVE_OK);
  else
   REQUIRE(archive_write_set_options(output.get(), "zip:compression=store") == ARCHIVE_OK);
  REQUIRE(archive_write_open_filename(output.get(), path.c_str()) == ARCHIVE_OK);
  std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
  archive_entry_set_pathname(entry.get(), "payload.bin");
  archive_entry_set_filetype(entry.get(), AE_IFREG);
  archive_entry_set_perm(entry.get(), 0600);
  archive_entry_set_size(entry.get(), payload.size());
  REQUIRE(archive_write_header(output.get(), entry.get()) == ARCHIVE_OK);
  REQUIRE(archive_write_data(output.get(), payload.data(), payload.size()) == static_cast<la_ssize_t>(payload.size()));
  REQUIRE(archive_write_close(output.get()) == ARCHIVE_OK);
 }
 auto encoded = file_bytes(path);
 if (gzip)
  encoded.resize(encoded.size() / 2);
 else {
  const auto position = encoded.find(payload);
  REQUIRE(position != std::string::npos);
  encoded[position + payload.size() / 2] ^= 1;
 }
 mmltk::testsupport::write_text_file(path, encoded);
 const auto consume = [&] {
  BenchmarkArchive input(path);
  if (!input.next()) throw BenchmarkArchiveError("missing consumed member");
  (void)input.read(payload.size());
 };
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
 const auto workspace = BenchmarkArchive::input_requirement(path, 1024).workspace_bytes;
 CHECK(workspace < (8ULL << 20));
 CHECK(BenchmarkArchive::input_requirement(path, 1024, 2).workspace_bytes > workspace);
 detail::CompileExecution execution(1, {}, {.transient_bytes = workspace, .descriptors = 16});
 {
  BenchmarkArchive input(path, &execution, 1024);
  // The ordinary stream admits the valid expansion without a parallel cache.
  // Replay preserves the callback cursor and every subsequently consumed byte.
  CHECK(input.allowance().bytes() == workspace);
  CHECK_FALSE(execution.try_reserve({1, 0}).has_value());
  REQUIRE(input.next());
  (void)input.read(1024);
  REQUIRE(input.next());
  REQUIRE(input.member() == "expanded.bin");
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
 SECTION("BGZF finder prefetch obeys its control ceiling") {
  control_limit = true;
  bgzf = true;
 }
 SECTION("cancellation during a late serial prefix replay") { cancel_replay = true; }
 SECTION("consumed concatenated member CRC remains required") {
  concatenated = true;
  corrupt_crc = true;
 }
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
 const auto workspace = BenchmarkArchive::input_requirement(path, 1024, 2).workspace_bytes;
 BenchmarkCompilePipeline execution(4, {}, {.transient_bytes = workspace, .descriptors = 16});
 {
  BenchmarkArchive input(path, &execution.execution(), 1024, {}, 2, true, {}, control_limit ? 1024 : 2, 0, control_limit ? 1 : 32768);
  REQUIRE(input.next());
  const auto first = input.member_position();
  const auto initial = input.read(1024);
  CHECK(std::string(initial.begin(), initial.end()) == "first");
  REQUIRE(input.next());
  REQUIRE(input.member() == "payload.bin");
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
   REQUIRE(input.next());
   REQUIRE(input.member() == "last.bin");
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
  CHECK(execution.execution().try_reserve({workspace, 0}).has_value());
 }
 CHECK(execution.execution().try_reserve({workspace, 16}).has_value());
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
 std::atomic<unsigned> preparation_updates{0}, archive_updates{0};
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.annotations.preparation" && Json::parse(fields).at("completed").get<std::uint64_t>() != 0) ++preparation_updates;
  if (event == "benchmark.archive.image_ready" && Json::parse(fields).at("entries").get<std::uint64_t>() != 0) ++archive_updates;
 };
 catalog.release_observer = [&](CoconutEdition, CoconutReleaseBoundary boundary) {
  if (boundary == CoconutReleaseBoundary::MetadataConsumed && ++releases == catalog.releases.size()) metadata.set_value();
 };
 auto compiling = std::async(std::launch::async, [&] {
  compile_benchmark_recipe(config, &catalog, nullptr, {}, [&](const std::filesystem::path&, std::uint64_t) {
   if (!pixel_reported.exchange(true)) pixels.set_value();
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  originals.reset();
 });
 mmltk::testsupport::await_test_promise(metadata, "all canonical metadata while original parsing is held", 5s);
 CHECK(preparation_updates.load() != 0);
 CHECK(archive_updates.load() != 0);
 mmltk::testsupport::await_test_promise(pixels, "known pixel input while original parsing is held", 5s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 originals.reset();
 mmltk::testsupport::await_test_future(compiling, "originals and independent pixels settle", 5s);
 CHECK(CompiledDataset::open(config.output_dir / "train.bin").header().num_images == 4);
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
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  source.reset();
 });
 mmltk::testsupport::await_test_promise(independent, "independent physical source resolution with one shared CPU", 5s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 source.reset();
 mmltk::testsupport::await_test_future(compiling, "held source resolves after release", 5s);
}
TEST_CASE("XL discovery normalizes ready labels before later JSON parsing", "[benchmark][coconut][archive]") {
 bool metadata_only = true;
 SECTION("metadata observations") {}
 SECTION("normalize ready masks before discovering later records") { metadata_only = false; }
 ScopedTempDir root("xl-incremental-discovery");
 const auto path = root.path() / "xl.tar";
 const std::array<std::uint32_t, 9> pixels{1, 1, 1, 1, 1, 1, 1, 1, 1};
 const std::array<std::pair<std::string, std::string>, 3> members{
  {{"coconuts_xlarge/panseg/objects365_v2_00000003.png", png(3, 3, pixels)}, {"coconuts_xlarge/panseg_info/objects365_v2_00000003.json", Json::array({segment()}).dump()},
   {"coconuts_xlarge/panseg_info/objects365_v2_00000002.json", "invalid later JSON"}}
 };
 tar(path, members);
 const auto cache = BenchmarkCacheLayout::create(root.path() / "cache");
 const auto images = cache.source_downloads("objects365") / "physical.tar";
 const auto jpeg = white_jpeg();
 const std::array<std::pair<std::string, std::string>, 1> image_members{{{objects(3).member, jpeg}}};
 tar(images, image_members);
 CoconutRecipeCatalog catalog;
 catalog.releases.push_back({CoconutEdition::XLarge, "fixture-xl", "fixture-v1", 1, {}});
 catalog.images.push_back(
  {CoconutImageNamespace::Objects365V2, 32, "patch-32", {"physical", "unused", images.filename().string(), std::filesystem::file_size(images), "", BenchmarkDatasetSource::kObjects365V2}});
 auto admitted = local_physical_archives(cache, catalog);
 const auto trace = mmltk::backend::data::testsupport::benchmark_trace();
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1, .descriptors = 16}, {}, trace);
 CoconutPhysicalMembership physical(admitted, catalog, true, cache, execution.storage(), execution);
 for (auto& source : admitted) {
  physical.admitted(source, source.download);
  physical.consume(source);
 }
 CoconutImportRequest input;
 input.execution = &execution;
 input.trace = trace;
 input.edition = CoconutEdition::XLarge;
 input.input_identity = "xl-discovery";
 input.metadata_only = metadata_only;
 input.mask_archive = path;
 input.physical_membership = &physical;
 std::vector<std::uint64_t> discovered;
 std::vector<std::uint64_t> archive_entries;
 std::vector<std::uint64_t> normalized;
 input.progress = [&](auto rows) { normalized.push_back(rows); };
 input.preparation_progress = [&](auto stage, auto rows) {
  if (stage == CoconutPreparationStage::Archive) {
   CHECK(discovered.empty());
   archive_entries.push_back(rows);
  } else {
   CHECK(stage == CoconutPreparationStage::Annotations);
   discovered.push_back(rows);
  }
 };
 CHECK_THROWS(import_coconut_annotations(input));
 CHECK(discovered == std::vector<std::uint64_t>{1});
 CHECK(archive_entries == std::vector<std::uint64_t>{1});
 if (!metadata_only) CHECK(normalized == std::vector<std::uint64_t>{0, 1});
 const auto image_root = cache.source_images("objects365") / "patch-32";
 CHECK(file_bytes(cached_image_path(image_root, 3)) == jpeg);
 const auto geometry = execution.geometry(physical.identity_root(CoconutImageNamespace::Objects365V2), 3);
 REQUIRE(geometry);
 CHECK(geometry->width == 3);
 CHECK(geometry->height == 3);
 CHECK(execution.execution().try_reserve({execution.execution().transient_target(), 16}).has_value());
}
TEST_CASE("failed XL generations retire oversized backing before source repair", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 bool invalid_png = false, pinned = false, diagnostics = false;
 SECTION("valid JSON followed by malformed JSON") {}
 SECTION("malformed consumed mask after successful discovery") { invalid_png = true; }
 SECTION("matching pinned malformed input keeps its source failure") {
  pinned = true;
  SECTION("without tracing") {}
  SECTION("with tracing") { diagnostics = true; }
 }
 ScopedTempDir root("xl-repair-retirement");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::Coconut);
 auto& release = *std::ranges::find(catalog.releases, CoconutEdition::XLarge, &CoconutReleaseComponent::edition);
 auto& artifact = release.annotations.front();
 const auto path = local.cache.source_downloads("coconut-" + std::string(release.name)) / artifact.filename;
 const auto payload = file_bytes(path);
 const std::array<std::uint32_t, 9> support{1, 1, 1, 1, 1, 1, 1, 1, 1};
 const std::array<std::pair<std::string, std::string>, 4> malformed{
  {{"coconuts_xlarge/panseg/objects365_v2_00000001.png", invalid_png ? "malformed PNG" : png(3, 3, support)},
   {"coconuts_xlarge/panseg_info/objects365_v2_00000001.json", Json::array({segment()}).dump()}, {"coconuts_xlarge/panseg/objects365_v2_00000002.png", png(3, 3, support)},
   {"coconuts_xlarge/panseg_info/objects365_v2_00000002.json", invalid_png ? Json::array({segment()}).dump() : "malformed later JSON"}}
 };
 tar(path, malformed);
 artifact.expected_size = 0;
 artifact.expected_sha256 = pinned ? mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(path)) : "";
 mmltk::backend::data::testsupport::HttpServer server(payload);
 artifact.url = server.url("xl-repair");
 server.GateNextRequest();
 std::atomic<bool> cancelled{false};
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Coconut});
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 unsigned matched_digests = 0;
 if (diagnostics)
  config.trace = [&](std::string_view event, std::string_view fields) {
   if (event == "benchmark.download.failure_sha256") {
    const auto facts = Json::parse(fields);
    if (facts.at("artifact") == artifact.artifact_id && facts.at("matches_expected") == true) ++matched_digests;
   }
  };
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog, nullptr, {}, {}, {.transient_bytes = 1, .descriptors = 26}); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  server.ReleaseRequest();
 });
 if (pinned) {
  REQUIRE(compiling.wait_for(10s) == std::future_status::ready);
  CHECK_NOTHROW(compiling.get());
  CHECK(CompiledDataset::open(config.output_dir / "train.bin").header().num_images == (pinned ? 3 : 4));
  CHECK(server.requests() == 0);
  CHECK(matched_digests == (diagnostics && pinned ? 1U : 0U));
 } else {
  REQUIRE(server.WaitRequest());
  CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
  server.ReleaseRequest();
  mmltk::testsupport::await_test_future(compiling, "XL repair reuses the unchanged one-byte scheduling target", 10s);
  CHECK(server.requests() == 1);
  CHECK(file_bytes(path) == payload);
  CHECK(CompiledDataset::open(config.output_dir / "train.bin").header().num_images == 4);
 }
 server.Check();
}
TEST_CASE("required release checksum read failures remain fatal independently of tracing", "[benchmark][coconut][download][trace]") {
 bool diagnostics = false;
 SECTION("without tracing") {}
 SECTION("with tracing") { diagnostics = true; }
 ScopedTempDir root("coconut-required-checksum");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::CoconutStock);
 auto& release = *std::ranges::find(catalog.releases, CoconutEdition::Large, &CoconutReleaseComponent::edition);
 auto& artifact = release.annotations.front();
 const auto path = local.cache.source_downloads("coconut-" + std::string(release.name)) / artifact.filename;
 const auto mask_path = local.cache.source_downloads("coconut-" + std::string(release.name)) / release.annotations.back().filename;
 const auto original = file_bytes(path);
 mmltk::backend::data::testsupport::HttpServer server(original);
 artifact.url = server.url("required-checksum");
 artifact.expected_sha256 = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(path));
 bool reached = false;
 catalog.release_observer = [&](CoconutEdition edition, CoconutReleaseBoundary boundary) {
  if (edition != CoconutEdition::Large || boundary != CoconutReleaseBoundary::MasksStarted) return;
  // Metadata consumption has settled. Fail the actual mask import and make
  // the required catalog-checksum read encounter a distinct local I/O error.
  std::filesystem::remove(path);
  std::filesystem::create_directory(path);
  mmltk::testsupport::write_text_file(mask_path, std::string(std::filesystem::file_size(mask_path), 'x'));
  reached = true;
 };
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 std::filesystem::create_directories(config.output_dir);
 const auto previous = config.output_dir / "previous";
 mmltk::testsupport::write_text_file(previous, "retained publication");
 unsigned repaired = 0;
 if (diagnostics)
  config.trace = [&](std::string_view event, std::string_view) {
   if (event == "benchmark.download.invalidated") ++repaired;
  };
 CHECK_THROWS_WITH(compile_benchmark_recipe(config, &catalog), "cannot inspect digest input file");
 CHECK(reached);
 CHECK(repaired == 0);
 CHECK(server.requests() == 0);
 CHECK(std::filesystem::is_directory(path));
 CHECK(file_bytes(previous) == "retained publication");
 CHECK_FALSE(std::filesystem::exists(config.output_dir / "train.bin"));
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
  CAPTURE(format, gzip, reverse);
  auto input = request(physical, CoconutEdition::XLarge);
  input.mask_archive = root.path() / (gzip ? "xl.tar.gz" : "xl.archive");
  input.records = std::make_shared<CoconutAnnotationRecords>();
  const auto& first = reverse ? upper : lower;
  const auto& second = reverse ? lower : upper;
  const std::array<std::pair<std::string, std::string>, 6> members{
   {{mask_prefix + first + ".png", mask}, {info_prefix + second + ".json", info}, {"unused/" + std::string(150, 'x'), std::string(256U << 10, 'x')}, {info_prefix + first + ".json", info},
    {mask_prefix + second + ".png", mask},
    // XL has always ignored masks with no offered JSON record.
    {mask_prefix + "objects365_v2_00000099.png", "unused invalid PNG"}}
  };
  // USTAR cannot encode an oversized single path component. A long prefix is
  // representable there, while PAX/GNU retain genuine extension records.
  auto contents = std::vector(members.begin(), members.end());
  if (format == ARCHIVE_FORMAT_TAR_USTAR) contents[2].first = "unused/payload.bin";
  tar(input.mask_archive, contents, false, format);
  input.metadata_only = true;
  const auto metadata = import_coconut_annotations(input);
  REQUIRE(metadata.size() == 1);
  REQUIRE(input.records->size() == 2);
  CHECK(input.records->record(0).image_id == 2);
  CHECK(input.records->record(0).first_segment_ordinal == 0);
  CHECK(input.records->record(1).first_segment_ordinal == 2);
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
  if (!expected)
   expected = component;
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
   input.accept_partial = false;
   auto rejected = contents;
   if (std::string_view(failure) == "missing")
    rejected.erase(rejected.begin());
   else if (std::string_view(failure) == "duplicate")
    rejected.push_back(rejected.front());
   else if (std::string_view(failure) == "invalid PNG")
    rejected.front().second = "invalid required PNG";
   input.limits.max_pixels = std::string_view(failure) == "actual geometry" ? 1 : 64U * 1024U * 1024U;
   tar(input.mask_archive, rejected, false, format);
   if (std::string_view(failure) == "duplicate")
    CHECK_THROWS(import_coconut_annotations(input));
   else {
    input.accept_partial = true;
    const auto partial = import_coconut_annotations(input);
    REQUIRE(partial.size() == 1);
    CHECK(std::ranges::any_of(std::views::iota(std::size_t{0}, partial.front().index().image_count()), [&](std::size_t row) { return !partial.front().required_labels_valid(row); }));
   }
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
 base.image_id = 7;
 base.file_name = "000000000007.jpg";
 CoconutRecord large;
 large.image_id = 601;
 large.physical_stem = "objects365_v2_00000001";
 for (auto& source : admitted) {
  physical.admitted(source, source.download);
  physical.consume(source);
 }
 const auto original = physical.resolve(CoconutEdition::Base, base);
 const auto independent = physical.resolve(CoconutEdition::Large, large);
 const auto other_path = cached_image_path(local.cache.source_images("objects365") / objects_archive.origin.cache_shard, 1);
 struct stat first{}, second{};
 REQUIRE(::stat(other_path.c_str(), &first) == 0);
 const auto train_root = local.cache.source_images("coco") / train.origin.cache_shard;
 const auto admitted_image = execution.image_input(physical.identity_root(CoconutImageNamespace::CocoTrain), 7);
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
 execution.retire_source(physical.identity_root(CoconutImageNamespace::CocoTrain));
 CHECK_FALSE(execution.image_input(physical.identity_root(CoconutImageNamespace::CocoTrain), 7));
 physical.withdraw(train);
 const auto replacement = root.path() / "replacement.tar.gz";
 const std::array<std::uint32_t, 1> pixel{0x001234};
 const auto encoded = png(1, 1, pixel);
 const std::array<std::pair<std::string, std::string>, 1> members{{{coco(7).member, encoded}}};
 tar(replacement, members);
 std::filesystem::rename(replacement, train.download.path);
 train.download.identity = "changed-physical-generation";
 physical.admitted(train, train.download);
 physical.consume(train);
 CHECK(physical.resolve(CoconutEdition::Large, large) == independent);
 REQUIRE(::stat(other_path.c_str(), &second) == 0);
 CHECK(first.st_ino == second.st_ino);
 const auto changed = physical.resolve(CoconutEdition::Base, base);
 CHECK(changed.archive_identity != original.archive_identity);
 CHECK(file_bytes(cached_image_path(train_root, 7)) == encoded);
 const auto geometry = execution.geometry(physical.identity_root(CoconutImageNamespace::CocoTrain), 7);
 REQUIRE(geometry);
 CHECK(geometry->width == 1);
 CHECK(geometry->height == 1);
 CHECK(execution.execution().try_reserve({execution.execution().transient_target(), 16}).has_value());
}
TEST_CASE("physical readiness captures each generation before reentrant and late subscriptions", "[benchmark][coconut][pipeline][recovery]") {
 const bool late = GENERATE(false, true);
 ScopedTempDir root("physical-generation-subscription");
 LocalCoconutRecipe local(root.path());
 const std::array conflicting{std::pair{coco(7, CoconutImageNamespace::CocoUnlabeled).member, white_jpeg()}};
 replace_physical_images(local.cache, local.catalog, CoconutImageNamespace::CocoUnlabeled, conflicting);
 auto admitted = local_physical_archives(local.cache, local.catalog);
 BenchmarkCompilePipeline pipeline(1, {}, {.transient_bytes = 1, .descriptors = 16});
 CoconutPhysicalMembership physical(admitted, local.catalog, true, local.cache, pipeline.storage(), pipeline);
 auto& train = *std::ranges::find(admitted, CoconutImageNamespace::CocoTrain, [](const auto& archive) { return archive.origin.source; });
 CoconutRecord record;
 record.image_id = 7;
 record.file_name = "000000000007.jpg";
 const auto provisional = physical.identify(CoconutEdition::Base, record);
 CHECK(provisional.archive_identity == "pending");
 auto current = physical.readiness(CoconutEdition::Base, record);
 CHECK_FALSE(current.terminal);
 CHECK_FALSE(current.image);
 const auto mutate = [&](const auto& change) {
  const auto before = current;
  unsigned callbacks = 0;
  CoconutPhysicalMembership::Readiness observed;
  const auto subscribe = [&] {
   before.changed.subscribe([&](std::exception_ptr failure) {
    if (failure) std::rethrow_exception(failure);
    observed = physical.readiness(CoconutEdition::Base, record);
    (void)physical.discovery_complete();
    (void)physical.identify(CoconutEdition::Base, record);
    ++callbacks;
   });
  };
  if (!late) subscribe();
  change();
  if (late) subscribe();
  CHECK(callbacks == 1);
  CHECK(before.changed.ready());
  CHECK(observed.generation > before.generation);
  current = physical.readiness(CoconutEdition::Base, record);
  CHECK(current.generation >= observed.generation);
 };
 mutate([&] { physical.admitted(train, train.download); });
 CHECK_FALSE(current.terminal);
 mutate([&] { REQUIRE(physical.consume(train)); });
 REQUIRE(current.image);
 CHECK(physical.resolve(CoconutEdition::Base, record) == *current.image);
 const auto first = *current.image;
 mutate([&] { physical.withdraw(train); });
 CHECK_FALSE(current.terminal);
 CHECK_FALSE(current.image);
 CHECK_FALSE(pipeline.image_input(physical.identity_root(CoconutImageNamespace::CocoTrain), record.image_id));
 auto replacement = train.download;
 replacement.identity = "replacement-generation";
 mutate([&] { physical.admitted(train, replacement, true); });
 mutate([&] { REQUIRE(physical.consume(train)); });
 REQUIRE(current.image);
 CHECK(current.image->archive_identity != first.archive_identity);
 CHECK(physical.resolve(CoconutEdition::Base, record) == *current.image);
 mutate([&] { physical.unavailable(train); });
 REQUIRE(current.image);
 CHECK(physical.resolve(CoconutEdition::Base, record) == *current.image);
 auto& unlabeled = *std::ranges::find(admitted, CoconutImageNamespace::CocoUnlabeled, [](const auto& archive) { return archive.origin.source; });
 physical.admitted(unlabeled, unlabeled.download);
 const auto conflict_change = physical.readiness(CoconutEdition::Base, record).changed;
 unsigned conflict_callbacks = 0;
 bool retired = false, ambiguous = false;
 std::exception_ptr callback_failure;
 const auto subscribe_conflict = [&] {
  conflict_change.subscribe([&](std::exception_ptr failure) {
   callback_failure = failure;
   retired = !pipeline.image_input(physical.identity_root(CoconutImageNamespace::CocoTrain), record.image_id);
   const auto observed = physical.readiness(CoconutEdition::Base, record);
   ambiguous = observed.terminal && !observed.image;
   ++conflict_callbacks;
  });
 };
 if (!late) subscribe_conflict();
 REQUIRE(physical.consume(unlabeled));
 if (late) subscribe_conflict();
 CHECK(conflict_callbacks == 1);
 CHECK_FALSE(callback_failure);
 CHECK(retired);
 CHECK(ambiguous);
 CHECK_THROWS_AS(physical.resolve(CoconutEdition::Base, record), CoconutPhysicalMembershipError);
}
TEST_CASE("physical readiness registrations release callbacks without cancelling same-image peers", "[benchmark][coconut][pipeline][recovery]") {
 ScopedTempDir root("physical-registration-custody");
 LocalCoconutRecipe local(root.path());
 auto admitted = local_physical_archives(local.cache, local.catalog);
 BenchmarkCompilePipeline pipeline(1, {}, {.transient_bytes = 1, .descriptors = 16});
 CoconutPhysicalMembership physical(admitted, local.catalog, true, local.cache, pipeline.storage(), pipeline);
 auto& train = *std::ranges::find(admitted, CoconutImageNamespace::CocoTrain, [](const auto& archive) { return archive.origin.source; });
 physical.admitted(train, train.download);
 REQUIRE(physical.consume(train));
 CoconutRecord record;
 record.image_id = 7;
 record.file_name = "000000000007.jpg";
 auto retiring = physical.readiness(CoconutEdition::Base, record);
 auto surviving = physical.readiness(CoconutEdition::Base, record);
 REQUIRE(retiring.terminal);
 REQUIRE(surviving.terminal);
 unsigned retired_calls = 0, survivor_calls = 0;
 auto custody = std::make_shared<unsigned>(0);
 const std::weak_ptr<unsigned> retired_callback = custody;
 retiring.changed.subscribe([held = std::move(custody), &retired_calls](std::exception_ptr) { retired_calls += ++*held; });
 surviving.changed.subscribe([&](std::exception_ptr) { ++survivor_calls; });
 REQUIRE_FALSE(retired_callback.expired());
 retiring = {};
 CHECK(retired_callback.expired());
 CHECK_FALSE(surviving.changed.ready());
 physical.withdraw(train);
 CHECK(surviving.changed.ready());
 CHECK(survivor_calls == 1);
 CHECK(retired_calls == 0);
 physical.admitted(train, train.download);
 REQUIRE(physical.consume(train));
 for (auto& archive : admitted) physical.unavailable(archive);
 // A terminal observation may never see another source event. Its callback
 // custody must end immediately, including for distinct missing identities.
 std::vector<std::weak_ptr<unsigned>> released;
 for (unsigned id = 1000; id < 1032; ++id) {
  record.image_id = id;
  record.file_name = std::format("{:012}.jpg", id);
  auto terminal = physical.readiness(CoconutEdition::Base, record);
  REQUIRE(terminal.terminal);
  REQUIRE_FALSE(terminal.image);
  auto retained = std::make_shared<unsigned>(0);
  released.push_back(retained);
  terminal.changed.subscribe([held = std::move(retained), &retired_calls](std::exception_ptr) { retired_calls += ++*held; });
 }
 CHECK(std::ranges::all_of(released, [](const auto& weak) { return weak.expired(); }));
 CHECK(retired_calls == 0);
 physical.withdraw(train);
 CHECK(retired_calls == 0);
}
TEST_CASE("physical repair shares source selection admission bounds and rejection reasons", "[benchmark][coconut][archive][recovery]") {
 const bool blocking = GENERATE(false, true);
 const auto scenario = GENERATE(0, 1, 2, 3);
 CAPTURE(blocking, scenario);
 const bool unreadable = scenario == 3;
 ScopedTempDir root("physical-repair-decisions");
 const auto cache = BenchmarkCacheLayout::create(root.path() / "cache");
 const auto train = cache.source_downloads("coco") / "train.tar";
 const auto unlabeled = cache.source_downloads("coco") / "unlabeled.tar";
 tar(train, std::array{std::pair{coco(unreadable ? 0 : 1).member, unreadable ? std::string("invalid image") : white_jpeg(1, 1)}});
 tar(unlabeled, std::array{std::pair{coco(2, CoconutImageNamespace::CocoUnlabeled).member, white_jpeg(1, 1)}});
 const auto catalog = base_physical_catalog(train, 2, unlabeled);
 auto admitted = local_physical_archives(cache, catalog);
 BenchmarkCompilePipeline pipeline(1, {}, {.transient_bytes = 1, .descriptors = 13});
 std::vector<CoconutImageNamespace> repaired;
 std::vector<std::string> reasons;
 CoconutPhysicalMembership* owner = nullptr;
 CoconutPhysicalMembership physical(admitted, catalog, true, cache, pipeline.storage(), pipeline, {}, {}, [&](AdmittedRecipeArchive& archive, std::string reason) {
  repaired.push_back(archive.origin.source);
  reasons.push_back(std::move(reason));
  owner->admitted(archive, archive.download);
  (void)owner->consume(archive);
 });
 owner = &physical;
 physical.admitted(admitted[0], admitted[0].download);
 for (auto& archive : admitted) {
  physical.admitted(archive, archive.download);
  REQUIRE(physical.consume(archive));
 }
 CoconutRecord record;
 record.image_id = 0;
 record.file_name = "000000000000.jpg";
 if (scenario == 1) record.namespace_hint = CoconutImageNamespace::CocoTrain;
 if (scenario >= 2) record.namespace_hint = CoconutImageNamespace::CocoUnlabeled;
 if (blocking) {
  if (unreadable)
   CHECK(physical.resolve(CoconutEdition::Base, record).image_id == 0);
  else
   CHECK_THROWS_WITH(physical.resolve(CoconutEdition::Base, record), "missing physical image: " + record.file_name);
 } else {
  // The callback can finish synchronously. The captured completion still
  // reports that change to a subscriber registered only after readiness returns.
  for (unsigned remaining = 3; remaining; --remaining) {
   const auto pending = physical.readiness(CoconutEdition::Base, record);
   if (pending.terminal) break;
   REQUIRE(pending.changed.ready());
   unsigned callbacks = 0;
   pending.changed.subscribe([&](std::exception_ptr) { ++callbacks; });
   CHECK(callbacks == 1);
   CHECK_FALSE(pending.image);
  }
 }
 const auto final = physical.readiness(CoconutEdition::Base, record);
 CHECK(final.terminal);
 CHECK(final.image.has_value() == unreadable);
 if (unreadable) {
  REQUIRE(final.image);
  CHECK(*final.image == physical.resolve(CoconutEdition::Base, record));
  CHECK_FALSE(pipeline.image_input(physical.identity_root(CoconutImageNamespace::CocoTrain), record.image_id));
 } else
  CHECK_THROWS_AS(physical.resolve(CoconutEdition::Base, record), CoconutPhysicalMembershipError);
 const std::vector<CoconutImageNamespace> expected = scenario == 0   ? std::vector{CoconutImageNamespace::CocoUnlabeled, CoconutImageNamespace::CocoTrain, CoconutImageNamespace::CocoUnlabeled}
                                                     : scenario == 2 ? std::vector{CoconutImageNamespace::CocoUnlabeled, CoconutImageNamespace::CocoUnlabeled}
                                                                     : std::vector{CoconutImageNamespace::CocoTrain};
 CHECK(repaired == expected);
 for (const auto& reason : reasons) CHECK(reason == std::string(unreadable ? "unreadable required image: " : "missing required image: ") + record.file_name);
 const auto count = repaired.size();
 (void)physical.readiness(CoconutEdition::Base, record);
 CHECK(repaired.size() == count);
}
TEST_CASE("one physical repair claim survives racing joins and completion subscription", "[benchmark][coconut][pipeline][recovery]") {
 using namespace std::chrono_literals;
 const bool blocking = GENERATE(false, true);
 ScopedTempDir root("physical-repair-claim");
 const auto cache = BenchmarkCacheLayout::create(root.path() / "cache");
 const auto archive = cache.source_downloads("coco") / "train.tar";
 tar(archive, std::array{std::pair{coco(1).member, white_jpeg(1, 1)}});
 const auto catalog = base_physical_catalog(archive, 1);
 auto admitted = local_physical_archives(cache, catalog);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline pipeline(1, {}, {.transient_bytes = 1, .descriptors = 13}, cancellation);
 mmltk::testsupport::TestGate repairing("one source owns the pending repair");
 std::atomic<unsigned> claims{0}, callbacks{0};
 CoconutPhysicalMembership* owner = nullptr;
 CoconutPhysicalMembership physical(admitted, catalog, true, cache, pipeline.storage(), pipeline, cancellation, {}, [&](AdmittedRecipeArchive& source, std::string) {
  ++claims;
  repairing.receipt().ArriveAndWait();
  owner->unavailable(source);
 });
 owner = &physical;
 physical.admitted(admitted.front(), admitted.front().download);
 REQUIRE(physical.consume(admitted.front()));
 CoconutRecord record;
 record.image_id = 0;
 record.file_name = "000000000000.jpg";
 auto joining = std::async(std::launch::async, [&] {
  if (blocking) {
   try {
    (void)physical.resolve(CoconutEdition::Base, record);
   } catch (const CoconutPhysicalMembershipError& error) { return error.image_id() == 0; }
   return false;
  }
  const auto pending = physical.readiness(CoconutEdition::Base, record);
  return !pending.terminal && !pending.image && pending.changed.ready();
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled = true;
  repairing.Release();
  pipeline.execution().cancel();
 });
 REQUIRE(repairing.WaitEntered(5s));
 const auto waiting = physical.readiness(CoconutEdition::Base, record);
 CHECK_FALSE(waiting.terminal);
 CHECK_FALSE(waiting.image);
 CHECK_FALSE(waiting.changed.ready());
 CHECK(claims.load() == 1);
 std::latch start{2};
 std::atomic<bool> terminal{false};
 auto subscribing = std::async(std::launch::async, [&] {
  start.arrive_and_wait();
  waiting.changed.subscribe([&](std::exception_ptr) {
   terminal = physical.readiness(CoconutEdition::Base, record).terminal;
   (void)physical.discovery_complete();
   ++callbacks;
  });
 });
 start.arrive_and_wait();
 repairing.Release();
 mmltk::testsupport::await_test_future(subscribing, "subscription races the source terminal publication", 5s);
 CHECK(mmltk::testsupport::await_test_future(joining, "repair settles both blocking and nonblocking joins", 5s));
 CHECK(claims.load() == 1);
 CHECK(callbacks.load() == 1);
 CHECK(terminal.load());
 CHECK(waiting.changed.ready());
 CHECK(physical.readiness(CoconutEdition::Base, record).terminal);
}
TEST_CASE("excluded Objects365 shards retain verified ambiguity before eligibility", "[benchmark][coconut][archive][recovery]") {
 const bool conflict = GENERATE(false, true);
 ScopedTempDir root("physical-excluded-shard");
 const auto cache = BenchmarkCacheLayout::create(root.path() / "cache");
 CoconutRecipeCatalog catalog;
 catalog.releases = {coconut_release_component(CoconutEdition::Large), coconut_release_component(CoconutEdition::XLarge)};
 for (const auto [shard, id] : std::array{std::pair{32U, 1U}, std::pair{17U, 2U}}) {
  const auto archive = cache.source_downloads("objects365") / ("patch" + std::to_string(shard) + ".tar");
  const auto image = objects(id);
  std::vector<std::pair<std::string, std::string>> members{{image.member, white_jpeg(1, 1)}};
  if (shard == 32 && conflict) members.push_back(members.front());
  tar(archive, members);
  catalog.images.push_back(
   {CoconutImageNamespace::Objects365V2, static_cast<std::uint16_t>(shard), "patch-" + std::to_string(shard),
    {"physical-" + std::to_string(shard), "unused", archive.filename().string(), std::filesystem::file_size(archive), "", BenchmarkDatasetSource::kObjects365V2}});
 }
 auto admitted = local_physical_archives(cache, catalog);
 BenchmarkCompilePipeline pipeline(1, {}, {.transient_bytes = 1, .descriptors = 13});
 unsigned repairs = 0;
 CoconutPhysicalMembership* owner = nullptr;
 CoconutPhysicalMembership physical(admitted, catalog, false, cache, pipeline.storage(), pipeline, {}, {}, [&](AdmittedRecipeArchive& archive, std::string) {
  ++repairs;
  owner->unavailable(archive);
 });
 owner = &physical;
 physical.admitted(admitted.front(), admitted.front().download);
 REQUIRE(physical.consume(admitted.front()));
 CoconutRecord record;
 record.image_id = 601;
 record.file_name = "601.jpg";
 record.physical_stem = "objects365_v2_00000001";
 CHECK(physical.eligible(CoconutEdition::Large, admitted.front()));
 CHECK_FALSE(physical.eligible(CoconutEdition::XLarge, admitted.front()));
 const auto discovering = physical.readiness(CoconutEdition::XLarge, record);
 CHECK(discovering.terminal == conflict);
 CHECK_FALSE(discovering.image);
 CHECK(repairs == 0);
 physical.admitted(admitted.back(), admitted.back().download);
 REQUIRE(physical.consume(admitted.back()));
 const auto ready = physical.readiness(CoconutEdition::XLarge, record);
 CHECK(ready.terminal);
 CHECK_FALSE(ready.image);
 CHECK_THROWS_WITH(physical.resolve(CoconutEdition::XLarge, record), std::string(conflict ? "ambiguous physical image identity: " : "missing physical image: ") + record.file_name);
 if (conflict)
  CHECK_THROWS_AS(physical.identify(CoconutEdition::XLarge, record), CoconutPhysicalMembershipError);
 else {
  CHECK(physical.identify(CoconutEdition::XLarge, record).archive_identity == "pending");
  const auto selected = physical.readiness(CoconutEdition::Large, record);
  REQUIRE(selected.image);
  CHECK(physical.resolve(CoconutEdition::Large, record) == *selected.image);
 }
 CHECK(repairs == 0);
 // A genuinely unknown identity still requests repair from the eligible set.
 record.physical_stem = "objects365_v2_00000003";
 (void)physical.readiness(CoconutEdition::XLarge, record);
 CHECK(repairs == 1);
 CHECK(physical.readiness(CoconutEdition::XLarge, record).terminal);
}
TEST_CASE("retained archive images keep storage after decoder CPUs return", "[benchmark][coconut][archive][pipeline]") {
 using mmltk::backend::data::detail::CompileWorkClass;
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
 ArchiveExtractionRequest request{
  .archive_path = path,
  .source_identity = "retained-gzip",
  .output_root = root.path() / "images",
  .source = "coco",
  .shard = "train2017",
  .selected_image_ids = ids,
  .image_id_parser = [](std::string_view) { return std::optional<std::uint64_t>{1}; },
  .validator =
   [&](std::uint64_t, std::span<const std::uint8_t> bytes) {
  // Extraction owns this header grant even when the forward reader has
  // returned its optional decompression helpers between chunks.
  const auto state = execution.execution().snapshot();
  live_cpus_charged = state.running[static_cast<std::size_t>(CompileWorkClass::Extraction)] > 0 && state.active > 0 && state.active + state.external <= execution.execution().workers();
  return BenchmarkImageDecoder{}.read_header(bytes);
 },
  .decompression_workers = 2,
  .image_ready =
   [&](const CachedImageReady& ready) {
  retained = ready.payload;
  if (failure) throw std::runtime_error("readiness callback failure");
 },
  .execution = &execution
 };
 if (failure)
  CHECK_THROWS(extract_selected_archive_images(request));
 else
  CHECK(extract_selected_archive_images(request).image_count == 1);
 execution.files().join();
 CHECK(live_cpus_charged);
 REQUIRE(retained);
 CHECK(retained->storage() == BenchmarkEncodedImage::Storage::Pooled);
 CHECK(std::string(retained->encoded().begin(), retained->encoded().end()) == encoded);
 CHECK(retained->allowance().bytes() == encoded.size());
 CHECK(retained->allowance().workspace().size() == encoded.size());
 CHECK(execution.execution().snapshot().active == 0);
 CHECK(execution.execution().snapshot().external == 0);
 CHECK(execution.execution().try_reserve({0, 0, false, 3}).has_value());
 CHECK_FALSE(execution.execution().try_reserve({execution.execution().transient_target(), 0}).has_value());
 const std::weak_ptr<const void> backing = retained->backing();
 retained.reset();
 CHECK(backing.expired());
 CHECK(execution.execution().try_reserve({execution.execution().transient_target(), 16}).has_value());
}
TEST_CASE("paused archive custody cannot retain retired external execution", "[benchmark][coconut][archive][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPU lanes");
 ScopedTempDir root("paused-archive-cpu-custody");
 const auto path = root.path() / "archive.tar.gz";
 const std::array<std::pair<std::string, std::string>, 1> members{{{"member", "bytes"}}};
 tar(path, members);
 BenchmarkCompilePipeline execution(4);
 BenchmarkArchive archive(path, &execution.execution(), 1024, {}, 2);
 REQUIRE(archive.next());
 auto retained = archive.allowance();
 CHECK_FALSE(execution.execution().try_reserve({0, 0, false, 3}).has_value());
 archive.pause();
 CHECK(execution.execution().try_reserve({0, 0, false, 3}).has_value());
 CHECK(execution.execution().try_reserve({execution.execution().transient_target(), 0}).has_value());
 retained = {};
 CHECK(execution.execution().try_reserve({execution.execution().transient_target(), 0}).has_value());
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
 const auto trace = mmltk::backend::data::testsupport::benchmark_trace();
 const auto budget = GENERATE(std::uint64_t{1}, std::uint64_t{4096});
 CAPTURE(budget);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = budget, .descriptors = 13}, cancellation, trace);
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{images}};
 split.images = {{1, 3, 3, 0, 0}, {2, 3, 3, 0, 0}};
 BenchmarkWriteRequest pixels{.split = split, .output_path = root.path() / "result.bin", .resolution = 1, .num_workers = 1, .cancel_requested = cancellation, .execution = &execution};
 BenchmarkSplitWriter writer(pixels, true);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 const auto publication = execution.source_publication(images, {});
 std::atomic<unsigned> consumed{0};
 const std::array<std::uint64_t, 2> selected{1, 2};
 auto extraction = std::async(std::launch::async, [&] {
  return extract_selected_archive_images(
   {.archive_path = path,
    .source_identity = "oversized-continuation",
    .output_root = images,
    .source = "coco",
    .shard = "train2017",
    .selected_image_ids = selected,
    .image_id_parser = [](std::string_view name) { return std::optional<std::uint64_t>{name == "1.jpg" ? 1U : 2U}; },
    .cancel_requested = cancellation,
    .trace = trace,
    .image_ready =
     [&](CachedImageReady ready) {
   if (!publication.consume(std::move(ready))) throw std::runtime_error("missing synchronous pixel consumer");
   require_condition(!ready.payload, "synchronous consumption retained its caller's encoded input");
   ++consumed;
  },
    .execution = &execution});
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  execution.execution().cancel();
 });
 REQUIRE(mmltk::testsupport::await_test_future(extraction, "oversized producer with required pixel continuation").image_count == 2);
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
 write_coconut_parquet(paths[0], Json::array({hf_row(7, encoded, Json::array({segment(9, 200, false), segment()}), 1, 1), hf_row(8, encoded, Json::array({segment()}), 1, 1)}));
 write_coconut_parquet(paths[1], Json::array({hf_row(9, encoded, Json::array({segment(3), segment(1)}), 1, 1)}), coconut_parquet_schema(true));
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256ULL << 20, .descriptors = 13});
 CoconutAnnotationRecords records;
 std::vector<std::string> names(3);
 read_coconut_parquet(paths, {}, {}, [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput& input) {
  CHECK(input.membership);
  names.at(record.source_ordinal) = record.file_name;
 }, true, &execution.execution(), &execution.files(), {}, {}, &records);
 REQUIRE(records.size() == 3);
 REQUIRE(records.groups.size() == 3);
 CoconutAnnotationInput retained;
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 std::size_t payload_rows = 0;
 const auto consume = [&] {
  read_coconut_parquet(paths, {}, cancellation, [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput& input) {
   CHECK_FALSE(input.membership);
   CHECK(record.file_name == names[record.source_ordinal]);
   if (!retained.backing && (fail || cancel || allocation_failure || ++payload_rows == 3)) {
    retained = input;
    if (cancel) cancelled.store(true);
    if (allocation_failure) throw std::bad_alloc{};
    if (fail) throw std::runtime_error("retained PNG consumer failed");
   }
  }, false, &execution.execution(), &execution.files(), {}, {}, &records);
 };
 if (allocation_failure)
  CHECK_THROWS_AS(consume(), std::bad_alloc);
 else if (fail || cancel)
  CHECK_THROWS(consume());
 else
  consume();
 REQUIRE(retained.backing);
 CHECK(std::string(retained.png.begin(), retained.png.end()) == encoded);
 if (!fail && !cancel && !allocation_failure) {
  CHECK(records.row(0).segment_ordinal() == 0);
  CHECK(records.row(1).segment_ordinal() == 2);
  CHECK(records.row(2).segment_ordinal() == 3);
  CHECK(records.groups[2].first_segment == 3);
 }
 records.discard();
 CHECK(std::string(retained.png.begin(), retained.png.end()) == encoded);
 // The batch keeps its fixed pinned bank lease, including array/offset storage beyond
 // the borrowed PNG extent, after every reader/footer descriptor has retired.
 CHECK_FALSE(execution.execution().try_reserve({(256ULL << 20) - retained.png.size(), 13}));
 for (const auto& descriptor : std::filesystem::directory_iterator("/proc/self/fd")) {
  std::error_code error;
  const auto path = std::filesystem::read_symlink(descriptor.path(), error);
  if (!error)
   for (const auto& input : paths) CHECK(path != input);
 }
 const std::weak_ptr<const void> backing = retained.backing;
 retained = {};
 CHECK(backing.expired());
 CHECK(execution.execution().try_reserve({256ULL << 20, 13}));
}
TEST_CASE("panoptic JSON keeps decoded metadata and one unchanged inventory seal", "[benchmark][coconut][annotations][cache]") {
 ScopedTempDir root("panoptic-semantic-rows");
 const std::array physical{objects(7)};
 auto input = request(physical, CoconutEdition::Large);
 input.records = std::make_shared<CoconutAnnotationRecords>();
 input.annotation_json = root.path() / "annotations.json";
 input.mask_archive = root.path() / "masks.tar";
 const std::string image = R"({"id":[],"\u0069d":7,"width":"bad","width":2,"height":1,"file_name":"objects365_v2_00000007.jpg"})";
 const std::string annotation =
  R"({"image_id":7,"file_name":"objects365_v2_00000007.png","segments_info":{},"segments_info":[{"id":false,"id":1,"category_id":1,"isthing":1,"bbox":[0,0,2,1],"area":18446744073709551616}]})";
 const auto document = std::string{"{\"annotations\":["} + annotation + "],\"\\u0069mages\":[" + image + "],\"categories\":" + category_catalog().dump() + "}";
 mmltk::testsupport::write_text_file(input.annotation_json, document);
 const std::array<std::uint32_t, 2> pixels{1, 1};
 const std::array<std::pair<std::string, std::string>, 1> masks{{{"panoptic_object365/objects365_v2_00000007.png", png(2, 1, pixels)}}};
 tar(input.mask_archive, masks);
 input.metadata_only = true;
 std::map<CoconutPreparationStage, std::vector<std::uint64_t>> preparation;
 input.preparation_progress = [&](auto stage, auto count) { preparation[stage].push_back(count); };
 const auto metadata = import_coconut_annotations(input);
 for (const auto stage : {CoconutPreparationStage::Images, CoconutPreparationStage::Annotations, CoconutPreparationStage::Membership}) CHECK(preparation.at(stage) == std::vector<std::uint64_t>{1});
 REQUIRE(metadata.size() == 1);
 REQUIRE(input.records->size() == 1);
 CHECK(input.records->record(0).file_name == "objects365_v2_00000007.png");
 CHECK(input.records->record(0).width == 2);
 bool changed = false;
 SECTION("identical admitted inventory is shared") {}
 SECTION("changed release-row facts require a new inventory generation") {
  auto changed_row = input.records->record(0);
  changed_row.source_ordinal = 7;
  input.records->adopt({std::move(changed_row)});
  changed = true;
 }
 std::filesystem::remove(input.annotation_json);
 input.metadata_only = false;
 auto full = import_coconut_annotations(input);
 REQUIRE(full.size() == 1);
 REQUIRE(full[0].index().box_count() == 1);
 CHECK((&full[0].inventory_image(0) == &metadata[0].inventory_image(0)) == !changed);
 CHECK((full[0].index().annotation_sha256 == metadata[0].index().annotation_sha256) == !changed);
 CHECK(metadata[0].inventory_image(0).source_ordinal == 0);
 CHECK(full[0].inventory_image(0).source_ordinal == (changed ? 7 : 0));
 CHECK(full[0].index().storage().boxes[0].source_ordinal == 0);
 const auto path = root.path() / "component.bin";
 store_coconut_component(path, full[0]);
 REQUIRE(full[0].completion());
 CHECK_FALSE(metadata[0].completion());
 CHECK(coconut_component_storage_bytes(full[0]) ==
       std::filesystem::file_size(path) + std::filesystem::file_size(path.string() + ".inventory") + std::filesystem::file_size(path.string() + ".complete.json"));
 const auto proof = read_json_file(path.string() + ".complete.json");
 CHECK(proof.contains("coconut"));
 for (const auto& field : {"edition", "source", "input_identity", "normalization", "inventory_identity", "inventory_count"}) {
  auto incomplete = proof;
  incomplete["coconut"].erase(field);
  write_json_atomically(path.string() + ".complete.json", incomplete, {});
  CHECK_FALSE(load_coconut_component(path, full[0].edition(), full[0].source(), full[0].input_identity()));
 }
 write_json_atomically(path.string() + ".complete.json", proof, {});
 CHECK(load_coconut_component(path, full[0].edition(), full[0].source(), full[0].input_identity()));
}
TEST_CASE("blocking controller local notices leave pipeline and native output owners idle", "[benchmark][coconut][pipeline][resources]") {
 using namespace std::chrono_literals;
 using detail::CompileExecution;
 ScopedTempDir root("coconut-owner-local-readiness");
 std::atomic<unsigned> dispatches{0};
 std::atomic<bool> expect_pipeline{false};
 std::promise<void> pipeline_ready;
 const BenchmarkTraceSink trace = [&](std::string_view event, const Json&) {
  if (event != "benchmark.pipeline.ready") return;
  ++dispatches;
  if (expect_pipeline) mmltk::testsupport::release_test_promise(pipeline_ready);
 };
 BenchmarkCompilePipeline pipeline(1, {}, {}, {}, trace);
 const std::array physical{coco(7)};
 auto input = request(physical);
 input.prepare_partial_staging(pipeline, root.path() / "staging");
 mmltk::testsupport::TestGate initialized("native output registration is live before input starts");
 input.progress = [&](std::uint64_t count) {
  if (!count) initialized.receipt().ArriveAndWait();
 };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup finish_import([&] {
  initialized.Release();
  pipeline.execution().cancel();
 });
 REQUIRE(initialized.WaitEntered(5s));
 mmltk::testsupport::TestGate initial("prior owner callbacks settled"), delivered("blocking controller received global Local");
 unsigned notices = 0;
 CompileExecution::Readiness controller(pipeline.execution(), CompileChange::Local, [&](CompileExecution::Readiness&, auto changes) {
  if (!includes(changes.causes, CompileChange::Local)) return;
  if (++notices == 1)
   initial.receipt().ArriveAndWait();
  else if (notices == 2)
   delivered.receipt().ArriveAndWait();
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  initial.Release();
  delivered.Release();
 });
 controller.notify();
 REQUIRE(initial.WaitEntered(5s));
 const auto before = pipeline.execution().snapshot().costs.notifier_deliveries;
 const auto dispatched = dispatches.load();
 const auto generation = pipeline.execution().admission_generation();
 pipeline.execution().notify_change(CompileChange::Local);
 CHECK(pipeline.execution().admission_generation() == generation + 1);
 initial.Release();
 REQUIRE(delivered.WaitEntered(5s));
 // The first controller callback has returned. The second is still active;
 // no pipeline, native-output, or idle file callback joined the Local batch.
 CHECK(pipeline.execution().snapshot().costs.notifier_deliveries == before + 1);
 CHECK(dispatches == dispatched);
 delivered.Release();
 expect_pipeline = true;
 pipeline.original_generation(root.path() / "images", 1, false);
 mmltk::testsupport::await_test_promise(pipeline_ready, "targeted pipeline source publication still dispatches", 5s);
 initialized.Release();
 const auto result = mmltk::testsupport::await_test_future(importing, "targeted native output completion still settles its import", 10s);
 REQUIRE(result.size() == 1);
 CHECK(result.front().index().image_count() == 1);
 CHECK(result.front().index().box_count() == 1);
 CHECK(result.front().required_labels_valid(0));
}
TEST_CASE("a live Parquet normalizer lends actual workspace to pixels on one CPU", "[benchmark][coconut][parquet][pipeline]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("parquet-normalizer-pixels");
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = 13}, {}, mmltk::backend::data::testsupport::benchmark_trace());
 const auto images = root.path() / "images";
 auto [split, encoded] = cached_mask_pixel_fixture(images);
 BenchmarkWriteRequest output{.split = split, .output_path = root.path() / "pixels.bin", .resolution = 1, .num_workers = 1, .execution = &execution};
 BenchmarkSplitWriter writer(output);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 auto pixel = mmltk::backend::data::testsupport::prepare_benchmark_pixel(execution, writer);
 const std::array physical{coco(7)};
 auto input = request(physical);
 input.execution = &execution;
 input.parquet_shards = {root.path() / "annotations.parquet"};
 write_coconut_parquet(input.parquet_shards.front(), Json::array({hf_row(7, encoded, Json::array({segment(), segment(2)}), 1, 1)}));
 mmltk::testsupport::TestGate normalizing("normalizer retains its live Arrow batch and support");
 bool completed_inside_normalizer = false;
 input.rejected_object = [&](const CoconutPhysicalImage&, const CoconutRecord&, const CoconutSegment&, std::string_view, std::uint32_t) {
  normalizing.receipt().ArriveAndWait();
  execution.execution().cooperate();
  completed_inside_normalizer = writer.image_complete(0);
 };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] { normalizing.Release(); });
 REQUIRE(normalizing.WaitEntered(5s));
 CHECK(execution.execution().snapshot().leased > 0);
 auto pixel_completion = execution.execution().submit(std::move(pixel));
 normalizing.Release();
 const auto result = mmltk::testsupport::await_test_future(importing, "Arrow reader resumes after pixel loan retirement", 10s);
 CHECK(completed_inside_normalizer);
 REQUIRE(result.size() == 1);
 REQUIRE(result[0].index().box_count() == 1);
 CHECK(result[0].index().rejected.degenerate_boxes == 1);
 CHECK(execution.execution().try_reserve({256ULL << 20, 11}));  // Writer staging and cached input directory remain live.
}
TEST_CASE("panoptic projection preserves document and consumed-row admission limits", "[benchmark][coconut][annotations]") {
 ScopedTempDir root("panoptic-row-limits");
 const std::array physical{objects(7)};
 auto input = request(physical, CoconutEdition::Large);
 input.metadata_only = true;
 input.annotation_json = root.path() / "annotations.json";
 input.limits.max_segments = 1;
 Json annotation{{"image_id", 7}, {"file_name", "objects365_v2_00000007.png"}, {"segments_info", Json::array({segment()})}};
 Json document{
  {"categories", category_catalog()}, {"images", Json::array({{{"id", 7}, {"width", 1}, {"height", 1}, {"file_name", "objects365_v2_00000007.jpg"}}})}, {"annotations", Json::array({annotation})}
 };
 bool accepted = false;
 std::optional<std::string> raw;
 SECTION("UTF-8 document marker") {
  raw = std::string("\xef\xbb\xbf") + document.dump();
  accepted = true;
 }
 SECTION("bounded ignored row text") {
  document["annotations"][0]["extra"] = std::string(4096, 'x');
  accepted = true;
 }
 SECTION("oversized ignored row text") { document["annotations"][0]["extra"] = std::string(4097, 'x'); }
 SECTION("events include ignored row values") { document["annotations"][0]["extra"] = std::vector<unsigned>(161, 0); }
 SECTION("depth applies to ignored envelope values") {
  Json nested = 0;
  for (int depth = 0; depth < 17; ++depth) nested = Json::array({std::move(nested)});
  document["extra"] = std::move(nested);
 }
 SECTION("fractional representation is not an exact ID") { document["annotations"][0]["image_id"] = 7.0; }
 SECTION("overflowing JSON number") {
  auto text = document.dump();
  text.insert(1, "\"extra\":1e9999,");
  raw = std::move(text);
 }
 SECTION("ignored envelope syntax remains required") {
  auto text = document.dump();
  text.insert(1, "\"extra\":[true false],");
  raw = std::move(text);
 }
 SECTION("escaped duplicate selected envelope name") {
  auto text = document.dump();
  text.insert(1, "\"\\u0069mages\":[],");
  raw = std::move(text);
 }
 mmltk::testsupport::write_text_file(input.annotation_json, raw.value_or(document.dump()));
 if (accepted)
  CHECK(import_coconut_annotations(input).front().index().image_count() == 1);
 else
  CHECK_THROWS(import_coconut_annotations(input));
}
TEST_CASE("panoptic rows consume unknown values and retain exact last-value semantics", "[benchmark][coconut][annotations]") {
 ScopedTempDir root("panoptic-value-admission");
 const std::array physical{objects(7)};
 auto input = request(physical, CoconutEdition::Large);
 input.metadata_only = true;
 input.annotation_json = root.path() / "annotations.json";
 input.records = std::make_shared<CoconutAnnotationRecords>();
 std::string extra;
 bool valid = false;
 SECTION("escaped nested names and valid unknown values") {
  extra = R"("unknown":{"text":"\uD834\uDD1E","values":[null,false,-12.5,18446744073709551616]} ,)";
  valid = true;
 }
 SECTION("unknown string escape") { extra = R"("unknown":"\q",)"; }
 SECTION("unknown unpaired surrogate") { extra = R"("unknown":"\uD834",)"; }
 SECTION("unknown raw UTF-8") { extra = "\"unknown\":\"" + std::string("\xc0\xaf") + "\","; }
 SECTION("unknown raw control character") { extra = "\"unknown\":\"bad\ntext\","; }
 SECTION("unknown overflowing number") { extra = R"("unknown":1e9999,)"; }
 SECTION("unknown leading zero") { extra = R"("unknown":01,)"; }
 SECTION("unknown truncated fraction") { extra = R"("unknown":1.,)"; }
 SECTION("unknown malformed literal") { extra = R"("unknown":tru,)"; }
 SECTION("unknown missing array separator") { extra = R"("unknown":[true false],)"; }
 SECTION("unknown trailing array comma") { extra = R"("unknown":[1,],)"; }
 SECTION("unknown trailing object comma") { extra = R"("unknown":{"a":1,},)"; }
 SECTION("unknown mismatched delimiter") { extra = R"("unknown":{"a":1],)"; }
 SECTION("selected value with invalid syntax cannot be superseded") { extra = R"("image_id":01,)"; }
 SECTION("escaped unknown key is consumed") { extra = R"("\uD800":null,)"; }
 SECTION("unknown selected-row depth") { extra = "\"unknown\":" + std::string(16, '[') + "0" + std::string(16, ']') + ","; }
 SECTION("invalid shapes and nulls can be superseded") {
  extra = R"("image_id":{},"segments_info":null,)";
  valid = true;
 }
 SECTION("a superseded segment count keeps last-key semantics") {
  input.limits.max_segments = 1;
  extra = R"("segments_info":[{},{}],)";
  valid = true;
 }
 const std::string image = R"({"id":[],"\u0069d":7,"width":"bad","width":2,"height":1,"file_name":"objects365_v2_00000007.jpg"})";
 const std::string annotation =
  "{" + extra +
  R"("image_id":7,"file_name":"objects365_v2_00000007.png","segments_info":[{"id":false,"\u0069d":1,"category_id":1,"isthing":null,"isthing":true,"iscrowd":{},"iscrowd":1,"ignore":[],"ignore":false,"bbox":{},"bbox":[0,0,2,1,99],"bbox":[0,0,2,1],"area":18446744073709551616}]})";
 mmltk::testsupport::write_text_file(input.annotation_json, "{\"categories\":" + category_catalog().dump() + ",\"images\":[" + image + "],\"annotations\":[" + annotation + "]}");
 if (!valid) {
  CHECK_THROWS(import_coconut_annotations(input));
  return;
 }
 const auto components = import_coconut_annotations(input);
 REQUIRE(components.size() == 1);
 REQUIRE(input.records->size() == 1);
 const auto& record = input.records->record(0);
 CHECK(record.image_id == 7);
 CHECK(record.width == 2);
 CHECK(record.height == 1);
 REQUIRE(record.segments.size() == 1);
 const auto& value = record.segments[0];
 CHECK(value.id == 1);
 CHECK(value.category_id == 1);
 CHECK(value.isthing);
 CHECK(value.crowd);
 CHECK_FALSE(value.ignore);
 REQUIRE(value.area);
 CHECK(*value.area == 18446744073709551616.0);
 const std::array<double, 4> bounds{0, 0, 2, 1};
 REQUIRE(value.bbox);
 CHECK(*value.bbox == bounds);
}
TEST_CASE("XL segment projection retains its distinct text and nesting policy", "[benchmark][coconut][annotations]") {
 ScopedTempDir root("xl-value-admission");
 const std::array physical{objects(7)};
 auto input = request(physical, CoconutEdition::XLarge);
 input.metadata_only = true;
 input.mask_archive = root.path() / "annotations.tar";
 input.records = std::make_shared<CoconutAnnotationRecords>();
 Json ignored = std::string(4097, 'x');
 for (int i = 0; i < 18; ++i) ignored = Json::array({std::move(ignored)});
 auto value = segment();
 value["unknown"] = std::move(ignored);
 const std::array<std::pair<std::string, std::string>, 1> entries{{{"coconuts_xlarge/panseg_info/objects365_v2_00000007.json", Json::array({value}).dump()}}};
 tar(input.mask_archive, entries);
 const auto components = import_coconut_annotations(input);
 REQUIRE(components.size() == 1);
 REQUIRE(input.records->size() == 1);
 REQUIRE(input.records->record(0).segments.size() == 1);
 CHECK(input.records->record(0).segments[0].id == 1);
}
TEST_CASE("Parquet binding admits optional fields and rejects required schema during membership", "[benchmark][coconut][parquet]") {
 ScopedTempDir root("parquet-bound-fields");
 const std::array<std::uint32_t, 1> pixels{1};
 auto row = hf_row(7, png(1, 1, pixels), Json::array({segment()}), 1, 1);
 auto fields = coconut_parquet_schema()->fields();
 bool valid = false;
 SECTION("absent crowd and ignore default to false") {
  auto segments = static_cast<const arrow::StructType&>(*coconut_parquet_segment_type(false)).fields();
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
 SECTION("unconsumed mask has the wrong schema") {
  fields[0] = arrow::field("mask", arrow::utf8());
  row["mask"] = "wrong";
 }
 const auto path = root.path() / "records.parquet";
 write_coconut_parquet(path, Json::array({row}), arrow::schema(fields));
 const std::array paths{path};
 if (valid) {
  read_coconut_parquet(paths, {}, {}, [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput& input) {
   if (input.membership) return;
   REQUIRE(record.segments.size() == 1);
   CHECK_FALSE(record.segments[0].crowd);
   CHECK_FALSE(record.segments[0].ignore);
  });
 } else
  CHECK_THROWS(read_coconut_parquet(paths, {}, {}, [](std::size_t, const CoconutRecord&, const CoconutAnnotationInput&) {}, true));
}
TEST_CASE("Parquet projections retain reordered nested fields and list encodings across batches", "[benchmark][coconut][parquet]") {
 bool legacy_list = false, integer_area = false;
 SECTION("standard list element and floating area") {}
 SECTION("external list element name and integer area") { legacy_list = integer_area = true; }
 ScopedTempDir root("parquet-projected-batches");
 const std::array<std::uint32_t, 1> pixels{1};
 const auto encoded = png(1, 1, pixels);
 auto segment_fields = static_cast<const arrow::StructType&>(*coconut_parquet_segment_type(integer_area)).fields();
 segment_fields.push_back(arrow::field("ignore", arrow::int64()));
 segment_fields.push_back(arrow::field("extra", arrow::utf8()));
 std::ranges::reverse(segment_fields);
 auto fields = coconut_parquet_schema(integer_area)->fields();
 auto annotation_fields = static_cast<const arrow::StructType&>(*fields[1]->type()).fields();
 annotation_fields[2] = arrow::field("segments_info", arrow::list(arrow::field("segment_entry", arrow::struct_(segment_fields))));
 std::ranges::reverse(annotation_fields);
 fields[1] = arrow::field("segments_info", arrow::struct_(annotation_fields));
 auto image_fields = static_cast<const arrow::StructType&>(*fields[2]->type()).fields();
 image_fields.push_back(arrow::field("extra", arrow::utf8()));
 std::ranges::reverse(image_fields);
 fields[2] = arrow::field("image_info", arrow::struct_(image_fields));
 auto mask_fields = static_cast<const arrow::StructType&>(*fields[0]->type()).fields();
 std::ranges::reverse(mask_fields);
 fields[0] = arrow::field("mask", arrow::struct_(mask_fields));
 fields.push_back(arrow::field("unselected", arrow::utf8()));
 std::ranges::reverse(fields);
 Json rows = Json::array();
 std::vector<std::uint64_t> prefixes;
 std::uint64_t prefix = 0;
 for (unsigned row = 0; row < 25; ++row) {
  prefixes.push_back(prefix);
  Json segments = Json::array();
  for (unsigned i = 0; i < row % 4; ++i) {
   auto value = segment(i + 1, i == 0 ? 200 : 1, i != 0);
   value["ignore"] = 1;
   value["extra"] = "kept in the nested projection";
   if (i != 0) value["area"] = integer_area ? 5.0 : 2.5;
   segments.push_back(std::move(value));
   ++prefix;
  }
  auto value = hf_row(row + 1, encoded, std::move(segments), 1, 1);
  value["image_info"]["extra"] = "not a native field";
  value["unselected"] = "not an image or mask column";
  if (row % 2) value["mask"]["path"] = "external-mask.png";
  rows.push_back(std::move(value));
 }
 parquet::ArrowWriterProperties::Builder properties;
 if (legacy_list) properties.disable_compliant_nested_types();
 const std::array paths{root.path() / "records.parquet"};
 write_coconut_parquet(paths[0], rows, arrow::schema(fields), 12, properties.build());
 CoconutAnnotationRecords retained;
 std::size_t observed = 0;
 const auto membership = [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput& input) {
  REQUIRE(input.membership);
  CHECK(record.image_id == ++observed);
  CHECK(record.width == 1);
  CHECK(record.height == 1);
 };
 read_coconut_parquet(paths, {}, {}, membership, true, nullptr, nullptr, {}, {}, &retained);
 REQUIRE(observed == 25);
 REQUIRE(retained.groups.size() == 3);
 const auto hidden = root.path() / "retained-only.parquet";
 std::filesystem::rename(paths[0], hidden);
 observed = 0;
 read_coconut_parquet(paths, {}, {}, membership, true, nullptr, nullptr, {}, {}, &retained);
 CHECK(observed == 25);
 std::filesystem::rename(hidden, paths[0]);
 observed = 0;
 read_coconut_parquet(paths, {}, {}, [&](std::size_t group, const CoconutRecord& record, const CoconutAnnotationInput& input) {
  CHECK_FALSE(input.membership);
  CHECK(record.source_ordinal == observed);
  CHECK(record.image_id == ++observed);
  CHECK(group == record.source_ordinal / 12);
  CHECK(record.segments.size() == record.source_ordinal % 4);
  CHECK(std::string(input.png.begin(), input.png.end()) == encoded);
  for (std::size_t i = 0; i < record.segments.size(); ++i) {
   CHECK(record.segments[i].id == i + 1);
   CHECK(record.segments[i].ignore);
   CHECK(record.segments[i].isthing == (i != 0));
   if (!i)
    CHECK_FALSE(record.segments[i].area);
   else {
    REQUIRE(record.segments[i].area);
    CHECK(*record.segments[i].area == (integer_area ? 5.0 : 2.5));
   }
  }
 }, false, nullptr, nullptr, {}, {}, &retained);
 CHECK(observed == 25);
 for (std::size_t row = 0; row < retained.size(); ++row) CHECK(retained.row(row).segment_ordinal() == prefixes[row]);
}
TEST_CASE("independent Parquet groups seal identical artifacts across shared CPU counts", "[benchmark][coconut][parquet][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPU lanes");
 ScopedTempDir root("parquet-canonical-groups");
 const std::array<std::uint32_t, 1> pixels{1};
 const auto encoded = png(1, 1, pixels);
 const std::array physical{coco(7), coco(8), coco(9)};
 const auto path = root.path() / "records.parquet";
 const auto segments = Json::array({segment(9, 200, false), segment(), segment(2)});
 write_coconut_parquet(path, Json::array({hf_row(9, encoded, segments, 1, 1), hf_row(7, encoded, segments, 1, 1), hf_row(8, encoded, segments, 1, 1)}));
 for (const auto workers : {1U, 2U}) {
  BenchmarkCompilePipeline execution(workers, {}, {.transient_bytes = 4ULL << 30, .descriptors = 24});
  auto input = request(physical);
  input.execution = &execution;
  input.parquet_shards = {path};
  auto result = import_coconut_annotations(input);
  REQUIRE(result.size() == 1);
  CHECK(result[0].index().storage().boxes[0].source_ordinal == 4);
  CHECK(result[0].index().storage().boxes[1].source_ordinal == 7);
  CHECK(result[0].index().storage().boxes[2].source_ordinal == 1);
  CHECK(result[0].index().rejected.raw_records == 9);
  CHECK(result[0].index().rejected.degenerate_boxes == 3);
  store_coconut_component(root.path() / (std::to_string(workers) + ".bin"), result[0]);
 }
 for (const auto suffix : {"", ".inventory", ".complete.json"}) CHECK(file_bytes(root.path() / (std::string("1.bin") + suffix)) == file_bytes(root.path() / (std::string("2.bin") + suffix)));
}
TEST_CASE("Parquet segment prefixes include declared groups and reject unrepresentable ordinals", "[benchmark][coconut][parquet]") {
 CoconutAnnotationRecords records;
 records.resize(2);
 records.admit_metadata(0, {});
 records.admit_metadata(1, {});
 records.admit_segments(0, {}, 2);
 records.admit_segments(1, {}, 1);
 records.groups = {{0, 1, 0, 3}, {1, 1, 0, 2}};
 bool valid = false;
 SECTION("declared stuff and omitted segment positions remain in the prefix") { valid = true; }
 SECTION("group total overflow") {
  records.groups[0].segments = UINT64_MAX;
  records.json_prefix(1, 0);
 }
 SECTION("record offset overflow") { records.groups[0].segments = UINT64_MAX; }
 SECTION("row range overflow") { records.groups[0].first_row = UINT64_MAX; }
 if (valid) {
  records.rebase_parquet_segments();
  CHECK(records.groups[1].first_segment == 3);
  CHECK(records.row(0).segment_ordinal() == 2);
  CHECK(records.row(1).segment_ordinal() == 4);
  records.rebase_parquet_segments();
  CHECK(records.row(1).segment_ordinal() == 4);
  CHECK(records.record(1).first_segment_ordinal == 1);
 } else
  CHECK_THROWS(records.rebase_parquet_segments());
}
TEST_CASE("panoptic JSON publishes physical facts before unrelated envelope tails", "[benchmark][coconut][annotations][pipeline]") {
 bool reversed = false, malformed = false, duplicate = false, cancel = false, malformed_row = false;
 SECTION("ready arrays precede valid ignored tail") {}
 SECTION("reversed arrays and escaped selected field") { reversed = true; }
 SECTION("malformed ignored tail follows ready facts") { malformed = true; }
 SECTION("malformed later selected row follows a bounded ready batch") { malformed_row = true; }
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
 catalog.images.push_back(
  {CoconutImageNamespace::Objects365V2, 32, "patch-32", {"physical", "unused", archive.filename().string(), std::filesystem::file_size(archive), "", BenchmarkDatasetSource::kObjects365V2}});
 PhysicalMembershipFixture fixture(cache, catalog);
 auto& [admitted, cancelled, cancellation, execution, physical] = fixture;
 const auto image_root = cache.source_images("objects365") / "patch-32";
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{image_root}};
 split.images = {{3, 3, 3, 0, 0}};
 BenchmarkWriteRequest output{.split = split, .output_path = root.path() / "pixels.bin", .resolution = 3, .num_workers = 1, .execution = &execution};
 if (cancel) output.progress = {.context = &cancelled, .image_completed = [](void* state) { static_cast<std::atomic<bool>*>(state)->store(true); }};
 BenchmarkSplitWriter writer(output);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 if (!cancel)
  writer.write_remaining(output);
 else {
  try {
   writer.write_remaining(output);
  } catch (const std::exception&) {}
 }
 CoconutImportRequest input;
 input.execution = &execution;
 input.cancellation = cancellation;
 input.edition = CoconutEdition::Large;
 input.input_identity = "json-discovery";
 input.metadata_only = true;
 input.annotation_json = root.path() / "annotations.json";
 input.physical_membership = &physical;
 const std::string categories = "\"categories\":" + category_catalog().dump();
 const std::string images = R"("\u0069mages":[{"id":3,"width":3,"height":3,"file_name":"objects365_v2_00000003.jpg"}])";
 std::string annotations = R"("annotations":[{"image_id":3,"file_name":"objects365_v2_00000003.png","segments_info":[]}])";
 if (malformed_row) {
  Json row{{"image_id", 3}, {"file_name", "objects365_v2_00000003.png"}, {"segments_info", Json::array()}, {"unknown", std::vector<std::string>(65, std::string(4096, 'x'))}};
  annotations = "\"annotations\":[" + row.dump() + R"(,{"unknown":1e9999}])";
 }
 const auto prefix = "{" + (reversed ? annotations + "," + images + "," + categories : categories + "," + images + "," + annotations);
 const auto tail = ",\"ignored\":\"" + std::string(2U << 20, 'x') + (malformed ? "" : "\"") + (duplicate ? ",\"images\":[]}" : malformed ? "" : "}");
 mmltk::testsupport::write_text_file(input.annotation_json, prefix + tail);
 if (malformed || duplicate || cancel || malformed_row)
  CHECK_THROWS(import_coconut_annotations(input));
 else {
  const auto result = import_coconut_annotations(input);
  REQUIRE(result.size() == 1);
  REQUIRE(result[0].inventory().size() == 1);
  CHECK(result[0].inventory()[0].source_ordinal == 0);
 }
 CHECK(file_bytes(cached_image_path(image_root, 3)) == jpeg);
 REQUIRE(execution.geometry(image_root, 3));
 if (cancel)
  CHECK(cancelled.load());
 else {
  execution.drain();
  CHECK(writer.image_complete(0));
 }
}
TEST_CASE("consecutive Parquet groups retain compressed physical progress with bounded continuation", "[benchmark][coconut][parquet][pipeline]") {
 bool cancel = false, fail = false, pressure = false, warm = false;
 SECTION("one CPU thirteen descriptors and an opened gzip generation") {}
 SECTION("warm metadata followed by a full forward gzip sequence") { warm = true; }
 SECTION("cancellation retires the consecutive reader") { cancel = true; }
 SECTION("consumer failure retires the consecutive reader") { fail = true; }
 SECTION("independent retained capacity releases at the group boundary") { pressure = true; }
 CAPTURE(cancel, fail, pressure, warm);
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
 const auto catalog = base_physical_catalog(archive, 4);
 // A fixed Arrow window and an independent retained charge must both fit.
 PhysicalMembershipFixture fixture(cache, catalog, 512ULL << 20);
 auto& [admitted, cancelled, cancellation, execution, physical] = fixture;
 const auto images = cache.source_images("coco") / "train2017";
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{images}};
 for (unsigned id = 1; id <= 4; ++id) split.images.push_back({id, 3, 3, 0, 0});
 BenchmarkWriteRequest output{.split = split, .output_path = root.path() / "pixels.bin", .resolution = 3, .num_workers = 1, .execution = &execution};
 BenchmarkSplitWriter writer(output);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 if (!cancel)
  writer.write_remaining(output);
 else {
  try {
   writer.write_remaining(output);
  } catch (const std::exception&) {}
 }
 const std::array paths{root.path() / "records.parquet"};
 write_coconut_parquet(paths[0], rows);
 CoconutAnnotationRecords retained;
 if (warm)
  read_coconut_parquet(paths, {}, cancellation, [&](std::size_t, const CoconutRecord&, const CoconutAnnotationInput& input) { CHECK(input.membership); }, true, &execution.execution(),
   &execution.files(), {}, {}, &retained);
 CompileAllowance blocker;
 std::size_t metadata_rows = 0, payload_rows = 0, retired_inputs = 0, retired_groups = 0;
 Json transitions = Json::array();
 const auto consume = [&] {
  read_coconut_parquet(paths, {}, cancellation, [&](std::size_t group, const CoconutRecord& record, const CoconutAnnotationInput& input) {
   const auto usage = execution.execution().resource_usage();
   transitions.push_back({{"group", group}, {"membership", input.membership}, {"backing_bytes", input.live_bytes}, {"used", usage.bytes}, {"cpus", usage.cpu_workers}});
   CHECK(bool(input.backing) == !input.membership);
   CHECK(record.source_ordinal == group);
   CHECK(record.image_id == group + 1);
   const auto membership = physical.resolve(CoconutEdition::Base, record);
   CHECK(membership.member == coco(static_cast<unsigned>(record.image_id)).member);
   if ((input.membership || warm) && record.image_id == 1) {
    const auto unrelated = execution.execution().reserve({0, 0});
    if (pressure)
     blocker = execution.execution().reserve({256ULL << 20, 0});
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
   if (!cancelled.load()) execution.execution().run(CompileTaskKind::Metadata, {}, [](std::size_t) {});
  }, false, &execution.execution(), &execution.files(), [&] {
   ++retired_groups;
   blocker = {};
  }, {}, &retained);
 };
 try {
  if (cancel || fail)
   CHECK_THROWS(consume());
  else
   consume();
 } catch (const std::exception& error) {
  INFO(transitions.dump());
  FAIL("consecutive Parquet consumption failed: " << error.what());
 }
 INFO(transitions.dump());
 CHECK(retired_inputs == 0);
 CHECK(retired_groups >= 2);
 if (!cancel && !fail) {
  CHECK(metadata_rows == (warm ? 0 : 4));
  CHECK(payload_rows == 4);
  REQUIRE(retained.groups.size() == 4);
  for (std::size_t group = 0; group < 4; ++group) {
   CHECK(retained.groups[group].first_segment == group * 2);
   CHECK(retained.row(group).segment_ordinal() == group * 2);
   CHECK(file_bytes(cached_image_path(images, group + 1)) == jpeg);
  }
  execution.drain();
  writer.finish(output);
  const auto compiled = CompiledDataset::open(output.output_path);
  CHECK(compiled.header().num_images == 4);
  for (std::size_t i = 1; i < 4; ++i) CHECK(std::memcmp(compiled.image_pixels(0), compiled.image_pixels(static_cast<std::uint32_t>(i)), 27 * sizeof(float)) == 0);
 }
}
TEST_CASE("quiesced Parquet batches retain their backing while another artifact progresses", "[benchmark][coconut][parquet][pipeline]") {
 ScopedTempDir root("parquet-shared-batch");
 const std::array<std::uint32_t, 1> ids{1};
 const auto encoded = png(1, 1, ids);
 const std::array main_paths{root.path() / "sequence.parquet"}, other_paths{root.path() / "independent.parquet"};
 write_coconut_parquet(main_paths[0], Json::array({hf_row(1, encoded, Json::array({segment()}), 1, 1), hf_row(2, encoded, Json::array({segment()}), 1, 1)}));
 write_coconut_parquet(other_paths[0], Json::array({hf_row(3, encoded, Json::array({segment()}), 1, 1)}));
 BenchmarkCompilePipeline execution(2, {}, {.transient_bytes = 2ULL << 30, .descriptors = 13});
 CoconutAnnotationInput retained_batch;
 bool independent = false;
 read_coconut_parquet(main_paths, {}, {}, [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput& input) {
  if (input.membership) return;
  if (record.image_id == 1) {
   retained_batch = input;
   CHECK(input.live_bytes >= input.png.size());
   CHECK(execution.execution().snapshot().leased >= kParquetPoolBytes);
   read_coconut_parquet(
    other_paths, {}, {}, [&](std::size_t, const CoconutRecord& row, const CoconutAnnotationInput&) { independent = row.image_id == 3; }, true, &execution.execution(), &execution.files());
  }
  CHECK(std::string(input.png.begin(), input.png.end()) == encoded);
 }, false, &execution.execution(), &execution.files());
 CHECK(independent);
 REQUIRE(retained_batch.backing);
 CHECK(std::string(retained_batch.png.begin(), retained_batch.png.end()) == encoded);
 retained_batch = {};
 CHECK(execution.execution().try_reserve({2ULL << 30, 13}));
}
TEST_CASE("repeated warm Parquet membership needs no physical workspace or Arrow reads", "[benchmark][coconut][parquet][pipeline]") {
 bool cancel = false, fail = false;
 SECTION("consecutive cached membership groups") {}
 SECTION("cancellation retains the cached opened generation") { cancel = true; }
 SECTION("consumer failure releases temporary source custody") { fail = true; }
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
 const auto catalog = base_physical_catalog(archive, 4);
 PhysicalMembershipFixture fixture(cache, catalog);
 auto& [admitted, cancelled, cancellation, execution, physical] = fixture;
 const std::array paths{root.path() / "records.parquet"};
 write_coconut_parquet(paths[0], rows);
 auto retained = std::make_shared<CoconutAnnotationRecords>();
 std::size_t cold_rows = 0;
 read_coconut_parquet(paths, {}, cancellation, [&](std::size_t, const CoconutRecord&, const CoconutAnnotationInput& input) {
  CHECK(input.membership);
  ++cold_rows;
 }, true, &execution.execution(), &execution.files(), {}, {}, retained.get());
 REQUIRE(cold_rows == 4);
 // A warm call must use its retained typed rows without opening any projection.
 std::filesystem::remove(paths[0]);
 std::unique_ptr<CompileResourceWait> pressure;
 std::size_t published = 0, retired = 0;
 const auto consume = [&] {
  read_coconut_parquet(paths, {}, cancellation, [&](std::size_t group, const CoconutRecord& record, const CoconutAnnotationInput& input) {
   CHECK(input.membership);
   CHECK(input.png.empty());
   CHECK_FALSE(input.backing);
   CHECK(record.source_ordinal == group);
   CHECK(record.image_id == group + 1);
   CHECK(input.live_bytes == 0U);
   CHECK(execution.execution().try_reserve({execution.execution().transient_target(), 0, true}));
   const auto member = physical.resolve(CoconutEdition::Base, record);
   CHECK(member.member == coco(static_cast<unsigned>(record.image_id)).member);
   ++published;
   if (published == 2) pressure.reset();
   if (published == 1) {
    pressure = execution.execution().defer_resources();
    // Consecutive groups must keep the already-open compressed generation.
    std::filesystem::rename(archive, archive.string() + ".opened");
    mmltk::testsupport::write_text_file(archive, "replacement is not an archive");
   }
   const auto work = [&] {
    CHECK(execution.execution().try_reserve({execution.execution().transient_target(), 0, true}));
    execution.execution().run(CompileTaskKind::Metadata, {}, [&](std::size_t) {
     if (published == 2 && fail) throw std::runtime_error("warm membership consumer failed");
     if (published == 2 && cancel) cancelled.store(true);
    });
   };
   work();
  }, true, &execution.execution(), &execution.files(), {}, {}, retained.get());
 };
 if (cancel || fail)
  CHECK_THROWS(consume());
 else
  consume();
 CHECK(published == (cancel || fail ? 2 : 4));
 CHECK(retired == 0);
 cancelled.store(false);
 const auto images = cache.source_images("coco") / "train2017";
 for (std::size_t i = 1; i <= published; ++i) CHECK(file_bytes(cached_image_path(images, i)) == jpeg);
 if (!cancel && !fail) {
  // Reach the product Importer::consume path with the same already-ready rows.
  // Physical membership remains cached after its actual reader retired above.
  retained->identity = "warm-membership";
  CoconutImportRequest input;
  input.execution = &execution;
  input.edition = CoconutEdition::Base;
  input.input_identity = retained->identity;
  input.metadata_only = true;
  input.records = retained;
  input.parquet_shards.assign(paths.begin(), paths.end());
  input.physical_membership = &physical;
  input.expected_rows = 4;
  input.cancellation = cancellation;
  const auto components = import_coconut_annotations(input);
  REQUIRE(components.size() == 1);
  CHECK(components[0].index().image_count() == 4);
  REQUIRE(components[0].inventory().size() == 4);
  for (std::size_t i = 0; i < 4; ++i) {
   CHECK(components[0].inventory()[i].source_ordinal == i);
   CHECK(components[0].inventory()[i].physical.image_id == i + 1);
  }
 }
 retained->discard();
 execution.retire_source(images);
 CHECK(execution.execution().try_reserve({execution.execution().transient_target(), 13}));
}
TEST_CASE("recovered support keeps immutable original custody after readers retire", "[benchmark][coconut][recovery]") {
 const std::array<RLEPair, 1> original_mask{{{0, 2}}};
 std::vector<CoconutSegmentSupport> support(1);
 std::weak_ptr<NormalizedAnnotationBacking> backing;
 {
  auto original = fixture_index(recovery_originals(7, original_mask));
  backing = original.backing;
  CoconutRecoveryOriginals indexed(&original, nullptr);
  CoconutMaskRecovery recovery(indexed);
  CoconutRecord record;
  record.image_id = 7;
  record.segments = {{.id = 10, .category_id = 18, .isthing = true}};
  CoconutRecoveryImage facts{7, 0, {}};
  recovery.apply(CoconutImageNamespace::CocoTrain, record, 3, 3, support, facts);
  REQUIRE(facts.objects.size() == 1);
  CHECK(support[0].runs.data() == original.mask_rle_pairs.data());
  CHECK(support[0].runs.retained_bytes() == 0);
  recovery.retire_scratch();
  CHECK(recovery.retained_bytes() == 0);
 }
 CHECK_FALSE(backing.expired());
 REQUIRE(support[0].runs.size() == 1);
 CHECK(support[0].runs[0].length == 2);
 std::pmr::vector<RLEPair> carved{{3, 1}};
 const auto capacity = carved.capacity() * sizeof(RLEPair);
 support[0].runs.replace(carved);
 CHECK(backing.expired());
 CHECK(support[0].runs.retained_bytes() == capacity);
 CHECK(support[0].runs[0].start == 3);
 support[0].runs.clear();
 CHECK(support[0].runs.retained_bytes() == capacity);
 support.clear();
}
TEST_CASE("unfinished component keeps completed native rows across physical repair", "[benchmark][coconut][pipeline][cache]") {
 ScopedTempDir root("coconut-partial-native-custody");
 const std::array physical{coco(7), coco(8, CoconutImageNamespace::CocoUnlabeled)};
 const std::array<std::uint32_t, 9> full{1, 1, 1, 1, 1, 1, 1, 1, 1};
 const std::array<std::uint32_t, 1> tiny{1};
 const auto path = root.path() / "images.parquet";
 write_coconut_parquet(path, Json::array({hf_row(7, png(3, 3, full), Json::array({segment()})), hf_row(8, png(1, 1, tiny), Json::array({segment()}), 1, 1, "unlabeled2017")}));
 auto retained = std::make_shared<CoconutAnnotationRecords>();
 auto first = request(std::span(physical).first(1));
 first.records = retained;
 first.parquet_shards = {path};
 CHECK_THROWS_AS(import_coconut_annotations(first), CoconutPhysicalMembershipError);
 REQUIRE(retained->size() == 2);
 REQUIRE(retained->row(0).native());
 CHECK_FALSE(retained->row(1).native());
 const std::weak_ptr<const CoconutNativeImage> native = retained->row(0).native();
 auto replacement = physical;
 auto retry = request(replacement);
 retry.records = retained;
 retry.parquet_shards = {path};
 retry.limits.max_pixels = 1;
 bool changed = false;
 SECTION("unchanged native input needs no second PNG decode") {}
 SECTION("a changed physical artifact cannot authorize the old native row") {
  changed = true;
  replacement[0].archive_identity = "replacement-generation";
 }
 SECTION("changed annotation inputs discard completed native rows") {
  changed = true;
  retry.input_identity = "replacement-annotation-generation";
 }
 SECTION("changed original generation requires native recovery again") {
  changed = true;
  retry.originals = [](CoconutImageNamespace, bool, std::stop_token) { return CoconutOriginalReadiness{{}, true, 2}; };
 }
 if (changed)
  CHECK_THROWS(import_coconut_annotations(retry));
 else {
  const auto result = import_coconut_annotations(retry);
  REQUIRE(result.size() == 2);
  REQUIRE(result[0].index().box_count() == 1);
  CHECK(result[0].index().image(0).width == 3);
  CHECK(result[0].index().storage().mask_rle_pairs.front().length == 9);
  CHECK(result[1].index().storage().boxes.front().source_ordinal == 1);
  CHECK(std::ranges::none_of(retained->rows(), [](const auto& row) { return static_cast<bool>(row.native()); }));
  CHECK(native.expired());
 }
}
TEST_CASE("pending train originals leave mixed Base unlabeled rows runnable", "[benchmark][coconut][pipeline][recovery]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("coconut-pending-native-split");
 const std::array<std::uint32_t, 1> pixel{1};
 Json rows = Json::array();
 std::vector<CoconutPhysicalImage> physical;
 for (unsigned id = 1; id <= 65; ++id) {
  const bool train = id == 1;
  physical.push_back(coco(id, train ? CoconutImageNamespace::CocoTrain : CoconutImageNamespace::CocoUnlabeled));
  rows.push_back(hf_row(id, png(1, 1, pixel), Json::array({segment()}), 1, 1, train ? "train2017" : "unlabeled2017"));
 }
 auto input = request(physical);
 input.records = std::make_shared<CoconutAnnotationRecords>();
 input.parquet_shards = {root.path() / "mixed.parquet"};
 write_coconut_parquet(input.parquet_shards.front(), rows, coconut_parquet_schema(), 65);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1, .descriptors = 13}, {}, mmltk::backend::data::testsupport::benchmark_trace());
 input.execution = &execution;
 input.trace = mmltk::backend::data::testsupport::benchmark_trace();
 CompileExecution::CompletionSource originals;
 std::promise<void> pending_original;
 std::atomic<bool> observed_original{false};
 std::atomic<bool> terminal{false};
 input.originals = [&](CoconutImageNamespace source, bool wait, std::stop_token) {
  if (source != CoconutImageNamespace::CocoTrain) throw std::logic_error("unlabeled row waited for train originals");
  (void)wait;
  if (!observed_original.exchange(true)) pending_original.set_value();
  return CoconutOriginalReadiness{{}, terminal.load(), 1, originals.completion()};
 };
 std::promise<void> independent;
 input.progress = [&](std::uint64_t count) {
  if (count == 64) independent.set_value();
 };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  terminal.store(true);
  originals.settle();
 });
 mmltk::testsupport::await_test_promise(pending_original, "original readiness subscription");
 mmltk::testsupport::await_test_promise(independent, "unlabeled native rows finish while train originals are pending");
 REQUIRE(input.records->row(63).native());
 CHECK(input.records->row(63).native()->lineage().component.source == CoconutImageNamespace::CocoUnlabeled);
 CHECK_FALSE(input.records->row(0).native());
 CHECK(importing.wait_for(0ms) == std::future_status::timeout);
 terminal.store(true);
 originals.settle();
 const auto result = mmltk::testsupport::await_test_future(importing, "optional originals settle and native recovery completes");
 REQUIRE(result.size() == 2);
 CHECK(result[0].index().image_count() == 1);
 CHECK(result[1].index().image_count() == 64);
 input.records->discard();
 CHECK(execution.execution().try_reserve({1, 13}));
}
TEST_CASE("COCO original split publication is independent for warm and cold admission", "[benchmark][coconut][cache][recovery]") {
 using namespace std::chrono_literals;
 bool cold = false, hold_train = false;
 SECTION("warm train can publish while validation publication is held") {}
 SECTION("warm validation can publish while train publication is held") { hold_train = true; }
 SECTION("cold train can publish while validation publication is held") { cold = true; }
 SECTION("cold required validation can publish while train publication is held") {
  cold = true;
  hold_train = true;
 }
 ScopedTempDir root("coco-independent-split-publication");
 LocalCoconutRecipe local(root.path());
 auto catalog = local_custom_catalog(local);
 if (cold) {
  for (const auto split : {"train2017", "val2017"}) remove_normalized_annotation_index(local.cache.source_indexes("coco") / (std::string(split) + ".normalized.bin"));
  const auto path = local.cache.source_downloads("coco") / catalog.coco_annotations.filename;
  const std::array<std::pair<std::string, std::string>, 2> members{
   {{"annotations/instances_train2017.json", file_bytes(local.cache.source_indexes("coco") / "train2017.fixture.json")},
    {"annotations/instances_val2017.json", file_bytes(local.cache.source_indexes("coco") / "val2017.fixture.json")}}
  };
  tar(path, members);
  catalog.coco_annotations.expected_size = std::filesystem::file_size(path);
 }
 BenchmarkTraceSink trace;
 ProgressReporter progress({}, trace);
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = 13});
 CocoAnnotationCache cache(local.cache, catalog.coco_annotations, {CocoSplitAdmission::Optional, CocoSplitAdmission::Required}, 2, 1, 1, {}, trace, &execution);
 mmltk::testsupport::TestGate held("one original split admission");
 std::promise<CocoAnnotationSplit> ready;
 cache.observe_splits([&](CocoAnnotationSplit value) {
  if (!value.index) return;
  if (value.training == hold_train)
   held.receipt().ArriveAndWait();
  else
   ready.set_value(std::move(value));
 });
 auto prepare = std::async(std::launch::async, [&] {
  cache.discover(progress);
  if (cache.pending_download()) {
   auto downloaded = download_artifacts({*cache.pending_download()}, 1, {}).front();
   auto completed = cache.completed_indexes();
   cache.settle(std::move(downloaded), progress, 1, completed, 2);
  }
  return cache.finish();
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] { held.Release(); });
 REQUIRE(held.WaitEntered(5s));
 const auto independent = mmltk::testsupport::await_test_promise(ready, "independently ready original split");
 CHECK(independent.training != hold_train);
 CHECK(independent.terminal);
 REQUIRE(independent.index);
 CHECK(prepare.wait_for(0ms) == std::future_status::timeout);
 held.Release();
 const auto originals = mmltk::testsupport::await_test_future(prepare, "both original splits settle");
 REQUIRE(originals.train);
 REQUIRE(originals.validation);
 CHECK(independent.index->backing == (hold_train ? originals.validation->backing : originals.train->backing));
}
TEST_CASE("new original split admission is withdrawn with its failed source attempt", "[benchmark][coconut][cache][recovery]") {
 bool parser_rejection = false, warm_train = false;
 SECTION("consumed stream failure withdraws newly admitted siblings") {}
 SECTION("sibling parser rejection and replacement reparses a newly admitted split") { parser_rejection = true; }
 SECTION("sibling parser rejection and replacement retains an independently warm split") {
  parser_rejection = true;
  warm_train = true;
 }
 ScopedTempDir root("coco-split-attempt-withdrawal");
 LocalCoconutRecipe local(root.path());
 auto artifact = local_custom_catalog(local).coco_annotations;
 for (const auto split : {"train2017", "val2017"})
  if (!warm_train || std::string_view(split) != "train2017") remove_normalized_annotation_index(local.cache.source_indexes("coco") / (std::string(split) + ".normalized.bin"));
 const auto path = local.cache.source_downloads("coco") / artifact.filename;
 const auto train = file_bytes(local.cache.source_indexes("coco") / "train2017.fixture.json");
 const auto validation = file_bytes(local.cache.source_indexes("coco") / "val2017.fixture.json") + std::string(100000, ' ');
 auto replacement_train = Json::parse(train);
 replacement_train["annotations"][0]["id"] = 71;
 std::array<std::pair<std::string, std::string>, 2> members{{{"annotations/instances_train2017.json", replacement_train.dump()}, {"annotations/instances_val2017.json", validation}}};
 tar(path, members);
 const auto complete = file_bytes(path);
 mmltk::backend::data::testsupport::HttpServer server(complete);
 artifact.url = server.url("repaired-originals");
 artifact.expected_size = 0;
 members[0].second = train;
 if (parser_rejection) {
  // Equal member/archive lengths cannot authorize reuse across explicit repair.
  // The complete second member fails semantic JSON parsing, not transport.
  members[1].second.assign(validation.size(), ' ');
  members[1].second.front() = '{';
 }
 tar(path, members);
 if (!parser_rejection) {
  // The first member is complete. The second has a valid header but a truncated
  // body, so its consumed stream failure invalidates the first admission too.
  std::filesystem::resize_file(path, 512 + ((train.size() + 511) / 512) * 512 + 512 + 100);
 }
 BenchmarkTraceSink trace;
 ProgressReporter progress({}, trace);
 CocoAnnotationCache cache(local.cache, artifact, {CocoSplitAdmission::Optional, CocoSplitAdmission::Required}, 2, 1, 1, {}, trace);
 std::mutex mutex;
 std::vector<CocoAnnotationSplit> train_events;
 cache.observe_splits([&](CocoAnnotationSplit split) {
  if (split.training) {
   const std::lock_guard lock(mutex);
   train_events.push_back(std::move(split));
  }
 });
 cache.discover(progress);
 auto downloaded = download_artifacts({*cache.pending_download()}, 1, {}).front();
 auto completed = cache.completed_indexes();
 cache.settle(std::move(downloaded), progress, 1, completed, 2);
 const auto originals = cache.finish();
 REQUIRE(originals.train);
 REQUIRE(originals.validation);
 CHECK(completed == 2);
 REQUIRE(train_events.size() == (warm_train ? 1 : 3));
 CHECK(train_events[0].terminal);
 REQUIRE(train_events[0].index);
 CHECK(train_events[0].index->boxes.front().annotation_id == 70);
 if (warm_train) {
  CHECK(train_events[0].index->backing == originals.train->backing);
  CHECK(originals.train->boxes.front().annotation_id == 70);
 } else {
  CHECK_FALSE(train_events[1].terminal);
  CHECK_FALSE(train_events[1].index);
  CHECK(train_events[1].generation > train_events[0].generation);
  CHECK(train_events[2].terminal);
  REQUIRE(train_events[2].index);
  CHECK(train_events[2].generation == train_events[1].generation);
  CHECK(train_events[0].index->backing != originals.train->backing);
  CHECK(originals.train->boxes.front().annotation_id == 71);
 }
 CHECK(train_events[0].index->images.size() == 2);  // Immutable withdrawn custody remains readable.
 CHECK(server.requests() == 1);
 server.Check();
}
TEST_CASE("late original withdrawal retains physical work and remaps changed stock membership", "[benchmark][coconut][pipeline][recovery]") {
 using namespace std::chrono_literals;
 bool changed = false;
 SECTION("unchanged stock IDs retain their pixels with replacement labels") {}
 SECTION("changed stock IDs restart deterministic placement") { changed = true; }
 CAPTURE(changed);
 ScopedTempDir root("coconut-stock-original-withdrawal");
 LocalCoconutRecipe local(root.path());
 auto originals = local_custom_catalog(local).coco_annotations;
 for (const auto split : {"train2017", "val2017"}) remove_normalized_annotation_index(local.cache.source_indexes("coco") / (std::string(split) + ".normalized.bin"));
 const auto path = local.cache.source_downloads("coco") / originals.filename;
 const auto train = file_bytes(local.cache.source_indexes("coco") / "train2017.fixture.json") + std::string(100000, ' ');
 const auto validation = file_bytes(local.cache.source_indexes("coco") / "val2017.fixture.json");
 auto replacement = Json::parse(validation);
 const unsigned selected_id = changed ? 10 : 9;
 replacement["images"][0]["id"] = selected_id;
 replacement["images"][0]["file_name"] = coco(selected_id, CoconutImageNamespace::CocoValidation).member;
 replacement["annotations"][0]["image_id"] = selected_id;
 replacement["annotations"][0]["id"] = 901;
 replacement["annotations"][0]["category_id"] = 4;
 std::array<std::pair<std::string, std::string>, 2> members{{{"annotations/instances_val2017.json", replacement.dump()}, {"annotations/instances_train2017.json", train}}};
 tar(path, members);
 const auto replacement_archive = file_bytes(path);
 mmltk::backend::data::testsupport::HttpServer server(replacement_archive);
 originals.url = server.url("replacement-stock-membership");
 originals.expected_size = 0;
 members[0].second = validation;
 tar(path, members);
 std::filesystem::resize_file(path, 512 + ((validation.size() + 511) / 512) * 512 + 512 + 100);
 const std::array<std::pair<std::string, std::string>, 2> physical{
  {{coco(9, CoconutImageNamespace::CocoValidation).member, white_jpeg()}, {coco(10, CoconutImageNamespace::CocoValidation).member, white_jpeg()}}
 };
 replace_physical_images(local.cache, local.catalog, CoconutImageNamespace::CocoValidation, physical);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Stock, true});
 auto catalog = local.selected(config.selection.validation);
 catalog.stock_annotations = originals;
 mmltk::testsupport::TestGate incomplete_train("original source awaits its failing second member");
 std::atomic<bool> held{false}, opened{false}, cancelled{false};
 DeferredCancellationGate observation{cancelled};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Borrow(observation);
 std::promise<void> provisional_placement;
 std::atomic<unsigned> placements{0};
 std::mutex trace_mutex;
 Json latest_events = Json::object();
 config.trace = [&](std::string_view event, std::string_view fields) {
  {
   const std::lock_guard lock(trace_mutex);
   latest_events[std::string(event)] = Json::parse(fields);
  }
  if (event == "benchmark.labels.placed" && Json::parse(fields).at("split") == "val" && placements.fetch_add(1) == 0) provisional_placement.set_value();
  if (event != "benchmark.storage.reserved") return;
  const auto value = Json::parse(fields);
  if (value.at("path") == (local.cache.source_indexes("coco") / "source-json" / "instances_train2017.json").string() && value.at("target") == "extracted annotation staging" && !held.exchange(true))
   DeferredCancellationGate::next() = &incomplete_train;
 };
 std::promise<void> provisional_stock;
 std::mutex reads_mutex;
 std::map<std::uint64_t, unsigned> validation_reads;
 auto compiling = std::async(std::launch::async, [&] {
  compile_benchmark_recipe(config, &catalog, nullptr, {},
   [&](const std::filesystem::path& image_root, std::uint64_t id) {
   if (image_root != local.cache.source_images("coco") / "val2017") return;
   {
    const std::lock_guard lock(reads_mutex);
    ++validation_reads[id];
   }
   if (id == 9 && !opened.exchange(true)) provisional_stock.set_value();
  },
   // The held original stream must coexist with XL discovery's complete
   // parser/physical envelope (over 700 MiB). Tiny-target exclusive admission
   // is exercised by resource cases.
   {.transient_bytes = 1ULL << 30});
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  incomplete_train.Release();
 });
 REQUIRE(incomplete_train.WaitEntered(5s));
 try {
  mmltk::testsupport::await_test_promise(provisional_stock, "stock pixels use the first independently admitted original split", 5s);
  mmltk::testsupport::await_test_promise(provisional_placement, "stock metadata placement precedes unrelated original failure", 5s);
 } catch (const std::exception& error) {
  const auto snapshot = [&] {
   const std::lock_guard lock(trace_mutex);
   return latest_events.dump();
  }();
  INFO(snapshot);
  FAIL(error.what());
 }
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 incomplete_train.Release();
 mmltk::testsupport::await_test_future(compiling, "replacement originals settle selected image placement", 5s);
 const auto compiled = CompiledDataset::open(local.output / "val.bin");
 REQUIRE(compiled.header().num_images == 1);
 const auto slot = compiled_slot(compiled, selected_id, AnnotationSource::Coco);
 REQUIRE(compiled.image_labels(slot).size() == 1);
 CHECK(compiled.image_labels(slot)[0].annotation_id == 901);
 CHECK(compiled.image_labels(slot)[0].source_category_id == 4);
 CHECK(validation_reads[9] == 1);
 CHECK(validation_reads[10] == 1);  // Independent physical traversal also places unlisted speculative images.
 CHECK(placements.load() >= 2);     // The withdrawn original must not authorize its earlier placement.
 CHECK(server.requests() == 1);
 server.Check();
}
TEST_CASE("a quiesced Parquet reader releases its window for independent recovery", "[benchmark][coconut][pipeline][resources]") {
 ScopedTempDir root("coconut-reader-recovery-retirement");
 const std::array<std::uint32_t, 1> pixel{1};
 const std::array shards{root.path() / "groups.parquet"};
 write_coconut_parquet(shards.front(), Json::array({hf_row(1, png(1, 1, pixel), Json::array({segment()}), 1, 1), hf_row(2, png(1, 1, pixel), Json::array({segment()}), 1, 1)}));
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1, .descriptors = 13});
 bool recovered = false;
 CoconutAnnotationRecords records;
 read_coconut_parquet(shards, {}, {}, [&](std::size_t group, const CoconutRecord&, const CoconutAnnotationInput& input) {
  if (input.membership) return;
  if (!group) {
   auto completion = execution.execution().submit({.kind = CompileTaskKind::Recovery, .resources = {65536, 0}, .work = [&](std::size_t, const CompileAllowance&) { recovered = true; }});
   input.admitted(std::move(completion));
  }
 }, false, &execution.execution(), &execution.files(), {}, {}, &records);
 CHECK(recovered);
 records.discard();
 CHECK(execution.execution().try_reserve({1, 13}));
}
TEST_CASE("a Parquet group joins native recovery before JPEG geometry", "[benchmark][coconut][pipeline][resources]") {
 using namespace std::chrono_literals;
 bool fail_labels = false, fail_execution = false;
 SECTION("the next group and labels progress before JPEG geometry") {}
 SECTION("required label conversion failure remains fatal before JPEG geometry") { fail_labels = true; }
 SECTION("shared execution failure settles recovery while originals remain pending") { fail_execution = true; }
 ScopedTempDir root("coconut-native-recovery-boundary");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::Stock);
 std::erase_if(catalog.releases, [](const auto& release) { return release.edition != CoconutEdition::Base; });
 std::erase_if(catalog.images, [](const auto& image) { return image.source != CoconutImageNamespace::CocoTrain; });
 const std::array<std::pair<std::string, std::string>, 3> members{{{coco(7).member, white_jpeg(1, 1)}, {coco(10).member, white_jpeg(1, 1)}, {coco(11).member, white_jpeg(1, 1)}}};
 // CLEANUP-IGNORE: Only calls to existing archive/RAII fixture helpers and a structured binding match; the scenarios own different input generation and assertions.
 replace_physical_images(local.cache, catalog, CoconutImageNamespace::CocoTrain, members);
 PhysicalMembershipFixture fixture(local.cache, catalog, 1);
 auto& [admitted, cancelled, cancellation, execution, physical] = fixture;
 // Retain physical membership across a fresh pipeline attempt, whose geometry
 // arrives independently. Re-resolving a cold source would itself publish the
 // real header and could not model the missing-geometry boundary below.
 const auto images = physical.identity_root(CoconutImageNamespace::CocoTrain);
 execution.retire_source(images);
 // Only the first image has a label. Its oversized target run supplies a real
 // conversion failure after native normalization succeeds in the failure case.
 execution.label_configuration(fail_labels ? 65536 : 1, mmltk::backend::imaging::resample::ImageResizeMode::Stretch);
 const auto publication = execution.source_publication(images, {});
 publication.geometry_ready(10, {1, 1});
 publication.geometry_ready(11, {1, 1});
 const std::array<std::uint32_t, 1> foreground{1}, empty{};
 CoconutImportRequest input;
 input.edition = CoconutEdition::Base;
 input.input_identity = "native-recovery-boundary";
 input.physical_membership = &physical;
 input.execution = &execution;
 input.trace = mmltk::backend::data::testsupport::benchmark_trace();
 input.cancellation = cancellation;
 input.records = std::make_shared<CoconutAnnotationRecords>();
 input.expected_rows = 3;
 input.parquet_shards = {root.path() / "groups.parquet"};
 write_coconut_parquet(input.parquet_shards.front(),
  Json::array({hf_row(7, png(1, 1, foreground), Json::array({segment()}), 1, 1), hf_row(10, png(1, 1, empty), Json::array(), 1, 1), hf_row(11, png(1, 1, empty), Json::array(), 1, 1)}),
  coconut_parquet_schema(), 2);
 CompileExecution::CompletionSource originals;
 std::atomic<bool> original_terminal{false};
 std::promise<void> next_group;
 auto advanced = next_group.get_future();
 input.originals = [&](CoconutImageNamespace, bool, std::stop_token) { return CoconutOriginalReadiness{{}, original_terminal.load(), 1, originals.completion()}; };
 input.input_finished = [&] { next_group.set_value(); };
 auto importing = std::async(std::launch::async, [&] {
  auto result = import_coconut_annotations(input);
  if (fail_labels && !result.empty()) (void)execution.wait_image_labels(images, 7, result[0].image_input_identity(0));
  return result;
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  original_terminal.store(true);
  originals.settle();
  execution.execution().notify_change(CompileChange::Interruption);
 });
 if (fail_labels) {
  CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(importing, "fatal label conversion before JPEG geometry", 5s), "mask run length overflow");
  CHECK_FALSE(execution.geometry(images, 7));
  return;
 }
 mmltk::testsupport::await_test_future(advanced, "all source groups settle while original recovery is pending", 5s);
 CHECK_FALSE(execution.geometry(images, 7));
 if (fail_execution) {
  execution.execution().fail(std::make_exception_ptr(std::runtime_error("independent compiler task failed")));
  CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(importing, "interrupted recovery with pending originals", 5s), "independent compiler task failed");
  CHECK_FALSE(original_terminal.load());
  return;
 }
 original_terminal.store(true);
 originals.settle();
 // Native production and persistence settle without final geometry. Its owning
 // label input survives record retirement and the next recovery batch.
 const auto result = mmltk::testsupport::await_test_future(importing, "all native groups and labels settle before first JPEG geometry", 5s);
 REQUIRE(result.size() == 1);
 CHECK(result[0].index().image_count() == 3);
 CHECK(result[0].index().box_count() == 1);
 CHECK_FALSE(execution.geometry(images, 7));
 const auto dependency = result[0].image_input_identity(0);
 REQUIRE(execution.wait_image_labels(images, 7, dependency));
 publication.geometry_ready(7, {1, 1});
 execution.retire_attempt();
 input.records->discard();
 CHECK(execution.execution().try_reserve({1, 13}));
}
TEST_CASE("Large and XL component identities retain only their consumed physical artifacts", "[benchmark][coconut][cache][recovery]") {
 ScopedTempDir root("coconut-artifact-dependencies");
 const auto cache = BenchmarkCacheLayout::create(root.path() / "cache");
 CoconutRecipeCatalog catalog;
 catalog.releases = {coconut_release_component(CoconutEdition::Large), coconut_release_component(CoconutEdition::XLarge)};
 for (const auto shard : {32U, 17U})
  catalog.images.push_back(
   {CoconutImageNamespace::Objects365V2, static_cast<std::uint16_t>(shard), "patch-" + std::to_string(shard),
    {"physical-" + std::to_string(shard), "unused", "unused.tar", 0, "", BenchmarkDatasetSource::kObjects365V2}});
 std::vector<AdmittedRecipeArchive> admitted;
 for (const auto& origin : catalog.images) admitted.push_back({origin, {.identity = origin.artifact.artifact_id}});
 BenchmarkCompilePipeline execution(1);
 CoconutPhysicalMembership physical(admitted, catalog, false, cache, execution.storage(), execution);
 for (auto& archive : admitted) physical.admitted(archive, archive.download);
 const auto make = [&](CoconutEdition edition, unsigned id, std::uint16_t shard) {
  CoconutComponentBuilder builder;
  builder.edition = edition;
  builder.source = CoconutImageNamespace::Objects365V2;
  builder.annotation_input_identity = "annotations";
  builder.index.source = BenchmarkDatasetSource::kObjects365V2;
  builder.index.split = coconut_component_split(edition, builder.source);
  auto image = objects(id);
  image.shard = shard;
  image.member.replace(0, image.member.find('/'), "patch" + std::to_string(shard));
  image.archive_identity = "physical-" + std::to_string(shard);
  builder.inventory.push_back({image, id, 0});
  builder.index.images.push_back({id, 0, 0, 1, 1, shard, 0});
  CoconutPhysicalDependencies dependencies;
  dependencies.add(image);
  builder.input_identity = coconut_component_input_identity("annotations", builder.source, nullptr, &physical, edition, dependencies);
  return std::move(builder).finish({}, root.path());
 };
 const auto large = make(CoconutEdition::Large, 1, 32), xl = make(CoconutEdition::XLarge, 2, 17);
 CHECK(large.input_identity() == "85e14b83bcc3edd56806c20c1b01d76ca95a74708bd4e2eb944cd77d1e670de2");
 CHECK(xl.input_identity() == "e00877b60b7d5207b6d618da3dece3a828d8408ce674e17122ac57d1cae5c55e");
 const std::array parts{CoconutComponentPart{{large}, 0, 0}};
 const auto merged = merge_coconut_components(parts, "annotations", physical, root.path());
 REQUIRE(merged.size() == 1);
 CHECK(merged.front().input_identity() == large.input_identity());
 CHECK(merged.front().index().annotation_sha256 == large.index().annotation_sha256);
 CHECK(physical.eligible(CoconutEdition::Large, admitted[0]));
 CHECK_FALSE(physical.eligible(CoconutEdition::XLarge, admitted[0]));
 const auto xl_identity = xl.input_identity();
 const auto xl_mapping = xl.index().storage().images.data();
 const auto xl_root = physical.identity_root(CoconutImageNamespace::Objects365V2);
 const auto publication = physical.label_publication(CoconutEdition::XLarge, xl.inventory_image(0).physical);
 publication.geometry_ready(2, {1, 1});
 execution.label_configuration(1, mmltk::backend::imaging::resample::ImageResizeMode::Stretch);
 execution.labels_ready(publication, 2, xl.labels(0), xl.image_input_identity(0));
 physical.withdraw(admitted[0]);
 admitted[0].download.identity = "repaired-large-artifact";
 physical.admitted(admitted[0], admitted[0].download);
 CHECK_FALSE(large.matches_inputs("annotations", physical, nullptr));
 CHECK(large.matches_inputs("annotations", physical, nullptr, false));
 const auto current = merge_coconut_components(parts, "annotations", physical, root.path());
 REQUIRE(current.size() == 1);
 CHECK(current.front().input_identity() == "c47f90f4b81841b04982901f3606d0923b5e9d66bdcb3b7d31b38dca2c3a56b1");
 CHECK(current.front().matches_inputs("annotations", physical, nullptr));
 CHECK_FALSE(current.front().matches_inputs("annotations", physical, nullptr, false));
 CHECK(xl.matches_inputs("annotations", physical, nullptr));
 CHECK(xl.input_identity() == xl_identity);
 CHECK(xl.index().storage().images.data() == xl_mapping);
 REQUIRE(execution.geometry(xl_root, 2));
 REQUIRE(execution.wait_image_labels(xl_root, 2, xl.image_input_identity(0)));
 const auto path = root.path() / "component.bin";
 auto old = component_builder(xl);
 old.input_identity = "old-namespace-bound-identity";
 old.index.annotation_sha256.clear();
 store_coconut_component(path, std::move(old).finish({}, root.path()));
 CHECK_FALSE(load_coconut_component(path, CoconutEdition::XLarge, CoconutImageNamespace::Objects365V2, "annotations", {}, false, &physical));
 store_coconut_component(path, xl);
 REQUIRE(load_coconut_component(path, CoconutEdition::XLarge, CoconutImageNamespace::Objects365V2, "annotations", {}, false, &physical));
 REQUIRE(load_coconut_component(path, CoconutEdition::XLarge, CoconutImageNamespace::Objects365V2, "annotations", {}, false, &physical));
}
TEST_CASE("admitted Parquet rows survive physical consumers and omit reused projections", "[benchmark][coconut][parquet][cache]") {
 ScopedTempDir root("coconut-owned-parquet-rows");
 const std::array<std::uint32_t, 1> pixel{1};
 const auto encoded = png(1, 1, pixel);
 const std::array paths{root.path() / "rows.parquet"};
 write_coconut_parquet(
  paths.front(), Json::array({hf_row(1, encoded, Json::array({segment()}), 1, 1), hf_row(2, encoded, Json::array(), 1, 1), hf_row(3, encoded, Json::array(), 1, 1)}), coconut_parquet_schema(), 2);
 CoconutAnnotationRecords rows;
 read_coconut_parquet(paths, {}, {}, [](std::size_t, const CoconutRecord&, const CoconutAnnotationInput&) {}, true, nullptr, nullptr, {}, {}, &rows);
 REQUIRE(rows.size() == 3);
 CHECK(rows.row(1).metadata_ready());
 CHECK_FALSE(rows.row(1).complete());
 CHECK_THROWS_WITH(read_coconut_parquet(paths, {}, {},
                    [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput&) {
  if (record.image_id == 2) throw std::runtime_error("physical consumer rejected its member");
 }, false, nullptr, nullptr, {}, {}, &rows),
  "physical consumer rejected its member");
 REQUIRE(rows.row(0).complete());
 REQUIRE(rows.row(1).complete());
 CHECK(rows.record(1).segments.empty());
 CHECK_FALSE(rows.row(2).complete());
 const auto* first = &rows.record(0);
 const auto* segments = first->segments.data();
 CoconutImportLimits remaining;
 remaining.max_segments = 0;  // Already admitted things are borrowed, including in a partly reused source.
 std::size_t consumed = 0;
 read_coconut_parquet(paths, remaining, {}, [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput& input) {
  CHECK_FALSE(input.membership);
  CHECK_FALSE(input.png.empty());
  ++consumed;
  if (record.image_id == 1) {
   CHECK(&record == first);
   CHECK(record.segments.data() == segments);
  }
 }, false, nullptr, nullptr, {}, {}, &rows);
 CHECK(consumed == 3);
 CHECK(rows.row(2).complete());
 CHECK(rows.row(0).segment_ordinal() == 0);
 CHECK(rows.row(1).segment_ordinal() == 1);
 CHECK(rows.row(2).segment_ordinal() == 1);
 rows.rebase_parquet_segments();
 CHECK(rows.row(2).segment_ordinal() == 1);
 CHECK(rows.record(2).first_segment_ordinal == 0);
 std::filesystem::remove(paths.front());
 consumed = 0;
 read_coconut_parquet(paths, remaining, {}, [&](std::size_t, const CoconutRecord&, const CoconutAnnotationInput& input) {
  CHECK_FALSE(input.membership);
  CHECK(input.png.empty());
  CHECK_FALSE(input.backing);
  ++consumed;
 }, false, nullptr, nullptr, {}, {}, &rows, [](const CoconutRecord&) { return true; });
 CHECK(consumed == 3);
 rows.discard();
 CHECK(rows.size() == 0);
}
TEST_CASE("native RGB segment runs preserve exact IDs across dense prefixes unaligned boundaries and tails", "[benchmark][coconut][annotations][resources]") {
 constexpr std::array<std::uint32_t, 5> ids{0x010203, 0x020203, 0x020303, 0x020304, 0xffffff};
 constexpr std::uint32_t unknown = 0x800001;
 CoconutNativeWorkspace workspace(CoconutImportLimits{});
 std::uint64_t elapsed_ns = 0, total_pixels = 0, total_runs = 0, retained_peak = 0;
 for (const std::uint32_t width : {1U, 2U, 10U, 11U, 18U, 19U, 20U, 21U, 31U, 32U, 33U, 63U, 64U, 65U, 127U, 4097U}) {
  constexpr std::uint32_t height = 4;
  CoconutRecord record;
  record.image_id = 7;
  record.width = width;
  record.height = height;
  for (const auto id : ids) record.segments.push_back({.id = id, .category_id = 1, .isthing = true});
  std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
   pixels[i] = ids[(i / 37U + i / width) % ids.size()];
   if (i % 113U == 0)
    pixels[i] = 0;
   else if (i % 179U == 0)
    pixels[i] = unknown;
  }
  std::array<std::vector<RLEPair>, ids.size()> reference;
  std::uint32_t first_unknown = 0;
  for (std::size_t first = 0; first < pixels.size();) {
   auto end = first + 1;
   while (end < pixels.size() && pixels[end] == pixels[first]) ++end;
   const auto found = std::ranges::find(ids, pixels[first]);
   if (found != ids.end())
    reference[static_cast<std::size_t>(found - ids.begin())].push_back({static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(end - first)});
   else if (pixels[first] && !first_unknown)
    first_unknown = pixels[first];
   first = end;
  }
  const auto encoded = png(static_cast<int>(width), height, pixels);
  const auto input = std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
  const auto dimensions = workspace.admit_png(record, input);
  const auto started = std::chrono::steady_clock::now();
  workspace.decode(record, input, dimensions);
  elapsed_ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
  retained_peak = std::max(retained_peak, workspace.retained_bytes());
  total_pixels += pixels.size();
  CHECK(workspace.unlisted_segment() == first_unknown);
  auto support = workspace.support(record.segments.size());
  REQUIRE(support.size() == ids.size());
  for (std::size_t segment = 0; segment < ids.size(); ++segment) {
   const auto actual = support[segment].runs.view();
   REQUIRE(actual.size() == reference[segment].size());
   std::uint64_t area = 0;
   for (std::size_t run = 0; run < actual.size(); ++run) {
    CHECK(actual[run].start == reference[segment][run].start);
    CHECK(actual[run].length == reference[segment][run].length);
    area += actual[run].length;
   }
   CHECK(support[segment].area == area);
   CHECK(support[segment].valid);
   total_runs += actual.size();
  }
 }
 std::printf(
  "native_png_fixture pixels=%llu runs=%llu decode_support_ns=%llu retained_workspace_peak=%llu reference=independent_scalar_ID_runs timing_scope=PNG_decode_and_support_not_SIMD_only "
  "production_baseline=unavailable\n",
  static_cast<unsigned long long>(total_pixels), static_cast<unsigned long long>(total_runs), static_cast<unsigned long long>(elapsed_ns), static_cast<unsigned long long>(retained_peak));
 workspace.retire();
 CHECK(workspace.retained_bytes() == 0);
}
TEST_CASE("native image custody survives workspace reuse and physical retirement", "[benchmark][coconut][annotations][resources]") {
 const bool staged = GENERATE(false, true);
 ScopedTempDir root("native-image-mapped-custody");
 detail::StorageReservationPool storage(root.path(), {});
 CoconutStaging staging(root.path(), &storage);
 CoconutNativeWorkspace workspace(CoconutImportLimits{});
 CoconutRecord wide;
 wide.image_id = 7;
 wide.width = 64;
 wide.height = 1;
 wide.first_segment_ordinal = 4;
 wide.segments = {{.id = 1, .category_id = 1, .isthing = true}, {.id = 2, .category_id = 1, .isthing = true}};
 std::array<std::uint32_t, 64> pixels{};
 for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = 1 + i % 2;
 const auto encoded = png(64, 1, pixels);
 const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
 const auto dimensions = workspace.admit_png(wide, bytes);
 workspace.decode(wide, bytes, dimensions);
 const auto decoded_capacity = workspace.retained_bytes();
 REQUIRE(decoded_capacity > 0);
 auto lineage = std::make_shared<CoconutNativeLineage>();
 lineage->component.source = CoconutImageNamespace::CocoTrain;
 lineage->component.annotation_input_identity = "native-custody";
 lineage->index.source = coconut_index_source(lineage->component.source);
 lineage->index.split = coconut_component_split(lineage->component.edition, lineage->component.source);
 auto product = workspace.finish(wide, coco(7), dimensions, lineage, {});
 const auto capacity = workspace.retained_bytes();
 CHECK(capacity >= decoded_capacity);
 const auto identity = product->input_identity();
 if (staged) {
  const auto original = product;
  product = CoconutNativeImage::stage(std::move(product), staging);
  CHECK(product->input_identity() == identity);
  CHECK(&product->lineage() == &original->lineage());
  CHECK(std::memcmp(product->boxes().data(), original->boxes().data(), original->boxes().size_bytes()) == 0);
  CHECK(std::memcmp(product->runs().data(), original->runs().data(), original->runs().size_bytes()) == 0);
  CHECK(CoconutNativeImage::stage(product, staging) == product);
  staging.retire();
 }
 // Independent v1 image hashes pin the numeric namespace mapping and the
 // deliberate unlabeled/train equivalence; physical archive/shard is excluded.
 {
  ScopedTempDir identities("native-component-identities");
  struct IdentityCase {
   CoconutImageNamespace source;
   BenchmarkDatasetSource index_source;
   std::string_view suffix, hash;
  };
  const std::array cases{
   IdentityCase{CoconutImageNamespace::CocoTrain, BenchmarkDatasetSource::kCoco2017, "0", "461ecd8b7dd078df9bf761a44b0d7a78b5b6570b46dcb694e43f2b42f0b5e410"},
   IdentityCase{CoconutImageNamespace::CocoUnlabeled, BenchmarkDatasetSource::kCoco2017, "1", "461ecd8b7dd078df9bf761a44b0d7a78b5b6570b46dcb694e43f2b42f0b5e410"},
   IdentityCase{CoconutImageNamespace::CocoValidation, BenchmarkDatasetSource::kCoco2017, "2", "138c6d48894ee0f14a135186f0e0f777a0d3b7d92f84142b67e32a25a23851f0"},
   IdentityCase{CoconutImageNamespace::Objects365V1, BenchmarkDatasetSource::kObjects365V2, "3", "eaeca804f0b1697b075bdc8b0d676ae3487b233b983780b438bb626c768894f9"},
   IdentityCase{CoconutImageNamespace::Objects365V2, BenchmarkDatasetSource::kObjects365V2, "4", "2ee9962bfab217c4b4aac6babd090b815531ae68b0433d3dbabb29f22061e3a4"}
  };
  const std::array editions{
   std::pair{CoconutEdition::Base, "coconut-0-"}, std::pair{CoconutEdition::RelabeledValidation, "coconut-1-"}, std::pair{CoconutEdition::Large, "coconut-2-"},
   std::pair{CoconutEdition::XLarge, "coconut-3-"}, std::pair{CoconutEdition::ObjectsValidation, "coconut-4-"}
  };
  for (const auto& item : cases) {
   CAPTURE(item.suffix);
   for (const auto& [edition, prefix] : editions) CHECK(coconut_component_split(edition, item.source) == std::string(prefix) + std::string(item.suffix));
   CHECK(coconut_index_source(item.source) == item.index_source);
   const std::array images{item.index_source == BenchmarkDatasetSource::kCoco2017 ? coco(7, item.source) : objects(7, item.source)};
   auto rebound = CoconutNativeImage::bind(product, images.front());
   CHECK(rebound->input_identity() == item.hash);
   CHECK(rebound->lineage().index.source == item.index_source);
   CHECK(rebound->lineage().index.split == "coconut-0-" + std::string(item.suffix));
   if (staged) CHECK(rebound->runs().data() == product->runs().data());
   auto moved = images.front();
   ++moved.shard;
   const auto relocated = CoconutNativeImage::bind(rebound, moved);
   CHECK(relocated->input_identity() == item.hash);
   CHECK(relocated->inventory().physical == moved);
   moved.archive_identity = "replacement archive";
   CHECK(CoconutNativeImage::bind(relocated, moved)->input_identity() == item.hash);
   CHECK(rebound->inventory().physical == images.front());
   CoconutPhysicalMembership membership(images);
   CoconutPhysicalDependencies dependencies;
   dependencies.add(images.front());
   CoconutComponentBuilder builder;
   static_cast<CoconutComponentMetadata&>(builder) = rebound->lineage().component;
   builder.input_identity = coconut_component_input_identity(builder.annotation_input_identity, builder.source, nullptr, &membership, builder.edition, dependencies);
   static_cast<NormalizedAnnotationMetadata&>(builder.index) = rebound->lineage().index;
   builder.index.images.push_back(rebound->image());
   builder.index.boxes.assign(rebound->boxes().begin(), rebound->boxes().end());
   builder.index.mask_rle_pairs.assign(rebound->runs().begin(), rebound->runs().end());
   builder.inventory.push_back(rebound->inventory());
   const auto component = std::move(builder).finish({}, identities.path());
   CHECK(component.image_input_identity(0) == rebound->input_identity());
   const auto path = identities.path() / (std::string(item.suffix) + ".bin");
   store_coconut_component(path, component);
   const auto loaded = load_coconut_component(path, component.edition(), component.source(), "native-custody", {}, false, &membership);
   REQUIRE(loaded);
   CHECK(loaded->image_input_identity(0) == item.hash);
   CHECK(loaded->index().source == item.index_source);
   CHECK(loaded->index().split == rebound->lineage().index.split);
  }
 }
 auto physical = coco(7, CoconutImageNamespace::CocoUnlabeled);
 physical.shard = 3;
 const auto* staged_runs = product->runs().data();
 product = CoconutNativeImage::bind(std::move(product), physical);
 CHECK(product->inventory().physical == physical);
 CHECK(product->image().source_shard == 3);
 CHECK(product->lineage().component.source == CoconutImageNamespace::CocoUnlabeled);
 CHECK(lineage->component.source == CoconutImageNamespace::CocoTrain);
 CHECK(product->input_identity() == identity);
 if (staged) CHECK(product->runs().data() == staged_runs);
 CHECK(CoconutNativeImage::bind(product, physical) == product);
 CHECK(workspace.retained_bytes() == capacity);
 REQUIRE(product->boxes().size() == 2);
 CHECK(product->runs().size() == 64);
 CHECK(product->boxes()[0].source_ordinal == 4);
 CHECK(product->boxes()[1].source_ordinal == 5);
 const std::weak_ptr<const CoconutNativeImage> lifetime = product;
 {
  auto retained = CoconutNativeImage::read(product);
  CHECK(retained.view.storage().mask_rle_pairs.data() == product->runs().data());
  product.reset();
  CoconutRecord small;
  small.image_id = 8;
  small.width = small.height = 1;
  small.segments = {{.id = 1, .category_id = 1, .isthing = true}};
  const std::array<std::uint32_t, 1> pixel{1};
  const auto tiny = png(1, 1, pixel);
  const auto input = std::span(reinterpret_cast<const std::uint8_t*>(tiny.data()), tiny.size());
  workspace.decode(small, input, workspace.admit_png(small, input));
  CHECK(workspace.retained_bytes() == capacity);  // Inactive second-segment capacity remains owned and charged.
  auto next = workspace.finish(small, coco(8), {1, 1}, lineage, {});
  CHECK(next->runs().size() == 1);
  CHECK(retained.view.run_count() == 64);
  workspace.retire();
  CHECK(workspace.retained_bytes() == 0);
  CHECK_FALSE(lifetime.expired());
  if (staged) CHECK_FALSE(std::filesystem::is_empty(root.path()));
  CHECK(retained.view.image(0).width == 64);
  CHECK(retained.view.storage().boxes[1].source_ordinal == 5);
 }
 CHECK(lifetime.expired());
 CHECK(std::filesystem::is_empty(root.path()));
 CHECK(storage.outstanding() == 0);
}
TEST_CASE("prepared immutable ranges preserve alignment parallel claims and last borrower custody", "[benchmark][coconut][annotations][resources]") {
 ScopedTempDir root("coconut-prepared-ranges");
 detail::StorageReservationPool storage(root.path(), {});
 CoconutStaging staging(root.path(), &storage, 1024);
 CHECK_THROWS_AS(CoconutStaging::Request(1, 3), std::invalid_argument);
 CoconutStaging::Request first(17, 8);
 CHECK_FALSE(staging.try_claim(first));
 CHECK_THROWS_AS(first.target(), std::logic_error);
 staging.prepare(first);
 std::memset(first.target().data(), 0x31, first.target().size());
 auto retained = first.bytes();
 std::vector<std::future<CoconutStaging::Bytes>> copies;
 for (unsigned value = 1; value <= 8; ++value)
  copies.push_back(std::async(std::launch::async, [&, value] {
   CoconutStaging::Request range(31, 16);
   require_condition(staging.try_claim(range), "ordinary concurrent range did not share its prepared block");
   std::memset(range.target().data(), static_cast<int>(value), range.target().size());
   return range.bytes();
  }));
 std::set<const std::byte*> addresses;
 for (std::size_t i = 0; i < copies.size(); ++i) {
  auto range = mmltk::testsupport::await_test_future(copies[i], "disjoint prepared range fill");
  CHECK(range.owner == retained.owner);
  CHECK(reinterpret_cast<std::uintptr_t>(range.values.data()) % 16 == 0);
  CHECK(std::ranges::all_of(range.values, [&](auto value) { return value == std::byte(i + 1); }));
  CHECK(addresses.insert(range.values.data()).second);
 }
 CoconutStaging::Request abandoned(32, 32);
 REQUIRE(staging.try_claim(abandoned));
 const auto* hole = abandoned.target().data();
 abandoned = {};  // A failed producer's claim remains an unpublished hole.
 CoconutStaging::Request after(1, 1);
 REQUIRE(staging.try_claim(after));
 CHECK(after.target().data() >= hole + 32);
 CoconutStaging::Request oversized(2049, 64);
 CHECK_FALSE(staging.try_claim(oversized));
 staging.prepare(oversized);
 CHECK(oversized.bytes().owner != retained.owner);
 CHECK(oversized.target().size() == 2049);
 CHECK(reinterpret_cast<std::uintptr_t>(oversized.target().data()) % 64 == 0);
 auto survivor = oversized.bytes();
 std::memset(oversized.target().data(), 0x7a, oversized.target().size());
 staging.retire();
 first = {};
 after = {};
 oversized = {};
 CHECK(std::ranges::all_of(retained.values, [](auto value) { return value == std::byte{0x31}; }));
 CHECK(survivor.values.front() == std::byte{0x7a});
 CHECK(survivor.values.back() == std::byte{0x7a});
 CHECK(std::distance(std::filesystem::directory_iterator(root.path()), std::filesystem::directory_iterator{}) == 2);
 retained = {};
 survivor = {};
 CHECK(std::filesystem::is_empty(root.path()));
 CHECK(storage.outstanding() == 0);
}
TEST_CASE("compact staged support populates supplied native scratch without intermediate ownership", "[benchmark][coconut][recovery][resources]") {
 ScopedTempDir root("coconut-compact-support");
 CoconutStaging staging(root.path());
 CoconutRecord record;
 record.image_id = 7;
 record.width = 3;
 record.height = 1;
 record.segments = {{.id = 1, .category_id = 1, .isthing = true}, {.id = 2, .category_id = 1, .isthing = true}};
 const std::array<std::uint32_t, 3> pixels{1, 2, 1};
 const auto encoded = png(3, 1, pixels);
 const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
 auto lineage = std::make_shared<CoconutNativeLineage>();
 lineage->component.source = CoconutImageNamespace::CocoTrain;
 CoconutNativeWorkspace decoded(CoconutImportLimits{});
 decoded.decode(record, bytes, {3, 1});
 CoconutNativeOutput output;
 auto original = decoded.finish(record, coco(7), {3, 1}, lineage, {}, &output);
 CoconutNativeSupport support;
 output.retain_support(decoded.support(record.segments.size()), support);
 staging.prepare(output.request());
 output.fill();
 output = {};
 decoded.retire();
 staging.retire();
 REQUIRE(support.segments.size() == 2);
 REQUIRE(support.runs.size() == 3);
 CHECK(support.segments[0].area == 2);
 CHECK(support.segments[1].area == 1);
 std::array<std::byte, 65536> supplied{};
 CoconutNativeWorkspace recovered(CoconutImportLimits{});
 recovered.prepare_support(supplied);
 recovered.borrow_support(record, support, {3, 1});
 const auto held = recovered.support(record.segments.size());
 CHECK(held[0].runs.data() == support.runs.data());
 CHECK(held[1].runs.data() == support.runs.data() + support.segments[1].first_run);
 support = {};
 original.reset();
 CHECK_FALSE(std::filesystem::is_empty(root.path()));
 const auto product = recovered.finish(record, coco(7), {3, 1}, lineage, {});
 recovered.release_supplied();
 CHECK(std::filesystem::is_empty(root.path()));
 REQUIRE(product->runs().size() == 3);
 CHECK(product->boxes()[0].original_area == 2);
}
TEST_CASE("file preparation rejects invalid destinations and CPU lane waits before range publication", "[benchmark][coconut][resources]") {
 ScopedTempDir root("coconut-preparation-failure");
 BenchmarkCompilePipeline pipeline(1, {}, {.transient_bytes = 1, .descriptors = 13});
 const auto path = root.path() / "not-a-directory";
 mmltk::testsupport::write_text_file(path, "occupied");
 CoconutStaging failed(path, &pipeline.storage());
 CoconutStaging::Request request(32, 8);
 CHECK_THROWS(failed.prepare(request, &pipeline.execution(), &pipeline.files()));
 CHECK_FALSE(request.ready());
 CHECK(pipeline.execution().resource_usage().descriptors == 0);
 CoconutStaging staging(root.path(), &pipeline.storage());
 CHECK_THROWS_AS(staging.prepare(request, &pipeline.execution()), std::logic_error);
 std::atomic<bool> rejected{false};
 pipeline.execution().run(CompileTaskKind::Normalize, {}, [&](std::size_t) {
  try {
   staging.prepare(request, &pipeline.execution(), &pipeline.files());
  } catch (const std::logic_error&) { rejected = true; }
 });
 CHECK(rejected.load());
 CHECK_FALSE(request.ready());
 staging.prepare(request, &pipeline.execution(), &pipeline.files());
 CHECK(request.ready());
 CHECK(pipeline.execution().resource_usage().descriptors == 0);
}
TEST_CASE("staged segments share immutable backing and detach only the mutated copy", "[benchmark][coconut][annotations][resources]") {
 ScopedTempDir root("coconut-segment-custody");
 detail::StorageReservationPool storage(root.path(), {});
 CoconutStaging staging(root.path(), &storage);
 CoconutSegments original{{.id = 1, .category_id = 18, .isthing = true, .area = 2.0, .bbox = std::array<double, 4>{0, 0, 1, 2}}};
 original.stage(staging);
 staging.retire();
 REQUIRE(std::distance(std::filesystem::directory_iterator(root.path()), std::filesystem::directory_iterator{}) == 1);
 const auto path = std::filesystem::directory_iterator(root.path())->path();
 auto reader = original;
 auto changed = original;
 CHECK(reader.data() == original.data());
 changed[0].id = 2;
 CHECK(changed.data() != original.data());
 CHECK(reader.view()[0].id == 1);
 CHECK(changed.view()[0].id == 2);
 CHECK(changed.view()[0].area == reader.view()[0].area);
 CHECK(changed.view()[0].bbox == reader.view()[0].bbox);
 original = {};
 CHECK(std::filesystem::exists(path));
 CHECK(reader.view()[0].category_id == 18);
 reader = {};
 CHECK_FALSE(std::filesystem::exists(path));
 changed.stage(staging);
 staging.retire();
 CHECK(changed.view()[0].id == 2);
 changed = {};
 CoconutSegments empty;
 empty.stage(staging);
 CHECK(std::filesystem::is_empty(root.path()));
 CHECK(storage.outstanding() == 0);
}
TEST_CASE("borrowed native support retains the complete temporary backing", "[benchmark][coconut][recovery][resources]") {
 ScopedTempDir root("coconut-support-custody");
 detail::StorageReservationPool storage(root.path(), {});
 const std::array<RLEPair, 2> input{{{0, 2}, {4, 1}}};
 auto owner = std::make_shared<BenchmarkMappedArtifact>(storage, root.path() / "support", sizeof(input), "native support fixture");
 const auto path = owner->path();
 owner->write(input.data(), sizeof(input), 0);
 const auto runs = std::span(static_cast<const RLEPair*>(owner->map_readonly()), input.size());
 const std::weak_ptr<const BenchmarkMappedArtifact> lifetime = owner;
 CoconutSupportRuns support;
 support.borrow(runs, owner);
 owner.reset();
 CHECK_FALSE(lifetime.expired());
 CHECK(std::filesystem::exists(path));
 auto copied = support;
 support.clear();
 CHECK(copied.retained_bytes() == 0);
 REQUIRE(copied.size() == 2);
 CHECK(copied[1].start == 4);
 CHECK(copied[1].length == 1);
 CHECK_FALSE(lifetime.expired());
 copied.clear();
 CHECK(lifetime.expired());
 CHECK_FALSE(std::filesystem::exists(path));
 CHECK(storage.outstanding() == 0);
}
TEST_CASE("support run accounting follows retained capacity and ownership transfer", "[benchmark][coconut][recovery][resources]") {
 std::uint64_t owned = 0;
 CoconutSupportRuns source;
 source.account(&owned);
 for (std::uint32_t i = 0; i < 64; ++i) source.append(i * 2, i * 2 + 1);
 const auto high_water = source.retained_bytes();
 CHECK(owned == high_water);
 source.clear();
 CHECK(owned == high_water);
 source.append(0, 1);
 CoconutSupportRuns moved(std::move(source));
 CHECK(owned == high_water);
 CHECK(moved.size() == 1);
 auto backing = std::make_shared<const std::vector<RLEPair>>(std::initializer_list<RLEPair>{{4, 2}});
 moved.borrow(*backing, backing);
 CHECK(owned == high_water);
 CHECK(moved.data() == backing->data());
 std::pmr::vector<RLEPair> replacement{{8, 1}};
 const auto replacement_bytes = replacement.capacity() * sizeof(RLEPair);
 moved.replace(replacement);
 CHECK(owned == replacement_bytes);
 CHECK(replacement.capacity() * sizeof(RLEPair) == high_water);
 moved.account(nullptr);
 CHECK(owned == 0);
 CHECK(moved.retained_bytes() == replacement_bytes);
}
TEST_CASE("recovery workspaces retain their immutable original lookup across batches", "[benchmark][coconut][recovery][resources]") {
 std::unique_ptr<CoconutMaskRecovery> worker;
 std::weak_ptr<NormalizedAnnotationBacking> backing;
 const std::array<RLEPair, 1> runs{{{0, 2}}};
 {
  auto original = fixture_index(recovery_originals(7, runs));
  backing = original.backing;
  CoconutRecoveryOriginals lookup(&original, nullptr);
  CoconutMaskRecovery first(lookup);
  worker = first.make_workspace();
 }
 CHECK_FALSE(backing.expired());
 CoconutRecord record;
 record.image_id = 7;
 record.segments = {{.id = 10, .category_id = 18, .isthing = true}};
 std::vector<CoconutSegmentSupport> support(1);
 std::uint64_t capacity = 0;
 for (unsigned batch = 0; batch < 3; ++batch) {
  support[0].area = 0;
  support[0].bounds = {};
  support[0].runs.clear();
  support[0].recovered.reset();
  support[0].carved = false;
  CoconutRecoveryImage facts{7, 0, {}};
  worker->apply(CoconutImageNamespace::CocoTrain, record, 3, 3, support, facts);
  REQUIRE(facts.objects.size() == 1);
  CHECK(support[0].runs.retained_bytes() == 0);
  if (!batch)
   capacity = worker->retained_bytes();
  else
   CHECK(worker->retained_bytes() == capacity);
 }
 worker->retire_scratch();
 CHECK(worker->retained_bytes() == 0);
 CHECK_FALSE(backing.expired());
 worker.reset();
 CHECK_FALSE(backing.expired());
 support.clear();
 CHECK(backing.expired());
}
TEST_CASE("ready recovery that fits retains its forward Parquet reader on one CPU", "[benchmark][coconut][pipeline][resources]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("coconut-reader-recovery-coexistence");
 const std::array<std::uint32_t, 1> empty{};
 const auto encoded = png(1, 1, empty);
 Json rows = Json::array();
 std::vector<CoconutPhysicalImage> physical;
 for (unsigned id = 1; id <= 129; ++id) {
  rows.push_back(hf_row(id, encoded, id <= 64 ? Json::array({segment()}) : Json::array(), 1, 1));
  physical.push_back(coco(id));
 }
 const auto path = root.path() / "rows.parquet";
 write_coconut_parquet(path, rows, coconut_parquet_schema(), 64);
 std::atomic<bool> cancelled{false}, terminal{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 4ULL << 30, .descriptors = 13}, cancellation);
 auto input = request(physical);
 input.execution = &execution;
 input.cancellation = cancellation;
 input.records = std::make_shared<CoconutAnnotationRecords>();
 input.parquet_shards = {path};
 CompileExecution::CompletionSource originals;
 std::promise<void> completed_native;
 auto completed = completed_native.get_future();
 std::promise<void> later_group;
 auto advanced = later_group.get_future();
 input.originals = [&](CoconutImageNamespace, bool, std::stop_token) { return CoconutOriginalReadiness{{}, terminal.load(), 1, originals.completion()}; };
 input.preparation_progress = [&](CoconutPreparationStage stage, std::uint64_t count) {
  if (stage == CoconutPreparationStage::Annotations && count == 128) {
   terminal.store(true);
   originals.settle();
   std::filesystem::rename(path, root.path() / "opened-generation.parquet");
  }
 };
 input.input_finished = [&] { later_group.set_value(); };
 input.progress = [&](std::uint64_t count) {
  if (count == 64) completed_native.set_value();
 };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  terminal.store(true);
  originals.settle();
  execution.execution().notify_change(CompileChange::Interruption);
 });
 mmltk::testsupport::await_test_future(advanced, "next group while completed recovery retains no blocking input demand", 10s);
 const auto result = mmltk::testsupport::await_test_future(importing, "forward reader and all native rows settle", 10s);
 mmltk::testsupport::await_test_future(completed, "recovery completed independently");
 REQUIRE(result.size() == 1);
 CHECK(result.front().index().image_count() == 129);
 CHECK(result.front().index().box_count() == 0);
 CHECK(result.front().index().rejected.degenerate_boxes == 64);
 input.records->discard();
 CHECK(execution.execution().try_reserve({execution.execution().transient_target(), 13}));
}
TEST_CASE("parallel JSON rows preserve joins and all declared segment prefixes", "[benchmark][coconut][annotations][pipeline]") {
 ScopedTempDir root("coconut-parallel-json-rows");
 Json document{{"categories", category_catalog()}, {"images", Json::array()}, {"annotations", Json::array()}};
 std::vector<CoconutPhysicalImage> physical;
 std::vector<std::pair<std::string, std::string>> masks;
 std::vector<std::uint64_t> prefixes(16);
 const std::array<std::uint32_t, 1> pixel{1};
 const auto encoded = png(1, 1, pixel);
 for (unsigned id = 1; id <= 16; ++id) {
  physical.push_back(objects(id));
  const auto stem = std::filesystem::path(physical.back().member).stem().string();
  document["images"].push_back({{"id", id}, {"width", 1}, {"height", 1}, {"file_name", stem + ".jpg"}});
  masks.emplace_back("panoptic_object365/" + stem + ".png", encoded);
 }
 std::uint64_t prefix = 0;
 for (unsigned id = 16; id != 0; --id) {
  const auto stem = std::filesystem::path(physical[id - 1].member).stem().string();
  Json segments = Json::array({segment()});
  // Repeated wide/small rows exercise retained nested segment and bbox slots.
  for (unsigned i = 0; i < (id % 2 ? 0U : 128U); ++i) {
   auto value = segment(i + 2, 200, false);
   value["bbox"] = {0, 0, 1, 1};
   segments.push_back(std::move(value));
  }
  prefixes[id - 1] = prefix;
  prefix += segments.size();
  document["annotations"].push_back({{"image_id", id}, {"file_name", stem + ".png"}, {"segments_info", std::move(segments)}});
 }
 const auto json = root.path() / "annotations.json", archive = root.path() / "masks.tar";
 json_file(json, document);
 tar(archive, masks);
 std::optional<CoconutComponent> expected;
 for (const auto workers : {1U, 2U}) {
  if (workers > mmltk::common::system::allowed_cpu_set().size()) continue;
  BenchmarkCompilePipeline execution(workers, {}, {.transient_bytes = 4ULL << 30, .descriptors = 24});
  auto input = request(physical, CoconutEdition::Large);
  input.execution = &execution;
  input.records = std::make_shared<CoconutAnnotationRecords>();
  input.annotation_json = json;
  input.mask_archive = archive;
  auto result = import_coconut_annotations(input);
  REQUIRE(result.size() == 1);
  const auto& component = result.front();
  REQUIRE(component.index().box_count() == 16);
  CHECK(component.index().rejected.raw_records == prefix);
  for (std::size_t i = 0; i < 16; ++i) {
   CHECK(component.inventory_image(i).source_ordinal == 15 - i);
   CHECK(component.index().storage().boxes[i].source_ordinal == prefixes[i]);
  }
  if (!expected)
   expected = component;
  else {
   CHECK(component.index().annotation_sha256 == expected->index().annotation_sha256);
   CHECK(std::ranges::equal(component.inventory(), expected->inventory()));
   CHECK(std::memcmp(component.index().storage().boxes.data(), expected->index().storage().boxes.data(), 16 * sizeof(NormalizedBox)) == 0);
  }
 }
}
TEST_CASE("JSON physical failure retains successfully parsed immutable rows", "[benchmark][coconut][annotations][cache]") {
 ScopedTempDir root("coconut-json-physical-retry");
 const std::array physical{objects(7), objects(8)};
 Json document{{"categories", category_catalog()}, {"images", Json::array()}, {"annotations", Json::array()}};
 for (const auto& image : physical) {
  const auto stem = std::filesystem::path(image.member).stem().string();
  document["images"].push_back({{"id", image.image_id}, {"width", 1}, {"height", 1}, {"file_name", stem + ".jpg"}});
  document["annotations"].push_back({{"image_id", image.image_id}, {"file_name", stem + ".png"}, {"segments_info", Json::array({segment()})}});
 }
 const auto path = root.path() / "annotations.json";
 json_file(path, document);
 auto retained = std::make_shared<CoconutAnnotationRecords>();
 auto first = request(std::span(physical).first(1), CoconutEdition::Large);
 first.metadata_only = true;
 first.annotation_json = path;
 first.records = retained;
 CHECK_THROWS_AS(import_coconut_annotations(first), CoconutPhysicalMembershipError);
 REQUIRE(retained->size() == 2);
 REQUIRE(retained->row(0).complete());
 REQUIRE(retained->row(1).complete());
 const auto* segments = retained->record(0).segments.data();
 auto retry = request(physical, CoconutEdition::Large);
 retry.metadata_only = true;
 retry.annotation_json = path;
 retry.records = retained;
 retry.limits.max_segments = 0;
 const auto result = import_coconut_annotations(retry);
 REQUIRE(result.size() == 1);
 CHECK(result.front().index().image_count() == 2);
 CHECK(retained->record(0).segments.data() == segments);
 CHECK(retained->row(1).segment_ordinal() == 1);
}
TEST_CASE("recovery batches publish independent labels before withheld geometry", "[benchmark][coconut][pipeline][labels][recovery]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("coconut-label-recovery-batches");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::Stock);
 std::erase_if(catalog.releases, [](const auto& release) { return release.edition != CoconutEdition::Base; });
 std::erase_if(catalog.images, [](const auto& image) { return image.source != CoconutImageNamespace::CocoTrain; });
 const std::array<std::uint32_t, 5> ids{7, 10, 11, 12, 13};
 std::vector<std::pair<std::string, std::string>> members;
 Json rows = Json::array();
 const std::array<std::uint32_t, 1> foreground{1};
 for (auto id : ids) {
  members.emplace_back(coco(id).member, white_jpeg(1, 1));
  rows.push_back(hf_row(id, png(1, 1, foreground), Json::array({segment()}), 1, 1));
 }
 replace_physical_images(local.cache, catalog, CoconutImageNamespace::CocoTrain, members);
 PhysicalMembershipFixture fixture(local.cache, catalog, 1);
 auto& [admitted, cancelled, cancellation, execution, physical] = fixture;
 std::map<std::uint64_t, unsigned> conversions;
 execution.label_configuration(1, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, [&](const auto&, auto id, const auto&) { ++conversions[id]; });
 const auto images = physical.identity_root(CoconutImageNamespace::CocoTrain);
 execution.retire_source(images);
 const auto publication = execution.source_publication(images, {});
 for (auto id : ids)
  if (id != ids.front()) publication.geometry_ready(id, {1, 1});
 CoconutImportRequest input;
 input.edition = CoconutEdition::Base;
 input.input_identity = "label-recovery-batches";
 input.physical_membership = &physical;
 input.execution = &execution;
 input.cancellation = cancellation;
 input.records = std::make_shared<CoconutAnnotationRecords>();
 input.expected_rows = ids.size();
 input.parquet_shards = {root.path() / "batch.parquet"};
 write_coconut_parquet(input.parquet_shards.front(), rows, coconut_parquet_schema(), 2);
 NormalizedAnnotationBuilder stock;
 stock.annotation_sha256 = std::string(64, 'a');
 stock.split = "train2017";
 for (auto id : ids) stock.images.push_back({id, 0, 0, 1, 1, 0, 0});
 const auto stock_index = fixture_index(stock);
 const auto original_owner = std::make_shared<CoconutRecoveryOriginals>(&stock_index, nullptr);
 CoconutMaskRecovery recovery(*original_owner);
 CompileExecution::CompletionSource originals;
 std::atomic<bool> original_terminal{false};
 std::atomic<unsigned> offered{0};
 std::promise<void> all_offered;
 input.originals = [&](CoconutImageNamespace, bool, std::stop_token) {
  return CoconutOriginalReadiness{original_terminal.load() ? original_owner : nullptr, original_terminal.load(), 1, originals.completion()};
 };
 input.preparation_progress = [&](CoconutPreparationStage stage, std::uint64_t count) {
  if (stage == CoconutPreparationStage::Annotations && count == ids.size()) all_offered.set_value();
 };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  original_terminal.store(true);
  originals.settle();
  execution.execution().notify_change(CompileChange::Interruption);
 });
 mmltk::testsupport::await_test_promise(all_offered, "every native input precedes original readiness");
 for (auto id : ids) {
  const auto provisional = coconut_image_input_identity(input.input_identity, coco(id), {}) + ".provisional";
  REQUIRE(execution.wait_image_labels(images, id, provisional));
  CHECK(conversions[id] == 1);
 }
 original_terminal.store(true);
 originals.settle();
 const auto result = mmltk::testsupport::await_test_future(importing, "later ready recovery batches settle without first-image geometry");
 REQUIRE(result.size() == 1);
 REQUIRE(result[0].index().image_count() == ids.size());
 REQUIRE(result[0].original_generation() == 1);
 CHECK(result[0].recovery_policy() == kCoconutRecoveryPolicy);
 CHECK_FALSE(execution.geometry(images, ids.front()));
 unsigned fallbacks = 0;
 const auto fallback = [&](const CoconutComponent& component, std::size_t i) {
  const auto dependency = component.image_input_identity(i);
  if (execution.has_image_labels(images, ids[i], dependency, component.original_generation())) return true;
  ++fallbacks;
  return execution.labels_ready(execution.source_publication(images, {}, ids[i]), ids[i], component.labels(i), dependency, component.original_generation());
 };
 // Every cold native product is already owned, including the pending image.
 for (std::size_t i = 0; i < ids.size(); ++i) CHECK(fallback(result[0], i));
 CHECK(fallbacks == 0);
 for (std::size_t i = 1; i < ids.size(); ++i) REQUIRE(execution.wait_image_labels(images, ids[i], result[0].image_input_identity(i)));
 REQUIRE(execution.wait_image_labels(images, ids.front(), result[0].image_input_identity(0)));
 CHECK(conversions[ids.front()] == 2);
 publication.geometry_ready(ids.front(), {1, 1});
 for (std::size_t i = 0; i < ids.size(); ++i) {
  const auto dependency = result[0].image_input_identity(i);
  const auto early = execution.wait_image_labels(images, ids[i], dependency);
  REQUIRE(early);
  CHECK(fallback(result[0], i));
  CHECK(fallbacks == 0);
  CHECK(execution.wait_image_labels(images, ids[i], dependency) == early);
  CHECK(conversions[ids[i]] == 2);
 }
 // A new warm attempt owns the loaded mapping and publishes every cached image.
 const auto path = root.path() / "warm.normalized.bin";
 store_coconut_component(path, result[0]);
 auto warm = load_coconut_component(path, CoconutEdition::Base, CoconutImageNamespace::CocoTrain, input.input_identity, {}, false, &physical, &recovery);
 REQUIRE(warm);
 CHECK(warm->original_generation() == 0);
 *warm = warm->with_original({original_owner, true, 1});
 CHECK(warm->select_images({0}).original_generation() == 1);
 std::array parts{CoconutComponentPart{{warm->select_images({0, 2, 4}), warm->select_images({1, 3})}, 0, 0}};
 const auto composed = merge_coconut_components(parts, input.input_identity, physical, root.path());
 REQUIRE(composed.size() == 1);
 CHECK(composed.front().original_generation() == 1);
 execution.retire_attempt();
 // The preparation-captured warm input cannot become current merely because
 // fallback obtains a fresh physical publication after original withdrawal.
 execution.original_generation(images, 2, true);
 CHECK_FALSE(fallback(composed.front(), 0));
 CHECK_FALSE(execution.wait_image_labels(images, ids[0], composed.front().image_input_identity(0), composed.front().original_generation()));
 const auto rebound = warm->with_original({original_owner, true, 2});
 CHECK(warm->original_generation() == 1);
 // Retained disjoint rows must agree before they can share a component.
 parts.front().components = {composed.front().select_images({0}), rebound.select_images({1})};
 CHECK_THROWS_AS(merge_coconut_components(parts, input.input_identity, physical, root.path()), CoconutOriginalChanged);
 parts.front().components = {rebound};
 const auto current = merge_coconut_components(parts, input.input_identity, physical, root.path());
 REQUIRE(current.size() == 1);
 CHECK(current.front().original_generation() == 2);
 execution.original_generation(images, 2, false);
 CHECK_FALSE(execution.has_image_labels(images, ids[0], composed.front().image_input_identity(0), composed.front().original_generation()));
 CHECK_FALSE(fallback(composed.front(), 0));
 CHECK_FALSE(execution.wait_image_labels(images, ids[0], composed.front().image_input_identity(0), composed.front().original_generation()));
 fallbacks = 0;
 for (std::size_t i = 0; i < ids.size(); ++i) CHECK(fallback(current.front(), i));
 CHECK(fallbacks == ids.size());
 for (std::size_t i = 0; i < ids.size(); ++i) {
  REQUIRE(execution.wait_image_labels(images, ids[i], current.front().image_input_identity(i), current.front().original_generation()));
  CHECK(conversions[ids[i]] == 3);
  CHECK(fallback(current.front(), i));
  CHECK(fallbacks == ids.size());
  CHECK_FALSE(execution.has_image_labels(images, ids[i], composed.front().image_input_identity(i), composed.front().original_generation()));
  CHECK_FALSE(execution.wait_image_labels(images, ids[i], composed.front().image_input_identity(i), composed.front().original_generation()));
 }
}
TEST_CASE("asynchronous native labels retain their sealed image until conversion settles", "[benchmark][coconut][pipeline][labels][resources]") {
 using namespace std::chrono_literals;
 ScopedTempDir backing_root("asynchronous-native-backing");
 detail::StorageReservationPool storage(backing_root.path(), {});
 CoconutStaging staging(backing_root.path(), &storage);
 CoconutNativeWorkspace workspace(CoconutImportLimits{});
 CoconutRecord record;
 record.image_id = 7;
 record.width = record.height = 1;
 record.segments = {{.id = 1, .category_id = 1, .isthing = true}};
 const std::array<std::uint32_t, 1> pixel{1};
 const auto encoded = png(1, 1, pixel);
 const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
 workspace.decode(record, bytes, workspace.admit_png(record, bytes));
 auto lineage = std::make_shared<CoconutNativeLineage>();
 lineage->component.source = CoconutImageNamespace::CocoTrain;
 auto native = workspace.finish(record, coco(7), {1, 1}, lineage, {});
 native = CoconutNativeImage::stage(std::move(native), staging);
 staging.retire();
 const std::weak_ptr<const CoconutNativeImage> lifetime = native;
 BenchmarkCompilePipeline execution(1);
 mmltk::testsupport::TestGate held("native label consumer retains its sealed source");
 execution.label_configuration(1, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, [&](const auto&, auto, const auto&) { held.receipt().ArriveAndWait(); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  held.Release();
  execution.retire_attempt();
 });
 const std::filesystem::path root("native-label-lifetime");
 const auto publication = execution.source_publication(root, {});
 execution.labels_ready(publication, 7, CoconutNativeImage::labels(native), "native");
 native.reset();
 workspace.retire();
 CHECK_FALSE(lifetime.expired());
 publication.geometry_ready(7, {1, 1});
 REQUIRE(held.WaitEntered(5s));
 CHECK_FALSE(lifetime.expired());
 CHECK_FALSE(std::filesystem::is_empty(backing_root.path()));
 held.Release();
 auto product = execution.wait_image_labels(root, 7, "native");
 REQUIRE(product);
 REQUIRE(product->labels.size() == 1);
 CHECK(product->runs.front().length == 1);
 CHECK(lifetime.expired());
 CHECK(std::filesystem::is_empty(backing_root.path()));
 CHECK(storage.outstanding() == 0);
 const std::weak_ptr<const BenchmarkLabelChunk> placed_custody = product;
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{root}};
 split.images = {{7, 1, 1, 1, 0, AnnotationSource::CoconutCoco}};
 ScopedTempDir destination("native-direct-labels");
 BenchmarkSplitWriter writer({.split = split, .output_path = destination.path() / "labels.bin", .resolution = 1, .num_workers = 1});
 writer.write_labels(0, benchmark_entry(split.images[0]), {product->labels, product->runs}, 19);
 CHECK(writer.entry(0).num_instances == 1);
 product.reset();
 execution.retire_attempt();
 CHECK(placed_custody.expired());
}
TEST_CASE("component dependency summaries retain full backing and recorded generations", "[benchmark][coconut][cache]") {
 ScopedTempDir root("component-consumed-summary");
 auto first = objects(1), second = objects(2), unused = objects(3);
 first.shard = 1;
 first.archive_identity = "one";
 second.shard = 2;
 second.archive_identity = "two";
 unused.shard = 3;
 unused.archive_identity = "unused";
 std::array physical_images{first, second, unused};
 CoconutPhysicalMembership physical(physical_images);
 CoconutPhysicalDependencies dependencies;
 dependencies.add(first);
 dependencies.add(second);
 const auto expected = Json{
  {"domain", "coconut-physical-artifacts-v1"}, {"edition", CoconutEdition::Large}, {"source", CoconutImageNamespace::Objects365V2},
  {"artifacts", Json::array({Json::array({1, "one"}), Json::array({2, "two"})})}
 }.dump();
 CHECK(physical.dependency_identity(CoconutEdition::Large, CoconutImageNamespace::Objects365V2, dependencies) == expected);
 CoconutComponentBuilder builder;
 builder.edition = CoconutEdition::Large;
 builder.source = CoconutImageNamespace::Objects365V2;
 builder.annotation_input_identity = "annotations";
 builder.input_identity = coconut_component_input_identity("annotations", builder.source, nullptr, &physical, builder.edition, dependencies);
 builder.index.source = BenchmarkDatasetSource::kObjects365V2;
 builder.index.split = "coconut-2-4";
 for (const auto& image : std::array{first, second}) {
  builder.index.images.push_back({image.image_id, 0, 0, 1, 1, image.shard, 0});
  builder.inventory.push_back({image, image.image_id, image.image_id});
 }
 const auto component = std::move(builder).finish({}, root.path());
 const auto selected = component.select_images({0});
 CHECK(&selected.image_input_identity(0) == &component.image_input_identity(0));
 CHECK(component.image_input_identity(0) == coconut_image_input_identity("annotations", first, {}));
 physical_images[2].archive_identity = "unused replacement";
 CoconutPhysicalMembership unused_changed(physical_images);
 CHECK(selected.matches_inputs("annotations", unused_changed, nullptr));
 physical_images[1].archive_identity = "consumed replacement";
 CoconutPhysicalMembership changed(physical_images);
 CHECK_FALSE(selected.matches_inputs("annotations", changed, nullptr));
 CHECK(selected.matches_inputs("annotations", changed, nullptr, false));
 auto contradictory = first;
 contradictory.archive_identity = "another generation";
 CHECK_THROWS_WITH(dependencies.add(contradictory), "COCONut: component mixes physical artifact generations");
}
TEST_CASE("COCONut failure reports batch append and flush best effort through unwind", "[benchmark][coconut][report]") {
 ScopedTempDir root("coconut-report-batches");
 const BenchmarkTraceSink quiet;
 unsigned warnings = 0;
 ProgressReporter progress([&](const auto&) {
  ++warnings;
  throw std::runtime_error("ignored warning sink");
 }, quiet);
 const auto report_path = root.path() / "failed.txt";
 bool unwritable = false, empty = false, concurrent = false, oversized = false;
 SECTION("append accepted records on exceptional exit") {}
 SECTION("failed report storage remains nonfatal") {
  unwritable = true;
  std::filesystem::create_directory(report_path);
 }
 SECTION("empty image completions never create a report") { empty = true; }
 SECTION("concurrent image completions preserve every producer's order") { concurrent = true; }
 SECTION("direct oversized records remain dirty through image settlement") { oversized = true; }
 if (!unwritable && !empty) mmltk::testsupport::write_text_file(report_path, "{\"historical\":true}\n");
 const unsigned producers = concurrent ? 4U : 1U;
 const unsigned count = empty ? 0U : oversized ? 4U : 1024U / producers;
 const std::string reason = oversized ? std::string(80U * 1024U, 'x') : "missing support";
 try {
  CoconutFailureReport report(root.path(), progress);
  const auto reject = [&](unsigned producer) {
   for (unsigned i = 0; i < count; ++i) {
    report.reject(coco(7 + producer), 77, "base", i, producer + 1, reason);
    if (concurrent || oversized) report.flush();  // Actual image-terminal boundary.
   }
  };
  if (concurrent) {
   std::latch start(1);
   std::vector<std::jthread> writers;
   {
    const mmltk::testsupport::ScopedTestCleanup release([&] { start.count_down(); });
    for (unsigned producer = 0; producer < producers; ++producer)
     writers.emplace_back([&, producer] {
      start.wait();
      reject(producer);
     });
   }
   writers.clear();
  } else
   reject(0);
  if (empty) {
   for (unsigned image = 0; image < 1024; ++image) report.flush();
   CHECK_FALSE(std::filesystem::exists(report_path));
  }
  if (oversized) {
   // Oversized records bypass pending_; flush must still expose their bytes.
   std::ifstream visible(report_path);
   std::string line;
   unsigned lines = 0;
   while (std::getline(visible, line)) ++lines;
   CHECK(lines == count + 1U);
  }
  throw std::runtime_error("image cancellation");
 } catch (const std::runtime_error& error) { CHECK(std::string_view(error.what()) == "image cancellation"); }
 CHECK_NOTHROW(progress.flush());
 CHECK(warnings == (empty ? 0 : 1));
 if (empty) {
  CHECK_FALSE(std::filesystem::exists(report_path));
  return;
 }
 if (!unwritable) {
  std::ifstream input(report_path);
  std::string line;
  REQUIRE(std::getline(input, line));
  CHECK(Json::parse(line).at("historical") == true);
  std::array<unsigned, 4> records{};
  while (std::getline(input, line)) {
   const auto record = Json::parse(line);
   const auto producer = record.at("category_id").get<unsigned>() - 1;
   REQUIRE(producer < producers);
   CHECK(record.at("object_id") == records[producer]++);
   CHECK(record.at("image_id") == 7U + producer);
   CHECK(record.at("release_image_id") == 77);
   CHECK(record.at("reason") == reason);
  }
  for (unsigned producer = 0; producer < producers; ++producer) CHECK(records[producer] == count);
 }
}
TEST_CASE("benchmark publication retains empty-directory admission and refuses concurrent occupants", "[benchmark][coconut][publication]") {
 ScopedTempDir root("coconut-publication-admission");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock});
 const auto catalog = local.selected(CoconutValidation::CoconutStock);
 bool concurrent_empty = false, occupied = false;
 SECTION("admitted empty directory publishes without overwrite") {}
 SECTION("admitted empty directory becomes occupied") { occupied = true; }
 SECTION("an absent destination becomes an empty directory at publication") { concurrent_empty = true; }
 SECTION("overwrite commits and reports old-generation cleanup") { config.overwrite = true; }
 if (!concurrent_empty) std::filesystem::create_directories(config.output_dir);
 const auto previous = config.output_dir / "previous";
 if (config.overwrite) mmltk::testsupport::write_text_file(previous, "previous generation");
 bool started = false, completed = false;
 std::optional<Json> cleanup;
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.publication.started") {
   started = true;
   if (concurrent_empty) std::filesystem::create_directory(config.output_dir);
   if (occupied) mmltk::testsupport::write_text_file(previous, "concurrent occupant");
  }
  if (event == "benchmark.publication.old_generation_cleanup") cleanup = Json::parse(fields);
  if (event == "benchmark.publication.complete") {
   completed = true;
   throw std::runtime_error("diagnostic observer cannot revoke committed publication");
  }
 };
 if (occupied || concurrent_empty) {
  CHECK_THROWS(compile_benchmark_recipe(config, &catalog));
  CHECK_FALSE(completed);
  CHECK_FALSE(std::filesystem::exists(config.output_dir / "train.bin"));
  if (occupied) CHECK(file_bytes(previous) == "concurrent occupant");
  if (concurrent_empty) CHECK(std::filesystem::is_empty(config.output_dir));
 } else {
  REQUIRE_NOTHROW(compile_benchmark_recipe(config, &catalog));
  CHECK(completed);
  CHECK(CompiledDataset::open(config.output_dir / "train.bin").header().num_images == 4);
  CHECK_FALSE(std::filesystem::exists(previous));
 }
 CHECK(started);
 REQUIRE(cleanup.has_value() == config.overwrite);
 if (cleanup) {
  CHECK(cleanup->at("removed") == true);
  CHECK(cleanup->at("error") == "");
  CHECK_FALSE(std::filesystem::exists(cleanup->at("path").get<std::string>()));
 }
}
TEST_CASE("progress failure before commit differs from terminal notification failure", "[benchmark][coconut][publication]") {
 ScopedTempDir root("coconut-publication-callback");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 const auto catalog = local.selected(CoconutValidation::CoconutStock);
 std::filesystem::create_directories(config.output_dir);
 const auto previous = config.output_dir / "previous";
 mmltk::testsupport::write_text_file(previous, "published before compile");
 bool terminal = false;
 SECTION("pending pre-publication callback prevents the rename") {}
 SECTION("terminal callback fails after the successful rename") { terminal = true; }
 std::vector<std::pair<std::uint64_t, bool>> publication_observations;
 config.progress = [&](const BenchmarkCompileProgress& value) {
  if (value.phase == DatasetCompilePhase::Publishing) publication_observations.emplace_back(value.completed, std::filesystem::exists(config.output_dir / "train.bin"));
  if (value.phase == DatasetCompilePhase::Publishing && value.completed == (terminal ? 1U : 0U)) throw std::runtime_error("publication observer failure");
 };
 CHECK_THROWS_WITH(compile_benchmark_recipe(config, &catalog), "publication observer failure");
 REQUIRE_FALSE(publication_observations.empty());
 CHECK(publication_observations.back().first == (terminal ? 1U : 0U));
 for (const auto& [completed, published] : publication_observations) CHECK(published == (completed == 1U));
 CHECK(std::filesystem::exists(previous) == !terminal);
 CHECK(std::filesystem::exists(config.output_dir / "train.bin") == terminal);
 if (terminal) CHECK(CompiledDataset::open(config.output_dir / "train.bin").header().num_images == 4);
}
TEST_CASE("validation places its final metadata while a training release remains pending", "[benchmark][coconut][pipeline][labels]") {
 using namespace std::chrono_literals;
 auto choice = CoconutValidation::Coconut;
 bool fail_training = false;
 SECTION("enhanced validation with one CPU") {}
 SECTION("stock validation with one CPU") { choice = CoconutValidation::Stock; }
 SECTION("first training failure preserves publication after independent placement") { fail_training = true; }
 ScopedTempDir root("independent-validation-placement");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, choice}, true);
 auto catalog = local.selected(choice);
 compile_benchmark_recipe(config, &catalog);
 const auto [train, validation, manifest] = capture_publication_bytes(local.output);
 const auto train_semantics = benchmark_semantics(local.output / "train.bin"), validation_semantics = benchmark_semantics(local.output / "val.bin");
 mmltk::testsupport::TestGate training("training release mask owner");
 std::atomic<bool> cancelled{false};
 std::atomic<unsigned> placements{0};
 std::promise<void> validation_ready;
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.labels.placed" && Json::parse(fields).at("split") == "val" && placements.fetch_add(1) == 0) validation_ready.set_value();
 };
 catalog.release_observer = [&](CoconutEdition edition, CoconutReleaseBoundary boundary) {
  if (edition != CoconutEdition::Base || boundary != CoconutReleaseBoundary::MasksStarted) return;
  training.receipt().ArriveAndWait();
  if (fail_training) throw std::runtime_error("held training release failed");
 };
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  training.Release();
 });
 REQUIRE(training.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(validation_ready, "validation final placement beside pending training on one CPU", 5s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 CHECK(placements.load() >= 1);
 check_publication_bytes(local.output, train, validation, manifest);
 training.Release();
 if (fail_training) {
  CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(compiling, "first failure after independent validation", 5s), "held training release failed");
  check_publication_bytes(local.output, train, validation, manifest);
 } else {
  mmltk::testsupport::await_test_future(compiling, "training completion reuses validation placement", 5s);
  CHECK(benchmark_semantics(local.output / "train.bin") == train_semantics);
  CHECK(benchmark_semantics(local.output / "val.bin") == validation_semantics);
  CHECK(placements.load() >= 1);  // The final recipe consumes the same immutable generation.
 }
}
TEST_CASE("final XL selection restores labels after a preferred release withdraws cold or cached claims", "[benchmark][coconut][pipeline][cache][publication]") {
 using namespace std::chrono_literals;
 const bool warm = GENERATE(false, true);
 ScopedTempDir root("coconut-final-xl-restoration");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 auto catalog = local.selected(config.selection.validation);
 auto& large = *std::ranges::find(catalog.releases, CoconutEdition::Large, &CoconutReleaseComponent::edition);
 auto& xl = *std::ranges::find(catalog.releases, CoconutEdition::XLarge, &CoconutReleaseComponent::edition);
 auto& metadata = large.annotations.front();
 const auto metadata_path = local.cache.source_downloads("coconut-" + std::string(large.name)) / metadata.filename;
 auto document = Json::parse(file_bytes(metadata_path));
 document["annotations"][0]["object365_file_name"] = "objects365_v2_00000002";
 json_file(metadata_path, document);
 metadata.expected_size = 0;
 const std::string unavailable_metadata = "{";
 mmltk::backend::data::testsupport::HttpServer repair(unavailable_metadata);
 metadata.url = repair.url("unreadable-large-metadata");
 const std::array<std::uint32_t, 9> support{11, 11, 11, 11, 11, 11, 11, 11, 11};
 const auto mask = png(3, 3, support);
 const auto large_path = local.cache.source_downloads("coconut-" + std::string(large.name)) / large.annotations.back().filename;
 tar(large_path, std::array<std::pair<std::string, std::string>, 1>{{{"panoptic_object365/objects365_v2_00000002.png", mask}}});
 // Large and XL have distinguishable labels. Pin the healthy mask so bounded
 // repair replaces only the failed metadata generation.
 document["annotations"][0]["segments_info"] = Json::array({segment(11, 2)});
 json_file(metadata_path, document);
 large.annotations.back().expected_size = std::filesystem::file_size(large_path);
 large.annotations.back().expected_sha256 = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(large_path));
 const auto xl_path = local.cache.source_downloads("coconut-" + std::string(xl.name)) / xl.annotations.front().filename;
 std::vector<std::pair<std::string, std::string>> members;
 for (auto id : {2, 1}) {
  const auto stem = "objects365_v2_0000000" + std::to_string(id);
  members.emplace_back("coconuts_xlarge/panseg/" + stem + ".png", mask);
  members.emplace_back("coconuts_xlarge/panseg_info/" + stem + ".json", Json::array({segment(11, 1)}).dump());
 }
 tar(xl_path, members);
 xl.annotations.front().expected_size = std::filesystem::file_size(xl_path);
 std::optional<std::array<std::string, 3>> previous;
 if (warm) {
  compile_benchmark_recipe(config, &catalog);
  previous = capture_publication_bytes(local.output);
  const auto compiled = CompiledDataset::open(local.output / "train.bin");
  const auto labels = compiled.image_labels(compiled_slot(compiled, 2, AnnotationSource::CoconutObjects365V2));
  REQUIRE(labels.size() == 1);
  CHECK(labels.front().source_category_id == 2);
  // Keep the admitted row intact and break only the later inventory proof.
  unsigned damaged = 0;
  for (const auto& entry : std::filesystem::directory_iterator(local.cache.source_indexes("coconut-" + std::string(large.name)))) {
   if (!entry.path().string().ends_with(".normalized.bin.inventory")) continue;
   auto bytes = file_bytes(entry.path());
   bytes.back() ^= 1;
   mmltk::testsupport::write_text_file(entry.path(), bytes);
   ++damaged;
  }
  REQUIRE(damaged == 1);
 }
 auto held_large = ArtifactLease::acquire(local.cache.locks / (std::string(large.name) + ".annotations.lifecycle.lock"), {});
 mmltk::testsupport::TestGate cache_proof("preferred cached row before its late proof"), terminal("preferred native labels before terminal local rejection");
 std::atomic<bool> cancelled{false}, cached{false}, rejected{false};
 DeferredCancellationGate observation{cancelled};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Borrow(observation);
 std::array<std::promise<void>, 3> placements;
 std::atomic<unsigned> writes{0}, pixels{0};
 config.trace = [&](std::string_view event, std::string_view fields) {
  const auto value = Json::parse(fields);
  if (event == "benchmark.labels.placed" && value.at("split") == "train" && value.at("image_id") == 2) {
   const auto count = writes.fetch_add(1);
   if (count < placements.size()) placements[count].set_value();
  }
  if (event == "benchmark.archive.image_ready" && value.at("artifact") == coconut_namespace_name(CoconutImageNamespace::Objects365V2) && value.at("image_id") == 2) ++pixels;
  if (warm && event == "benchmark.annotations.cache_row" && value.at("edition") == CoconutEdition::Large && !cached.exchange(true)) DeferredCancellationGate::next() = &cache_proof;
 };
 catalog.release_observer = [&](CoconutEdition edition, CoconutReleaseBoundary boundary) {
  if (edition != CoconutEdition::Large || boundary != CoconutReleaseBoundary::MetadataConsumed || rejected.exchange(true)) return;
  terminal.receipt().ArriveAndWait();
  throw CoconutInputError("preferred release failed its terminal source proof");
 };
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog, nullptr, {}, {}, {.transient_bytes = 1, .descriptors = 26}); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled = true;
  held_large = {};
  cache_proof.Release();
  terminal.Release();
 });
 mmltk::testsupport::await_test_promise(placements[0], "XL writes the overlapping image before Large admission", 5s);
 held_large = {};
 if (warm) {
  REQUIRE(cache_proof.WaitEntered(5s));
  mmltk::testsupport::await_test_promise(placements[1], "cached Large replaces XL before its failed proof", 5s);
  cache_proof.Release();
 }
 REQUIRE(terminal.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(placements[warm ? 2 : 1], "native Large is physically written before local withdrawal", 5s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 if (previous) check_publication_bytes(local.output, (*previous)[0], (*previous)[1], (*previous)[2]);
 terminal.Release();
 mmltk::testsupport::await_test_future(compiling, "final XL replay after bounded Large repair", 15s);
 CHECK(rejected);
 CHECK(repair.requests() != 0);
 CHECK(pixels == 1);
 CHECK(writes == (warm ? 4 : 3));
 const auto train = CompiledDataset::open(local.output / "train.bin");
 CHECK(train.header().num_images == 4);
 CHECK(train.header().num_slots == 4);
 const auto labels = train.image_labels(compiled_slot(train, 2, AnnotationSource::CoconutObjects365V2));
 REQUIRE(labels.size() == 1);
 CHECK(labels.front().annotation_id == 11);
 CHECK(labels.front().source_category_id == 1);
 CHECK(labels.front().source_ordinal == 1);  // XL canonical stem order, not arrival order or the retired Large base.
 CHECK(labels.front().has_complete_source_ordinal());
 CHECK(CompiledDataset::open(local.output / "val.bin").header().num_images == 1);
 repair.Check();
}
TEST_CASE("COCONut independently writes pixels and labels while every opposite input is withheld", "[benchmark][coconut][pipeline][resources]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("coconut-independent-first-work");
 LocalCoconutRecipe local(root.path());
 const auto validation = GENERATE(CoconutValidation::Coconut, CoconutValidation::Stock, CoconutValidation::CoconutStock);
 const bool warm = GENERATE(false, true);
 const bool hold_annotations = GENERATE(false, true);
 const bool recover = GENERATE(false, true);
 CAPTURE(validation, warm, hold_annotations, recover);
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, validation, recover}, true);
 auto catalog = local.selected(validation);
 if (warm) compile_benchmark_recipe(config, &catalog);
 std::vector<ArtifactLease> held;
 if (hold_annotations) {
  for (const auto& release : catalog.releases) held.push_back(ArtifactLease::acquire(local.cache.locks / (std::string(release.name) + ".annotations.lifecycle.lock"), {}));
  if (validation == CoconutValidation::Stock || recover) held.push_back(ArtifactLease::acquire(local.cache.locks / "coco-annotations.lifecycle.lock", {}));
 } else
  for (const auto& image : catalog.images)
   held.push_back(ArtifactLease::acquire(local.cache.locks / (std::string(benchmark_source_name(image.artifact.source)) + "-" + image.cache_shard + ".images.lock"), {}));
 std::atomic<bool> cancelled{false}, completed{false};
 std::atomic<std::uint64_t> observed_pixels{0}, observed_labels{0};
 std::atomic<bool> total_known{false};
 std::promise<void> first;
 std::mutex evidence_mutex;
 std::array<std::pair<std::string, std::string>, 64> recent_events;
 std::size_t next_event = 0;
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 config.progress = [&](const BenchmarkCompileProgress& value) {
  observed_pixels = value.tracks.pixels.completed;
  observed_labels = value.tracks.labels.completed;
  if (hold_annotations && value.tracks.pixels.completed && !completed.exchange(true)) {
   total_known = value.tracks.pixels.total_known;
   first.set_value();
  }
 };
 config.trace = [&](std::string_view event, std::string_view fields) {
  {
   const std::lock_guard lock(evidence_mutex);
   auto& entry = recent_events[next_event++ % recent_events.size()];
   entry.first.assign(event);
   entry.second.assign(fields);
  }
  if (std::getenv("MMLTK_TEST_BENCHMARK_TRACE"))
   std::fprintf(
    stderr, "%s\n", Json{{"event", event}, {"payload", Json::parse(fields)}, {"validation", validation}, {"warm", warm}, {"hold_annotations", hold_annotations}, {"recover", recover}}.dump().c_str());
  if (!hold_annotations && event == "benchmark.labels.placed" && !completed.exchange(true)) first.set_value();
 };
 auto compiling = compile_constrained_coconut(config, catalog);
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled = true;
  held.clear();
 });
 mmltk::testsupport::await_test_promise(first, "first written payload with all opposite source inputs withheld", 5s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 if (hold_annotations) {
  CHECK_FALSE(total_known.load());
  CHECK(observed_labels.load() == 0);
 } else
  CHECK(observed_pixels.load() == 0);
 held.clear();
 REQUIRE_NOTHROW([&] {
  try {
   mmltk::testsupport::await_test_future(compiling, "independent source settlement", 15s);
  } catch (...) {
   const std::lock_guard lock(evidence_mutex);
   const auto count = std::min(next_event, recent_events.size());
   for (std::size_t i = next_event - count; i < next_event; ++i) {
    const auto& [event, fields] = recent_events[i % recent_events.size()];
    std::fprintf(stderr, "%s\n", Json{{"event", event}, {"payload", Json::parse(fields)}}.dump().c_str());
   }
   throw;
  }
 }());
 CHECK(CompiledDataset::open(local.output / "train.bin").header().num_images == 4);
}
TEST_CASE("ready Parquet artifact labels precede a held sibling and settle canonical prefixes", "[benchmark][coconut][pipeline][resources]") {
 using namespace std::chrono_literals;
 const bool warm = GENERATE(false, true);
 const std::string_view prefix = GENERATE("healthy", "release cache only", "missing optional extents", "broken footer", "damaged later group");
 CAPTURE(warm, prefix);
 const bool unavailable_prefix = prefix == "broken footer" || prefix == "damaged later group";
 const bool partial_prefix = prefix == "damaged later group";
 ScopedTempDir root("coconut-ready-parquet-artifact");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Stock, true}, true);
 auto catalog = local.selected(config.selection.validation);
 auto& release = catalog.releases.front();
 add_ready_parquet_part(local, catalog);
 std::vector<std::pair<std::filesystem::path, std::string>> retained_cache_bytes;
 if (warm) {
  compile_benchmark_recipe(config, &catalog);
  if (prefix == "release cache only") {
   const auto directory = local.cache.source_indexes("coconut-" + std::string(release.name));
   remove_coconut_part_caches(directory);
  }
  if (prefix == "missing optional extents") {
   const auto directory = local.cache.source_indexes("coconut-" + std::string(release.name));
   for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().string().ends_with(".part-0.normalized.bin.complete.json")) omit_optional_coconut_facts(entry.path());
    if (entry.path().string().ends_with(".part-0.normalized.bin") || entry.path().string().ends_with(".part-0.normalized.bin.inventory"))
     retained_cache_bytes.emplace_back(entry.path(), file_bytes(entry.path()));
   }
  }
 }
 const auto first_path = local.cache.source_downloads("coconut-" + std::string(release.name)) / release.annotations.front().filename;
 const auto first_bytes = file_bytes(first_path);
 const auto first_artifact = release.annotations.front();
 if (unavailable_prefix) {
  if (partial_prefix) {
   auto reader = parquet::ParquetFileReader::OpenFile(first_path.string());
   const auto column = reader->metadata()->RowGroup(1)->ColumnChunk(0);
   const auto offset = column->has_dictionary_page() ? column->dictionary_page_offset() : column->data_page_offset();
   const auto bytes = column->total_compressed_size();
   reader.reset();
   std::fstream damaged(first_path, std::ios::in | std::ios::out | std::ios::binary);
   damaged.seekp(offset);
   const std::string invalid_page(static_cast<std::size_t>(bytes), '\0');
   damaged.write(invalid_page.data(), static_cast<std::streamsize>(invalid_page.size()));
  } else
   mmltk::testsupport::write_text_file(first_path, "unreadable Parquet source extent");
  release.annotations.front().expected_size = std::filesystem::file_size(first_path);
  release.annotations.front().expected_sha256 = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(first_path));
  // The fixture is a different immutable source generation, including when
  // damaged column bytes happen to preserve the old artifact's length.
  release.annotations.front().url += "/damaged";
 }
 auto held = ArtifactLease::acquire(local.cache.locks / (release.annotations.front().artifact_id + ".lock"), {});
 std::atomic<bool> cancelled{false}, published{false};
 std::promise<void> labels, cache_ready;
 std::atomic<bool> admitted_native{false}, admitted_gather{false}, sealed_inventory{false}, direct_cache{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (std::getenv("MMLTK_TEST_BENCHMARK_TRACE")) std::fprintf(stderr, "%s\n", Json{{"event", event}, {"payload", Json::parse(fields)}}.dump().c_str());
  const auto value = Json::parse(fields);
  if (event == "benchmark.labels.placed" && value.at("image_id") == 11 && !published.exchange(true)) labels.set_value();
  if (event == "benchmark.coconut.native_staged" && value.at("image_id") == 11 && value.at("admitted_runs").get<std::uint64_t>() > 0) admitted_native = true;
  if (event == "benchmark.annotations.gathered" && value.at("validated_runs") == 0 && value.at("runs").get<std::uint64_t>() > 0) admitted_gather = true;
  if (event == "benchmark.annotations.inventory_sealed") sealed_inventory = true;
  if (event == "benchmark.annotations.cache_prepare" && value.at("path").get<std::string>().ends_with(".part-1.normalized.bin")) direct_cache = value.at("same_backing").get<bool>();
  if (event == "benchmark.annotations.cache_store" && value.at("path").get<std::string>().ends_with(".part-1.normalized.bin")) mmltk::testsupport::release_test_promise(cache_ready);
 };
 auto compiling = compile_constrained_coconut(config, catalog);
 const mmltk::testsupport::ScopedTestCleanup release_inputs([&] {
  cancelled = true;
  held = {};
 });
 mmltk::testsupport::await_test_promise(labels, "ready Parquet or normalized-cache labels before its sibling artifact", 5s);
 if (!warm) {
  mmltk::testsupport::await_test_promise(cache_ready, "ready part validates, gathers and persists while its source-order predecessor is held", 5s);
  CHECK(admitted_native.load());
  CHECK(admitted_gather.load());
  CHECK(sealed_inventory.load());
  CHECK(direct_cache.load());
 }
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 held = {};
 REQUIRE_NOTHROW(mmltk::testsupport::await_test_future(compiling, "per-artifact canonical ordinal settlement", 15s));
 const auto dataset = CompiledDataset::open(config.output_dir / "train.bin");
 REQUIRE(dataset.header().num_images == (unavailable_prefix ? (partial_prefix ? 4 : 3) : 5));
 const auto slot = compiled_slot(dataset, 11, AnnotationSource::CoconutCoco);
 REQUIRE(dataset.image_labels(slot).size() == 1);
 CHECK(dataset.image_labels(slot).front().source_ordinal == (unavailable_prefix ? 0 : 2));
 CHECK(dataset.image_labels(slot).front().has_complete_source_ordinal() == !unavailable_prefix);
 if (warm && prefix == "missing optional extents") {
  const auto manifest = Json::parse(file_bytes(config.output_dir / "benchmark_manifest.json"));
  const auto& components = manifest.at("recipe").at("components");
  const auto selected = std::ranges::find_if(components, [](const auto& component) { return component.at("edition") == CoconutEdition::Base && component.at("physical_source") == "coco-train2017"; });
  REQUIRE(selected != components.end());
  const auto completion = read_json_file((local.cache.root / selected->at("imported_index").get<std::string>()).string() + ".complete.json");
  CHECK(completion.at("coconut").at("source_extent_complete") == true);
  CHECK(completion.at("coconut").at("source_rows") == 3);
  CHECK(completion.at("coconut").at("source_segments") == 3);
  for (const auto& [path, bytes] : retained_cache_bytes) CHECK(file_bytes(path) == bytes);
 }
 if (unavailable_prefix) {
  const auto check_provenance = [&] {
   const auto manifest = Json::parse(file_bytes(config.output_dir / "benchmark_manifest.json"));
   bool reported = false;
   for (const auto& component : manifest.at("recipe").at("components")) {
    if (component.at("edition") != CoconutEdition::Base || component.at("physical_source") != "coco-train2017") continue;
    reported = std::ranges::find(component.at("incomplete_ordinal_images"), Json(11)) != component.at("incomplete_ordinal_images").end();
    const auto path = local.cache.root / component.at("imported_index").get<std::string>();
    const auto cached = load_coconut_component(path, CoconutEdition::Base, CoconutImageNamespace::CocoTrain, component.at("input_identity").get<std::string>());
    REQUIRE(cached);
    const auto position = partial_prefix ? 1U : 0U;
    CHECK(cached->index().image(position).source_image_id == 11);
    CHECK_FALSE(cached->source_ordinals_complete(position));
    CHECK((cached->index().storage().boxes[cached->index().image(position).first_box].flags & kAnnotationOrdinalIncomplete) != 0);
   }
   CHECK(reported);
  };
  check_provenance();
  compile_benchmark_recipe(config, &catalog);
  const auto reused = CompiledDataset::open(config.output_dir / "train.bin");
  CHECK_FALSE(reused.image_labels(compiled_slot(reused, 11, AnnotationSource::CoconutCoco)).front().has_complete_source_ordinal());
  check_provenance();
  // Only newly authoritative source bytes can settle the missing prefix.
  mmltk::testsupport::write_text_file(first_path, first_bytes);
  release.annotations.front() = first_artifact;
  compile_benchmark_recipe(config, &catalog);
  const auto restored = CompiledDataset::open(config.output_dir / "train.bin");
  const auto& label = restored.image_labels(compiled_slot(restored, 11, AnnotationSource::CoconutCoco)).front();
  CHECK(label.has_complete_source_ordinal());
  CHECK(label.source_ordinal == 2);
 }
}
TEST_CASE("COCONut distinguishes partial labels valid backgrounds corrupt foreground and empty required splits", "[benchmark][coconut][publication]") {
 const std::string_view outcome = GENERATE("partial labels", "corrupt foreground", "background without PNG", "unlisted mask labels", "empty validation");
 ScopedTempDir root("coconut-local-rejection-outcomes");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 auto catalog = local.selected(config.selection.validation);
 compile_benchmark_recipe(config, &catalog);
 const auto [previous_train, previous_validation, previous_manifest] = capture_publication_bytes(local.output);
 const auto edition = outcome == "empty validation" ? CoconutEdition::RelabeledValidation : CoconutEdition::Base;
 auto& release = *std::ranges::find(catalog.releases, edition, &CoconutReleaseComponent::edition);
 auto& artifact = release.annotations.front();
 const auto path = local.cache.source_downloads("coconut-" + std::string(release.name)) / artifact.filename;
 if (outcome == "empty validation")
  mmltk::testsupport::write_text_file(path, "unreadable required validation artifact");
 else {
  const std::array<std::uint32_t, 9> support{1, 1, 1, 1, 2, 1, 1, 0, 1}, empty{};
  const auto first = segment(1, outcome == "corrupt foreground" ? 12 : 1);
  const auto second = segment(2, (outcome == "background without PNG" || outcome == "unlisted mask labels") ? 2 : 12);
  const auto background = outcome == "background without PNG" ? std::string("invalid PNG") : png(3, 3, outcome == "unlisted mask labels" ? support : empty);
  write_coconut_parquet(path, Json::array({hf_row(7, png(3, 3, support), Json::array({first, second})), hf_row(8, background, Json::array(), 3, 3, "unlabeled2017")}));
 }
 artifact.expected_size = std::filesystem::file_size(path);
 artifact.expected_sha256 = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(path));
 if (outcome == "empty validation") {
  CHECK_THROWS_WITH(compile_benchmark_recipe(config, &catalog), "required COCONut split contains no healthy samples");
  check_publication_bytes(local.output, previous_train, previous_validation, previous_manifest);
  return;
 }
 for (unsigned admission = 0; admission < 2; ++admission) {
  CAPTURE(admission, outcome);
  compile_benchmark_recipe(config, &catalog);
  const auto train = CompiledDataset::open(local.output / "train.bin");
  REQUIRE(train.header().num_images == ((outcome == "corrupt foreground" || outcome == "unlisted mask labels") ? 3 : 4));
  if (outcome != "unlisted mask labels") CHECK(train.image_labels(compiled_slot(train, 8, AnnotationSource::CoconutCoco)).empty());
  if (outcome != "corrupt foreground") {
   const auto labels = train.image_labels(compiled_slot(train, 7, AnnotationSource::CoconutCoco));
   REQUIRE(labels.size() == (outcome == "partial labels" ? 1 : 2));
   CHECK(labels.front().annotation_id == 1);
   CHECK(labels.front().source_ordinal == 0);
   CHECK(labels.front().has_complete_source_ordinal());
  }
  const auto manifest = Json::parse(file_bytes(local.output / "benchmark_manifest.json"));
  if (outcome == "corrupt foreground" || outcome == "unlisted mask labels") CHECK_FALSE(manifest.at("quarantined_images").empty());
 }
}
TEST_CASE("COCONut local identity rejection retains later archive and annotation rows", "[benchmark][coconut][pipeline][publication]") {
 using namespace std::chrono_literals;
 const std::string_view issue = GENERATE("malformed identity", "unsafe identity", "duplicate annotations", "cross-artifact duplicate annotations", "duplicate pixels");
 const bool cross_artifact = issue == "cross-artifact duplicate annotations";
 const bool duplicate_pixels = issue == "duplicate pixels";
 ScopedTempDir root("coconut-local-identity");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Stock}, true);
 if (std::getenv("MMLTK_TEST_BENCHMARK_TRACE"))
  config.trace = [](std::string_view event, std::string_view fields) {
   if (event == "benchmark.resource.wait") std::fprintf(stderr, "%s\n", Json{{"event", event}, {"payload", Json::parse(fields)}}.dump().c_str());
  };
 auto catalog = local.selected(config.selection.validation);
 auto& release = catalog.releases.front();
 const std::array<std::uint32_t, 9> pixels{1, 1, 1, 1, 1, 1, 1, 1, 1};
 auto bad = hf_row(7, png(3, 3, pixels), Json::array({segment()}));
 if (issue == "malformed identity" || issue == "unsafe identity") {
  bad["image_info"]["file_name"] = issue == "unsafe identity" ? "../malformed.jpg" : "malformed.jpg";
  bad["segments_info"]["file_name"] = issue == "unsafe identity" ? "../malformed.png" : "malformed.png";
 }
 const auto path = local.cache.source_downloads("coconut-" + std::string(release.name)) / release.annotations.front().filename;
 auto rows = Json::array({bad});
 if (issue == "duplicate annotations") rows.push_back(bad);
 auto healthy = segment();
 if (cross_artifact) healthy["iscrowd"] = 1;
 rows.push_back(hf_row(11, png(3, 3, pixels), Json::array({healthy})));
 write_coconut_parquet(path, rows);
 release.expected_rows = rows.size();
 auto& artifact = release.annotations.front();
 artifact.expected_size = std::filesystem::file_size(path);
 artifact.expected_sha256 = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(path));
 std::vector<std::filesystem::path> strict_shards{path};
 if (cross_artifact) {
  auto second = artifact;
  second.artifact_id += "-duplicate";
  second.filename = "duplicate.parquet";
  second.url += "/duplicate";
  const auto second_path = path.parent_path() / second.filename;
  write_coconut_parquet(second_path, Json::array({bad, hf_row(12, png(3, 3, pixels), Json::array({segment()}))}));
  second.expected_size = std::filesystem::file_size(second_path);
  second.expected_sha256 = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(second_path));
  release.annotations.push_back(second);
  release.expected_rows += 2;
  strict_shards.push_back(second_path);
 }
 if (!duplicate_pixels) {
  const std::array strict_physical{coco(7), coco(11), coco(12)};
  auto strict = request(strict_physical);
  strict.parquet_shards = strict_shards;
  CHECK_THROWS_AS(import_coconut_annotations(strict), CoconutInputError);
 }
 std::vector<std::pair<std::string, std::string>> members{{coco(7).member, white_jpeg()}};
 if (duplicate_pixels) members.emplace_back(coco(7).member, white_jpeg());
 members.emplace_back(coco(11).member, white_jpeg());
 if (cross_artifact) members.emplace_back(coco(12).member, white_jpeg());
 replace_physical_images(local.cache, catalog, CoconutImageNamespace::CocoTrain, members);
 for (unsigned admission = 0; admission < 2; ++admission) {
  if (cross_artifact) {
   auto held = ArtifactLease::acquire(local.cache.locks / (release.annotations.back().artifact_id + ".lock"), {});
   std::atomic<bool> cancelled{false}, first_written{false}, validation_written{false};
   std::atomic<unsigned> duplicate_pixels_written{0};
   std::promise<void> first_labels, validation_labels;
   config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
   config.trace = [&, diagnostic = config.trace](std::string_view event, std::string_view fields) {
    if (diagnostic) diagnostic(event, fields);
    const auto value = Json::parse(fields);
    if (event == "benchmark.labels.placed") {
     if (value.at("image_id") == 7 && !first_written.exchange(true)) first_labels.set_value();
     if (value.at("image_id") == 9 && !validation_written.exchange(true)) validation_labels.set_value();
    }
    if (event == "benchmark.archive.image_ready" && value.at("artifact") == coconut_namespace_name(CoconutImageNamespace::CocoTrain) && value.at("image_id") == 7) ++duplicate_pixels_written;
   };
   std::optional<std::array<std::string, 3>> previous;
   if (admission) previous = capture_publication_bytes(local.output);
   auto compiling = compile_constrained_coconut(config, catalog);
   const mmltk::testsupport::ScopedTestCleanup release_input([&] {
    cancelled = true;
    held = {};
   });
   mmltk::testsupport::await_test_promise(first_labels, "first duplicate's real labels before its sibling artifact", 5s);
   mmltk::testsupport::await_test_promise(validation_labels, "healthy validation labels beside pending duplicate input", 5s);
   CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
   if (previous) check_publication_bytes(local.output, (*previous)[0], (*previous)[1], (*previous)[2]);
   held = {};
   mmltk::testsupport::await_test_future(compiling, "ambiguous artifact rows retain healthy later identities", 15s);
   CHECK(duplicate_pixels_written == 1);
   config.trace = {};
   config.cancel_requested = {};
  } else
   compile_benchmark_recipe(config, &catalog, nullptr, {}, {}, {.transient_bytes = 1, .descriptors = 13});
  const auto train = CompiledDataset::open(local.output / "train.bin");
  CHECK(train.header().num_images == (cross_artifact ? 4 : 3));
  CHECK_THROWS(compiled_slot(train, 7, AnnotationSource::CoconutCoco));
  const auto labels = train.image_labels(compiled_slot(train, 11, AnnotationSource::CoconutCoco));
  REQUIRE(labels.size() == 1);
  CHECK(labels.front().source_ordinal == (issue == "duplicate annotations" ? 2 : 1));
  CHECK(labels.front().has_complete_source_ordinal());
  if (cross_artifact) {
   CHECK(labels.front().is_crowd());
   const auto later = train.image_labels(compiled_slot(train, 12, AnnotationSource::CoconutCoco));
   REQUIRE(later.size() == 1);
   CHECK(later.front().source_ordinal == 3);
   CHECK(later.front().has_complete_source_ordinal());
   CHECK_FALSE(later.front().is_crowd());
   CHECK(CompiledDataset::open(local.output / "val.bin").header().num_images == 1);
   CHECK(train.header().num_slots == 6);
   for (const auto& entry : train.image_entries())
    if (!entry.eligible()) CHECK(entry.num_instances == 0);
  }
 }
 if (cross_artifact) {
  std::ifstream input(local.cache.root / "failed.txt");
  std::string line;
  unsigned rejected = 0;
  while (std::getline(input, line)) {
   const auto record = Json::parse(line);
   if (record.at("reason") != "ambiguous annotation physical identity") continue;
   CHECK(record.at("image_id") == 7);
   CHECK(record.at("release_image_id") == 7);
   CHECK(record.at("source") == coconut_namespace_name(CoconutImageNamespace::CocoTrain));
   CHECK(record.at("release") == release.name);
   ++rejected;
  }
  CHECK(rejected == 4);  // Both offered rows, once for cold and once for warm.
  const auto previous = capture_publication_bytes(local.output);
  std::atomic<bool> cancelled{true};
  config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
  CHECK_THROWS(compile_benchmark_recipe(config, &catalog));
  check_publication_bytes(local.output, previous[0], previous[1], previous[2]);
  config.cancel_requested = {};
  catalog.release_observer = [](CoconutEdition edition, CoconutReleaseBoundary boundary) {
   if (edition == CoconutEdition::Base && boundary == CoconutReleaseBoundary::MasksStarted) throw std::runtime_error("held duplicate source failure");
  };
  CHECK_THROWS_WITH(compile_benchmark_recipe(config, &catalog), "held duplicate source failure");
  check_publication_bytes(local.output, previous[0], previous[1], previous[2]);
 }
 CHECK(file_bytes(local.cache.root / "failed.txt").find((duplicate_pixels || cross_artifact || issue == "duplicate annotations") ? "ambiguous" : "malformed") != std::string::npos);
}
TEST_CASE("warm component rows publish labels before late proof and withdraw only their labels", "[benchmark][coconut][cache][pipeline]") {
 using namespace std::chrono_literals;
 const bool corrupt_proof = GENERATE(false, true);
 ScopedTempDir root("coconut-incremental-cache-proof");
 const std::array physical{coco(7), coco(11)};
 auto input = request(physical);
 input.parquet_shards = {root.path() / "annotations.parquet"};
 const std::array<std::uint32_t, 9> pixels{1, 1, 1, 1, 1, 1, 1, 1, 1};
 write_coconut_parquet(input.parquet_shards.front(), Json::array({hf_row(7, png(3, 3, pixels), Json::array({segment()})), hf_row(11, png(3, 3, pixels), Json::array({segment()}))}));
 auto originals = fixture_index(recovery_originals(7, std::array<RLEPair, 1>{{{0, 1}}}));
 CoconutRecoveryOriginals original_lookup(&originals, nullptr);
 CoconutMaskRecovery recovery(original_lookup);
 input.recovery = &recovery;
 const auto component = import_coconut_annotations(input).front();
 const auto path = root.path() / "component.normalized.bin";
 store_coconut_component(path, component);
 const auto index_bytes = file_bytes(path);
 const auto inventory_path = std::filesystem::path(path.string() + ".inventory");
 const auto inventory_bytes = file_bytes(inventory_path);
 if (corrupt_proof) {
  auto bytes = inventory_bytes;
  bytes.back() ^= 1;
  mmltk::testsupport::write_text_file(inventory_path, bytes);
 }
 const auto images = root.path() / "images", independent = root.path() / "independent";
 prepare_cached_image_directory(images);
 prepare_cached_image_directory(independent);
 const auto jpeg = white_jpeg();
 const auto encoded = std::span(reinterpret_cast<const std::uint8_t*>(jpeg.data()), jpeg.size());
 BenchmarkEncodedImage::publish(cached_image_path(images, 7), encoded, {});
 BenchmarkEncodedImage::publish(cached_image_path(independent, 11), encoded, {});
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{images}, {independent}};
 BenchmarkCompilePipeline pipeline(1);
 BenchmarkSplitWriter writer({.split = split, .output_path = root.path() / "result.bin", .resolution = 3, .num_workers = 1});
 pipeline.bind_source(images, writer, AnnotationSource::CoconutCoco);
 pipeline.bind_source(independent, writer, AnnotationSource::CoconutCoco);
 pipeline.label_configuration(3, mmltk::backend::imaging::resample::ImageResizeMode::Stretch);
 const auto own = pipeline.source_publication(images, {}), other = pipeline.source_publication(independent, {});
 other({11});
 pipeline.labels_ready(other, 11, component.labels(1), "independent");
 pipeline.wait_image(independent, 11);
 REQUIRE(pipeline.wait_image_labels(independent, 11, "independent"));
 const auto other_slot = *pipeline.image_slot(independent, 11);
 mmltk::testsupport::TestGate late("remaining warm inventory and recovery proof");
 std::atomic<std::size_t> withdrawn{0};
 std::string dependency;
 const CoconutCacheAdmission admission{[&](const CoconutComponent& row, std::size_t position) {
  const auto id = row.index().image(position).source_image_id;
  pipeline.labels_ready(own, id, row.labels(position), row.image_input_identity(position));
  if (!position) {
   dependency = row.image_input_identity(position);
   late.receipt().ArriveAndWait();
  }
 }, [&](const CoconutComponent& rows, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) pipeline.retire_labels(images, rows.index().image(i).source_image_id, rows.image_input_identity(i));
  withdrawn = count;
 }};
 auto loading =
  std::async(std::launch::async, [&] { return load_coconut_component(path, component.edition(), component.source(), component.input_identity(), {}, false, nullptr, nullptr, false, admission); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  late.Release();
  pipeline.retire_attempt();
 });
 REQUIRE(late.WaitEntered(5s));
 REQUIRE(pipeline.wait_image_labels(images, 7, dependency));
 const auto slot = *pipeline.image_slot(images, 7);
 CHECK(writer.entry(slot).num_instances == 1);
 CHECK_FALSE(writer.image_complete(slot));
 CHECK(writer.image_complete(other_slot));
 CHECK(writer.entry(other_slot).num_instances == 1);
 CHECK(loading.wait_for(0ms) == std::future_status::timeout);
 late.Release();
 const auto loaded = mmltk::testsupport::await_test_future(loading, "warm cache terminal proof", 5s);
 CHECK(loaded.has_value() == !corrupt_proof);
 CHECK(withdrawn == (corrupt_proof ? 2U : 0U));
 CHECK(pipeline.image_slot(images, 7) == slot);
 CHECK(pipeline.image_slot(independent, 11) == other_slot);
 CHECK(pipeline.has_image_labels(images, 7, dependency) == !corrupt_proof);
 CHECK(pipeline.has_image_labels(independent, 11, "independent"));
 CHECK(writer.image_complete(other_slot));
 CHECK(writer.entry(other_slot).num_instances == 1);
 CHECK(file_bytes(path) == index_bytes);
 if (!corrupt_proof) CHECK(file_bytes(inventory_path) == inventory_bytes);
}
TEST_CASE("final COCONut caches retain optional v3 extents and supersede modern parts", "[benchmark][coconut][cache][publication]") {
 const bool corrupt_proof = GENERATE(false, true);
 const bool modern_parts = GENERATE(false, true);
 const auto edition = GENERATE(CoconutEdition::Large, CoconutEdition::Base, CoconutEdition::RelabeledValidation);
 ScopedTempDir root("coconut-version-three-reuse");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock}, true);
 auto catalog = local.selected(config.selection.validation);
 if (edition == CoconutEdition::Base) add_ready_parquet_part(local, catalog);
 compile_benchmark_recipe(config, &catalog);
 const auto expected = benchmark_semantics(local.output / "train.bin"), expected_validation = benchmark_semantics(local.output / "val.bin");
 const auto& release = *std::ranges::find(catalog.releases, edition, &CoconutReleaseComponent::edition);
 const auto directory = local.cache.source_indexes("coconut-" + std::string(release.name));
 // A real release-level v3 layout predates per-artifact caches entirely.
 if (!modern_parts) remove_coconut_part_caches(directory);
 std::vector<std::pair<std::filesystem::path, std::string>> stable;
 unsigned completions = 0;
 for (const auto& entry : std::filesystem::directory_iterator(directory)) {
  if (!entry.path().string().ends_with(".normalized.bin.complete.json")) continue;
  if (entry.path().filename().string().find(".part-") != std::string::npos) continue;
  if (!modern_parts) omit_optional_coconut_facts(entry.path());
  const auto cache_path = std::filesystem::path(entry.path().string().substr(0, entry.path().string().size() - std::string(".complete.json").size()));
  stable.emplace_back(cache_path, file_bytes(cache_path));
  stable.emplace_back(cache_path.string() + ".inventory", file_bytes(cache_path.string() + ".inventory"));
  if (corrupt_proof) {
   auto bytes = stable.back().second;
   bytes.back() ^= 1;
   mmltk::testsupport::write_text_file(stable.back().first, bytes);
  } else
   stable.emplace_back(entry.path(), file_bytes(entry.path()));
  ++completions;
 }
 REQUIRE(completions != 0);
 std::atomic<unsigned> cached{0}, normalized{0}, pixels{0}, full_release{0};
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.annotations.release_cache_selected" && Json::parse(fields).at("edition") == edition) ++full_release;
  if (event == "benchmark.annotations.cache_row" && Json::parse(fields).at("edition") == edition) ++cached;
  if (event == "benchmark.archive.image_ready") {
   const auto fields_json = Json::parse(fields);
   const auto source = edition == CoconutEdition::Large  ? CoconutImageNamespace::Objects365V2
                       : edition == CoconutEdition::Base ? CoconutImageNamespace::CocoTrain
                                                         : CoconutImageNamespace::CocoValidation;
   const auto id = edition == CoconutEdition::Large ? 1 : edition == CoconutEdition::Base ? 7 : 9;
   if (fields_json.at("artifact") == coconut_namespace_name(source) && fields_json.at("image_id") == id) ++pixels;
  }
  if (event == "benchmark.annotations.progress" && Json::parse(fields).at("edition") == edition) ++normalized;
 };
 compile_benchmark_recipe(config, &catalog);
 CHECK(cached != 0);
 if (!corrupt_proof) CHECK(normalized == 0);
 if (corrupt_proof && !modern_parts) CHECK(normalized != 0);
 if (edition != CoconutEdition::Large) CHECK(full_release == (corrupt_proof ? 0 : 1));
 CHECK(pixels == 1);
 CHECK(benchmark_semantics(local.output / "train.bin") == expected);
 CHECK(benchmark_semantics(local.output / "val.bin") == expected_validation);
 if (edition == CoconutEdition::Base) {
  const auto dataset = CompiledDataset::open(local.output / "train.bin");
  const auto& label = dataset.image_labels(compiled_slot(dataset, 11, AnnotationSource::CoconutCoco)).front();
  CHECK(label.source_ordinal == 2);
  CHECK(label.has_complete_source_ordinal());
 }
 if (!corrupt_proof && !modern_parts)
  for (const auto& entry : std::filesystem::directory_iterator(directory)) CHECK(entry.path().filename().string().find(".part-") == std::string::npos);
 for (const auto& [path, bytes] : stable) CHECK(file_bytes(path) == bytes);
}
TEST_CASE("failed part cache retracts its indexing while independent sibling labels survive held repair", "[benchmark][coconut][cache][progress][pipeline]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("coconut-part-cache-withdrawal");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Stock}, true);
 auto catalog = local.selected(config.selection.validation);
 add_ready_parquet_part(local, catalog);
 compile_benchmark_recipe(config, &catalog);
 const auto expected = benchmark_semantics(local.output / "train.bin");
 const auto& base = catalog.releases.front();
 const auto directory = local.cache.source_indexes("coconut-" + std::string(base.name));
 const auto inventory = directory / "coco-train2017.part-0.normalized.bin.inventory";
 auto invalid = file_bytes(inventory);
 invalid.back() ^= 1;
 mmltk::testsupport::write_text_file(inventory, invalid);
 auto held = ArtifactLease::acquire(local.cache.locks / (base.annotations.front().artifact_id + ".lock"), {});
 mmltk::testsupport::TestGate repair("raw normalization after failed part cache proof");
 std::atomic<bool> cancelled{false}, sibling_ready{false}, invalidation_seen{false};
 std::atomic<unsigned> starts{0}, sibling_placements{0}, sibling_pixels{0};
 std::promise<void> sibling, sibling_started, withdrawal;
 std::atomic<std::uint64_t> withdrawn_rows{0}, retained_rows{0};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.labels.placed" && Json::parse(fields).at("image_id") == 11) {
   ++sibling_placements;
   if (!sibling_ready.exchange(true)) sibling.set_value();
  }
  if (event == "benchmark.archive.image_ready" && Json::parse(fields).at("image_id") == 11) ++sibling_pixels;
 };
 config.progress = [&](const BenchmarkCompileProgress& value) {
  withdrawn_rows = value.tracks.labels.invalidated;
  if (value.tracks.labels.invalidated && !invalidation_seen.exchange(true)) {
   retained_rows = value.tracks.labels.completed;
   withdrawal.set_value();
  }
 };
 catalog.release_observer = [&](CoconutEdition edition, CoconutReleaseBoundary boundary) {
  if (edition != CoconutEdition::Base || boundary != CoconutReleaseBoundary::MasksStarted) return;
  if (starts.fetch_add(1) == 0)
   sibling_started.set_value();
  else
   repair.receipt().ArriveAndWait();
 };
 auto compiling = compile_constrained_coconut(config, catalog);
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled = true;
  held = {};
  repair.Release();
 });
 mmltk::testsupport::await_test_promise(sibling, "healthy sibling cached labels before failed artifact admission", 5s);
 mmltk::testsupport::await_test_promise(sibling_started, "healthy sibling independent input consumption", 5s);
 held = {};
 REQUIRE(repair.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(withdrawal, "failed cache indexing withdrawal before held normalization", 5s);
 CHECK(withdrawn_rows == 1);
 CHECK(retained_rows >= 1);
 CHECK(sibling_placements == 1);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 repair.Release();
 mmltk::testsupport::await_test_future(compiling, "cache repair preserves sibling products", 15s);
 CHECK(withdrawn_rows == 1);
 CHECK(sibling_placements == 1);
 CHECK(sibling_pixels == 1);
 CHECK(benchmark_semantics(local.output / "train.bin") == expected);
}
TEST_CASE("artifact identity settlement withdraws exact labels and preserves pixels and surviving ordinals", "[benchmark][coconut][pipeline][cache]") {
 const bool warm = GENERATE(false, true);
 const bool incomplete = GENERATE(false, true);
 ScopedTempDir directory("coconut-artifact-identity-join");
 const std::array identities{coco(7), coco(11), coco(12)};
 auto input = request(identities);
 const std::array<std::uint32_t, 9> foreground{1, 1, 1, 1, 1, 1, 1, 1, 1};
 std::array<std::vector<CoconutComponent>, 2> components;
 for (std::size_t part = 0; part < components.size(); ++part) {
  input.input_identity = "part-" + std::to_string(part);
  input.parquet_shards = {directory.path() / (input.input_identity + ".parquet")};
  write_coconut_parquet(
   input.parquet_shards.front(), Json::array({hf_row(7, png(3, 3, foreground), Json::array({segment()})), hf_row(part ? 12 : 11, png(3, 3, foreground), Json::array({segment()}))}));
  components[part] = import_coconut_annotations(input);
  REQUIRE(components[part].size() == 1);
  if (warm) {
   const auto path = directory.path() / (input.input_identity + ".normalized.bin");
   store_coconut_component(path, components[part].front());
   auto cached = load_coconut_component(path, CoconutEdition::Base, CoconutImageNamespace::CocoTrain, input.input_identity, {}, false, input.physical_membership);
   REQUIRE(cached);
   components[part] = {std::move(*cached)};
  }
 }
 CHECK(components[0].front().image_input_identity(0) != components[1].front().image_input_identity(0));
 const auto images = directory.path() / "images";
 prepare_cached_image_directory(images);
 const auto encoded = white_jpeg();
 for (const auto& image : identities) BenchmarkEncodedImage::publish(cached_image_path(images, image.image_id), {reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()}, {});
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{images}};
 BenchmarkCompilePipeline pipeline(1);
 std::atomic<unsigned> pixel_writes{0};
 BenchmarkSplitWriter writer(
  {.split = split, .output_path = directory.path() / "result.bin", .resolution = 3, .num_workers = 1, .progress = {.context = &pixel_writes, .image_completed = [](void* value) {
  ++*static_cast<std::atomic<unsigned>*>(value);
 }}});
 const mmltk::testsupport::ScopedTestCleanup retire([&] { pipeline.retire_attempt(); });
 const auto root = input.physical_membership->identity_root(CoconutImageNamespace::CocoTrain);
 pipeline.bind_source(root, writer, AnnotationSource::CoconutCoco);
 pipeline.label_configuration(3, mmltk::backend::imaging::resample::ImageResizeMode::Stretch);
 const auto publication = pipeline.source_publication(root, {});
 std::array<std::uint32_t, 3> slots{};
 for (std::size_t i = 0; i < identities.size(); ++i) {
  publication({.image_id = identities[i].image_id, .input_root = images});
  pipeline.wait_image(root, identities[i].image_id);
  slots[i] = *pipeline.image_slot(root, identities[i].image_id);
 }
 for (const auto& part : components) {
  const auto& component = part.front();
  const auto provisional = component.image_input_identity(0) + ".provisional";
  pipeline.labels_ready(publication, 7, component.labels(0), provisional);
  REQUIRE(pipeline.wait_image_labels(root, 7, provisional));
  for (std::size_t i = 0; i < component.index().image_count(); ++i) {
   const auto id = component.index().image(i).source_image_id;
   pipeline.labels_ready(publication, id, component.labels(i), component.image_input_identity(i));
   REQUIRE(pipeline.wait_image_labels(root, id, component.image_input_identity(i)));
  }
 }
 REQUIRE(writer.entry(slots[0]).num_instances == 1);
 REQUIRE(pixel_writes == 3);
 const std::array parts{CoconutComponentPart{components[0], 0, 0}, CoconutComponentPart{components[1], 2, incomplete ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{2}}};
 std::vector<CoconutInventoryImage> rejected;
 const auto merged = merge_coconut_components(parts, "release", *input.physical_membership, directory.path(), {}, nullptr, &pipeline, [&](const CoconutInventoryImage& image, std::string_view reason) {
  CHECK_FALSE(pipeline.execution().on_lane());
  CHECK(reason == "ambiguous annotation physical identity");
  rejected.push_back(image);
 });
 REQUIRE(rejected.size() == 2);
 for (const auto& image : rejected) CHECK(image.physical == identities.front());
 REQUIRE(merged.size() == 1);
 REQUIRE(merged.front().index().image_count() == 2);
 CHECK(merged.front().inventory_image(0).physical.image_id == 11);
 CHECK(merged.front().inventory_image(0).source_ordinal == 1);
 CHECK(merged.front().inventory_image(1).physical.image_id == 12);
 CHECK(merged.front().inventory_image(1).source_ordinal == 3);
 CHECK(merged.front().source_ordinals_complete(0));
 CHECK(merged.front().source_ordinals_complete(1) == !incomplete);
 CHECK(merged.front().index().storage().boxes[0].source_ordinal == 1);
 CHECK(merged.front().index().storage().boxes[1].source_ordinal == (incomplete ? 1 : 3));
 CHECK(((merged.front().index().storage().boxes[1].flags & kAnnotationOrdinalIncomplete) != 0) == incomplete);
 for (const auto& part : components) {
  const auto& component = part.front();
  CHECK_FALSE(pipeline.has_image_labels(root, 7, component.image_input_identity(0)));
  CHECK_FALSE(pipeline.has_image_labels(root, 7, component.image_input_identity(0) + ".provisional"));
  CHECK(pipeline.has_image_labels(root, component.index().image(1).source_image_id, component.image_input_identity(1)));
 }
 CHECK(writer.entry(slots[0]).num_instances == 0);
 CHECK(writer.entry(slots[1]).num_instances == 1);
 CHECK(writer.entry(slots[2]).num_instances == 1);
 for (std::size_t i = 0; i < identities.size(); ++i) {
  CHECK(pipeline.image_slot(root, identities[i].image_id) == slots[i]);
  CHECK(writer.image_complete(slots[i]));
 }
 CHECK(pixel_writes == 3);
}
TEST_CASE("custom source admission keeps ready label peers useful while its first projection is held", "[benchmark][coconut][pipeline][resources]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPU lanes");
 ScopedTempDir root("custom-label-ready-window");
 LocalCoconutRecipe local(root.path());
 auto [custom, ids] = custom_training_window(local);
 auto config = local.compiler_config({BenchmarkDatasetVariant::CocoCustom, CoconutValidation::CoconutStock}, true);
 config.num_workers = 4;
 compile_benchmark_recipe(config, nullptr, &custom);
 const auto image_root = local.cache.source_images("coco") / "train2017";
 mmltk::testsupport::TestGate first("first ready custom label projection");
 std::atomic<bool> cancelled{false}, armed{false}, peer_delivered{false};
 DeferredCancellationGate observation{cancelled};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Borrow(observation);
 std::promise<void> peer_written;
 std::mutex mutex;
 std::set<std::uint64_t> peers;
 const auto diagnostic_trace = config.trace;
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (diagnostic_trace) diagnostic_trace(event, fields);
  if (event != "benchmark.labels.projection_started" && event != "benchmark.labels.slot_written") return;
  const auto value = Json::parse(fields);
  if (value.at("root").get<std::string>() != image_root.string()) return;
  const auto id = value.at("image_id").get<std::uint64_t>();
  if (event == "benchmark.labels.projection_started" && id == ids.front() && !armed.exchange(true)) {
   DeferredCancellationGate::next() = &first;
  } else if (event == "benchmark.labels.slot_written" && id != ids.front()) {
   {
    const std::lock_guard lock(mutex);
    peers.insert(id);
    if (peers.size() == ids.size() - 1 && !peer_delivered.exchange(true)) peer_written.set_value();
   }
  }
 };
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, nullptr, &custom); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled = true;
  first.Release();
 });
 REQUIRE(first.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(peer_written, "every independent label refills the window before the first selected result", 5s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 {
  const std::lock_guard lock(mutex);
  CHECK_FALSE(peers.empty());
  CHECK(peers.size() == ids.size() - 1);
 }
 first.Release();
 mmltk::testsupport::await_test_future(compiling, "bounded custom label ready-window completion", 5s);
 const auto compiled = CompiledDataset::open(local.output / "train.bin");
 std::set<std::uint64_t> observed;
 for (const auto& entry : compiled.image_entries())
  if (entry.eligible() && entry.source == AnnotationSource::Coco) observed.insert(entry.source_image_id);
 CHECK(observed == std::set<std::uint64_t>(ids.begin(), ids.end()));
}
TEST_CASE("native PNG normalization borrows the pinned compile context with retained row custody", "[benchmark][coconut][annotations][resources]") {
 using namespace mmltk::backend::data::detail;
 CoconutRecord record;
 record.image_id = 7;
 record.width = 3;
 record.height = 2;
 record.segments = {{.id = 1, .category_id = 1, .isthing = true}, {.id = 2, .category_id = 2, .isthing = true}};
 const std::array<std::uint32_t, 6> ids{1, 2, 2, 1, 1, 2};
 const auto encoded = png(3, 2, ids);
 const auto input = std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
 CoconutNativeWorkspace workspace(CoconutImportLimits{});
 const auto dimensions = workspace.admit_png(record, input);
 const auto bytes = workspace.decoder_workspace_bytes(input) + workspace.support_workspace_bytes(record, ids.size(), record.segments.size());
 CompileExecution execution(1, {}, {.transient_bytes = bytes + encoded.size()}, {}, [](std::string_view, const CompileExecutionSnapshot&) {});
 auto row = execution.reserve({encoded.size(), 0});
 const void* base = nullptr;
 for (unsigned pass = 0; pass < 2; ++pass) {
  CompileAllowance borrowed;
  execution
   .submit(
    {.kind = CompileTaskKind::Normalize,
     .resources = {.bytes = bytes, .supplied_workspace = true},
     .work =
      [&](std::size_t, const CompileAllowance& allowance) {
   borrowed = allowance;
   const auto memory = allowance.workspace();
   if (base)
    CHECK(memory.data() == base);
   else
    base = memory.data();
   workspace.decode(record, input, dimensions, memory);
  },
     .parent = row,
     .finite_consumer = true,
     .retain_workspace = false})
   .wait();
  const auto support = workspace.support(record.segments.size());
  REQUIRE(support.size() == 2);
  CHECK(support[0].area == 3);
  CHECK(support[1].area == 3);
  REQUIRE(support[0].runs.view().size() == 2);
  CHECK(support[0].runs.view()[0].start == 0);
  CHECK(support[0].runs.view()[0].length == 1);
  CHECK(support[0].runs.view()[1].start == 3);
  CHECK(support[0].runs.view()[1].length == 2);
  workspace.release_supplied();
  borrowed = {};
  CHECK(execution.resource_usage().bytes == encoded.size());
 }
 row = {};
 CHECK(execution.resource_usage().bytes == 0);
 CHECK(execution.snapshot().workspace_reuses >= 1);
}
TEST_CASE("archive publication retains its forward decoder through ordinary backpressure", "[benchmark][coconut][archive][resources]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 const bool files = GENERATE(false, true);
 const bool pressure = GENERATE(false, true);
 CAPTURE(files, pressure);
 ScopedTempDir directory("archive-publication-backpressure");
 const auto path = directory.path() / "source.tar.gz", output = directory.path() / "images";
 const auto jpeg = white_jpeg();
 const std::array<std::pair<std::string, std::string>, 1> members{{{"1.jpg", jpeg}}};
 tar(path, members);
 prepare_cached_image_directory(output);
 mmltk::common::io::FileHandle descriptor(::open(output.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
 REQUIRE(descriptor.get() >= 0);
 BenchmarkCompilePipeline pipeline(2, {}, {.transient_bytes = 512U << 20});
 BenchmarkArchive reader(path, &pipeline.execution());
 REQUIRE(reader.next());
 const auto decoder_bytes = reader.retained_workspace_bytes();
 REQUIRE(decoder_bytes > 0);
 mmltk::testsupport::TestGate waiting("ordinary publication capacity wait"), quiesced("publication responds to resource pressure");
 std::atomic<unsigned> waits{0};
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json&) {
  if (files && pressure && event == "benchmark.images.input_released") quiesced.receipt().ArriveAndWait();
  if (event != (files ? "benchmark.images.file_wait" : "benchmark.images.buffer_wait")) return;
  const auto count = ++waits;
  if (count == 1)
   waiting.receipt().ArriveAndWait();
  else if (count == 2 && pressure)
   quiesced.receipt().ArriveAndWait();
 };
 BenchmarkImagePublication publication(output, {}, nullptr, {}, &pipeline, descriptor.get(), true, reader, trace);
 std::vector<BenchmarkImagePublication::Input> inputs;
 std::vector<mmltk::backend::data::detail::FileWorkQueue::Reservation> records;
 if (files) {
  while (auto record = pipeline.files().try_reserve_record()) records.push_back(std::move(*record));
  REQUIRE_FALSE(records.empty());
 } else {
  for (unsigned i = 0; i < 4; ++i) inputs.push_back(publication.acquire(jpeg.size()));
 }
 std::unique_ptr<CompileResourceWait> resource_wait;
 std::future<void> work;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  waiting.Release();
  quiesced.Release();
  resource_wait.reset();
  inputs.clear();
  records.clear();
  pipeline.execution().cancel();
  if (work.valid()) work.wait();
 });
 work = std::async(std::launch::async, [&] {
  auto input = publication.acquire(jpeg.size());
  publication.read(input);
  require_condition(std::ranges::equal(input->encoded, std::span(reinterpret_cast<const std::uint8_t*>(jpeg.data()), jpeg.size())), "backpressure changed the opened archive payload");
  const auto header = BenchmarkImageDecoder{}.read_header(input->encoded);
  publication.submit(1, std::move(input), header, {}, [](const CachedImageReady&, std::exception_ptr failure) {
   if (failure) std::rethrow_exception(failure);
  });
  publication.finish();
 });
 REQUIRE(waiting.WaitEntered(5s));
 CHECK_FALSE(pipeline.execution().resource_pressure());
 CHECK(reader.retained_workspace_bytes() == decoder_bytes);
 if (pressure) {
  resource_wait = pipeline.execution().defer_resources();
  waiting.Release();
  REQUIRE(quiesced.WaitEntered(5s));
  CHECK(reader.retained_workspace_bytes() == 0);
  resource_wait.reset();
 }
 inputs.clear();
 records.clear();
 waiting.Release();
 quiesced.Release();
 mmltk::testsupport::await_test_future(work, "forward publication resumes from bounded backpressure", 5s);
 CHECK(mmltk::common::io::sha256_file(cached_image_path(output, 1)) == mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(jpeg.data()), jpeg.size())));
 CHECK(pipeline.execution().snapshot().leased == 0);
}
TEST_CASE("one physical artifact refills from completed pixels while its first consumer is held", "[benchmark][coconut][pipeline]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPUs");
 ScopedTempDir directory("physical-publication-window");
 const auto cache = BenchmarkCacheLayout::create(directory.path() / "cache");
 const auto archive = cache.source_downloads("coco") / "physical.tar";
 std::vector<std::pair<std::string, std::string>> members;
 for (unsigned id = 1; id <= 12; ++id) members.emplace_back(coco(id).member, white_jpeg());
 tar(archive, members);
 const auto catalog = base_physical_catalog(archive, static_cast<std::uint32_t>(members.size()));
 bool warm = false, mixed = false, hold_write = false, cancel_write = false;
 SECTION("cold forward source") {}
 SECTION("cold writes overlap later members and their pixels") { hold_write = true; }
 SECTION("cancellation joins the held cache write before releasing its input") { hold_write = cancel_write = true; }
 SECTION("retained physical inventory") { warm = true; }
 SECTION("retained healthy peers with missing input") { warm = mixed = true; }
 if (warm) {
  PhysicalMembershipFixture cached(cache, catalog);
  if (mixed) std::filesystem::remove(cached_image_path(cache.source_images("coco") / "train2017", 2));
 }
 auto admitted = local_physical_archives(cache, catalog);
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 struct Observed {
  mmltk::testsupport::TestGate held{"first physical pixel retains its completion"};
  std::atomic<unsigned> completed{0};
  std::promise<void> independent;
  unsigned expected = 12;
  bool hold_write = false;
  std::atomic<unsigned> writes{0};
  std::mutex mutex;
  std::vector<std::thread::id> services;
 } observed;
 observed.expected = mixed || hold_write ? 11 : 12;
 observed.hold_write = hold_write;
 const BenchmarkTraceSink trace = [&](std::string_view event, const Json& value) {
  if (event != "file.running" || value.at("service") != "output" || value.at("bytes") != members.front().second.size()) return;
  {
   const std::lock_guard lock(observed.mutex);
   const auto thread = std::this_thread::get_id();
   if (std::ranges::find(observed.services, thread) == observed.services.end()) observed.services.push_back(thread);
  }
  if (!observed.writes.fetch_add(1) && hold_write) observed.held.receipt().ArriveAndWait();
 };
 BenchmarkCompilePipeline pipeline(4, {}, {}, {}, trace);
 CoconutPhysicalMembership physical(admitted, catalog, true, cache, pipeline.storage(), pipeline);
 BenchmarkWriteRequest request{
  .split = split,
  .output_path = directory.path() / "pixels.bin",
  .resolution = 3,
  .num_workers = 4,
  .progress =
   {.context = &observed,
    .image_completed =
     [](void* context) {
  auto& value = *static_cast<Observed*>(context);
  const auto count = value.completed.fetch_add(1);
  if (!count && !value.hold_write)
   value.held.receipt().ArriveAndWait();
  else if (count + 1 == value.expected)
   value.independent.set_value();
 }},
  .execution = &pipeline
 };
 BenchmarkSplitWriter writer(request, true);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 pipeline.bind_source(physical.identity_root(CoconutImageNamespace::CocoTrain), writer, AnnotationSource::CoconutCoco);
 physical.admitted(admitted.front(), admitted.front().download);
 auto consuming = std::async(std::launch::async, [&] { return physical.consume(admitted.front(), {}, warm && !mixed); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { observed.held.Release(); });
 REQUIRE(observed.held.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(observed.independent, "same-artifact pixels refill past a held first completion", 5s);
 CHECK(consuming.wait_for(0ms) == std::future_status::timeout);
 CHECK_FALSE(physical.discovery_complete());
 if (hold_write) {
  CHECK(observed.writes == members.size());
  const std::lock_guard lock(observed.mutex);
  CHECK(observed.services.size() == 2);
 }
 if (cancel_write) {
  pipeline.execution().cancel();
  CHECK(consuming.wait_for(0ms) == std::future_status::timeout);
  CHECK(pipeline.execution().snapshot().leased >= members.front().second.size());
 }
 observed.held.Release();
 if (cancel_write) {
  CHECK_THROWS(mmltk::testsupport::await_test_future(consuming, "cancelled physical publication joins its writer", 5s));
  pipeline.files().join();
  CHECK_FALSE(physical.discovery_complete());
  CHECK(writer.completed() == members.size() - 1);
  CHECK(pipeline.execution().snapshot().leased == 0);
  return;
 }
 const auto complete = mmltk::testsupport::await_test_future(consuming, "physical publication window drain", 5s);
 if (!complete) REQUIRE(physical.consume(admitted.front()));
 CHECK(writer.completed() == members.size());
 CHECK(physical.summary(admitted.front()).image_count == members.size());
 CHECK(physical.discovery_complete());
}
TEST_CASE("custom COCO labels and pixels progress before unrelated metadata and image inputs settle", "[benchmark][pipeline][annotations]") {
 using namespace std::chrono_literals;
 bool hold_images = false;
 SECTION("COCO pixels and labels precede the supplemental metadata join") {}
 SECTION("canvas labels precede their held JPEG source") { hold_images = true; }
 ScopedTempDir directory("custom-independent-canvas-labels");
 LocalCoconutRecipe local(directory.path());
 auto catalog = local_custom_catalog(local);
 auto config = local.compiler_config({BenchmarkDatasetVariant::CocoCustom, CoconutValidation::Stock});
 config.num_workers = 1;
 std::atomic<bool> cancelled{false}, label_seen{false}, image_seen{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 auto metadata = ArtifactLease::acquire(local.cache.locks / "objects365-annotations.lifecycle.lock", {});
 auto images = hold_images ? ArtifactLease::acquire(local.cache.locks / "coco-train2017.images.lock", {}) : ArtifactLease{};
 const auto root = local.cache.source_images("coco") / "train2017";
 std::promise<void> labels, pixels;
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event == "benchmark.labels.slot_written") {
   const auto value = Json::parse(fields);
   if (value.at("root").get<std::string>() == root.string() && value.at("image_id") == 7 && !label_seen.exchange(true)) labels.set_value();
  }
  if (event == "benchmark.images.complete" && Json::parse(fields).at("root").get<std::string>() == root.string() && !image_seen.exchange(true)) pixels.set_value();
 };
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, nullptr, &catalog); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled = true;
  images = {};
  metadata = {};
 });
 mmltk::testsupport::await_test_promise(labels, "canvas labels complete before held unrelated metadata", 5s);
 if (hold_images)
  CHECK_FALSE(image_seen.load());
 else
  mmltk::testsupport::await_test_promise(pixels, "physical COCO completion precedes the supplemental metadata join", 5s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 images = {};
 metadata = {};
 mmltk::testsupport::await_test_future(compiling, "independent custom sources retain canonical final selection", 10s);
 check_custom_compilation(local.output);
}
TEST_CASE("physical cached failures share bounded ordered repair on the retained archive generation", "[benchmark][coconut][archive][cache]") {
 const bool compressed = GENERATE(false, true);
 ScopedTempDir directory("physical-collected-cache-repair");
 const auto cache = BenchmarkCacheLayout::create(directory.path() / "cache");
 const auto archive = cache.source_downloads("coco") / (compressed ? "physical.tar.gz" : "physical.tar");
 const auto jpeg = white_jpeg();
 const std::array<std::pair<std::string, std::string>, 3> members{{{coco(1).member, jpeg}, {coco(2).member, jpeg}, {coco(3).member, jpeg}}};
 tar(archive, members);
 const auto catalog = base_physical_catalog(archive, members.size());
 { PhysicalMembershipFixture cached(cache, catalog); }
 const auto images = cache.source_images("coco") / "train2017";
 const auto peer = cached_image_path(images, 3);
 struct stat before{};
 REQUIRE(::stat(peer.c_str(), &before) == 0);
 auto admitted = local_physical_archives(cache, catalog);
 BenchmarkCompilePipeline pipeline(1);
 CoconutPhysicalMembership physical(admitted, catalog, true, cache, pipeline.storage(), pipeline);
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 std::array<unsigned, 3> opens{};
 BenchmarkSplitWriter writer({.split = split,
                              .output_path = directory.path() / "pixels.bin",
                              .resolution = 3,
                              .num_workers = 1,
                              .image_opened =
                               [&](const auto&, std::uint64_t id) {
  const auto attempt = ++opens.at(id - 1);
  if (id == 1 && attempt == 2) {
   // The forward owner is open before the second cached decode. Collected
   // raw extents or gzip replay must retain that inode after path replacement.
   std::filesystem::rename(archive, archive.string() + ".opened");
   mmltk::testsupport::write_text_file(archive, "replacement is not an archive");
  }
  if (id < 3 && attempt <= 2) throw BenchmarkImageError("injected cached image read failure");
 },
                              .execution = &pipeline},
  true);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 pipeline.bind_source(physical.identity_root(CoconutImageNamespace::CocoTrain), writer, AnnotationSource::CoconutCoco);
 physical.admitted(admitted.front(), admitted.front().download);
 REQUIRE(physical.consume(admitted.front()));
 CHECK(opens == std::array<unsigned, 3>{3, 3, 1});
 CHECK(writer.completed() == members.size());
 CHECK(physical.summary(admitted.front()).image_count == members.size());
 CHECK(physical.summary(admitted.front()).image_bytes == members.size() * jpeg.size());
 struct stat after{};
 REQUIRE(::stat(peer.c_str(), &after) == 0);
 CHECK(after.st_ino == before.st_ino);
 CHECK(file_bytes(peer) == jpeg);
 CHECK(physical.discovery_complete());
}
TEST_CASE("continuing physical repair advances beside an unrelated accepted pixel", "[benchmark][coconut][archive][cache][pipeline]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPUs");
 const bool compressed = GENERATE(false, true);
 ScopedTempDir directory("physical-repair-continuation");
 const auto cache = BenchmarkCacheLayout::create(directory.path() / "cache");
 const auto archive = cache.source_downloads("coco") / (compressed ? "physical.tar.gz" : "physical.tar");
 constexpr unsigned held_id = 18;
 std::vector<std::pair<std::string, std::string>> members{{coco(held_id).member, white_jpeg()}};
 for (unsigned id = 1; id < held_id; ++id) members.emplace_back(coco(id).member, white_jpeg());
 tar(archive, members);
 const auto catalog = base_physical_catalog(archive, static_cast<std::uint32_t>(members.size()));
 { PhysicalMembershipFixture cached(cache, catalog); }
 auto admitted = local_physical_archives(cache, catalog);
 BenchmarkCompilePipeline pipeline(4);
 CoconutPhysicalMembership physical(admitted, catalog, true, cache, pipeline.storage(), pipeline);
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 std::array<unsigned, held_id> opens{};
 mmltk::testsupport::TestGate held("unrelated cached pixel stays accepted during forward repair");
 struct Observed {
  std::promise<void> repaired;
  std::atomic<unsigned> completed{0};
 } observed;
 BenchmarkSplitWriter writer({.split = split,
                              .output_path = directory.path() / "pixels.bin",
                              .resolution = 3,
                              .num_workers = 4,
                              .progress =
                               {.context = &observed,
                                .image_completed =
                                 [](void* context) {
  auto& value = *static_cast<Observed*>(context);
  if (!value.completed.fetch_add(1)) value.repaired.set_value();
 }},
                              .image_opened =
                               [&](const auto&, std::uint64_t id) {
  const auto attempt = ++opens.at(id - 1);
  if (attempt == 1 || (id != held_id && attempt == 2)) throw BenchmarkImageError("cached pixel needs its forward source repair");
  if (id == held_id) held.receipt().ArriveAndWait();
 },
                              .execution = &pipeline},
  true);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 pipeline.bind_source(physical.identity_root(CoconutImageNamespace::CocoTrain), writer, AnnotationSource::CoconutCoco);
 physical.admitted(admitted.front(), admitted.front().download);
 // Record the matching inventory generation, while proving its failed cached
 // pixels need repair. The continuing pass then keeps one unrelated peer held.
 REQUIRE_FALSE(physical.consume(admitted.front(), {}, true));
 auto consuming = std::async(std::launch::async, [&] { return physical.consume(admitted.front()); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { held.Release(); });
 REQUIRE(held.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(observed.repaired, "a continuing repair compiles before its unrelated peer settles", 5s);
 CHECK(consuming.wait_for(0ms) == std::future_status::timeout);
 CHECK_FALSE(physical.discovery_complete());
 held.Release();
 REQUIRE(mmltk::testsupport::await_test_future(consuming, "forward repair reconciles its exact source generation", 5s));
 CHECK(writer.completed() == members.size());
 CHECK(physical.summary(admitted.front()).image_count == members.size());
 CHECK(opens.back() == 2);
 for (unsigned id = 1; id < held_id; ++id) CHECK(opens[id - 1] == 3);
}
TEST_CASE("Parquet artifact readiness bypasses a held batch borrower", "[benchmark][coconut][parquet][pipeline][resources]") {
 using namespace mmltk::backend::data::detail;
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 ScopedTempDir root("parquet-artifact-readiness");
 const std::array<std::uint32_t, 1> pixel{1};
 const auto encoded = png(1, 1, pixel);
 std::vector<std::filesystem::path> paths;
 SECTION("independent artifacts") {
  paths = {root.path() / "held.parquet", root.path() / "ready.parquet"};
  for (std::size_t i = 0; i < paths.size(); ++i) write_coconut_parquet(paths[i], Json::array({hf_row(static_cast<std::uint32_t>(i + 1), encoded, Json::array({segment()}), 1, 1)}));
 }
 SECTION("later groups in one artifact") {
  paths = {root.path() / "groups.parquet"};
  write_coconut_parquet(paths.front(), Json::array({hf_row(1, encoded, Json::array({segment()}), 1, 1), hf_row(2, encoded, Json::array({segment()}), 1, 1)}));
 }
 BenchmarkCompilePipeline pipeline(2, {}, {.transient_bytes = 1ULL << 30, .descriptors = 13});
 auto semantic = pipeline.execution().open_class(CompileWorkClass::Annotations);
 mmltk::testsupport::TestGate held("first artifact retains its immutable batch");
 std::promise<void> sibling;
 auto importing = std::async(std::launch::async, [&] {
  read_coconut_parquet(paths, {}, {}, [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput& input) {
   if (input.membership) return;
   require_condition(!pipeline.execution().on_lane(), "semantic handoff is nested inside input projection");
   if (record.image_id == 1) {
    auto task = pipeline.execution().submit({.kind = CompileTaskKind::Normalize, .work = [&, backing = input.backing, png = input.png](std::size_t, const CompileAllowance&) {
     require_condition(std::string(png.begin(), png.end()) == encoded, "held batch lost its immutable PNG");
     held.receipt().ArriveAndWait();
    }});
    input.admitted(task);
   } else
    mmltk::testsupport::release_test_promise(sibling);
  }, false, &pipeline.execution(), &pipeline.files());
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  held.Release();
  pipeline.execution().cancel();
 });
 REQUIRE(held.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(sibling, "ready artifact projects while its sibling retains a normal batch", 5s);
 const auto snapshot = pipeline.execution().snapshot();
 CHECK(snapshot.live[static_cast<std::size_t>(CompileWorkClass::Extraction)]);
 CHECK(snapshot.live[static_cast<std::size_t>(CompileWorkClass::Annotations)]);
 CHECK(work_class(CompileTaskKind::Projection) == CompileWorkClass::Extraction);
 held.Release();
 mmltk::testsupport::await_test_future(importing, "both artifact continuations settle", 5s);
}
// CLEANUP-IGNORE: Independent normalization and staging-retirement cases only share namespace and CPU eligibility setup.
TEST_CASE("independent native normalization enters shared annotation lanes", "[benchmark][coconut][annotations][pipeline]") {
 using namespace mmltk::backend::data::detail;
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires an Extraction floor and two Annotation lanes");
 ScopedTempDir root("coconut-native-lanes");
 const std::array<std::uint32_t, 1> background{};
 const auto encoded = png(1, 1, background);
 const std::array physical{coco(1), coco(2)};
 auto input = request(physical);
 input.parquet_shards = {root.path() / "rows.parquet"};
 write_coconut_parquet(
  input.parquet_shards.front(), Json::array({hf_row(1, encoded, Json::array({segment()}), 1, 1), hf_row(2, encoded, Json::array({segment()}), 1, 1)}), coconut_parquet_schema(), 2);
 BenchmarkCompilePipeline pipeline(3, {}, {.transient_bytes = 1ULL << 30, .descriptors = 13});
 input.execution = &pipeline;
 input.inventory_directory = root.path() / "staging";
 mmltk::testsupport::TestGate entered("independent native support normalization");
 input.rejected_object = [&](const auto&, const auto&, const auto&, auto, auto) { entered.receipt().ArriveAndWait(); };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  entered.Release();
  pipeline.execution().cancel();
 });
 REQUIRE(entered.WaitEntered(5s, 2));
 CHECK(pipeline.execution().snapshot().running[static_cast<std::size_t>(CompileWorkClass::Annotations)] == 2);
 entered.Release();
 const auto result = mmltk::testsupport::await_test_future(importing, "independent native products gather in source order", 5s);
 REQUIRE(result.size() == 1);
 CHECK(result.front().index().image_count() == 2);
 CHECK(result.front().index().rejected.degenerate_boxes == 2);
}
TEST_CASE("a legal exceptional native row releases its own pinned Parquet batch", "[benchmark][coconut][parquet][annotations][resources]") {
 using namespace mmltk::backend::data::detail;
 using namespace std::chrono_literals;
 ScopedTempDir root("coconut-exceptional-batch");
 const std::array<CompileWorkspaceProfile, 1> profiles{{{64U << 10, 0}}};
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(1, profiles, kParquetPoolBytes, 0, 320ULL << 20));
 BenchmarkCompilePipeline pipeline(1, {}, {.descriptors = 13, .workspace_plan = plan});
 const auto locked = pipeline.execution().snapshot().normal_locked;
 auto source = pipeline.execution().reserve(CompileResources::handles(0, false, pipeline.execution().descriptor_limit()));
 std::vector<std::uint32_t> pixels(64 * 64, 1);
 const auto encoded = png(64, 64, pixels);
 const std::array physical{coco(7)};
 auto input = request(physical);
 input.execution = &pipeline;
 input.parent_allowance = source;
 input.inventory_directory = root.path() / "staging";
 input.records = std::make_shared<CoconutAnnotationRecords>();
 input.records->identity = input.input_identity;
 // All descriptor headroom belongs to this source. Native rollover must draw
 // its actual descriptor from that promise even while its arena is exceptional.
 input.records->staging = std::make_shared<CoconutStaging>(input.inventory_directory, &pipeline.storage(), 1);
 input.parquet_shards = {root.path() / "row.parquet"};
 write_coconut_parquet(input.parquet_shards.front(), Json::array({hf_row(7, encoded, Json::array({segment()}), 64, 64)}));
 std::atomic<bool> transferred{false};
 input.trace = [&](std::string_view event, const Json& value) {
  if (event == "benchmark.coconut.projected_batch" && value.at("staged_transfer").get<bool>()) transferred = true;
 };
 mmltk::testsupport::TestGate completed("exceptional native values outlive the retired arena");
 input.progress = [&](std::uint64_t count) {
  if (count == 1) completed.receipt().ArriveAndWait();
 };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  completed.Release();
  pipeline.execution().cancel();
 });
 REQUIRE(completed.WaitEntered(5s));
 CHECK(transferred.load());
 CHECK(pipeline.execution().snapshot().exceptional == 0);
 CHECK(pipeline.execution().snapshot().normal_locked == locked);
 completed.Release();
 const auto result = mmltk::testsupport::await_test_future(importing, "oversized row returns to the retained source generation", 5s);
 REQUIRE(result.size() == 1);
 REQUIRE(result.front().index().box_count() == 1);
 CHECK(result.front().index().storage().mask_rle_pairs.front().length == pixels.size());
 CHECK(pipeline.execution().snapshot().exceptional == 0);
 CHECK(pipeline.execution().snapshot().normal_locked == locked);
}
TEST_CASE("one pinned normalizer context drains every row of a shared Parquet batch", "[benchmark][coconut][parquet][annotations][resources]") {
 using namespace mmltk::backend::data::detail;
 ScopedTempDir root("coconut-single-normal-context");
 const std::array<std::uint32_t, 1> pixel{1};
 const auto encoded = png(1, 1, pixel);
 const auto png_bytes = std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
 CoconutRecord record;
 record.width = record.height = 1;
 record.segments = {{.id = 1, .category_id = 1, .isthing = true}};
 CoconutNativeWorkspace workspace(CoconutImportLimits{});
 const auto bytes = workspace.decoder_workspace_bytes(png_bytes) + workspace.support_workspace_bytes(record, 1, 1);
 const std::array<CompileWorkspaceProfile, 1> profiles{{{bytes, 0}}};
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(1, profiles, kParquetPoolBytes, 0));
 BenchmarkCompilePipeline pipeline(1, {}, {.descriptors = 13, .workspace_plan = plan});
 struct Observation {
  CompileExecution& execution;
  mutable std::atomic<bool> projected{false}, normalized{false};
  bool cancelled() const noexcept {
   if (execution.on_lane()) {
    const auto snapshot = execution.snapshot();
    if (snapshot.running[static_cast<std::size_t>(CompileWorkClass::Extraction)]) projected = true;
    if (snapshot.running[static_cast<std::size_t>(CompileWorkClass::Annotations)]) normalized = true;
   }
   return false;
  }
 } observation{pipeline.execution()};
 std::vector<CoconutPhysicalImage> physical;
 Json rows = Json::array();
 for (unsigned id = 1; id <= 8; ++id) {
  physical.push_back(coco(id));
  rows.push_back(hf_row(id, encoded, Json::array({segment()}), 1, 1));
 }
 auto input = request(physical);
 input.execution = &pipeline;
 input.inventory_directory = root.path() / "staging";
 input.cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(observation);
 input.parquet_shards = {root.path() / "batch.parquet"};
 write_coconut_parquet(input.parquet_shards.front(), rows, coconut_parquet_schema(), 8);
 const auto locked = pipeline.execution().snapshot().normal_locked;
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] { pipeline.execution().cancel(); });
 const auto components = mmltk::testsupport::await_test_future(importing, "one context settles all retained batch descendants");
 REQUIRE(components.size() == 1);
 CHECK(components.front().index().image_count() == 8);
 CHECK(components.front().index().box_count() == 8);
 CHECK(observation.projected.load());
 CHECK(observation.normalized.load());
 CHECK(pipeline.execution().snapshot().exceptional == 0);
 CHECK(pipeline.execution().snapshot().normal_locked == locked);
}
TEST_CASE("physical and original recovery subscriptions bypass a parked first row", "[benchmark][coconut][pipeline][recovery]") {
 using namespace std::chrono_literals;
 bool cancel_parked = false;
 SECTION("a ready physical sibling settles before train originals") {}
 SECTION("cancellation wakes the parked physical and original dependencies") { cancel_parked = true; }
 ScopedTempDir root("coconut-recovery-readiness");
 const auto cache = BenchmarkCacheLayout::create(root.path() / "cache");
 const auto train = cache.source_downloads("coco") / "train.tar";
 const auto unlabeled = cache.source_downloads("coco") / "unlabeled.tar";
 tar(train, std::array{std::pair{coco(1).member, white_jpeg(1, 1)}});
 constexpr unsigned peers = 32;
 std::vector<std::pair<std::string, std::string>> members;
 for (unsigned id = 2; id <= peers + 1; ++id) members.emplace_back(coco(id, CoconutImageNamespace::CocoUnlabeled).member, white_jpeg(1, 1));
 tar(unlabeled, members);
 const auto catalog = base_physical_catalog(train, peers + 1, unlabeled);
 PhysicalMembershipFixture fixture(cache, catalog, 1, false);
 auto& pipeline = fixture.execution;
 auto& physical = fixture.physical;
 std::atomic<bool> terminal{false};
 CoconutImportRequest input;
 input.execution = &pipeline;
 input.physical_membership = &physical;
 input.input_identity = "readiness-generation";
 input.records = std::make_shared<CoconutAnnotationRecords>();
 input.cancellation = fixture.cancellation;
 input.parquet_shards = {root.path() / "rows.parquet"};
 input.expected_rows = peers + 1;
 const std::array<std::uint32_t, 1> pixel{1};
 Json rows = Json::array({hf_row(1, png(1, 1, pixel), Json::array({segment()}), 1, 1)});
 for (unsigned id = 2; id <= peers + 1; ++id) rows.push_back(hf_row(id, png(1, 1, pixel), Json::array({segment()}), 1, 1, "unlabeled2017"));
 write_coconut_parquet(input.parquet_shards.front(), rows);
 CompileExecution::CompletionSource originals;
 input.originals = [&](CoconutImageNamespace source, bool, std::stop_token) {
  if (source != CoconutImageNamespace::CocoTrain) throw std::logic_error("unlabeled recovery requested train originals");
  return CoconutOriginalReadiness{{}, terminal.load(), 1, originals.completion()};
 };
 std::promise<void> source_done, sibling, train_parked;
 std::atomic<unsigned> train_observations{0}, settled_peers{0}, retired_rows{0};
 std::atomic<std::uint64_t> completed_rows{0};
 std::atomic<bool> final_rows_before_join{false};
 input.progress = [&](std::uint64_t rows_complete) { completed_rows = rows_complete; };
 input.trace = [&](std::string_view event, const Json& fields) {
  const auto id = fields.value("image_id", std::uint64_t{0});
  if (event == "benchmark.annotations.physical_join_wait") final_rows_before_join = completed_rows.load() == peers + 1;
  if (event == "benchmark.coconut.recovery_observed" && id == 1) ++train_observations;
  if (event == "benchmark.coconut.recovery_retired") ++retired_rows;
  if (event == "benchmark.coconut.recovery_parked" && id == 1) mmltk::testsupport::release_test_promise(train_parked);
  if (event == "benchmark.coconut.recovery_settled" && id != 1 && ++settled_peers == peers) mmltk::testsupport::release_test_promise(sibling);
 };
 input.input_finished = [&] { source_done.set_value(); };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  fixture.cancelled.store(true);
  terminal.store(true);
  originals.settle();
  pipeline.execution().cancel();
 });
 mmltk::testsupport::await_test_promise(source_done, "both native rows release their forward source before physical readiness", 5s);
 mmltk::testsupport::await_test_promise(train_parked, "train row atomically subscribes to its own missing facts", 5s);
 const auto initial_observations = train_observations.load();
 CHECK_FALSE(input.records->row(0).native());
 CHECK_FALSE(input.records->row(1).native());
 if (cancel_parked) {
  fixture.cancelled.store(true);
  pipeline.execution().cancel();
  CHECK_THROWS(mmltk::testsupport::await_test_future(importing, "cancel subscribed recovery dependencies", 5s));
 } else {
  physical.admitted(fixture.admitted[1], fixture.admitted[1].download);
  REQUIRE(physical.consume(fixture.admitted[1]));
  mmltk::testsupport::await_test_promise(sibling, "ready unlabeled recovery bypasses the parked train row", 5s);
  // Only the admitted/completed source boundaries can affect train ambiguity.
  // Individual unrelated image and recovery completions never revisit it.
  CHECK(train_observations.load() <= initial_observations + 2);
  CHECK(settled_peers.load() == peers);
  CHECK_FALSE(input.records->row(0).native());
  REQUIRE(input.records->row(1).native());
  CHECK(input.records->row(1).native()->inventory().physical.source == CoconutImageNamespace::CocoUnlabeled);
  CHECK(importing.wait_for(0ms) == std::future_status::timeout);
  physical.admitted(fixture.admitted[0], fixture.admitted[0].download);
  REQUIRE(physical.consume(fixture.admitted[0]));
  terminal.store(true);
  originals.settle();
  const auto components = mmltk::testsupport::await_test_future(importing, "both recovery generations settle", 5s);
  REQUIRE(components.size() == 2);
  CHECK(completed_rows.load() == peers + 1);
  CHECK(final_rows_before_join.load());
  CHECK(components[0].index().image(0).source_image_id == 1);
  CHECK(components[1].index().image(0).source_image_id == 2);
  CHECK(components[1].index().image_count() == peers);
 }
 CHECK(retired_rows.load() == peers + 1);
}
TEST_CASE("original readiness delivers a generation settled before subscription", "[benchmark][coconut][pipeline][recovery]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("coconut-original-observation-subscription");
 const std::array identities{coco(7)};
 auto input = request(identities);
 BenchmarkCompilePipeline pipeline(1, {}, {.transient_bytes = 1, .descriptors = 13});
 input.execution = &pipeline;
 input.parquet_shards = {root.path() / "row.parquet"};
 const std::array<std::uint32_t, 1> pixel{1};
 write_coconut_parquet(input.parquet_shards.front(), Json::array({hf_row(7, png(1, 1, pixel), Json::array({segment()}), 1, 1)}));
 CompileExecution::CompletionSource originals;
 std::atomic<bool> terminal{false};
 input.originals = [&](CoconutImageNamespace, bool, std::stop_token) { return CoconutOriginalReadiness{{}, terminal.load(), 1, originals.completion()}; };
 mmltk::testsupport::TestGate observed("row joins its original generation before callback registration");
 std::atomic<unsigned> subscriptions{0}, notifications{0}, notified_rows{0}, retired{0};
 input.trace = [&](std::string_view event, const Json& fields) {
  if (event == "benchmark.coconut.original_subscribed") {
   ++subscriptions;
   observed.receipt().ArriveAndWait();
  }
  if (event == "benchmark.coconut.original_notified") {
   ++notifications;
   notified_rows = fields.at("rows").get<unsigned>();
  }
  if (event == "benchmark.coconut.recovery_retired") ++retired;
 };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  terminal = true;
  originals.settle();
  observed.Release();
  pipeline.execution().cancel();
 });
 REQUIRE(observed.WaitEntered(5s));
 terminal = true;
 originals.settle();
 CHECK(notifications.load() == 0);
 observed.Release();
 const auto result = mmltk::testsupport::await_test_future(importing, "already settled original generation immediately delivers its live row");
 REQUIRE(result.size() == 1);
 CHECK(result.front().index().image_count() == 1);
 CHECK(subscriptions.load() == 1);
 CHECK(notifications.load() == 1);
 CHECK(notified_rows.load() == 1);
 CHECK(retired.load() == 1);
}
TEST_CASE("original generation delivery visits only live recovery rows after physical rebinding", "[benchmark][coconut][pipeline][recovery]") {
 const bool cancel_remaining = GENERATE(false, true);
 ScopedTempDir root("coconut-original-registration-custody");
 LocalCoconutRecipe local(root.path());
 constexpr unsigned count = 8, first_id = 7;
 std::vector<std::pair<std::string, std::string>> train_members, unlabeled_members;
 Json rows = Json::array();
 const std::array<std::uint32_t, 9> foreground{1, 1, 1, 1, 1, 1, 1, 1, 1};
 for (unsigned id = first_id; id < first_id + count; ++id) {
  train_members.emplace_back(coco(id).member, white_jpeg());
  if (id != first_id) unlabeled_members.emplace_back(coco(id, CoconutImageNamespace::CocoUnlabeled).member, white_jpeg());
  rows.push_back(hf_row(id, png(3, 3, foreground), Json::array({segment()})));
 }
 replace_physical_images(local.cache, local.catalog, CoconutImageNamespace::CocoTrain, train_members);
 replace_physical_images(local.cache, local.catalog, CoconutImageNamespace::CocoUnlabeled, unlabeled_members);
 PhysicalMembershipFixture fixture(local.cache, local.catalog, 1, false);
 auto& pipeline = fixture.execution;
 auto& physical = fixture.physical;
 auto& train = *std::ranges::find(fixture.admitted, CoconutImageNamespace::CocoTrain, [](const auto& archive) { return archive.origin.source; });
 auto& unlabeled = *std::ranges::find(fixture.admitted, CoconutImageNamespace::CocoUnlabeled, [](const auto& archive) { return archive.origin.source; });
 std::atomic<bool> terminal{false};
 physical.admitted(train, train.download);
 REQUIRE(physical.consume(train));
 CoconutImportRequest input;
 input.execution = &pipeline;
 input.physical_membership = &physical;
 input.input_identity = "original-registration-generation";
 input.records = std::make_shared<CoconutAnnotationRecords>();
 input.cancellation = fixture.cancellation;
 input.expected_rows = count;
 input.parquet_shards = {root.path() / "rows.parquet"};
 write_coconut_parquet(input.parquet_shards.front(), rows);
 CompileExecution::CompletionSource originals;
 input.originals = [&](CoconutImageNamespace source, bool, std::stop_token) {
  if (source != CoconutImageNamespace::CocoTrain) throw std::logic_error("unlabeled row subscribed to original train annotations");
  return CoconutOriginalReadiness{{}, terminal.load(), 1, originals.completion()};
 };
 std::promise<void> parked, peers_retired, live_revisited;
 std::atomic<unsigned> parked_mask{0}, retired{0}, subscriptions{0}, notifications{0};
 std::atomic<std::uint64_t> notification_rows{0}, completed{0}, revisit_generation{UINT64_MAX};
 std::array<std::atomic<unsigned>, count> observations{};
 input.progress = [&](std::uint64_t value) { completed = value; };
 input.trace = [&](std::string_view event, const Json& fields) {
  if (event == "benchmark.coconut.original_subscribed") ++subscriptions;
  if (event == "benchmark.coconut.original_notified") {
   ++notifications;
   notification_rows = fields.at("rows").get<std::uint64_t>();
  }
  const auto id = fields.value("image_id", std::uint64_t{0});
  if (id < first_id || id >= first_id + count) return;
  if (event == "benchmark.coconut.recovery_observed") ++observations[id - first_id];
  if (event == "benchmark.coconut.recovery_parked") {
   const unsigned bit = 1U << (id - first_id);
   if ((parked_mask.fetch_or(bit) | bit) == (1U << count) - 1) mmltk::testsupport::release_test_promise(parked);
   if (id == first_id && fields.at("physical_generation").get<std::uint64_t>() >= revisit_generation.load()) mmltk::testsupport::release_test_promise(live_revisited);
  }
  if (event == "benchmark.coconut.recovery_retired" && ++retired == count - 1) mmltk::testsupport::release_test_promise(peers_retired);
 };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  fixture.cancelled = true;
  terminal = true;
  originals.settle();
  pipeline.execution().cancel();
 });
 mmltk::testsupport::await_test_promise(parked, "terminal physical rows share one pending original generation");
 CHECK(subscriptions.load() == 1);
 // Seven physical identities move to the original-free namespace. Their
 // terminal row retirement must remove them from both readiness owners.
 physical.withdraw(train);
 tar(train.download.path, std::span(train_members).first(1));
 auto replacement = train.download;
 replacement.identity = "train-retains-first-image";
 physical.admitted(train, replacement, true);
 REQUIRE(physical.consume(train));
 physical.admitted(unlabeled, unlabeled.download);
 REQUIRE(physical.consume(unlabeled));
 mmltk::testsupport::await_test_promise(peers_retired, "rebound peers retire while first image still waits for originals");
 CHECK(subscriptions.load() == 1);
 REQUIRE(input.records->row(1).native());
 CHECK(input.records->row(1).native()->inventory().physical.source == CoconutImageNamespace::CocoUnlabeled);
 std::array<unsigned, count> before{};
 for (unsigned i = 1; i < count; ++i) before[i] = observations[i].load();
 CoconutRecord first;
 first.image_id = first_id;
 first.file_name = "000000000007.jpg";
 revisit_generation = physical.readiness(CoconutEdition::Base, first).generation + 1;
 physical.unavailable(unlabeled);
 mmltk::testsupport::await_test_promise(live_revisited, "source boundary revisits the remaining live original waiter");
 for (unsigned i = 1; i < count; ++i) CHECK(observations[i].load() == before[i]);
 if (cancel_remaining) {
  fixture.cancelled = true;
  pipeline.execution().cancel();
  CHECK_THROWS(mmltk::testsupport::await_test_future(importing, "cancellation retires the last original and physical registration"));
  terminal = true;
  originals.settle();
  CHECK(notifications.load() == 0);
 } else {
  terminal = true;
  originals.settle();
  const auto result = mmltk::testsupport::await_test_future(importing, "one live original row completes after its rebound peers");
  REQUIRE(result.size() == 2);
  CHECK(result[0].index().image_count() == 1);
  CHECK(result[1].index().image_count() == count - 1);
  CHECK(completed.load() == count);
  CHECK(notifications.load() == 1);
  CHECK(notification_rows.load() == 1);
 }
 CHECK(retired.load() == count);
 CHECK(subscriptions.load() == 1);
 const auto final_observations = observations[0].load();
 physical.unavailable(train);
 CHECK(observations[0].load() == final_observations);
}
namespace {
BenchmarkCompilePipeline parquet_projection_pipeline() {
 using namespace mmltk::backend::data::detail;
 const std::array<CompileWorkspaceProfile, 1> profiles{{{64U << 10, 0}}};
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(1, profiles, kParquetPoolBytes, 0));
 return BenchmarkCompilePipeline(1, {}, {.descriptors = 13, .workspace_plan = std::move(plan)}, {}, [](std::string_view, const Json&) {});
}
}  // namespace
TEST_CASE("Parquet contiguous fit is distinct from its logical payload cap", "[benchmark][coconut][parquet][resources]") {
 using namespace mmltk::backend::data::detail;
 constexpr std::int64_t unit = 1U << 20, small = 80 * unit, large = 100 * unit;
 const bool moving = GENERATE(false, true);
 CAPTURE(moving);
 auto pipeline = parquet_projection_pipeline();
 const auto locked = pipeline.execution().snapshot().normal_locked;
 std::size_t retry_bytes = 0;
 // A failed moving reallocation must leave the original pointer intact. A
 // failed allocation has the same logical headroom after freeing its prefix.
 for (unsigned attempt = 0; attempt < 2; ++attempt) {
  const auto bytes = attempt ? retry_bytes : kParquetPoolBytes;
  auto allowance =
   pipeline.execution().reserve({.bytes = bytes, .supplied_workspace = true, .workspace_class = CompileWorkClass::Extraction, .workspace_window = CompileWorkspaceWindow::SharedSource});
  CoconutParquetPool pool(allowance);
  std::uint8_t* first = nullptr;
  std::uint8_t* second = nullptr;
  std::uint8_t* next = nullptr;
  arrow_ok(pool.Allocate(small, 64, &first));
  arrow_ok(pool.Allocate(small, 64, &second));
  first[0] = 71;
  second[0] = 93;
  const auto* stable = second;
  const auto first_address = first;
  std::int64_t first_size = small;
  if (!moving) {
   pool.Free(first, small, 64);
   first = nullptr;
  }
  const mmltk::testsupport::ScopedTestCleanup release([&] {
   if (first) pool.Free(first, first_size, 64);
   if (second) pool.Free(second, small, 64);
   if (next) pool.Free(next, large, 64);
  });
  pool.begin_call();
  auto status = moving ? pool.Reallocate(small, large, 64, &first) : pool.Allocate(large, 64, &next);
  if (moving && status.ok()) first_size = large;
  CHECK(second == stable);
  CHECK(second[0] == 93);
  CHECK(pipeline.execution().snapshot().normal_locked == locked);
  if (!attempt) {
   REQUIRE(status.IsCapacityError());
   REQUIRE(pool.fragmented_extent() > kParquetPoolBytes);
   retry_bytes = pool.fragmented_extent();
   CHECK(pool.bytes_allocated() == (moving ? 2 * small : small));
   if (moving) {
    CHECK(first == first_address);
    CHECK(first[0] == 71);
   }
   pool.begin_call();
   std::uint8_t* rejected = nullptr;
   CHECK(pool.Allocate(kParquetPoolBytes, 64, &rejected).IsOutOfMemory());
   CHECK(pool.fragmented_extent() == 0);
   CHECK(rejected == nullptr);
   if (moving) {
    CHECK(pool.Reallocate(small, kParquetPoolBytes, 64, &first).IsOutOfMemory());
    CHECK(pool.fragmented_extent() == 0);
    CHECK(first == first_address);
   }
   if (first) {
    pool.Free(first, small, 64);
    first = nullptr;
   }
   pool.quiesce();
   allowance = {};
   CHECK(pipeline.execution().snapshot().leased >= kParquetPoolBytes);
   CHECK(second == stable);
   CHECK(second[0] == 93);
   pool.Free(second, small, 64);
   second = nullptr;
   CHECK(pool.retained_capacity() == 0);
   CHECK(pipeline.execution().snapshot().leased == 0);
  } else {
   REQUIRE(status.ok());
   CHECK(pool.bytes_allocated() == small + large);
   CHECK(pool.max_memory() <= static_cast<std::int64_t>(kParquetPoolBytes));
   CHECK(pool.retained_capacity() == retry_bytes);
   CHECK(pipeline.execution().snapshot().exceptional >= retry_bytes);
   if (moving) {
    CHECK(first[0] == 71);
    next = std::exchange(first, nullptr);
   }
   pool.Free(next, large, 64);
   next = nullptr;
   pool.Free(second, small, 64);
   second = nullptr;
   pool.close();
   allowance = {};
   CHECK(pipeline.execution().snapshot().exceptional == 0);
  }
 }
 CHECK(pipeline.execution().snapshot().exceptional_peak > locked);
 CHECK(pipeline.execution().snapshot().normal_locked == locked);
}
TEST_CASE("Parquet staged batches retain one forward reader and reusable source window", "[benchmark][coconut][parquet][resources]") {
 using namespace mmltk::backend::data::detail;
 const bool pressure = GENERATE(false, true);
 ScopedTempDir root("parquet-forward-window");
 const std::array paths{root.path() / "rows.parquet"};
 constexpr unsigned count = 96;
 Json rows = Json::array();
 for (unsigned id = 1; id <= count; ++id) rows.push_back(hf_row(id, std::string(16384, static_cast<char>(id)), Json::array(), 1, 1));
 write_coconut_parquet(paths.front(), rows, coconut_parquet_schema(), count);
 auto pipeline = parquet_projection_pipeline();
 CompileAllowance held;
 CompileExecution::Completion blocked;
 std::promise<void> waiting;
 std::atomic<bool> observed{false};
 CompileExecution::Readiness readiness(pipeline.execution(), CompileChange::Resources | CompileChange::Cpu | CompileChange::Records | CompileChange::Local | CompileChange::Pressure, [&](CompileExecution::Readiness&, auto) {
  if (pressure && pipeline.execution().resource_pressure() && !observed.exchange(true)) mmltk::testsupport::release_test_promise(waiting);
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] { pipeline.execution().cancel(); });
 if (pressure) {
  const auto bytes = pipeline.execution().workspace_capacity();
  held = pipeline.execution().reserve({.bytes = bytes, .supplied_workspace = true});
  blocked = pipeline.execution().submit({.kind = CompileTaskKind::Normalize, .resources = {.bytes = bytes, .supplied_workspace = true}, .work = [](std::size_t, const CompileAllowance&) {}});
  mmltk::testsupport::await_test_promise(waiting, "ordinary scratch contention leaves the protected source window useful");
  CHECK_FALSE(pipeline.execution().oversized_workspace_pending());
 }
 CoconutAnnotationRecords retained;
 retained.staging_directory = root.path() / "staging";
 unsigned accepted = 0, opened = 0;
 std::uint64_t replayed = 0, readiness_rows = 0;
 std::vector<CoconutAnnotationInput> durable;
 const BenchmarkTraceSink trace = [&](std::string_view event, const Json& value) {
  if (event == "benchmark.coconut.projection_opened") {
   ++opened;
   replayed += value.at("replayed_rows").get<std::uint64_t>();
  } else if (event == "benchmark.coconut.metadata_readiness")
   readiness_rows += value.at("inspected_rows").get<std::uint64_t>();
 };
 read_coconut_parquet(paths, {}, {}, [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput& input) {
  if (input.membership) return;
  require_condition(record.image_id == ++accepted, "forward projection duplicated or reordered a row");
  durable.push_back(input);
  durable.back().retire_projection = {};
  durable.back().admitted = {};
 }, false, &pipeline.execution(), &pipeline.files(), {}, {}, &retained, {}, [](const CoconutRecord&, std::span<const std::uint8_t>) { return true; }, trace);
 held = {};
 if (pressure) blocked.wait();
 REQUIRE(accepted == count);
 CHECK(opened == 2);  // One metadata projection and one mask projection.
 CHECK(replayed == 0);
 CHECK(readiness_rows <= count);
 for (unsigned i = 0; i < count; ++i) {
  REQUIRE(durable[i].png.size() == 16384);
  CHECK(durable[i].png.front() == i + 1);
  CHECK(durable[i].png.back() == i + 1);
 }
 CHECK(pipeline.execution().snapshot().leased == 0);
 CHECK(pipeline.execution().snapshot().exceptional == 0);
}
TEST_CASE("large projected Parquet batches keep the opened generation through staging and interruption", "[benchmark][coconut][parquet][resources]") {
 using namespace mmltk::backend::data::detail;
 const bool interrupt = GENERATE(false, true);
 ScopedTempDir root("parquet-opened-projection");
 const std::array paths{root.path() / "rows.parquet"};
 Json rows = Json::array();
 // Distinct compressible payloads exercise Arrow's growth and moving copies
 // without requiring a large encoded file or changing its ordinary allocator.
 for (unsigned id = 1; id <= 16; ++id) rows.push_back(hf_row(id, std::string((8 + (id % 3)) * 1024 * 1024, static_cast<char>(id)), Json::array(), 1, 1));
 parquet::WriterProperties::Builder writer;
 writer.compression(parquet::Compression::ZSTD);
 writer.disable_dictionary();
 writer.write_batch_size(1);
 write_coconut_parquet(paths.front(), rows, coconut_parquet_schema(), 16, parquet::default_arrow_writer_properties(), writer.build());
 rows = Json{};
 auto pipeline = parquet_projection_pipeline();
 const auto locked = pipeline.execution().snapshot().normal_locked;
 CoconutAnnotationRecords retained;
 retained.staging_directory = root.path() / "staging";
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 unsigned accepted = 0;
 std::vector<CoconutAnnotationInput> durable;
 const auto consume = [&](std::size_t, const CoconutRecord& record, const CoconutAnnotationInput& input) {
  if (input.membership) return;
  require_condition(record.image_id == ++accepted, "physical replay duplicated or reordered a row");
  require_condition(input.png.size() == (8 + (record.image_id % 3)) * 1024 * 1024, "physical replay changed the opened payload extent");
  require_condition(input.png.front() == record.image_id && input.png.back() == record.image_id, "physical replay changed the opened payload");
  require_condition(pipeline.execution().snapshot().exceptional == 0, "exceptional source backing escaped into semantic admission");
  durable.push_back(input);
  durable.back().admitted = {};
  if (accepted == 1) {
   std::filesystem::rename(paths.front(), root.path() / "opened.parquet");
   mmltk::testsupport::write_text_file(paths.front(), "replacement must never be opened");
  }
  if (interrupt && accepted == 8) cancelled.store(true);
 };
 unsigned replays = 0;
 bool replay_retired = true;
 const BenchmarkTraceSink trace = [&](std::string_view event, const Json& value) {
  if (event != "benchmark.coconut.projection_replay") return;
  ++replays;
  replay_retired = replay_retired && value.at("exceptional").get<std::uint64_t>() == 0 && value.at("normal_locked").get<std::uint64_t>() == locked;
 };
 // Every completed batch is durably transferred, so even a one-source-window
 // session can resume its physical prefix while earlier inputs stay readable.
 const auto run = [&] {
  read_coconut_parquet(
   paths, {}, cancellation, consume, false, &pipeline.execution(), &pipeline.files(), {}, {}, &retained, {}, [](const CoconutRecord&, std::span<const std::uint8_t>) { return true; }, trace);
 };
 if (interrupt) {
  CHECK_THROWS(run());
  CHECK(accepted == 8);
 } else {
  REQUIRE_NOTHROW(run());
  CHECK(accepted == 16);
 }
 for (unsigned i = 0; i < durable.size(); ++i) {
  CHECK(durable[i].png.front() == i + 1);
  CHECK(durable[i].png.back() == i + 1);
  CHECK(retained.row(i).complete());
 }
 CHECK(pipeline.execution().snapshot().normal_locked == locked);
 CHECK(pipeline.execution().snapshot().exceptional == 0);
 CHECK(replay_retired);
 if (replays) CHECK(pipeline.execution().snapshot().exceptional_peak > locked);
}
TEST_CASE("native staging and arena retirement precede controller settlement", "[benchmark][coconut][annotations][recovery][resources]") {
 using namespace mmltk::backend::data::detail;
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires an Extraction floor and two Annotation lanes");
 const bool recovery = GENERATE(false, true);
 CAPTURE(recovery);
 ScopedTempDir root("coconut-task-staging");
 const std::array<CompileWorkspaceProfile, 1> profiles{{{1U << 20, 0}}};
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(3, profiles, kParquetPoolBytes, 0, kParquetPoolBytes + (2U << 20)));
 REQUIRE(plan->contexts == 2);
 BenchmarkCompilePipeline pipeline(3, {}, {.descriptors = 13, .workspace_plan = plan});
 const std::array physical{coco(1), coco(2)};
 auto input = request(physical);
 input.execution = &pipeline;
 input.inventory_directory = root.path() / "staging";
 input.records = std::make_shared<CoconutAnnotationRecords>();
 input.parquet_shards = {root.path() / "rows.parquet"};
 const std::array<std::uint32_t, 1> foreground{1};
 const auto encoded = png(1, 1, foreground);
 const auto segments = Json::array({segment(), segment(2)});
 write_coconut_parquet(input.parquet_shards.front(), Json::array({hf_row(1, encoded, segments, 1, 1), hf_row(2, encoded, segments, 1, 1)}), coconut_parquet_schema(), 2);
 CompileExecution::CompletionSource originals;
 std::atomic<bool> terminal{false};
 if (recovery) input.originals = [&](CoconutImageNamespace, bool, std::stop_token) { return CoconutOriginalReadiness{{}, terminal.load(), 1, originals.completion()}; };
 std::promise<void> projected, staged;
 std::atomic<unsigned> normalized{0};
 input.input_finished = [&] { mmltk::testsupport::release_test_promise(projected); };
 mmltk::testsupport::TestGate settlement("first immutable image waits in controller publication"), first_work("first work waits for its independent sibling"),
  second("second image has not staged its native ranges");
 input.progress = [&](std::uint64_t count) {
  if (count == 1) settlement.receipt().ArriveAndWait();
 };
 input.rejected_object = [&](const auto&, const CoconutRecord& record, const auto&, auto, auto) { (record.image_id == 1 ? first_work : second).receipt().ArriveAndWait(); };
 input.trace = [&](std::string_view event, const Json& value) {
  const auto kind = recovery ? CompileTaskKind::Recovery : CompileTaskKind::Normalize;
  if (event == "benchmark.coconut.native_staged" && value.at("kind") == CompileTaskKind::Normalize) ++normalized;
  if (event == "benchmark.coconut.native_staged" && value.at("image_id") == 2 && value.at("kind") == kind) {
   require_condition(pipeline.execution().on_lane(), "native output copied outside an admitted task");
   require_condition(value.at("boxes") == 1 && value.at("runs") == 1, "staging omitted live native spans");
   mmltk::testsupport::release_test_promise(staged);
  }
 };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  first_work.Release();
  second.Release();
  settlement.Release();
  terminal.store(true);
  originals.settle();
  pipeline.execution().cancel();
 });
 if (recovery) {
  mmltk::testsupport::await_test_promise(projected, "provisional support is immutable before original readiness", 5s);
  CHECK(normalized.load() == 2);
  CHECK_FALSE(input.records->row(0).native());
  CHECK_FALSE(input.records->row(1).native());
  terminal.store(true);
  originals.settle();
 }
 REQUIRE(first_work.WaitEntered(5s));
 REQUIRE(second.WaitEntered(5s));
 const auto both_leased = pipeline.execution().snapshot().leased;
 first_work.Release();
 REQUIRE(settlement.WaitEntered(5s));
 CHECK(pipeline.execution().snapshot().leased < both_leased);
 const auto first = input.records->row(0).native();
 REQUIRE(first);
 REQUIRE(first->boxes().size() == 1);
 REQUIRE(first->runs().size() == 1);
 std::array<std::byte, sizeof(NormalizedBox)> first_box;
 std::array<std::byte, sizeof(RLEPair)> first_run;
 std::memcpy(first_box.data(), first->boxes().data(), first_box.size());
 std::memcpy(first_run.data(), first->runs().data(), first_run.size());
 // The first slice has retired before controller settlement, while the second
 // task retains its own slice. One planned context's capacity can be reused
 // from the shared node pool without mutating the first immutable output.
 auto reusing = std::async(std::launch::async, [&] {
  pipeline.execution().run(CompileTaskKind::Normalize, {.bytes = plan->context_bytes, .supplied_workspace = true}, [&](std::size_t) {
   auto bytes = pipeline.execution().current_allowance().workspace();
   std::memset(bytes.data(), 0xa5, bytes.size());
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release_reuse([&] { pipeline.execution().cancel(); });
 mmltk::testsupport::await_test_future(reusing, "completed native arena is reusable while its controller is held", 5s);
 CHECK(std::memcmp(first_box.data(), first->boxes().data(), first_box.size()) == 0);
 CHECK(std::memcmp(first_run.data(), first->runs().data(), first_run.size()) == 0);
 second.Release();
 mmltk::testsupport::await_test_promise(staged, "independent admitted work stages while the first settlement is held", 5s);
 CHECK(importing.wait_for(0ms) == std::future_status::timeout);
 CHECK(std::distance(std::filesystem::directory_iterator(input.inventory_directory), std::filesystem::directory_iterator{}) == 1);
 settlement.Release();
 const auto components = mmltk::testsupport::await_test_future(importing, "staged native products gather after publication resumes", 5s);
 REQUIRE(components.size() == 1);
 CHECK(components.front().index().image_count() == 2);
 CHECK(components.front().index().box_count() == 2);
 CHECK(pipeline.execution().snapshot().leased == 0);
 input.records->staging->retire();
 CHECK(first->runs().front().length == 1);
 CHECK(std::memcmp(first_box.data(), first->boxes().data(), first_box.size()) == 0);
}
TEST_CASE("native pending output keeps its arena while a full file queue leaves CPU work runnable", "[benchmark][coconut][annotations][resources]") {
 using namespace mmltk::backend::data::detail;
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires independent Extraction Annotation and Output grants");
 const auto stop = GENERATE(0, 1, 2);
 CAPTURE(stop);
 ScopedTempDir root("coconut-pending-preparation");
 const std::array<CompileWorkspaceProfile, 1> profiles{{{1U << 20, 0}}};
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(3, profiles, kParquetPoolBytes, 0, kParquetPoolBytes + (1U << 20)));
 REQUIRE(plan->contexts == 1);
 BenchmarkCompilePipeline pipeline(3, {}, {.descriptors = 13, .workspace_plan = plan});
 const std::array physical{coco(7)};
 auto input = request(physical);
 input.prepare_partial_staging(pipeline, root.path() / "staging");
 mmltk::testsupport::TestGate physical_call("held physical file service");
 std::promise<void> full;
 std::atomic<unsigned> accepted{0}, settled{0};
 input.rejected_object = [&](const auto&, const auto&, const auto&, auto, auto) {
  for (;;) {
   auto operation = pipeline.files().try_submit({.completed = [&](std::exception_ptr) { ++settled; }, .work = [receipt = physical_call.receipt()] { receipt.ArriveAndWait(); }});
   if (!operation) break;
   ++accepted;
  }
  mmltk::testsupport::release_test_promise(full);
 };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  physical_call.Release();
  pipeline.execution().cancel();
 });
 mmltk::testsupport::await_test_promise(full, "normalization fills the bounded physical queue", 5s);
 REQUIRE(physical_call.WaitEntered(5s));
 CHECK(accepted.load() == 6);
 auto independent = std::async(std::launch::async, [&] { pipeline.execution().run(CompileTaskKind::Normalize, {}, [](std::size_t) {}); });
 const mmltk::testsupport::ScopedTestCleanup release_independent([&] {
  physical_call.Release();
  pipeline.execution().cancel();
 });
 mmltk::testsupport::await_test_future(independent, "CPU work enters while native output waits for file capacity", 5s);
 CHECK_FALSE(pipeline.execution().try_reserve({.bytes = plan->context_bytes, .supplied_workspace = true}));
 CHECK(importing.wait_for(0ms) == std::future_status::timeout);
 if (stop == 1) pipeline.files().cancel();
 if (stop == 2) pipeline.execution().cancel();
 physical_call.Release();
 if (stop)
  CHECK_THROWS(mmltk::testsupport::await_test_future(importing, "cancelled or rejected preparation settles its native owner", 5s));
 else {
  const auto components = mmltk::testsupport::await_test_future(importing, "pending preparation resumes and publishes immutable output", 5s);
  REQUIRE(components.size() == 1);
  REQUIRE(components.front().index().box_count() == 1);
  CHECK(components.front().index().storage().mask_rle_pairs.front().length == 1);
 }
 pipeline.files().join();
 CHECK(settled.load() == accepted.load());
 input.records->discard();  // Failure retains the independently valid source generation until its owner retires it.
 CHECK(pipeline.execution().snapshot().leased == 0);
 CHECK(pipeline.execution().resource_usage().descriptors == 0);
}
TEST_CASE("accepted staging preparation retains charged custody through physical cancellation drain", "[benchmark][coconut][resources]") {
 using namespace mmltk::backend::data::detail;
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires independent file and semantic grants");
 ScopedTempDir root("coconut-accepted-preparation");
 CompileExecution execution(2, {}, {.transient_bytes = 65536, .descriptors = 4});
 FileWorkQueue files(execution, 1);
 CoconutStaging staging(root.path());
 struct Preparation {
  CompileAllowance descriptor;
  CoconutStaging::Request range{32, 16};
 };
 auto context = std::make_shared<Preparation>();
 context->descriptor = execution.reserve(CompileResources::handles(1));
 std::weak_ptr<Preparation> lifetime = context;
 std::shared_ptr<Preparation> prepared;
 std::atomic<unsigned> settlements{0};
 mmltk::testsupport::TestGate physical("accepted preparation still owns its descriptor and target");
 auto completion = files.try_submit({.completed = [&, context](std::exception_ptr error) {
  require_condition(!error, "running preparation was settled before physical return");
  prepared = context;
  ++settlements;
 }, .work = [&, context] {
  physical.receipt().ArriveAndWait();
  staging.prepare(context->range, context->descriptor);
 }});
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  physical.Release();
  files.cancel();
  files.join();
 });
 REQUIRE(completion);
 REQUIRE(physical.WaitEntered(5s));
 context.reset();
 CHECK_FALSE(lifetime.expired());
 CHECK(execution.resource_usage().descriptors == 1);
 CHECK_FALSE(files.try_submit({.work = [] {}}));
 auto sibling = std::async(std::launch::async, [&] { execution.run(CompileTaskKind::Normalize, {}, [](std::size_t) {}); });
 const mmltk::testsupport::ScopedTestCleanup release_sibling([&] {
  physical.Release();
  execution.cancel();
 });
 mmltk::testsupport::await_test_future(sibling, "independent CPU task passes held physical staging", 5s);
 files.cancel();
 CHECK_FALSE(completion->ready());
 CHECK_FALSE(lifetime.expired());
 CHECK_THROWS(files.try_submit({.work = [] {}}));
 physical.Release();
 completion->wait();
 files.join();
 REQUIRE(prepared);
 CHECK(prepared->range.ready());
 CHECK(settlements.load() == 1);
 prepared->descriptor = {};
 CHECK(execution.resource_usage().descriptors == 0);
 execution.run(CompileTaskKind::Normalize, {}, [&](std::size_t) { std::memset(prepared->range.target().data(), 0x46, 32); });
 auto immutable = prepared->range.bytes();
 prepared.reset();
 staging.retire();
 CHECK(lifetime.expired());
 CHECK(immutable.values.front() == std::byte{0x46});
 CHECK_FALSE(std::filesystem::is_empty(root.path()));
 immutable = {};
 CHECK(std::filesystem::is_empty(root.path()));
}
TEST_CASE("component planning and physical output retain their admitted custody", "[benchmark][coconut][annotations][resources]") {
 using namespace mmltk::backend::data::detail;
 using namespace std::chrono_literals;
 const auto workers = GENERATE(std::size_t{1}, std::size_t{2});
 const bool metadata_only = GENERATE(false, true);
 const auto failure = GENERATE(0, 1, 2);
 CAPTURE(workers, metadata_only, failure);
 if (mmltk::common::system::allowed_cpu_set().size() < workers) SKIP("requires the selected assigned CPUs");
 ScopedTempDir root("coconut-component-admission");
 const std::array physical{coco(7), coco(11)};
 auto input = request(physical);
 CoconutComponentBuilder source;
 source.edition = input.edition;
 source.source = CoconutImageNamespace::CocoTrain;
 source.input_identity = "previous";
 source.annotation_input_identity = input.input_identity;
 source.index.source = coconut_index_source(source.source);
 source.index.split = coconut_component_split(source.edition, source.source);
 for (std::size_t i = 0; i < physical.size(); ++i) {
  source.index.images.push_back({.source_image_id = physical[i].image_id, .width = 1, .height = 1});
  source.inventory.push_back({physical[i], physical[i].image_id, i});
 }
 auto previous_builder = source;
 const auto previous = std::move(previous_builder).finish({}, root.path());
 const auto published = root.path() / "published.normalized.bin";
 store_coconut_component(published, previous);
 const auto previous_index = file_bytes(published);
 const auto previous_inventory = file_bytes(published.string() + ".inventory");
 const auto previous_completion = file_bytes(published.string() + ".complete.json");
 BenchmarkCompilePipeline pipeline(workers);
 auto allowance = pipeline.execution().reserve(coconut_component_assembly_resources(physical.size()));
 mmltk::testsupport::TestGate metadata("component inventory planning holds its Metadata grant");
 mmltk::testsupport::TestGate preparation("normalized destination preparation holds its physical service");
 mmltk::testsupport::TestGate sealing("inventory sealing holds its physical service");
 struct Observation {
  CompileExecution& execution;
  const CompileAllowance& allowance;
  mmltk::testsupport::TestGate& metadata;
  mmltk::testsupport::TestGate& preparation;
  mmltk::testsupport::TestGate& sealing;
  bool metadata_only;
  std::thread::id controller;
  mutable std::atomic<bool> planned{false}, gathered{false}, prepared{false}, sealed{false}, charged{true};
  std::atomic<bool> stop{false};
  bool cancelled() const noexcept {
   if (execution.on_lane()) {
    const auto current = execution.current_allowance();
    if (!current.aliases(allowance)) charged = false;
    const auto state = execution.snapshot();
    if (state.running[static_cast<std::size_t>(CompileWorkClass::Annotations)] && !planned.exchange(true)) metadata.receipt().ArriveAndWait();
    if (state.running[static_cast<std::size_t>(CompileWorkClass::Gather)]) gathered = true;
   } else if (std::this_thread::get_id() != controller && execution.snapshot().services) {
    if (!metadata_only && !prepared.exchange(true))
     preparation.receipt().ArriveAndWait();
    else if (!sealed.exchange(true))
     sealing.receipt().ArriveAndWait();
   }
   return stop.load();
  }
 } observation{pipeline.execution(), allowance, metadata, preparation, sealing, metadata_only, {}};
 std::vector<CoconutComponentImage> rows;
 for (std::size_t i : {std::size_t{1}, std::size_t{0}}) rows.push_back({&source, &source.index, &source.index.images[i], {}, {}, &source.inventory[i], nullptr, 0});
 const std::array<std::size_t, 2> ordinals{20, 10};
 auto assembling = std::async(std::launch::async, [&] {
  observation.controller = std::this_thread::get_id();
  return assemble_coconut_component(source, rows, {}, metadata_only, 2, 0, ordinals, {}, *input.physical_membership, nullptr, root.path(),
   mmltk::common::concurrency::CancellationObservation::Borrow(observation), &pipeline.storage(), &pipeline, allowance, false, {});
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  metadata.Release();
  preparation.Release();
  sealing.Release();
  pipeline.execution().cancel();
 });
 REQUIRE(metadata.WaitEntered(5s));
 CHECK(pipeline.execution().snapshot().running[static_cast<std::size_t>(CompileWorkClass::Annotations)] == 1);
 CHECK(pipeline.execution().resource_usage().descriptors == 1);
 observation.stop = failure == 1;
 metadata.Release();
 if (failure != 1 && !metadata_only) {
  REQUIRE(preparation.WaitEntered(5s));
  CHECK(observation.gathered.load());
  CHECK(pipeline.execution().snapshot().services == 1);
  CHECK(pipeline.execution().resource_usage().descriptors == 1);
  if (workers > 1) {
   auto independent = std::async(std::launch::async, [&] { pipeline.execution().run(CompileTaskKind::Metadata, {}, [](std::size_t) {}); });
   const mmltk::testsupport::ScopedTestCleanup unblock([&] { preparation.Release(); });
   mmltk::testsupport::await_test_future(independent, "independent CPU work completes while component preparation is held", 5s);
   CHECK(assembling.wait_for(0ms) == std::future_status::timeout);
  }
  preparation.Release();
 }
 if (failure != 1) {
  REQUIRE(sealing.WaitEntered(5s));
  CHECK(pipeline.execution().snapshot().services == 1);
  CHECK(pipeline.execution().resource_usage().descriptors == 1);
  observation.stop = failure == 2;
  sealing.Release();
 }
 CHECK(observation.charged.load());
 if (failure)
  CHECK_THROWS(mmltk::testsupport::await_test_future(assembling, "failed planning or inventory sealing cannot complete a component", 5s));
 else {
  const auto component = mmltk::testsupport::await_test_future(assembling, "one controller joins admitted planning, copies and physical output", 5s);
  REQUIRE(component);
  CHECK(component->index().image_count() == 2);
  CHECK(component->inventory_image(0).physical.image_id == 7);
  CHECK(component->inventory_image(1).physical.image_id == 11);
  CHECK(component->inventory_image(0).source_ordinal == (metadata_only ? 0 : 20));
  CHECK(component->inventory_image(1).source_ordinal == (metadata_only ? 1 : 10));
  CHECK(component->source_extent_complete());
  if (!metadata_only) REQUIRE_NOTHROW(admit_coconut_component(*component));
 }
 pipeline.files().join();
 allowance = {};
 CHECK(pipeline.execution().resource_usage().descriptors == 0);
 CHECK(pipeline.execution().snapshot().leased == 0);
 CHECK(file_bytes(published) == previous_index);
 CHECK(file_bytes(published.string() + ".inventory") == previous_inventory);
 CHECK(file_bytes(published.string() + ".complete.json") == previous_completion);
}
TEST_CASE("stock recovery compiles a completed physical archive while a peer download is held", "[benchmark][coconut][pipeline][preload]") {
 using namespace std::chrono_literals;
 ScopedTempDir root("coconut-ready-archive-held-download");
 LocalCoconutRecipe local(root.path());
 auto catalog = local.selected(CoconutValidation::Stock);
 const std::array<std::pair<std::string, std::string>, 1> members{{{coco(7).member, white_jpeg(3, 3, 1U << 20)}}};
 replace_physical_images(local.cache, catalog, CoconutImageNamespace::CocoTrain, members);
 ServedPhysicalArchive held(local.cache, catalog, CoconutImageNamespace::CocoTrain, "held-train");
 REQUIRE(std::filesystem::remove(held.path));
 held.server.GateNextTransfer();
 // Release the completed peer only after the download is physically held, so
 // its later pixel receipt proves useful overlap rather than completion order.
 std::optional<ArtifactLease> ready_archive(ArtifactLease::acquire(local.cache.locks / "objects365-patch-32.images.lock", {}));
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::Stock, true});
 config.resolution = 2;
 config.perceptual_downscale = true;
 config.resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
 std::atomic<bool> cancelled{false}, delivered{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 std::promise<void> pixels;
 config.trace = [&](std::string_view event, std::string_view fields) {
  if (event != "benchmark.image.stage_completed") return;
  const auto value = Json::parse(fields);
  if (value.at("stage") == "pixels" && value.at("image_id") == 1 && !value.at("failed").get<bool>() && !delivered.exchange(true)) pixels.set_value();
 };
 auto compiling = std::async(std::launch::async, [&] { compile_benchmark_recipe(config, &catalog); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  ready_archive.reset();
  held.server.ReleasePartial();
 });
 REQUIRE(held.server.WaitPartial());
 CHECK_FALSE(delivered.load());
 ready_archive.reset();
 mmltk::testsupport::await_test_promise(pixels, "completed archive pixels while a different archive downloads on one CPU", 5s);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 CHECK_FALSE(std::filesystem::exists(config.output_dir / "train.bin"));
 held.server.ReleasePartial();
 mmltk::testsupport::await_test_future(compiling, "stock recovery joins the held download and publishes", 5s);
 CHECK(CompiledDataset::open(config.output_dir / "train.bin").header().num_images == 4);
 CHECK(CompiledDataset::open(config.output_dir / "val.bin").header().num_images == 1);
 held.server.Check();
}
TEST_CASE("native staging retains its record while an image uses another slice of shared backing", "[benchmark][coconut][annotations][resources]") {
 using namespace mmltk::backend::data::detail;
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 ScopedTempDir root("native-copy-record-custody");
 const std::array<CompileWorkspaceProfile, 1> profiles{{{4U << 20, 0}}};
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(2, profiles, kParquetPoolBytes, 0, kParquetPoolBytes + (4U << 20)));
 REQUIRE(plan->contexts == 1);
 mmltk::testsupport::TestGate prepared("native staging has physically prepared its immutable range");
 mmltk::testsupport::TestGate pixels("independent image keeps its admitted slice while native copy resumes");
 std::atomic<bool> hold_next{false}, bounded{true};
 std::promise<void> copied;
 const BenchmarkTraceSink trace = [&](std::string_view event, const Json& fields) {
  if (event == "file.physical_completed" && fields.at("service") == "output" && hold_next.exchange(false)) prepared.receipt().ArriveAndWait();
  if (event == "benchmark.image.stage_started" && fields.at("stage") == "pixels") pixels.receipt().ArriveAndWait();
  if (event == "compile.execution") {
   if (fields.at("records").get<std::size_t>() > 2 || fields.at("leased").get<std::uint64_t>() > plan->budget + fields.at("exceptional_reserved").get<std::uint64_t>()) bounded = false;
  }
 };
 BenchmarkCompilePipeline pipeline(2, {}, {.descriptors = 16, .task_records = 2, .workspace_plan = plan}, {}, trace);
 const std::array identities{coco(7)};
 auto input = request(identities);
 input.prepare_partial_staging(pipeline, root.path() / "native");
 input.rejected_object = [&](const auto&, const auto&, const auto&, auto, auto) { hold_next = true; };
 input.trace = [&](std::string_view event, const Json&) {
  if (event == "benchmark.coconut.native_staged") mmltk::testsupport::release_test_promise(copied);
 };
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 const auto jpeg = white_jpeg(1, 1);
 BenchmarkEncodedImage::publish(cached_image_path(images, 99), {reinterpret_cast<const std::uint8_t*>(jpeg.data()), jpeg.size()}, {});
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 BenchmarkSplitWriter writer({.split = split, .output_path = root.path() / "pixels.bin", .resolution = 1, .num_workers = 2, .execution = &pipeline}, true);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 pipeline.bind_source(images, writer, AnnotationSource::CoconutCoco);
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  prepared.Release();
  pixels.Release();
  pipeline.execution().cancel();
 });
 REQUIRE(prepared.WaitEntered(5s));
 CHECK(pipeline.execution().snapshot().records == 1);
 const auto publication = pipeline.source_publication(images, {});
 publication({99});
 REQUIRE(pixels.WaitEntered(5s));
 CHECK(pipeline.execution().snapshot().records == 2);
 CHECK(pipeline.execution().snapshot().arena.live_allocations >= 2);
 CHECK_FALSE(pipeline.image_completion(images, 99).ready());
 prepared.Release();
 mmltk::testsupport::await_test_promise(copied, "native copy reuses its retained record under a full record bank", 5s);
 pixels.Release();
 const auto components = mmltk::testsupport::await_test_future(importing, "native storage retires without cancelling the competing image", 5s);
 pipeline.wait_image(images, 99);
 REQUIRE(components.size() == 1);
 CHECK(components.front().index().box_count() == 1);
 CHECK(writer.completed() == 1);
 pipeline.drain();
 CHECK(pipeline.execution().snapshot().records == 0);
 CHECK(pipeline.execution().snapshot().leased == 0);
 CHECK(bounded);
}
TEST_CASE("Coconut cost spans preserve disabled and unwinding diagnostic behavior", "[benchmark][coconut][trace]") {
 const bool unwinding = GENERATE(false, true);
 std::size_t constructions = 0;
 const auto fields = [&] {
  ++constructions;
  return Json{{"rows_revisited", 18446744073709551615ULL}};
 };
 {
  const BenchmarkTraceSink disabled;
  BenchmarkCostSpan quiet(disabled, BenchmarkCostKind::RecoveryDrain, fields);
  quiet.finish();
 }
 CHECK(constructions == 0);
 std::vector<std::pair<std::string, Json>> records;
 const BenchmarkTraceSink enabled = [&](std::string_view event, const Json& value) {
  records.emplace_back(event, value);
  throw std::runtime_error("diagnostics cannot replace the source failure");
 };
 const auto run = [&] {
  BenchmarkCostSpan span(enabled, BenchmarkCostKind::RecoveryDrain, fields);
  if (unwinding) throw std::runtime_error("source failed");
  span.finish();
  span.finish();
 };
 if (unwinding)
  CHECK_THROWS_WITH(run(), "source failed");
 else
  CHECK_NOTHROW(run());
 REQUIRE(records.size() == 2);
 CHECK(records[0].first == "benchmark.cost.started");
 CHECK(records[1].first == "benchmark.cost.completed");
 CHECK(records[0].second.at("span_id") == records[1].second.at("span_id"));
 CHECK(records[1].second.at("failed") == unwinding);
 CHECK(records[1].second.at("rows_revisited").get<std::uint64_t>() == UINT64_MAX);
}
TEST_CASE("admitted physical headers recover labels before the same image finishes pixels", "[benchmark][coconut][pipeline][recovery]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires independent extraction, pixels and recovery lanes");
 const bool unchanged = GENERATE(false, true);
 bool replace_original = false;
 SECTION("unchanged generation retains its recovered admission until pixels finish") {}
 SECTION("original replacement before physical settlement rejects the stale recovery") { replace_original = true; }
 ScopedTempDir root("coconut-header-ready-recovery");
 const auto cache = BenchmarkCacheLayout::create(root.path() / "cache");
 const auto archive = cache.source_downloads("coco") / "train.tar";
 tar(archive, std::array{std::pair{coco(7).member, white_jpeg(3, 3)}});
 const auto catalog = base_physical_catalog(archive, 1);
 auto admitted = local_physical_archives(cache, catalog);
 mmltk::testsupport::TestGate pixels("same image retains its conversion lane");
 BenchmarkCompilePipeline pipeline(3, {}, {.transient_bytes = 1ULL << 30, .descriptors = 26}, {}, [&](std::string_view event, const Json& fields) {
  if (event == "benchmark.image.stage_started" && fields.at("stage") == "pixels" && fields.at("image_id") == 7) pixels.receipt().ArriveAndWait();
 });
 CoconutPhysicalMembership physical(admitted, catalog, true, cache, pipeline.storage(), pipeline);
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 BenchmarkSplitWriter writer({.split = split, .output_path = root.path() / "pixels.bin", .resolution = 2, .num_workers = 3, .perceptual_downscale = true, .execution = &pipeline}, true);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 const auto image_root = physical.identity_root(CoconutImageNamespace::CocoTrain);
 pipeline.bind_source(image_root, writer, AnnotationSource::CoconutCoco);
 const std::array<RLEPair, 1> original_mask{{{0, 2}}};
 auto original = fixture_index(recovery_originals(7, original_mask));
 auto original_owner = std::make_shared<CoconutRecoveryOriginals>(&original, nullptr);
 std::atomic<bool> originals_ready{false};
 std::atomic<std::uint64_t> original_generation{1};
 CompileExecution::CompletionSource originals_changed;
 CoconutImportRequest input;
 input.execution = &pipeline;
 input.physical_membership = &physical;
 input.input_identity = "header-before-pixels";
 input.records = std::make_shared<CoconutAnnotationRecords>();
 input.inventory_directory = root.path() / "staging";
 input.parquet_shards = {root.path() / "labels.parquet"};
 std::array<std::uint32_t, 9> canvas{30, 30, 30, 30, 30, 30, 30, 30, 30};
 if (unchanged) canvas[0] = canvas[1] = 10;
 write_coconut_parquet(input.parquet_shards.front(), Json::array({hf_row(7, png(3, 3, canvas), Json::array({segment(10, 18), segment(30, 63)}))}));
 input.originals = [&](CoconutImageNamespace, bool, std::stop_token) {
  const bool ready = originals_ready.load();
  return CoconutOriginalReadiness{ready ? original_owner : nullptr, ready, ready ? original_generation.load() : 0U, originals_changed.completion()};
 };
 std::promise<void> projected, recovered;
 std::atomic<unsigned> unchanged_finalizations{0}, recovery_copies{0};
 std::atomic<std::uint64_t> admitted_runs{0}, gathered_validation{UINT64_MAX}, row_completions{0}, retired_rows{0};
 input.progress = [&](std::uint64_t count) { row_completions = count; };
 input.input_finished = [&] { mmltk::testsupport::release_test_promise(projected); };
 input.trace = [&](std::string_view event, const Json& fields) {
  if (event == "benchmark.coconut.native_finalized" && fields.at("unchanged").get<bool>()) ++unchanged_finalizations;
  if (event == "benchmark.coconut.native_staged") {
   admitted_runs += fields.at("admitted_runs").get<std::uint64_t>();
   if (fields.at("kind") == CompileTaskKind::Recovery) ++recovery_copies;
  }
  if (event == "benchmark.coconut.recovery_settled") mmltk::testsupport::release_test_promise(recovered);
  if (event == "benchmark.coconut.recovery_retired") ++retired_rows;
  if (event == "benchmark.annotations.gathered") gathered_validation = fields.at("validated_runs").get<std::uint64_t>();
 };
 auto importing = std::async(std::launch::async, [&] { return import_coconut_annotations(input); });
 std::future<bool> consuming;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  pixels.Release();
  originals_ready = true;
  originals_changed.settle();
  pipeline.execution().cancel();
 });
 mmltk::testsupport::await_test_promise(projected, "native support releases its source before originals and image headers", 5s);
 physical.admitted(admitted.front(), admitted.front().download);
 consuming = std::async(std::launch::async, [&] { return physical.consume(admitted.front()); });
 REQUIRE(pixels.WaitEntered(5s));
 CHECK_FALSE(pipeline.image_completion(image_root, 7).ready());
 originals_ready = true;
 originals_changed.settle();
 mmltk::testsupport::await_test_promise(recovered, "same-image recovery consumes admitted header and original generation while pixels are held", 5s);
 const auto& native = input.records->row(0).native();
 REQUIRE(native);
 REQUIRE(native->admission());
 REQUIRE(native->recovery().objects.size() == (unchanged ? 0 : 1));
 if (!unchanged) CHECK(native->recovery().objects.front().original_annotation_id == 200);
 CHECK(unchanged_finalizations == (unchanged ? 1 : 0));
 CHECK(recovery_copies == (unchanged ? 0 : 1));
 CHECK(admitted_runs.load() > 0);
 CHECK_FALSE(pipeline.image_completion(image_root, 7).ready());
 CHECK(importing.wait_for(0ms) == std::future_status::timeout);
 CHECK(row_completions.load() == 1);
 if (replace_original) original_generation = 2;
 pixels.Release();
 CHECK(mmltk::testsupport::await_test_future(consuming, "physical eligibility joins recovered labels", 5s));
 if (replace_original) {
  CHECK_THROWS_AS(mmltk::testsupport::await_test_future(importing, "late original replacement cannot seal stale recovered payload", 5s), CoconutOriginalChanged);
  CHECK(gathered_validation.load() == UINT64_MAX);
  CHECK(row_completions.load() == 1);
  CHECK(retired_rows.load() == 1);
  pipeline.drain();
  CHECK(pipeline.execution().snapshot().leased == 0);
  return;
 }
 const auto components = mmltk::testsupport::await_test_future(importing, "final membership seals the admitted native row", 5s);
 REQUIRE(components.size() == 1);
 CHECK(components.front().recovery_image(0).objects.size() == (unchanged ? 0 : 1));
 CHECK(components.front().original_generation() == 1);
 CHECK(retired_rows.load() == 1);
 CHECK(gathered_validation.load() == 0);
 CHECK(writer.completed() == 1);
}
TEST_CASE("bounded gzip read turns preserve members and yield one CPU to a peer", "[benchmark][coconut][archive][resources]") {
 using namespace std::chrono_literals;
 using namespace mmltk::backend::data::detail;
 using namespace mmltk::backend::data::testsupport;
 using mmltk::common::concurrency::CancellationObservation;
 ScopedTempDir root("archive-copy-turns");
 const std::array inputs{profile_image_fixture(641, 641, FixtureImageEncoding::Png), profile_image_fixture(29, 31, FixtureImageEncoding::Jpeg)};
 REQUIRE(inputs[0].encoded.size() > (1U << 20));
 std::vector<std::size_t> order(32, 1);
 order[0] = 0;
 const auto path = root.path() / "source.tar.gz";
 const auto names = profile_archive_fixture(path, inputs, order);
 CompileExecution execution(1, {}, {.transient_bytes = 8U << 20});
 const BenchmarkTraceSink trace{CompileTraceSelection({"benchmark.archive."}), [](std::string_view, const Json&) {}};
 BenchmarkArchive input(path, &execution, 0, {}, 1, true, {}, 1024, 0, 32768, trace);
 REQUIRE(input.next());
 CHECK(input.member() == names.front());
 mmltk::testsupport::TestGate copying("archive entered its first bounded read grant");
 struct Boundary {
  std::function<void()> entered;
  mutable std::atomic<bool> first{true};
  bool cancelled() const noexcept {
   if (first.exchange(false)) entered();
   return false;
  }
 } boundary{[&] { copying.receipt().ArriveAndWait(); }};
 std::vector<std::uint8_t> bytes(inputs[0].encoded.size());
 auto reading = std::async(std::launch::async, [&] { input.read_into(bytes, CancellationObservation::Borrow(boundary)); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { copying.Release(); });
 REQUIRE(copying.WaitEntered(5s));
 std::uint64_t peer_turn = 0;
 auto peer = execution.submit({.kind = CompileTaskKind::Pixels, .work = [&](std::size_t, const CompileAllowance&) { peer_turn = input.observation().read_turns; }});
 copying.Release();
 peer.wait();
 mmltk::testsupport::await_test_future(reading, "multi-block copy yields bounded archive grants", 5s);
 CHECK(std::ranges::equal(bytes, inputs[0].encoded));
 auto evidence = input.observation();
 CHECK(peer_turn >= 1);
 CHECK(peer_turn < evidence.read_turns);
 CHECK(evidence.read_turns < evidence.read_blocks);
 CHECK(evidence.max_turn_blocks <= 8);
 CHECK(evidence.max_turn_bytes <= (1U << 20));
 CHECK(evidence.copied_bytes == bytes.size());
 for (std::size_t i = 1; i < order.size(); ++i) {
  REQUIRE(input.next());
  REQUIRE(input.member() == names[i]);
  bytes.resize(inputs[order[i]].encoded.size());
  input.read_into(bytes);
  CHECK(std::ranges::equal(bytes, inputs[order[i]].encoded));
 }
 // Cancellation interrupts real prefix replay, then the same retained source
 // generation can restore and consume the last member successfully.
 input.suspend();
 struct CancelReplay {
  const BenchmarkArchive& source;
  std::uint64_t before;
  bool cancelled() const noexcept { return source.observation().replay_decoded_bytes > before; }
 } cancellation{input, input.observation().replay_decoded_bytes};
 CHECK_THROWS(input.read_into(bytes, CancellationObservation::Borrow(cancellation)));
 CHECK(input.retained_workspace_bytes() == 0);
 input.read_into(bytes);
 CHECK(std::ranges::equal(bytes, inputs.back().encoded));
 // The arbitrary callback remains outside the Archive CPU grant and may
 // reserve work through the same execution owner without recursive waiting.
 REQUIRE(input.seek(names.back()));
 std::size_t consumed = 0;
 input.consume(bytes.size(), [&](std::span<const std::uint8_t> block, std::uint64_t offset) {
  CHECK_FALSE(execution.on_lane());
  auto admitted = execution.reserve({.bytes = 1});
  CHECK(std::ranges::equal(block, std::span(inputs.back().encoded).subspan(offset, block.size())));
  consumed += block.size();
 });
 CHECK(consumed == bytes.size());
 evidence = input.observation();
 CHECK(evidence.restores >= 2);
 CHECK(evidence.replay_decoded_bytes > 0);
 CHECK(evidence.replay_source_bytes > 0);
 // Known tar header offsets restore directly after decoder replay; they do
 // not require a second format-level scan through earlier members.
 CHECK(evidence.skipped_headers == 0);
 input.pause();
 CHECK(execution.resource_usage().descriptors == 1);
 CHECK(input.retained_workspace_bytes() == 0);
}
TEST_CASE("borrowed archive workspace stays charged while explicit and exclusive retirement close decoders", "[benchmark][coconut][archive][resources]") {
 using namespace mmltk::backend::data::detail;
 ScopedTempDir root("archive-physical-custody");
 const auto path = root.path() / "source.tar.gz";
 const std::array<std::pair<std::string, std::string>, 1> members{{{"member.bin", "physical decoder custody"}}};
 tar(path, members);
 CompileExecution execution(1, {}, {.transient_bytes = 8U << 20});
 SECTION("borrowed pinned envelope is not returned by local alias reset") {
  auto supplied = execution.reserve({.bytes = 4U << 20, .supplied_workspace = true});
  BenchmarkArchive input(path, &execution, 0, {}, 1, true, supplied);
  auto occupied = execution.reserve({.bytes = 4U << 20});
  auto wait = execution.defer_resources({.bytes = 1});
  CHECK_FALSE(input.reclamation_useful());
  input.suspend();
  CHECK(supplied.workspace().size() == (4U << 20));
  CHECK(execution.resource_usage().bytes >= (8U << 20));
  CHECK(execution.resource_usage().descriptors == 1);
 }
 SECTION("oversized workspace drains even when one decoder is smaller than the request") {
  BenchmarkArchive input(path, &execution);
  const CompileResources demand{.bytes = 9U << 20, .supplied_workspace = true};
  REQUIRE_FALSE(execution.try_reserve(demand));
  auto wait = execution.defer_resources(demand);
  CHECK(execution.oversized_workspace_pending());
  REQUIRE(input.reclamation_useful());
  input.suspend();
  auto accepted = execution.try_reserve(demand);
  REQUIRE(accepted);
  CHECK(accepted->exceptional_workspace());
  CHECK(execution.resource_usage().descriptors == 1);
  accepted.reset();
  wait.reset();
  REQUIRE(input.try_resume(0));
  REQUIRE(input.next());
  const auto payload = input.read(1024);
  CHECK(std::string(payload.begin(), payload.end()) == members.front().second);
  input.pause();
 }
 CHECK(execution.resource_usage().bytes == 0);
 CHECK(execution.resource_usage().descriptors == 0);
}
TEST_CASE("unchanged native finalization requires a staged clean payload and rebinds its canonical lineage", "[benchmark][coconut][annotations][recovery]") {
 enum class Input { Background, Mask, Box, Dropped, Malformed, Degenerate, Unlisted, InvalidStuff };
 const auto kind = GENERATE(Input::Background, Input::Mask, Input::Box, Input::Dropped, Input::Malformed, Input::Degenerate, Input::Unlisted, Input::InvalidStuff);
 ScopedTempDir root("native-unchanged-proof");
 CoconutStaging staging(root.path());
 CoconutRecord record;
 record.image_id = 7;
 record.width = 3;
 record.height = 1;
 record.source_ordinal = 17;
 record.first_segment_ordinal = 41;
 if (kind != Input::Background) record.segments = {{.id = 1, .category_id = 1, .isthing = true}};
 if (kind == Input::Box) record.segments[0].bbox = std::array<double, 4>{0, 0, 2, 1};
 if (kind == Input::Malformed) record.segments[0].category_id = 999;
 if (kind == Input::Degenerate) record.segments[0].bbox = std::array<double, 4>{0, 0, 0, 1};
 if (kind == Input::InvalidStuff) {
  record.segments[0].isthing = false;
  record.segments[0].valid = false;
 }
 const bool foreground = kind == Input::Mask || kind == Input::Malformed || kind == Input::Degenerate || kind == Input::InvalidStuff;
 const std::array<std::uint32_t, 3> pixels{foreground ? 1U : kind == Input::Unlisted ? 2U : 0U, 0, 0};
 const auto encoded = png(3, 1, pixels);
 const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
 auto lineage = std::make_shared<CoconutNativeLineage>();
 lineage->component.source = CoconutImageNamespace::CocoTrain;
 lineage->component.annotation_input_identity = "unchanged-proof-input";
 lineage->index.source = coconut_index_source(lineage->component.source);
 lineage->index.split = coconut_component_split(lineage->component.edition, lineage->component.source);
 CoconutNativeWorkspace workspace(CoconutImportLimits{});
 workspace.decode(record, bytes, {3, 1});
 auto provisional = workspace.finish(record, coco(7), {3, 1}, lineage, {});
 if (!provisional->boxes().empty()) CHECK_FALSE(provisional->can_finalize_unchanged());
 provisional = CoconutNativeImage::stage(provisional, staging);
 const bool clean = kind == Input::Background || kind == Input::Mask || kind == Input::Box;
 CHECK(provisional->can_finalize_unchanged() == clean);
 auto final_lineage = std::make_shared<CoconutNativeLineage>(*lineage);
 final_lineage->component.original_generation = 23;
 final_lineage->component.original_annotation_identity = "current-original";
 final_lineage->component.recovery_policy = kCoconutRecoveryPolicy;
 auto physical = coco(7, CoconutImageNamespace::CocoUnlabeled);
 physical.shard = 3;
 physical.archive_identity = "current-physical";
 if (!clean) {
  CHECK_THROWS_AS(CoconutNativeImage::bind(provisional, physical, final_lineage), std::logic_error);
  unsigned reports = 0;
  workspace.decode(record, bytes, {3, 1});
  const auto ordinary = workspace.finish(record, coco(7), {3, 1}, final_lineage, [&](const auto&, const auto&, const auto&, auto, auto) { ++reports; });
  // An invalid segment is absent from the admitted ID table, so its pixels
  // still produce the ordinary unlisted-segment report.
  CHECK(reports == (kind == Input::Unlisted ? 2 : 1));
  CHECK(ordinary->required_labels_valid() == provisional->required_labels_valid());
  CHECK(ordinary->rejected().malformed_records == provisional->rejected().malformed_records);
  CHECK(ordinary->rejected().degenerate_boxes == provisional->rejected().degenerate_boxes);
  return;
 }
 auto stale_lineage = std::make_shared<CoconutNativeLineage>(*final_lineage);
 stale_lineage->component.annotation_input_identity = "replacement-annotation";
 CHECK_THROWS_AS(CoconutNativeImage::bind(provisional, physical, stale_lineage), std::logic_error);
 const auto final = CoconutNativeImage::bind(provisional, physical, final_lineage);
 if (!final->boxes().empty()) REQUIRE(final->admission());
 CHECK(final->boxes().data() == provisional->boxes().data());
 CHECK(final->runs().data() == provisional->runs().data());
 CHECK(final->inventory().source_ordinal == record.source_ordinal);
 CHECK(final->inventory().physical == physical);
 CHECK(final->image().source_shard == 3);
 CHECK(final->segment_begin() == record.first_segment_ordinal);
 CHECK(final->lineage().component.original_generation == 23);
 CHECK(final->lineage().component.source == physical.source);
 CHECK(final->lineage().index.split == coconut_component_split(final_lineage->component.edition, physical.source));
 CHECK(final->input_identity() == coconut_image_input_identity(lineage->component.annotation_input_identity, physical, "current-original"));
 CHECK(final->input_identity() != provisional->input_identity());
 CHECK(final->recovery().objects.empty());
 CHECK(final->recovery().omissions.empty());
 CHECK(final->rejected().raw_records == provisional->rejected().raw_records);
 if (!final->boxes().empty()) CHECK(final->boxes().front().source_ordinal == 41);
 const auto read = CoconutNativeImage::read(final);
 REQUIRE_NOTHROW(admit_normalized_annotations(read.view.storage()));
}
TEST_CASE("bounded Pipeline native fixtures retain clean bytes across unchanged and real recovery", "[benchmark][coconut][pipeline][recovery]") {
 using namespace mmltk::backend::data::testsupport;
 ScopedTempDir root("pipeline-native-fixture");
 const std::array inputs{profile_image_fixture(8, 8, FixtureImageEncoding::Png)};
 const std::array<std::size_t, 8> order{};
 auto plan = std::make_shared<detail::CompileWorkspacePlan>(detail::compile_workspace_plan(1, 8, false, kParquetPoolBytes, 0, BenchmarkEncodedImage::normal_window_bytes));
 BenchmarkCompilePipeline pipeline(1, {}, {.workspace_plan = plan});
 std::array<NormalizedAnnotationReadView, 2> results;
 for (unsigned recovered = 0; recovered != 2; ++recovered) {
  PipelineNativeFixture fixture(root.path() / std::to_string(recovered), inputs, order, recovered != 0);
  std::uint64_t unchanged = 0, objects = 0, copy_bytes = 0, staging_bytes = 0, recovery_copies = 0, gather_tasks = 0;
  std::mutex observations;
  const BenchmarkTraceSink trace = [&](std::string_view event, const Json& value) {
   const std::lock_guard lock(observations);
   if (event == "benchmark.coconut.native_finalized") {
    unchanged += value.at("unchanged").get<bool>();
    objects += value.at("recovered").get<std::uint64_t>();
   }
   if (event == "benchmark.coconut.native_staged") {
    copy_bytes += value.at("copied_bytes").get<std::uint64_t>();
    staging_bytes += value.at("staged_bytes").get<std::uint64_t>();
    recovery_copies += value.at("kind") == CompileTaskKind::Recovery;
   }
   if (event == "benchmark.annotations.gathered") gather_tasks += value.at("tasks").get<std::uint64_t>();
  };
  results[recovered] = fixture.run(pipeline, trace);
  CHECK(unchanged == (recovered ? 2 : 8));
  CHECK(objects == (recovered ? 6 : 0));
  CHECK(recovery_copies == (recovered ? 6 : 0));
  CHECK(copy_bytes > 0);
  CHECK(staging_bytes >= copy_bytes);
  CHECK(gather_tasks == 1);
  CHECK(results[recovered].image_count() == 8);
  CHECK(results[recovered].box_count() == 6);
  CHECK(pipeline.execution().snapshot().leased == 0);
 }
 const auto& clean = results[0].storage();
 const auto& recovered = results[1].storage();
 REQUIRE(clean.boxes.size() == recovered.boxes.size());
 REQUIRE(clean.mask_rle_pairs.size() == recovered.mask_rle_pairs.size());
 CHECK(std::memcmp(clean.boxes.data(), recovered.boxes.data(), clean.boxes.size_bytes()) == 0);
 CHECK(std::memcmp(clean.mask_rle_pairs.data(), recovered.mask_rle_pairs.data(), clean.mask_rle_pairs.size_bytes()) == 0);
}
TEST_CASE("modern final caches leave held original snapshots to independent part callbacks", "[benchmark][coconut][cache][pipeline][recovery]") {
 using namespace std::chrono_literals;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires original inspection and independent annotation lanes");
 const bool training = GENERATE(false, true);
 CAPTURE(training);
 ScopedTempDir root("modern-cache-held-original");
 LocalCoconutRecipe local(root.path());
 auto config = local.compiler_config({BenchmarkDatasetVariant::Coconut, CoconutValidation::CoconutStock, true}, true);
 auto catalog = local.selected(config.selection.validation);
 catalog.stock_annotations = local_custom_catalog(local).coco_annotations;
 const auto originals = local.cache.source_downloads("coco") / catalog.stock_annotations.filename;
 const std::array<std::pair<std::string, std::string>, 2> original_members{
  {{"annotations/instances_train2017.json", file_bytes(local.cache.source_indexes("coco") / "train2017.fixture.json")},
   {"annotations/instances_val2017.json", file_bytes(local.cache.source_indexes("coco") / "val2017.fixture.json")}}
 };
 tar(originals, original_members);
 catalog.stock_annotations.expected_size = std::filesystem::file_size(originals);
 add_ready_parquet_part(local, catalog);
 compile_benchmark_recipe(config, &catalog);
 const auto expected_train = benchmark_semantics(local.output / "train.bin");
 const auto expected_validation = benchmark_semantics(local.output / "val.bin");
 config.num_workers = 3;
 mmltk::testsupport::TestGate held("current original snapshot has not been published");
 std::atomic<bool> cancelled{false}, entered{false};
 std::atomic<unsigned> full_routes{0};
 std::promise<void> labels;
 const std::string original_split = training ? "train2017" : "val2017";
 remove_normalized_annotation_index(local.cache.source_indexes("coco") / (original_split + ".normalized.bin"));
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 const auto diagnostics = mmltk::backend::data::testsupport::benchmark_trace();
 config.trace = [&](std::string_view event, std::string_view encoded) {
  const auto fields = Json::parse(encoded);
  if (diagnostics) diagnostics(event, fields);
  if (event == "benchmark.annotations.release_cache_selected" && fields.at("edition") == (training ? CoconutEdition::Base : CoconutEdition::RelabeledValidation)) ++full_routes;
  if (event == "benchmark.labels.placed" && fields.at("image_id") == (training ? 8 : 9)) mmltk::testsupport::release_test_promise(labels);
 };
 auto compiling = std::async(std::launch::async, [&] {
  compile_benchmark_recipe(config, &catalog, nullptr, {}, {}, {}, [&](bool current_training, CocoAnnotationInputBoundary boundary) {
   if (current_training == training && boundary == CocoAnnotationInputBoundary::Parsing && !entered.exchange(true)) held.receipt().ArriveAndWait();
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled = true;
  held.Release();
 });
 REQUIRE(held.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(labels, "part labels publish before their full-cache original prerequisite", 5s);
 CHECK(full_routes == 0);
 CHECK(compiling.wait_for(0ms) == std::future_status::timeout);
 held.Release();
 mmltk::testsupport::await_test_future(compiling, "held original snapshot settles independent part products", 15s);
 CHECK(benchmark_semantics(local.output / "train.bin") == expected_train);
 CHECK(benchmark_semantics(local.output / "val.bin") == expected_validation);
}
