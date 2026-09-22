#include "detail/benchmark_annotation_cache.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stb_image_write.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "src/test_support/async_test_utils.hpp"
#include "src/common/io/staging_directory.h"
#include "detail/benchmark_annotations.h"
#include "detail/benchmark_cache.h"
#include "detail/benchmark_compiler.h"
#include "detail/benchmark_download.h"
#include "detail/benchmark_images.h"
#include "detail/benchmark_sampling.h"
#include "detail/benchmark_storage.h"
#include "detail/benchmark_writer.h"
#include "detail/benchmark_image_decoder.h"
#include "detail/benchmark_progress.h"
#include "detail/open_images_acquisition.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/environment_test_utils.hpp"
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "src/backend/data/benchmark_hash.h"
#include "src/common/io/file_digest.h"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/compiled_dataset.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/backend/data/tests/benchmark_http_fixture.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/io/file_memory.h"
#include "src/common/io/scoped_fd.h"
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace mmltk::backend::data;
using namespace mmltk::backend::data::benchmark_internal;
using mmltk::common::io::FileHandle;
namespace {
void require_condition(const bool condition, const char* message) {
 if (!condition) { throw std::runtime_error(message); }
}
[[nodiscard]] BenchmarkWriteRequest benchmark_write_request(const PreparedBenchmarkSplit& split, fs::path output, const std::uint32_t resolution,
                                                            const mmltk::common::concurrency::CancellationObservation cancellation = {}) {
 return {
  .split = split,
  .output_path = std::move(output),
  .resolution = resolution,
  .num_workers = 2,
  .worker_cpus = {},
  .overwrite = false,
  .cancel_requested = cancellation,
  .progress = {},
 };
}
void test_benchmark_trace_gate_is_lazy() {
 std::size_t builder_invocations = 0U;
 const BenchmarkTraceSink disabled;
 trace_benchmark_event(disabled, "benchmark.test.disabled", [&] {
  ++builder_invocations;
  return nlohmann::json{{"value", 1U}};
 });
 REQUIRE(builder_invocations == 0U);
 std::size_t deliveries = 0U;
 const BenchmarkTraceSink enabled = [&](const std::string_view event, const nlohmann::json& fields) {
  ++deliveries;
  CHECK(event == "benchmark.test.enabled");
  CHECK(fields.at("value") == 2U);
 };
 trace_benchmark_event(enabled, "benchmark.test.enabled", [&] {
  ++builder_invocations;
  return nlohmann::json{{"value", 2U}};
 });
 REQUIRE(builder_invocations == 1U);
 REQUIRE(deliveries == 1U);
}
// Observe physical identity as well as bytes: a diagnostic failure must not
// silently replace an admitted artifact with an equivalent new file.
class RetainedArtifact final {
public:
 explicit RetainedArtifact(fs::path path)
     : path_(std::move(path)), digest_(mmltk::common::io::sha256_file(path_)), before_(mmltk::common::io::FileSnapshot::Read(path_)) {}
 void Check() const {
  CHECK(mmltk::common::io::FileSnapshot::Read(path_) == before_);
  CHECK(mmltk::common::io::sha256_file(path_) == digest_);
 }

private:
 fs::path path_;
 mmltk::common::io::Sha256Digest digest_;
 mmltk::common::io::FileSnapshot before_;
};
const BenchmarkTraceSink throwing_trace = [](std::string_view, const nlohmann::json&) { throw std::runtime_error("diagnostic sink failure"); };
void write_text(const fs::path& path, const std::string& text) {
 fs::create_directories(path.parent_path());
 std::ofstream output(path, std::ios::binary | std::ios::trunc);
 output << text;
 require_condition(output.good(), "failed to write benchmark test input");
}
void corrupt_byte(const fs::path& path, const std::uint64_t offset) {
 std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
 file.seekg(static_cast<std::streamoff>(offset));
 char value = 0;
 file.read(&value, 1);
 require_condition(file.good(), "failed to read benchmark corruption target");
 value ^= static_cast<char>(0x5A);
 file.seekp(static_cast<std::streamoff>(offset));
 file.write(&value, 1);
 require_condition(file.good(), "failed to corrupt benchmark test artifact");
}
std::vector<std::uint8_t> make_payload(const std::size_t bytes) {
 std::vector<std::uint8_t> payload(bytes);
 for (std::size_t index = 0; index < payload.size(); ++index) { payload[index] = static_cast<std::uint8_t>((index * 131U + 17U) & 0xFFU); }
 return payload;
}
[[nodiscard]] bool has_generated_payload(const fs::path& path, const std::size_t bytes) {
 const FileHandle file = FileHandle::open_readonly(path.string());
 std::array<std::uint8_t, 64U * 1024U> block{};
 bool exact = true;
 for (std::size_t offset = 0U; offset < bytes; offset += block.size()) {
  file.pread_all(block.data(), block.size(), offset);
  for (std::size_t i = 0U; i < block.size(); ++i) exact &= block[i] == static_cast<std::uint8_t>(((offset + i) * 131U + 17U) & 0xFFU);
 }
 return exact;
}
using mmltk::backend::data::testsupport::HttpServer;
DownloadRequest request_for(const fs::path& root, const std::string& id, const std::string& url, const std::vector<std::uint8_t>& payload) {
 return DownloadRequest{
  id, url, root / (id + ".bin"), root / "locks" / (id + ".lock"), payload.size(), mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(payload)), 3U,
 };
}
void test_benchmark_download_cache_lifecycle() {
 mmltk::testsupport::ScopedTempDir root("downloads");
 const std::vector<std::uint8_t> payload = make_payload(std::size_t{8U} * 1024U * 1024U);
 HttpServer server(payload);
 DownloadRequest retry = request_for(root.path(), "retry", server.url("retry"), payload);
 retry.source = BenchmarkDatasetSource::kObjects365V2;
 // Artifact spelling is deliberately unrelated to the owning source.
 server.fail_next(1);
 std::vector<DownloadProgress> retry_updates;
 const auto retry_result = download_artifacts({retry}, 1U, {}, [&](const auto& update) { retry_updates.push_back(update); });
 REQUIRE(retry_result.size() == 1U);
 REQUIRE(retry_result[0].attempts == 2U);
 CHECK(std::ranges::any_of(retry_updates, [](const auto& update) { return update.attempt == 2U && update.completed_bytes > 0U; }));
 DownloadRequest resume = request_for(root.path(), "resume", server.url("resume"), payload);
 std::atomic<bool> cancel{false};
 server.GateNextTransfer();
 mmltk::testsupport::TestGate received("HTTP partial bytes persisted");
 auto download = std::async(std::launch::async, [&] {
  try {
   (void)download_artifacts({resume}, 1U, mmltk::common::concurrency::CancellationObservation::Atomic(cancel), [&](const DownloadProgress& progress) {
    if (progress.completed_bytes >= HttpServer::partial_bytes) received.receipt().ArriveAndWait();
   });
   return false;
  } catch (const std::exception&) { return true; }
 });
 const mmltk::testsupport::ScopedTestCleanup cancel_download([&] {
  cancel.store(true, std::memory_order_release);
  received.Release();
  server.ReleasePartial();
 });
 REQUIRE(server.WaitPartial());
 REQUIRE(received.WaitEntered(3s));
 cancel.store(true, std::memory_order_release);
 received.Release();
 // Keep the sender parked until cancellation settles, so the physical
 // partial file is independent of callback frequency and scheduler speed.
 REQUIRE(mmltk::testsupport::await_test_future(download, "partial HTTP cancellation", 5s));
 REQUIRE(!fs::exists(resume.destination));
 REQUIRE(fs::file_size(resume.destination.string() + ".part") == HttpServer::partial_bytes);
 server.ReleasePartial();
 cancel.store(false, std::memory_order_release);
 std::vector<DownloadProgress> resumed_updates;
 resume.source = BenchmarkDatasetSource::kCoco2017;
 const auto resumed = download_artifacts({resume}, 1U, mmltk::common::concurrency::CancellationObservation::Atomic(cancel),
                                         [&](const auto& update) { resumed_updates.push_back(update); });
 REQUIRE_FALSE(resumed_updates.empty());
 CHECK(std::ranges::any_of(resumed_updates, [](const auto& update) {
  return update.resumed && update.retained_bytes == HttpServer::partial_bytes && update.completed_bytes > update.retained_bytes;
 }));
 for (const auto& update : resumed_updates) {
  CHECK(update.completed_bytes >= HttpServer::partial_bytes);
  CHECK(update.completed_bytes <= payload.size());
  CHECK(update.total_bytes == payload.size());
 }
 REQUIRE(resumed[0].resumed);
 REQUIRE(server.ranged_requests() > 0U);
 const std::uint64_t before_cache_hit = server.requests();
 const RetainedArtifact retained_download{resume.destination};
 const auto cached = download_artifacts({resume}, 1U, {}, {}, throwing_trace);
 retained_download.Check();
 CHECK(cached[0].identity == resumed[0].identity);
 REQUIRE(cached[0].cache_hit);
 REQUIRE(server.requests() == before_cache_hit);
 corrupt_byte(resume.destination, 0U);
 invalidate_download_artifact(resume);
 std::size_t byte_traces = 0U;
 const auto repaired = download_artifacts({resume}, 1U, {}, {}, [&](const auto event, const nlohmann::json& fields) {
  if (event == "benchmark.download.progress") {
   ++byte_traces;
   CHECK(fields.at("artifact") == "resume");
   CHECK_FALSE(fields.at("cache_hit").get<bool>());
   CHECK(fields.at("retained_bytes") == 0U);
  }
 });
 CHECK(byte_traces > 0U);
 REQUIRE(!repaired[0].cache_hit);
 REQUIRE(server.requests() > before_cache_hit);
 REQUIRE(repaired[0].size == payload.size());
 REQUIRE(!repaired[0].identity.empty());
 DownloadRequest unavailable = request_for(root.path(), "unavailable", "http://127.0.0.1:1/unavailable", payload);
 unavailable.maximum_attempts = 1U;
 bool source_failed = false;
 try {
  (void)download_artifacts({unavailable}, 1U, {});
 } catch (const std::exception&) { source_failed = true; }
 REQUIRE(source_failed);
 REQUIRE(!fs::exists(unavailable.destination));
 server.Check();
}
void test_benchmark_annotation_indexes() {
 mmltk::testsupport::ScopedTempDir root("annotations");
 const fs::path coco = root.path() / "mini-coco.json";
 write_text(coco, R"({"images":[{"id":1,"width":16,"height":8},{"id":2,"width":16,"height":8}],)"
                  R"("annotations":[{"image_id":1,"category_id":1,"bbox":[1,1,6,4]},)"
                  R"({"image_id":1,"category_id":1,"bbox":[1,1,6,4]},)"
                  R"({"image_id":1,"category_id":1,"bbox":[2,2,0,1]},)"
                  R"({"image_id":1,"category_id":2,"bbox":[1,1,2,2]}],)"
                  R"("categories":[{"id":1,"name":"person"}]})");
 const std::array<NumericCategoryMapping, 1> mappings{{
  {1U, 0U, "person"},
 }};
 AnnotationParseOptions options;
 options.source = BenchmarkDatasetSource::kCoco2017;
 options.split = "validation";
 options.expected_image_count = 2U;
 options.num_workers = 2;
 options.keep_images_without_mapped_boxes = true;
 const std::string digest = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(coco));
 NormalizedAnnotationIndex parsed = parse_coco_style_annotations(coco, digest, mappings, options);
 REQUIRE(parsed.images.size() == 2U);
 REQUIRE(parsed.boxes.size() == 2U);
 REQUIRE(parsed.rejected.duplicate_boxes == 0U);
 REQUIRE(parsed.rejected.degenerate_boxes == 1U);
 REQUIRE(parsed.rejected.unmapped_categories == 1U);
 parsed.rejected = {
  .raw_records = 101U, .unmapped_categories = 23U, .unknown_images = 37U, .malformed_records = 41U, .degenerate_boxes = 53U, .duplicate_boxes = 67U};
 const fs::path index_path = root.path() / "mini-coco.index";
 store_normalized_annotation_index(index_path, parsed, {});
 const RetainedArtifact retained_index{index_path};
 auto loaded = load_normalized_annotation_index(index_path, options.source, options.split, digest, {}, throwing_trace);
 retained_index.Check();
 if (!loaded.has_value()) { throw std::runtime_error("stored normalized annotation index did not reload"); }
 REQUIRE(loaded.value().images.size() == 2U);
 REQUIRE(loaded.value().boxes.size() == 2U);
 CHECK(image_ids(*loaded) == image_ids(parsed));
 CHECK(loaded->rejected.raw_records == 101U);
 CHECK(loaded->rejected.unmapped_categories == 23U);
 CHECK(loaded->rejected.unknown_images == 37U);
 CHECK(loaded->rejected.malformed_records == 41U);
 CHECK(loaded->rejected.degenerate_boxes == 53U);
 CHECK(loaded->rejected.duplicate_boxes == 67U);
 // Independent version-3 byte offsets and values protect the persisted format.
 std::ifstream persisted(index_path, std::ios::binary);
 std::uint32_t version = 0U;
 persisted.seekg(8);
 persisted.read(reinterpret_cast<char*>(&version), sizeof(version));
 REQUIRE(persisted.good());
 CHECK(version == 3U);
 std::uint64_t image_offset = 0U;
 persisted.seekg(36);
 persisted.read(reinterpret_cast<char*>(&image_offset), sizeof(image_offset));
 REQUIRE(persisted.good());
 CHECK(image_offset == 256U);
 std::array<std::uint64_t, 6> slots{};
 persisted.seekg(196);
 persisted.read(reinterpret_cast<char*>(slots.data()), sizeof(slots));
 REQUIRE(persisted.good());
 CHECK((slots == std::array<std::uint64_t, 6>{101U, 23U, 37U, 41U, 53U, 67U}));
 const nlohmann::json expected_rejections{{"raw_records", 101U},      {"unmapped_categories", 23U}, {"unknown_images", 37U},
                                          {"malformed_records", 41U}, {"degenerate_boxes", 53U},    {"duplicate_boxes", 67U}};
 const auto manifest_path = root.path() / "manifest" / "rejections.json";
 write_json_atomically(manifest_path, {{"rejected_records", reject_json(parsed.rejected)}}, {});
 CHECK(read_json_file(manifest_path).at("rejected_records") == expected_rejections);
 corrupt_byte(index_path, 0U);
 loaded = load_normalized_annotation_index(index_path, options.source, options.split, digest, {});
 REQUIRE(!loaded);
 store_normalized_annotation_index(index_path, parsed, {});
 const fs::path classes = root.path() / "classes.csv";
 const fs::path boxes = root.path() / "boxes.csv";
 write_text(classes, "/m/person,Person\n");
 write_text(boxes,
            "ImageID,Source,LabelName,Confidence,XMin,XMax,YMin,YMax,IsOccluded,IsTruncated,IsGroupOf\n"
            "0000000000000001,xclick,/m/person,1,0.1,0.8,0.2,0.9,0,0,0\n"
            "0000000000000001,xclick,/m/person,1,0.1,0.8,0.2,0.9,0,0,1\n"
            "0000000000000001,xclick,/m/person,1,0.5,0.5,0.2,0.9\n");
 const std::array<StringCategoryMapping, 1> open_mappings{{
  {"/m/person", 0U, "Person"},
 }};
 AnnotationParseOptions open_options;
 open_options.source = BenchmarkDatasetSource::kOpenImagesV7;
 open_options.split = "train";
 open_options.num_workers = 2;
 NormalizedAnnotationIndex open =
  parse_open_images_annotations(boxes, classes, mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(boxes)), open_mappings, open_options);
 REQUIRE(open.images.size() == 1U);
 REQUIRE(open.boxes.size() == 2U);
 REQUIRE(open.rejected.duplicate_boxes == 0U);
 CHECK(open.boxes[0].source_ordinal < open.boxes[1].source_ordinal);
 CHECK(decode_open_images_category(open.boxes[0].source_category_id) == "/m/person");
 CHECK((open.boxes[0].flags & kAnnotationCrowd) == 0U);
 CHECK((open.boxes[1].flags & kAnnotationCrowd) != 0U);
 REQUIRE(open.rejected.degenerate_boxes == 1U);
}
void test_benchmark_supplemental_sampling() {
 NormalizedAnnotationIndex source;
 source.source = BenchmarkDatasetSource::kObjects365V2;
 source.split = "train";
 source.annotation_sha256 = "synthetic";
 constexpr std::size_t kImageCount = 60U;
 source.images.reserve(kImageCount);
 source.boxes.reserve(kImageCount + 12U);
 std::array<std::uint32_t, kImageCount> original_box_counts{};
 for (std::size_t image_index = 0U; image_index < kImageCount; ++image_index) {
  const std::uint64_t first_box = source.boxes.size();
  source.boxes.push_back(NormalizedBox{0.1F, 0.2F, 0.8F, 0.9F, 0U, 0U, static_cast<std::uint8_t>(image_index % 3U), {}});
  if (image_index % 5U == 0U) { source.boxes.push_back(NormalizedBox{0.2F, 0.3F, 0.7F, 0.8F, 0U, 0U, static_cast<std::uint8_t>(image_index % 3U), {}}); }
  original_box_counts[image_index] = static_cast<std::uint32_t>(source.boxes.size() - first_box);
  source.images.push_back(
   NormalizedImage{image_index, first_box, original_box_counts[image_index], 640U, 480U, static_cast<std::uint16_t>(image_index % 4U), 0U});
 }
 source.rejected = {89, 7, 6, 5, 4, 3};
 for (std::size_t i = 0; i < source.boxes.size(); ++i) {
  auto& box = source.boxes[i];
  box.mask_rle_offset = source.mask_rle_pairs.size();
  box.mask_rle_pairs = i % 3 == 0 ? 0 : 2;
  box.flags = kAnnotationMask | kAnnotationCategory | kAnnotationId | kAnnotationIgnore;
  box.original_area = 3.25 + static_cast<double>(i);
  box.annotation_id = 800 + i;
  box.source_category_id = 30 + i;
  box.source_ordinal = 900 - i;
  if (box.mask_rle_pairs) {
   source.mask_rle_pairs.push_back({static_cast<std::uint32_t>(i), 1});
   source.mask_rle_pairs.push_back({static_cast<std::uint32_t>(i + 100), 2});
  }
 }
 NormalizedAnnotationIndex coco;
 coco.source = BenchmarkDatasetSource::kCoco2017;
 NormalizedAnnotationIndex open_images = source;
 open_images.source = BenchmarkDatasetSource::kOpenImagesV7;
 const std::array<std::uint64_t, 4U> shard_bytes{1U, 1U, 1U, 1U};
 const CombinedSupplementalSamplingResult first_combined = sample_combined_supplemental_indices(coco, source, open_images, shard_bytes);
 const CombinedSupplementalSamplingResult second_combined = sample_combined_supplemental_indices(coco, source, open_images, shard_bytes);
 const SupplementalSamplingResult& first = first_combined.objects365;
 const SupplementalSamplingResult& second = second_combined.objects365;
 REQUIRE(first.stats.full_images == kImageCount);
 REQUIRE(first.stats.full_boxes == source.boxes.size());
 REQUIRE(first_combined.target_images == (kImageCount * 2U) / 6U);
 REQUIRE(first.stats.selected_images + first_combined.open_images.stats.selected_images == first_combined.target_images);
 REQUIRE(first_combined.open_images.stats.selected_images >= first_combined.open_images_floor);
 REQUIRE(first_combined.open_images.stats.selected_images <= first_combined.open_images_ceiling);
 for (const auto* selected : {&first_combined.objects365.index, &first_combined.open_images.index}) {
  CHECK(reject_json(selected->rejected) == reject_json(source.rejected));
  std::size_t box_position = 0, run_position = 0;
  for (const auto& image : selected->images) {
   auto expected_image = source.images[image.source_image_id];
   const auto source_first = expected_image.first_box;
   expected_image.first_box = box_position;
   CHECK(std::memcmp(&image, &expected_image, sizeof(NormalizedImage)) == 0);
   for (std::size_t j = 0; j < image.box_count; ++j) {
    auto expected = source.boxes[source_first + j];
    const auto source_run = expected.mask_rle_offset;
    expected.mask_rle_offset = run_position;
    REQUIRE(box_position < selected->boxes.size());
    CHECK(std::memcmp(&selected->boxes[box_position++], &expected, sizeof(NormalizedBox)) == 0);
    for (std::size_t r = 0; r < expected.mask_rle_pairs; ++r) {
     REQUIRE(run_position < selected->mask_rle_pairs.size());
     CHECK(selected->mask_rle_pairs[run_position].start == source.mask_rle_pairs[source_run + r].start);
     CHECK(selected->mask_rle_pairs[run_position++].length == source.mask_rle_pairs[source_run + r].length);
    }
   }
  }
  CHECK(box_position == selected->boxes.size());
  CHECK(run_position == selected->mask_rle_pairs.size());
 }
 REQUIRE(!first.index.images.empty());
 REQUIRE(first.stats.selected_boxes == first.index.boxes.size());
 REQUIRE(first.index.images.size() == second.index.images.size());
 REQUIRE(first.index.boxes.size() == second.index.boxes.size());
 std::uint64_t previous_id = 0U;
 bool first_image = true;
 const auto boxes_equal = [](const NormalizedBox& left, const NormalizedBox& right) {
  return left.x1 == right.x1 && left.y1 == right.y1 && left.x2 == right.x2 && left.y2 == right.y2 && left.class_id == right.class_id &&
         std::ranges::equal(left.reserved, right.reserved);
 };
 for (std::size_t image_index = 0U; image_index < first.index.images.size(); ++image_index) {
  const NormalizedImage& selected = first.index.images[image_index];
  const NormalizedImage& repeated = second.index.images[image_index];
  REQUIRE(selected.source_image_id == repeated.source_image_id);
  REQUIRE(selected.box_count == repeated.box_count);
  REQUIRE(selected.first_box == repeated.first_box);
  REQUIRE((first_image || selected.source_image_id > previous_id));
  first_image = false;
  previous_id = selected.source_image_id;
  REQUIRE(selected.box_count == original_box_counts[static_cast<std::size_t>(selected.source_image_id)]);
  for (std::uint32_t box_offset = 0U; box_offset < selected.box_count; ++box_offset) {
   const NormalizedBox& selected_box = first.index.boxes[static_cast<std::size_t>(selected.first_box + box_offset)];
   const NormalizedImage& original = source.images[static_cast<std::size_t>(selected.source_image_id)];
   const NormalizedBox& original_box = source.boxes[static_cast<std::size_t>(original.first_box + box_offset)];
   const NormalizedBox& repeated_box = second.index.boxes[static_cast<std::size_t>(repeated.first_box + box_offset)];
   REQUIRE(boxes_equal(selected_box, original_box));
   REQUIRE(boxes_equal(selected_box, repeated_box));
  }
 }
 std::array<std::uint64_t, 80U> combined_class_images{};
 for (std::size_t class_id = 0U; class_id < combined_class_images.size(); ++class_id) {
  combined_class_images[class_id] = first.stats.selected_class_images[class_id] + first_combined.open_images.stats.selected_class_images[class_id];
 }
 const auto [minimum, maximum] = std::ranges::minmax_element(combined_class_images.begin(), combined_class_images.begin() + 3);
 REQUIRE(*maximum - *minimum <= 1U);
 REQUIRE(std::ranges::all_of(combined_class_images.begin() + 3, combined_class_images.end(), [](const std::uint64_t count) { return count == 0U; }));
}
void append_bytes(void* context, void* data, const int size) {
 auto& output = *static_cast<std::vector<std::uint8_t>*>(context);
 const auto* begin = static_cast<const std::uint8_t*>(data);
 output.insert(output.end(), begin, begin + size);
}
std::vector<std::uint8_t> make_jpeg(const std::uint8_t red, const std::uint8_t green, const std::uint8_t blue) {
 constexpr int width = 16;
 constexpr int height = 8;
 std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width * height * 3));
 for (std::size_t pixel = 0; pixel < rgb.size() / 3U; ++pixel) {
  rgb[pixel * 3U] = red;
  rgb[pixel * 3U + 1U] = green;
  rgb[pixel * 3U + 2U] = blue;
 }
 std::vector<std::uint8_t> jpeg;
 const int result = stbi_write_jpg_to_func(append_bytes, &jpeg, width, height, 3, rgb.data(), 95);
 require_condition(result != 0 && !jpeg.empty(), "failed to encode benchmark test JPEG");
 return jpeg;
}
void write_tar_octal(std::array<std::uint8_t, 512U>* header, const std::size_t offset, const std::size_t width, const std::uint64_t value) {
 std::array<char, 32U> digits{};
 const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value, 8);
 require_condition(converted.ec == std::errc{}, "failed to format benchmark test tar number");
 const std::size_t digit_count = static_cast<std::size_t>(converted.ptr - digits.data());
 require_condition(digit_count < width, "benchmark test tar number exceeds its field");
 std::fill_n(header->begin() + static_cast<std::ptrdiff_t>(offset), width - 1U, static_cast<std::uint8_t>('0'));
 std::copy_n(reinterpret_cast<const std::uint8_t*>(digits.data()), digit_count,
             header->begin() + static_cast<std::ptrdiff_t>(offset + width - digit_count - 1U));
 (*header)[offset + width - 1U] = '\0';
}
void write_jpeg_tar_entry(std::ostream& output, const std::uint64_t image_id, const std::span<const std::uint8_t> jpeg) {
 std::array<std::uint8_t, 512U> header{};
 const std::string name = "images/" + std::to_string(image_id) + ".jpg";
 require_condition(name.size() < 100U, "benchmark test tar entry name is too long");
 std::copy(name.begin(), name.end(), header.begin());
 write_tar_octal(&header, 100U, 8U, 0644U);
 write_tar_octal(&header, 108U, 8U, 0U);
 write_tar_octal(&header, 116U, 8U, 0U);
 write_tar_octal(&header, 124U, 12U, jpeg.size());
 write_tar_octal(&header, 136U, 12U, 0U);
 std::fill_n(header.begin() + 148, 8U, static_cast<std::uint8_t>(' '));
 header[156] = static_cast<std::uint8_t>('0');
 constexpr std::string_view magic{"ustar"};
 std::copy(magic.begin(), magic.end(), header.begin() + 257);
 header[262] = '\0';
 header[263] = static_cast<std::uint8_t>('0');
 header[264] = static_cast<std::uint8_t>('0');
 std::uint64_t checksum = 0U;
 for (const std::uint8_t byte : header) { checksum += byte; }
 write_tar_octal(&header, 148U, 7U, checksum);
 header[155] = static_cast<std::uint8_t>(' ');
 output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
 output.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
 const std::size_t padding = (512U - (jpeg.size() % 512U)) % 512U;
 const std::array<std::uint8_t, 1024U> zeros{};
 require_condition(padding <= zeros.size(), "benchmark test tar padding exceeds buffer");
 output.write(reinterpret_cast<const char*>(zeros.data()), static_cast<std::streamsize>(padding));
 require_condition(output.good(), "failed to write benchmark test tar entry");
}
void write_single_jpeg_tar(const fs::path& path, const std::uint64_t image_id, const std::span<const std::uint8_t> jpeg) {
 std::ofstream output(path, std::ios::binary | std::ios::trunc);
 write_jpeg_tar_entry(output, image_id, jpeg);
 const std::array<char, 1024U> terminator{};
 output.write(terminator.data(), static_cast<std::streamsize>(terminator.size()));
 require_condition(output.good(), "failed to write benchmark test tar");
}
void test_benchmark_archive_training_quarantine() {
 mmltk::testsupport::ScopedTempDir root("archive-quarantine");
 const fs::path archive_path = root.path() / "images.tar";
 write_single_jpeg_tar(archive_path, 1U, make_jpeg(240U, 8U, 8U));
 const std::vector<std::uint64_t> requested{1U, 2U};
 std::uint64_t progress_completed = 0U;
 std::uint64_t progress_total = 0U;
 const CachedImageDirectory extracted = extract_selected_archive_images(ArchiveExtractionRequest{
  .archive_path = archive_path,
  .source_identity = "objects:test",
  .output_root = root.path() / "images",
  .source = "objects365",
  .shard = "patch-0",
  .selected_image_ids = requested,
  .image_id_parser = [](const std::string_view path) -> std::optional<std::uint64_t> {
   return path.ends_with("/1.jpg") ? std::optional<std::uint64_t>{1U} : std::nullopt;
  },
  .cancel_requested = {},
  .progress =
   [&](const std::uint64_t completed, const std::uint64_t total) {
    progress_completed = completed;
    progress_total = total;
   },
  .validator = [](const std::uint64_t,
                  const std::span<const std::uint8_t> encoded) { require_condition(has_complete_image_markers(encoded), "archive test JPEG is incomplete"); },
  .trace = {},
  .quarantine_unavailable = true,
  .decompression_workers = 0U,
  .cache_write_workers = 0U,
  .activity = {},
 });
 REQUIRE(extracted.image_count == 1U);
 REQUIRE(extracted.quarantined.size() == 1U);
 REQUIRE(extracted.quarantined.front().image_id == 2U);
 REQUIRE(progress_completed == requested.size());
 REQUIRE(progress_total == requested.size());
 std::uint64_t cached_bytes = 0U;
 std::vector<CachedImageRejection> cached_rejections;
 REQUIRE(validate_cached_image_group(extracted.path, extracted.path / ".complete.json", "objects:test", requested, &cached_bytes, {}, {}, &cached_rejections));
 REQUIRE(cached_rejections.size() == 1U);
 REQUIRE(cached_rejections.front().image_id == 2U);
}
void test_benchmark_cached_image_writer_and_loader() {
 constexpr std::uint32_t kNanoResolution = 384U;
 mmltk::testsupport::ScopedTempDir root("writer");
 const std::vector<std::uint8_t> red = make_jpeg(240U, 8U, 8U);
 const std::vector<std::uint8_t> green = make_jpeg(8U, 240U, 8U);
 std::vector<std::uint8_t> padded = red;
 padded.insert(padded.end(), 64U, 0xFFU);
 REQUIRE(has_complete_image_markers(red));
 REQUIRE(has_complete_image_markers(padded));
 const fs::path image_root = root.path() / "images";
 const std::vector<std::uint64_t> ids{1U, 2U};
 const std::vector<std::uint64_t> requested_ids{1U, 2U, 3U};
 const std::array<CachedImageRejection, 1U> cache_rejections{{
  {3U, "missing from verified training archive"},
 }};
 prepare_cached_image_directory(image_root);
 write_cached_image_atomically(cached_image_path(image_root, 1U), red, {});
 write_cached_image_atomically(cached_image_path(image_root, 2U), green, {});
 const fs::path completion = image_root / ".complete.json";
 const RetainedArtifact retained_red{cached_image_path(image_root, 1U)};
 const RetainedArtifact retained_green{cached_image_path(image_root, 2U)};
 complete_cached_image_group(image_root, completion, "validation:mini", requested_ids, red.size() + green.size(), {}, throwing_trace, cache_rejections);
 const RetainedArtifact retained_completion{completion};
 const auto proof = read_json_file(completion);
 CHECK(proof.at("selection_sha256") == cached_image_selection_digest(ids));
 CHECK(proof.at("requested_selection_sha256") == cached_image_selection_digest(requested_ids));
 std::uint64_t cached_bytes = 0U;
 std::vector<CachedImageRejection> loaded_rejections;
 REQUIRE(validate_cached_image_group(image_root, completion, "validation:mini", requested_ids, &cached_bytes, {}, throwing_trace, &loaded_rejections));
 REQUIRE(cached_bytes == red.size() + green.size());
 REQUIRE(loaded_rejections.size() == 1U);
 REQUIRE(loaded_rejections.front().image_id == 3U);
 CHECK(loaded_rejections.front().reason == cache_rejections.front().reason);
 retained_red.Check();
 retained_green.Check();
 retained_completion.Check();
 PreparedBenchmarkSplit split;
 split.name = "validation";
 split.class_names = {"person"};
 split.sources.push_back(CachedImageSource{image_root});
 const mmltk::backend::imaging::resample::ImageResizeGeometry letterbox = mmltk::backend::imaging::resample::compute_image_resize_geometry(
  16U, 8U, kNanoResolution, kNanoResolution, mmltk::backend::imaging::resample::ImageResizeMode::Letterbox);
 REQUIRE(letterbox.resized_width == kNanoResolution);
 REQUIRE(letterbox.resized_height == 192U);
 REQUIRE(letterbox.offset_x == 0U);
 REQUIRE(letterbox.offset_y == 96U);
 split.labels = {
  benchmark_canvas_box(0U, 0.0F, 0.0F, 1.0F, 1.0F, letterbox),
  benchmark_canvas_box(0U, 0.125F, 0.25F, 0.875F, 0.75F, letterbox),
 };
 for (std::size_t index = 0U; index < ids.size(); ++index) {
  split.images.push_back(EncodedImageRecord{
   ids[index],
   16U,
   8U,
   static_cast<std::uint32_t>(index),
   1U,
   0U,
  });
 }
 const fs::path output = root.path() / "validation.bin";
 std::atomic<std::uint64_t> producer_events{0U};
 write_benchmark_split(BenchmarkWriteRequest{
  split,
  output,
  kNanoResolution,
  2,
  {},
  false,
  {},
  {.context = &producer_events,
   .image_completed = [](void* context) { static_cast<std::atomic<std::uint64_t>*>(context)->fetch_add(1U, std::memory_order_relaxed); }},
  false,
  mmltk::backend::imaging::resample::ImageResizeMode::Letterbox,
 });
 REQUIRE(producer_events.load(std::memory_order_relaxed) == split.images.size());
 REQUIRE_FALSE(static_cast<bool>(benchmark_write_request(split, output, kNanoResolution).progress));
 const CompiledDatasetInfo info = inspect_compiled_dataset(output);
 REQUIRE(info.image_count == 2U);
 REQUIRE(info.width == kNanoResolution);
 REQUIRE(info.height == kNanoResolution);
 REQUIRE(std::ranges::equal(info.class_names(), std::vector<std::string>{"person"}));
 const FileHandle file = FileHandle::open_readonly(output.string());
 const FileHeader header = read_compiled_header(file);
 const CompiledFileSections sections = validate_compiled_file_sections(header, file.size());
 std::vector<ImageEntry> index(header.num_images);
 std::vector<PackedInstance> labels(sections.label_count);
 file.pread_all(index.data(), index.size() * sizeof(ImageEntry), header.index_offset);
 file.pread_all(labels.data(), labels.size() * sizeof(PackedInstance), header.label_offset);
 validate_compiled_index_entries(index, header, labels.size());
 validate_compiled_original_image_dimensions(index);
 REQUIRE(index[0].original_width == 16U);
 REQUIRE(index[0].original_height == 8U);
 REQUIRE(validate_compiled_label_entries(labels, header, sections.rle_region_bytes) == 0U);
 REQUIRE(std::ranges::all_of(labels, [](const PackedInstance& label) { return label.mask_rle_offset == 0U && label.mask_rle_pairs == 0U; }));
 REQUIRE(labels[0].bbox_x1 == 0.0F);
 REQUIRE(labels[0].bbox_y1 == 96.0F);
 REQUIRE(labels[0].bbox_x2 == 384.0F);
 REQUIRE(labels[0].bbox_y2 == 288.0F);
 DatasetLoader::Config loader_config;
 loader_config.loading.h2d_dataloader = true;
 loader_config.compiled_path = output.string();
 loader_config.batch_size = 1U;
 loader_config.shuffle = false;
 loader_config.prefetch_factor = 2;
 loader_config.gather_workers = 1;
 DatasetLoader loader(loader_config);
 REQUIRE(loader.num_images() == 2U);
 REQUIRE(loader.num_rle_pairs() == 0U);
 loader.begin_epoch();
 Batch batch{};
 std::size_t loaded_images = 0U;
 while (loader.next_batch(batch)) {
  loader.wait_batch(batch);
  REQUIRE(batch.num_images == 1U);
  REQUIRE(batch.labels != nullptr);
  const std::size_t plane = static_cast<std::size_t>(kNanoResolution) * kNanoResolution;
  REQUIRE(loader.host_images(batch)[0] == 0.0F);
  REQUIRE(loader.host_images(batch)[plane] == 0.0F);
  REQUIRE(loader.host_images(batch)[plane * 2U] == 0.0F);
  const std::size_t content_pixel = static_cast<std::size_t>(100U) * kNanoResolution;
  REQUIRE((loader.host_images(batch)[content_pixel] > 0.75F || loader.host_images(batch)[plane + content_pixel] > 0.75F));
  loaded_images += batch.num_images;
  loader.release_batch(batch);
 }
 loader.synchronize();
 REQUIRE(loaded_images == 2U);
 const auto source_digest = mmltk::common::io::sha256_file(cached_image_path(image_root, 1U));
 const auto cache_manifest_digest = mmltk::common::io::sha256_file(completion);
 auto perceptual_request = benchmark_write_request(split, root.path() / "perceptual.bin", kNanoResolution);
 perceptual_request.perceptual_downscale = true;
 perceptual_request.resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Letterbox;
 write_benchmark_split(perceptual_request);
 // These sources enlarge; selecting perceptual shrinking changes no pixels or format facts.
 CHECK(mmltk::common::io::sha256_file(perceptual_request.output_path) == mmltk::common::io::sha256_file(output));
 CHECK(mmltk::common::io::sha256_file(cached_image_path(image_root, 1U)) == source_digest);
 CHECK(mmltk::common::io::sha256_file(completion) == cache_manifest_digest);
 CHECK(validate_cached_image_group(image_root, completion, "validation:mini", requested_ids, &cached_bytes, {}, throwing_trace, &loaded_rejections));
 const auto raw_cache_identity = read_json_file(completion);
 for (const bool perceptual : {false, true}) {
  BenchmarkCompilerConfig manifest_config;
  manifest_config.resolution = kNanoResolution;
  manifest_config.perceptual_downscale = perceptual;
  const auto staged = root.path() / (perceptual ? "filtered-stage" : "ordinary-stage");
  fs::create_directories(staged);
  // Acquired facts come from the real raw-cache fixture. The production
  // publication boundary owns the envelope and selected policy/version.
  const nlohmann::json acquired{{"image_cache", nlohmann::json::array({raw_cache_identity})},
                                {"train", {{"bytes", fs::file_size(output)}}},
                                {"val", {{"bytes", fs::file_size(perceptual_request.output_path)}}}};
  publish_benchmark_manifest(manifest_config, staged, image_root, acquired, {});
  const auto manifest = read_json_file(staged / "benchmark_manifest.json");
  CHECK(manifest.at("resampling").at("perceptual_downscale").get<bool>() == perceptual);
  CHECK(manifest.at("resampling").at("version").get<unsigned>() == 1U);
  CHECK(manifest.at("compiled_format_version").get<unsigned>() == FORMAT_VERSION);
  CHECK(manifest.at("normalized_annotation_version").get<unsigned>() == 3U);
  CHECK(manifest.at("resize_mode") == "stretch");
  CHECK(manifest.at("schema_version").get<unsigned>() == 3U);
  CHECK(manifest.at("image_cache").at(0) == raw_cache_identity);
  CHECK(manifest.at("resolution").get<unsigned>() == kNanoResolution);
  CHECK(mmltk::common::io::sha256_file(completion) == cache_manifest_digest);
  CHECK(mmltk::common::io::sha256_file(cached_image_path(image_root, 1U)) == source_digest);
 }
 const std::string original_digest = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(output));
 bool refused = false;
 try {
  write_benchmark_split(benchmark_write_request(split, output, kNanoResolution));
 } catch (const std::exception&) { refused = true; }
 REQUIRE(refused);
 REQUIRE(mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(output)) == original_digest);
 std::atomic<bool> cancel{true};
 const fs::path cancelled_output = root.path() / "cancelled.bin";
 bool cancelled = false;
 try {
  write_benchmark_split(benchmark_write_request(split, cancelled_output, kNanoResolution, mmltk::common::concurrency::CancellationObservation::Atomic(cancel)));
 } catch (const std::exception&) { cancelled = true; }
 REQUIRE(cancelled);
 REQUIRE(!fs::exists(cancelled_output));
 PreparedBenchmarkSplit broken = split;
 broken.images[0].source_width = 15U;
 const fs::path broken_output = root.path() / "broken.bin";
 bool failed = false;
 try {
  write_benchmark_split(benchmark_write_request(broken, broken_output, kNanoResolution));
 } catch (const std::exception&) { failed = true; }
 REQUIRE(failed);
 REQUIRE(!fs::exists(broken_output));
}
void test_benchmark_archive_quarantine_policy() {
 REQUIRE(archive_selection_allows_quarantine(BenchmarkDatasetSource::kCoco2017, "train2017"));
 REQUIRE(!archive_selection_allows_quarantine(BenchmarkDatasetSource::kCoco2017, "val2017"));
 REQUIRE(archive_selection_allows_quarantine(BenchmarkDatasetSource::kObjects365V2, "patch-0"));
}
void test_benchmark_event_cancellation_while_waiting_for_lock_without_progress() {
 mmltk::testsupport::ScopedTempDir root("event-cancellation");
 const fs::path output = root.path() / "compiled";
 const fs::path cache = root.path() / "cache";
 const std::string normalized_output = fs::weakly_canonical(fs::absolute(output)).generic_string();
 const fs::path lock_path = root.path() / ".cache" / "benchmark-dataset" / "v1" / "locks" /
                            ("output-" +
                             mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(
                              std::span(reinterpret_cast<const std::uint8_t*>(normalized_output.data()), normalized_output.size()))) +
                             ".lock");
 fs::create_directories(lock_path.parent_path());
 const mmltk::common::io::ScopedFd lock_descriptor{::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644)};
 const int descriptor = lock_descriptor.get();
 require_condition(descriptor >= 0, "failed to open benchmark heartbeat lock");
 require_condition(::flock(descriptor, LOCK_EX | LOCK_NB) == 0, "failed to acquire benchmark heartbeat lock");
 struct CancellationTag;
 using CancellationSource = mmltk::common::concurrency::EventCancellationSource<CancellationTag, false>;
 auto [cancellation, token] = CancellationSource::Mint();
 struct ReachedCancellation final {
  const decltype(token)* cancellation_token = nullptr;
  mmltk::testsupport::TestGate::Receipt reached;
  [[nodiscard]] bool cancelled() const noexcept {
   reached.ArriveAndWait();
   return cancellation_token->cancelled();
  }
 };
 mmltk::testsupport::TestGate reached_lock_wait("benchmark lock cancellation observation");
 const ReachedCancellation observed{.cancellation_token = &token, .reached = reached_lock_wait.receipt()};
 std::exception_ptr compile_error;
 auto compiler = std::async(std::launch::async, [&] {
  try {
   BenchmarkCompilerConfig config;
   config.output_dir = output;
   config.cache_dir = cache;
   config.resolution = 1U;
   config.num_workers = 1;
   config.overwrite = true;
   config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Borrow(observed);
   compile_benchmark_dataset(std::move(config));
  } catch (...) { compile_error = std::current_exception(); }
 });
 const mmltk::testsupport::ScopedTestCleanup cleanup([&] {
  static_cast<void>(cancellation.RequestCancel());
  reached_lock_wait.Release();
  (void)::flock(descriptor, LOCK_UN);
 });
 REQUIRE(reached_lock_wait.WaitEntered(3s));
 REQUIRE(cancellation.RequestCancel());
 reached_lock_wait.Release();
 mmltk::testsupport::await_test_future(compiler, "benchmark cancelled lock settlement", 3s);
 REQUIRE(compile_error != nullptr);
}
void test_benchmark_cli_source_status_preserves_active_transfer_state() {
 BenchmarkSourceProgress active;
 active.source = BenchmarkDatasetSource::kObjects365V2;
 active.activity = "Downloading objects365-train-patch-4";
 active.cache_hit = true;
 active.resumed = true;
 active.retry_count = 2U;
 const std::string status = format_benchmark_source_status(active, "extracting");
 REQUIRE(status.starts_with("Downloading objects365-train-patch-4"));
 REQUIRE(status.find("resumed") != std::string::npos);
 REQUIRE(status.find("2 retries") != std::string::npos);
 active.activity = "Verifying cached objects365-train-patch-4";
 REQUIRE(format_benchmark_source_status(active, "extracting").starts_with("Verifying cached"));
 active.complete = true;
 REQUIRE(format_benchmark_source_status(active, "extracting") == "Cache hit");
}
}  // namespace
TEST_CASE("benchmark cache roots share explicit environment and relative precedence", "[backend][data][benchmark][cache]") {
 mmltk::testsupport::ScopedTempDir root{"benchmark-cache-precedence"};
 const auto original_directory = fs::current_path();
 const mmltk::testsupport::ScopedEnvironmentVariable environment{"MMLTK_BENCHMARK_DATASET_CACHE_ROOT"};
 const mmltk::testsupport::ScopedTestCleanup restore_directory{[&] { fs::current_path(original_directory); }};
 fs::create_directories(root.path() / "working");
 fs::current_path(root.path() / "working");
 const auto environment_cache = root.path() / "environment-cache";
 REQUIRE(::setenv("MMLTK_BENCHMARK_DATASET_CACHE_ROOT", environment_cache.c_str(), 1) == 0);
 BenchmarkCompilerConfig config;
 config.output_dir = root.path() / "compiled";
 config.resolution = 1U;
 config.num_workers = 1;
 fs::path expected_cache = environment_cache;
 bool overlap = false;
 SECTION("explicit configuration overrides environment and resolves aliases") {
  fs::create_directories(root.path() / "explicit-cache");
  fs::create_directory_symlink(root.path() / "explicit-cache", root.path() / "cache-alias");
  config.cache_dir = "../cache-alias";
  expected_cache = root.path() / "explicit-cache";
 }
 SECTION("environment overrides invocation working directory") {}
 SECTION("empty environment uses the relative fallback") {
  REQUIRE(::setenv("MMLTK_BENCHMARK_DATASET_CACHE_ROOT", "", 1) == 0);
  expected_cache = fs::current_path() / ".cache/benchmark-dataset/v1";
 }
 SECTION("unset environment uses the relative fallback") {
  REQUIRE(::unsetenv("MMLTK_BENCHMARK_DATASET_CACHE_ROOT") == 0);
  expected_cache = fs::current_path() / ".cache/benchmark-dataset/v1";
 }
 SECTION("output inside environment cache is rejected") {
  config.output_dir = expected_cache / "compiled";
  overlap = true;
 }
 SECTION("cache inside output is rejected") {
  config.output_dir = root.path() / "compiled";
  expected_cache = config.output_dir / "cache";
  REQUIRE(::setenv("MMLTK_BENCHMARK_DATASET_CACHE_ROOT", expected_cache.c_str(), 1) == 0);
  overlap = true;
 }
 SECTION("equal cache and output is rejected") {
  config.output_dir = expected_cache;
  overlap = true;
 }
 const auto retained = expected_cache / "retained.fixture";
 write_text(retained, "existing cached bytes");
 const auto retained_digest = mmltk::common::io::sha256_file(retained);
 const auto retained_time = fs::last_write_time(retained);
 std::atomic<bool> cancelled{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 config.progress = [&](const BenchmarkCompileProgress&) { cancelled.store(true); };
 nlohmann::json paths;
 std::size_t path_reports = 0U;
 config.trace = [&](const std::string_view event, const std::string_view fields) {
  if (event == "benchmark.compile.paths") {
   ++path_reports;
   paths = nlohmann::json::parse(fields);
  }
 };
 if (overlap) {
  CHECK_THROWS_WITH(compile_benchmark_dataset(config), "benchmark output and cache directories must not overlap");
  CHECK_FALSE(fs::exists(expected_cache / "downloads"));
 } else {
  CHECK_THROWS_WITH(compile_benchmark_dataset(config), "benchmark dataset compilation cancelled");
  CHECK(fs::is_directory(expected_cache / "downloads"));
  CHECK_FALSE(fs::exists(config.output_dir));
 }
 REQUIRE(path_reports == 1U);
 CHECK(paths.at("cache_root") == fs::weakly_canonical(expected_cache).native());
 CHECK(paths.at("output_root") == fs::weakly_canonical(config.output_dir).native());
 CHECK_FALSE(paths.at("cache_root_truncated").get<bool>());
 CHECK_FALSE(paths.at("output_root_truncated").get<bool>());
 CHECK(mmltk::common::io::sha256_file(retained) == retained_digest);
 CHECK(fs::last_write_time(retained) == retained_time);
}
TEST_CASE("benchmark publication admission preserves separate physical output and retained files", "[backend][data][benchmark][cache]") {
 mmltk::testsupport::ScopedTempDir root{"benchmark-publication-admission"};
 const auto publication = root.path() / "compiled";
 auto cache = root.path() / "cache";
 BenchmarkCompilerConfig config;
 config.output_dir = root.path() / "compiled.tmp.fixture";
 config.publication_dir = publication;
 config.resolution = 1U;
 config.num_workers = 1;
 config.overwrite = true;
 bool overlap = true;
 SECTION("disjoint publication remains admitted") { overlap = false; }
 SECTION("cache equals final output") { cache = publication; }
 SECTION("cache lies below final output") { cache = publication / "cache"; }
 SECTION("final output lies below cache") { config.publication_dir = cache / "compiled"; }
 SECTION("relative final alias resolves before admission") {
  cache = publication;
  config.publication_dir = fs::relative(root.path(), fs::current_path()) / "unused/../compiled";
 }
 SECTION("symlink final alias resolves before admission") {
  fs::create_directories(cache);
  fs::create_directory_symlink(cache, publication);
 }
 SECTION("symlink cache alias resolves before admission") {
  fs::create_directories(publication);
  fs::create_directory_symlink(publication, cache);
 }
 config.cache_dir = cache;
 const auto final_output = fs::weakly_canonical(fs::absolute(config.publication_dir));
 const std::array retained{cache / "retained.fixture", final_output / "train.bin"};
 std::array<struct stat, 2U> before{};
 for (std::size_t index = 0; index < retained.size(); ++index) {
  write_text(retained[index], "retained bytes");
  REQUIRE(::stat(retained[index].c_str(), &before[index]) == 0);
 }
 std::atomic<bool> cancelled{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 config.progress = [&](const BenchmarkCompileProgress&) { cancelled.store(true); };
 CHECK_THROWS_WITH(compile_benchmark_dataset(config),
                   overlap ? "benchmark output and cache directories must not overlap" : "benchmark dataset compilation cancelled");
 cancelled.store(false);
 std::size_t path_trace_calls = 0U;
 config.trace = [&](std::string_view event, std::string_view) {
  if (event == "benchmark.compile.paths") ++path_trace_calls;
  throw std::runtime_error("diagnostic callback failure");
 };
 CHECK_THROWS_WITH(compile_benchmark_dataset(config),
                   overlap ? "benchmark output and cache directories must not overlap" : "benchmark dataset compilation cancelled");
 CHECK(path_trace_calls == 1U);
 CHECK(cancelled.load());
 CHECK_FALSE(fs::exists(config.output_dir));
 CHECK(fs::exists(cache / "downloads") == !overlap);
 for (std::size_t index = 0; index < retained.size(); ++index) {
  struct stat after{};
  REQUIRE(::stat(retained[index].c_str(), &after) == 0);
  CHECK(after.st_ino == before[index].st_ino);
  CHECK(after.st_mtim.tv_sec == before[index].st_mtim.tv_sec);
  CHECK(after.st_mtim.tv_nsec == before[index].st_mtim.tv_nsec);
  std::ifstream bytes{retained[index]};
  CHECK(std::string(std::istreambuf_iterator<char>{bytes}, {}) == "retained bytes");
 }
}
TEST_CASE("benchmark overlap admission leaves an absent publication destination absent", "[backend][data][benchmark][cache]") {
 mmltk::testsupport::ScopedTempDir root{"benchmark-absent-publication"};
 BenchmarkCompilerConfig config;
 config.publication_dir = root.path() / "compiled";
 config.output_dir = config.publication_dir / "compiled.tmp.fixture";
 config.cache_dir = config.publication_dir / "cache";
 config.resolution = 1U;
 config.num_workers = 1;
 std::atomic<bool> cancelled{false};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 config.progress = [&](const BenchmarkCompileProgress&) { cancelled.store(true); };
 REQUIRE_FALSE(fs::exists(config.publication_dir));
 CHECK_THROWS_WITH(compile_benchmark_dataset(config), "benchmark output and cache directories must not overlap");
 CHECK(cancelled.load());
 CHECK_FALSE(fs::exists(config.publication_dir));
 CHECK_FALSE(fs::exists(config.output_dir));
 CHECK_FALSE(fs::exists(config.cache_dir));
}
TEST_CASE("benchmark path traces bound escaped native bytes without changing admission", "[backend][data][benchmark][trace]") {
 mmltk::testsupport::ScopedTempDir root{"benchmark-path-encoding"};
 fs::path selected;
 std::string expected;
 bool truncated = false;
 bool replaced = false;
 SECTION("escape-heavy paths fit the envelope and report their bounded prefix") {
  auto prefix = root.path();
  for (unsigned index = 0U; index < 5U; ++index) { prefix /= std::string(200U, '\x01'); }
  // 1,000 control bytes cost 6,000 serialized bytes; five separators
  // and the temporary root are literal ASCII. The final slash costs one.
  const auto remaining = 6144U - (root.path().native().size() + 5U + 6000U + 1U);
  truncated = true;
  SECTION("control bytes exhaust the escaped budget") {
   selected = prefix / std::string(200U, '\x01');
   expected = prefix.native() + "/" + std::string(remaining / 6U, '\x01');
  }
  SECTION("truncation keeps a multibyte character whole at the budget boundary") {
   const std::string padding(remaining - 1U, 'a');
   selected = prefix / (padding + "\xe2\x82\xac-end");
   expected = prefix.native() + "/" + padding;
  }
 }
 SECTION("invalid native bytes are replaced with explicit loss reporting") {
  // Stray continuation, overlong, surrogate, out-of-range and incomplete
  // encodings: all twelve bytes are invalid at their respective positions.
  selected = root.path() / "\x80\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80\xe2\x82";
  expected = root.path().native() + "/";
  for (unsigned index = 0U; index < 12U; ++index) { expected += "\xef\xbf\xbd"; }
  replaced = true;
 }
 SECTION("valid UTF-8 and JSON escape characters retain their exact native meaning") {
  selected = root.path() / "\xc2\xa2\xe2\x82\xac\xf0\x9f\x98\x80\"\\\n\t";
  expected = selected.native();
 }
 BenchmarkCompilerConfig config;
 config.output_dir = selected;
 config.cache_dir = selected;
 config.resolution = 1U;
 std::atomic<bool> cancelled{true};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 // Both modes must reach the same product error, including non-UTF-8 paths.
 CHECK_THROWS_WITH(compile_benchmark_dataset(config), "benchmark output and cache directories must not overlap");
 nlohmann::json paths;
 std::size_t deliveries = 0U;
 std::size_t serialized_size = 0U;
 config.trace = [&](const std::string_view event, const std::string_view fields) {
  if (event != "benchmark.compile.paths") { return; }
  ++deliveries;
  serialized_size = fields.size();
  paths = nlohmann::json::parse(fields);
 };
 CHECK_THROWS_WITH(compile_benchmark_dataset(config), "benchmark output and cache directories must not overlap");
 REQUIRE(deliveries == 1U);
 // The backend contract reserves over 3 KiB of the 16 KiB runtime record
 // for its envelope, event name and subsequent timestamp fields.
 CHECK(serialized_size < 13U * 1024U);
 CHECK(paths.at("cache_root") == expected);
 CHECK(paths.at("output_root") == expected);
 CHECK(paths.at("cache_root_truncated") == truncated);
 CHECK(paths.at("output_root_truncated") == truncated);
 CHECK(paths.at("cache_root_utf8_replaced") == replaced);
 CHECK(paths.at("output_root_utf8_replaced") == replaced);
 CHECK_FALSE(fs::exists(selected));
}
TEST_CASE("benchmark destination preparation preserves parent and obstruction behavior", "[backend][data][benchmark][cache]") {
 mmltk::testsupport::ScopedTempDir root{"benchmark-parent"};
 const auto bare = root.path().filename().string() + ".json";
 const mmltk::testsupport::ScopedTestCleanup remove_bare([&] {
  for (const auto suffix : {"", ".lock", ".part", ".part.json", ".download.json"}) {
   std::error_code ignored;
   fs::remove(bare + suffix, ignored);
  }
 });
 const auto payload = make_payload(4096U);
 HttpServer server(payload);
 const std::array<fs::path, 3> paths{bare, fs::relative(root.path()) / "relative" / "value.json", root.path() / "absolute" / "value.json"};
 for (const auto& path : paths) {
  write_json_atomically(path, {{"value", 19U}}, {});
  CHECK(read_json_file(path).at("value") == 19U);
  { auto lease = ArtifactLease::acquire(path.string() + ".lock", {}); }
  fs::remove(path.string() + ".lock");
  fs::path staged;
  {
   mmltk::common::io::StagingDirectory staging(path, ".", ".next.XXXXXX", "test staging creation");
   staged = staging.path();
   CHECK(fs::is_directory(staged));
   write_text(staged / "partial", "partial publication");
  }
  CHECK_FALSE(fs::exists(staged));
  auto request = request_for(root.path(), "parent", server.url("parent"), payload);
  request.destination = path;
  const auto downloaded = download_artifacts({request}, 1U, {});
  REQUIRE(downloaded.size() == 1U);
  CHECK(mmltk::common::io::sha256_file(path) == mmltk::common::io::sha256_bytes(payload));
 }
 const auto obstruction = root.path() / "file-parent";
 write_text(obstruction, "unchanged");
 const auto before = mmltk::common::io::sha256_file(obstruction);
 CHECK_THROWS_AS(write_json_atomically(obstruction / "value.json", {{"value", 23U}}, {}), fs::filesystem_error);
 CHECK_THROWS_AS(ArtifactLease::acquire(obstruction / "value.lock", {}), fs::filesystem_error);
 CHECK_THROWS_AS(mmltk::common::io::StagingDirectory(obstruction / "output", ".", ".next.XXXXXX", "test staging creation"), fs::filesystem_error);
 auto request = request_for(root.path(), "obstructed", server.url("obstructed"), payload);
 request.destination = obstruction / "download.bin";
 request.maximum_attempts = 1U;
 CHECK_THROWS(download_artifacts({request}, 1U, {}));
 CHECK(mmltk::common::io::sha256_file(obstruction) == before);
 server.Check();
}
TEST_CASE("benchmark download cache lifecycle", "[backend][data][benchmark][download]") { test_benchmark_download_cache_lifecycle(); }
TEST_CASE("benchmark annotation indexes", "[backend][data][benchmark][annotations]") { test_benchmark_annotation_indexes(); }
TEST_CASE("normalized slices preserve exact fields and owned storage", "[backend][data][benchmark][annotations]") {
 NormalizedAnnotationIndex input;
 input.split = "unsorted";
 input.annotation_sha256 = "identity";
 input.rejected = {13, 2, 3, 4, 5, 6};
 input.images = {{30, 0, 2, 8, 4, 7, 0}, {10, 2, 0, 9, 5, 8, 0}, {20, 2, 1, 10, 6, 9, 0}, {40, 3, 1, 11, 7, 10, 0}};
 input.boxes = {{0.1F, 0.2F, 0.8F, 0.9F, 0, 2, 2, kAnnotationMask | kAnnotationCategory | kAnnotationId, {}, 4.5, 101, 3, 19},
                {0.2F, 0.3F, 0.7F, 0.8F, 2, 0, 3, kAnnotationMask | kAnnotationCategory, {}, 0, 0, 4, 23},
                {0.3F, 0.4F, 0.6F, 0.7F, 2, 2, 4, kAnnotationMask | kAnnotationCrowd | kAnnotationCategory, {}, 8.25, 0, 5, 29},
                {0.4F, 0.5F, 0.8F, 0.9F, 4, 1, 5, kAnnotationMask | kAnnotationIgnore | kAnnotationCategory, {}, 1, 0, 6, 31}};
 input.mask_rle_pairs = {{0, 2}, {5, 1}, {2, 3}, {9, 2}, {7, 1}};
 const auto original = input;
 std::vector<std::size_t> order{0, 1, 2, 3};
 SECTION("identity does not inspect boxes or runs") {
  // Invalid payload metadata proves the identity path does not visit boxes.
  input.boxes[0].mask_rle_offset = UINT64_MAX;
  const auto* boxes = input.boxes.data();
  const auto* runs = input.mask_rle_pairs.data();
  const auto box_capacity = input.boxes.capacity(), run_capacity = input.mask_rle_pairs.capacity();
  retain_normalized_image_slices(input, order);
  CHECK(input.boxes.data() == boxes);
  CHECK(input.mask_rle_pairs.data() == runs);
  CHECK(input.boxes.capacity() == box_capacity);
  CHECK(input.mask_rle_pairs.capacity() == run_capacity);
  CHECK(input.boxes[0].mask_rle_offset == UINT64_MAX);
  return;
 }
 SECTION("leading removal") { order = {1, 2, 3}; }
 SECTION("middle removal and explicit empty mask") { order = {0, 3}; }
 SECTION("trailing removal") { order = {0, 1, 2}; }
 SECTION("empty foreground only") { order = {1}; }
 SECTION("fully removed") { order.clear(); }
 SECTION("genuine smaller permutation") { order = {2, 0}; }
 SECTION("sort unsorted IDs including empty image") { order = {1, 2, 0, 3}; }
 const auto* images = input.images.data();
 const auto* boxes = input.boxes.data();
 const auto* runs = input.mask_rle_pairs.data();
 const auto image_capacity = input.images.capacity(), box_capacity = input.boxes.capacity(), run_capacity = input.mask_rle_pairs.capacity();
 retain_normalized_image_slices(input, order);
 if (std::ranges::is_sorted(order)) {
  CHECK(input.images.data() == images);
  CHECK(input.boxes.data() == boxes);
  CHECK(input.mask_rle_pairs.data() == runs);
  CHECK(input.images.capacity() == image_capacity);
  CHECK(input.boxes.capacity() == box_capacity);
  CHECK(input.mask_rle_pairs.capacity() == run_capacity);
 }
 CHECK(input.split == "unsorted");
 CHECK(input.annotation_sha256 == "identity");
 CHECK(reject_json(input.rejected) == reject_json(original.rejected));
 std::size_t next_box = 0, next_run = 0;
 REQUIRE(input.images.size() == order.size());
 for (std::size_t i = 0; i < order.size(); ++i) {
  auto expected_image = original.images[order[i]];
  const auto first = expected_image.first_box;
  expected_image.first_box = next_box;
  CHECK(std::memcmp(&input.images[i], &expected_image, sizeof(NormalizedImage)) == 0);
  for (std::size_t j = 0; j < expected_image.box_count; ++j) {
   auto expected_box = original.boxes[first + j];
   const auto run_start = expected_box.mask_rle_offset;
   expected_box.mask_rle_offset = next_run;
   REQUIRE(next_box < input.boxes.size());
   CHECK(std::memcmp(&input.boxes[next_box++], &expected_box, sizeof(NormalizedBox)) == 0);
   for (std::size_t r = 0; r < expected_box.mask_rle_pairs; ++r) {
    REQUIRE(next_run < input.mask_rle_pairs.size());
    CHECK(input.mask_rle_pairs[next_run].start == original.mask_rle_pairs[run_start + r].start);
    CHECK(input.mask_rle_pairs[next_run++].length == original.mask_rle_pairs[run_start + r].length);
   }
  }
 }
 CHECK(input.boxes.size() == next_box);
 CHECK(input.mask_rle_pairs.size() == next_run);
}
TEST_CASE("normalized slice admission and cancellation cannot append partial data", "[backend][data][benchmark][annotations]") {
 struct Cancellation {
  mutable std::size_t polls = 0;
  std::size_t stop = SIZE_MAX;
  bool cancelled() const noexcept { return polls++ >= stop; }
 };
 NormalizedAnnotationIndex source, destination;
 source.images = {{1, 0, 1, 8, 8, 0, 0}};
 source.boxes.resize(1);
 source.boxes[0].mask_rle_pairs = 131073;
 source.mask_rle_pairs.resize(131073, RLEPair{1, 1});
 SECTION("image position") { CHECK_THROWS(append_normalized_image_slice(destination, source, 1)); }
 SECTION("box offset") {
  source.images[0].first_box = UINT64_MAX;
  CHECK_THROWS(append_normalized_image_slice(destination, source, 0));
 }
 SECTION("box count") {
  source.images[0].box_count = 2;
  CHECK_THROWS(append_normalized_image_slice(destination, source, 0));
 }
 SECTION("mask offset") {
  source.boxes[0].mask_rle_offset = UINT64_MAX;
  CHECK_THROWS(append_normalized_image_slice(destination, source, 0));
 }
 SECTION("mask count") {
  source.boxes[0].mask_rle_pairs++;
  CHECK_THROWS(append_normalized_image_slice(destination, source, 0));
 }
 SECTION("self append") { CHECK_THROWS(append_normalized_image_slice(source, source, 0)); }
 SECTION("retention checks every span before compaction") {
  source.images.push_back({2, 1, 1, 8, 8, 0, 0});
  const std::array<std::size_t, 1> retained{1};
  CHECK_THROWS(retain_normalized_image_slices(source, retained));
  source.boxes.push_back(source.boxes[0]);
  source.boxes[1].mask_rle_offset = UINT64_MAX;
  CHECK_THROWS(retain_normalized_image_slices(source, retained));
  CHECK(source.images[0].source_image_id == 1);
 }
 SECTION("overlapping forward run moves retain exact content with bounded cancellation") {
  source.images = {{1, 0, 1, 8, 8, 0, 0}, {2, 1, 1, 8, 8, 0, 0}};
  source.boxes.insert(source.boxes.begin(), NormalizedBox{});
  source.boxes[0].mask_rle_pairs = 1;
  source.boxes[1].mask_rle_offset = 1;
  source.mask_rle_pairs.resize(131074);
  for (std::size_t i = 0; i < source.mask_rle_pairs.size(); ++i) source.mask_rle_pairs[i] = {static_cast<std::uint32_t>(i), 1};
  const std::array<std::size_t, 1> retained{1};
  Cancellation observed;
  auto complete = source;
  const auto* runs = complete.mask_rle_pairs.data();
  retain_normalized_image_slices(complete, retained, mmltk::common::concurrency::CancellationObservation::Borrow(observed));
  CHECK(complete.mask_rle_pairs.data() == runs);
  REQUIRE(complete.mask_rle_pairs.size() == 131073);
  for (std::size_t i = 0; i < complete.mask_rle_pairs.size(); ++i) {
   CHECK(complete.mask_rle_pairs[i].start == i + 1);
   CHECK(complete.mask_rle_pairs[i].length == 1);
  }
  for (std::size_t cut = 0; cut < observed.polls; ++cut) {
   auto interrupted = source;
   Cancellation stop{0, cut};
   CHECK_THROWS(retain_normalized_image_slices(interrupted, retained, mmltk::common::concurrency::CancellationObservation::Borrow(stop)));
  }
 }
 SECTION("every transfer cancellation point") {
  Cancellation observed;
  auto complete = destination;
  append_normalized_image_slice(complete, source, 0, mmltk::common::concurrency::CancellationObservation::Borrow(observed));
  REQUIRE(observed.polls >= 5);
  for (std::size_t cut = 0; cut < observed.polls; ++cut) {
   Cancellation stop{0, cut};
   CHECK_THROWS(append_normalized_image_slice(destination, source, 0, mmltk::common::concurrency::CancellationObservation::Borrow(stop)));
   CHECK(destination.images.empty());
   CHECK(destination.boxes.empty());
   CHECK(destination.mask_rle_pairs.empty());
  }
 }
 CHECK(destination.images.empty());
 CHECK(destination.boxes.empty());
 CHECK(destination.mask_rle_pairs.empty());
}
TEST_CASE("benchmark supplemental sampling", "[backend][data][benchmark][sampling]") { test_benchmark_supplemental_sampling(); }
TEST_CASE("benchmark cached image writer and loader", "[backend][data][benchmark][writer]") { test_benchmark_cached_image_writer_and_loader(); }
TEST_CASE("benchmark archive quarantine policy", "[backend][data][benchmark][images]") { test_benchmark_archive_quarantine_policy(); }
TEST_CASE("benchmark archive training quarantine", "[backend][data][benchmark][images]") { test_benchmark_archive_training_quarantine(); }
TEST_CASE("benchmark event cancellation without progress", "[backend][data][benchmark][cancel]") {
 test_benchmark_event_cancellation_while_waiting_for_lock_without_progress();
}
TEST_CASE("benchmark source status", "[backend][data][benchmark][progress]") { test_benchmark_cli_source_status_preserves_active_transfer_state(); }
TEST_CASE("benchmark trace gate is lazy", "[backend][data][benchmark][trace]") { test_benchmark_trace_gate_is_lazy(); }
TEST_CASE("benchmark annotations retain provenance crowd area masks and deterministic source order", "[backend][data][benchmark][annotations]") {
 mmltk::testsupport::ScopedTempDir root("faithful-benchmark");
 const auto path = root.path() / "annotations.json";
 nlohmann::json document{{"images", {{{"id", 0}, {"width", 16}, {"height", 8}}}},
                         {"categories", {{{"id", 1}, {"name", "person"}}, {{"id", 2}, {"name", "human"}}}},
                         {"annotations", nlohmann::json::array()}};
 for (unsigned ordinal = 0; ordinal != 128U; ++ordinal) {
  document["annotations"].push_back({{"id", 127U - ordinal},
                                     {"image_id", 0},
                                     {"category_id", ordinal % 2U + 1U},
                                     {"bbox", {-0.5, 1.25, 13.0, 5.5}},
                                     {"area", 7.25},
                                     {"iscrowd", ordinal % 2U},
                                     {"ignore", true},
                                     {"segmentation", nlohmann::json::array()}});
 }
 document["annotations"].push_back({{"image_id", 0}, {"category_id", 1}, {"segmentation", {{"size", {8, 16}}, {"counts", {0, 1, 127}}}}});
 write_text(path, document.dump());
 const std::array<NumericCategoryMapping, 2> mappings{{{1U, 0U, "person"}, {2U, 0U, "human"}}};
 AnnotationParseOptions options;
 options.split = "train";
 options.num_workers = 4;
 const auto digest = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(path));
 const auto parallel = parse_coco_style_annotations(path, digest, mappings, options);
 options.num_workers = 1;
 const auto sequential = parse_coco_style_annotations(path, digest, mappings, options);
 REQUIRE(parallel.boxes.size() == 129U);
 REQUIRE(parallel.boxes.size() == sequential.boxes.size());
 CHECK(std::memcmp(parallel.boxes.data(), sequential.boxes.data(), parallel.boxes.size() * sizeof(NormalizedBox)) == 0);
 CHECK(parallel.rejected.duplicate_boxes == 0U);
 for (unsigned ordinal = 0; ordinal != 128U; ++ordinal) {
  const auto& box = parallel.boxes[ordinal];
  CHECK(box.annotation_id == 127U - ordinal);
  CHECK(box.source_category_id == ordinal % 2U + 1U);
  CHECK(box.original_area == 7.25);
  CHECK(box.x1 == -0.5F / 16.0F);
  CHECK((box.flags & kAnnotationMask) != 0U);
  CHECK((box.flags & kAnnotationIgnore) != 0U);
  CHECK(((box.flags & kAnnotationCrowd) != 0U) == (ordinal % 2U != 0U));
  CHECK(box.mask_rle_pairs == 0U);
  if (ordinal) CHECK(parallel.boxes[ordinal - 1U].source_ordinal < box.source_ordinal);
 }
 CHECK(parallel.boxes.back().original_area == 1.0);
 CHECK(parallel.boxes.back().x2 == 1.0F / 16.0F);
 CHECK(parallel.boxes.back().y2 == 1.0F / 8.0F);
 const auto cache = root.path() / "normalized.index";
 store_normalized_annotation_index(cache, parallel, {});
 const auto loaded = load_normalized_annotation_index(cache, options.source, options.split, digest, {});
 REQUIRE(loaded);
 REQUIRE(loaded->boxes.size() == parallel.boxes.size());
 CHECK(std::memcmp(loaded->boxes.data(), parallel.boxes.data(), parallel.boxes.size() * sizeof(NormalizedBox)) == 0);
 CHECK(decode_open_images_category(encode_open_images_category("/m/0h8my_4")) == "/m/0h8my_4");
 CHECK_THROWS(encode_open_images_category("/m/toolongidentifier"));
 CHECK_FALSE(valid_open_images_category(0U));
 CHECK_FALSE(valid_open_images_category(0x610062U));
}
TEST_CASE("benchmark semantic admission isolates malformed masks and numeric overflow", "[backend][data][benchmark][annotations]") {
 mmltk::testsupport::ScopedTempDir root("benchmark-admission");
 const auto path = root.path() / "annotations.json";
 nlohmann::json document{{"images", {{{"id", 0}, {"width", 16}, {"height", 8}, {"file_name", "patch0/0.jpg"}}}},
                         {"categories", {{{"id", 1}, {"name", "person"}}}},
                         {"annotations", nlohmann::json::array()}};
 const nlohmann::json base{{"image_id", 0}, {"category_id", 1}, {"bbox", {-0.5, 1.25, 13.0, 5.5}}};
 const auto append = [&](nlohmann::json record) { document["annotations"].push_back(std::move(record)); };
 auto record = base;
 record["segmentation"] = nlohmann::json::array();
 append(record);
 record.erase("bbox");
 record["segmentation"] = {{"size", {8, 16}}, {"counts", {0, 1, 127}}};
 append(record);
 record = base;
 record["segmentation"] = {{0, 0, 2, 0, 2, 2, 0, 2}};
 append(record);
 record = base;
 record["segmentation"] = {{"size", {4, 4}}, {"counts", {16}}};
 append(record);
 record["segmentation"] = {{"size", {8, 16}}, {"counts", "!"}};
 append(record);
 record["segmentation"] = {{"size", {8, 16}}, {"counts", {127}}};
 append(record);
 record["segmentation"] = {{0, 0, 1, 1}};
 append(record);
 record = base;
 record["bbox"] = {1e100, 0.0, 1e100, 1.0};
 append(record);
 record["bbox"] = {0.0, 0.0, 1e200, 1e200};
 append(record);
 constexpr unsigned rejected_capacity = 4096U;
 record = base;
 record["segmentation"] = {{"size", {8, 16}}, {"counts", {127}}};
 for (unsigned rejected = 0; rejected < rejected_capacity; ++rejected) append(record);
 write_text(path, document.dump());
 const std::array<NumericCategoryMapping, 1> mappings{{{1U, 0U, "person"}}};
 const auto digest = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(path));
 for (const auto source : {BenchmarkDatasetSource::kCoco2017, BenchmarkDatasetSource::kObjects365V2}) {
  AnnotationParseOptions options;
  options.source = source;
  options.split = "train";
  options.num_workers = 1;
  const auto sequential = parse_coco_style_annotations(path, digest, mappings, options);
  options.num_workers = 4;
  const auto parallel = parse_coco_style_annotations(path, digest, mappings, options);
  REQUIRE(sequential.boxes.size() == 3U);
  REQUIRE(parallel.boxes.size() == sequential.boxes.size());
  CHECK(parallel.rejected.raw_records == 9U + rejected_capacity);
  CHECK(parallel.rejected.malformed_records == 6U + rejected_capacity);
  CHECK(sequential.boxes.capacity() == sequential.boxes.size());
  CHECK(parallel.boxes.capacity() == parallel.boxes.size());
  CHECK(sequential.mask_rle_pairs.capacity() == sequential.mask_rle_pairs.size());
  CHECK(parallel.mask_rle_pairs.capacity() == parallel.mask_rle_pairs.size());
  CHECK(reject_json(parallel.rejected) == reject_json(sequential.rejected));
  CHECK(std::memcmp(parallel.boxes.data(), sequential.boxes.data(), parallel.boxes.size() * sizeof(NormalizedBox)) == 0);
  REQUIRE(parallel.mask_rle_pairs.size() == sequential.mask_rle_pairs.size());
  CHECK(std::memcmp(parallel.mask_rle_pairs.data(), sequential.mask_rle_pairs.data(), parallel.mask_rle_pairs.size() * sizeof(RLEPair)) == 0);
  CHECK((parallel.boxes[0].flags & kAnnotationMask) != 0U);
  CHECK(parallel.boxes[0].mask_rle_pairs == 0U);
  CHECK(parallel.boxes[1].original_area == 1.0);
  CHECK(parallel.boxes[2].original_area == 4.0);
 }
 const auto classes = root.path() / "classes.csv";
 const auto boxes = root.path() / "boxes.csv";
 write_text(classes, "/m/person,Person\n");
 write_text(boxes,
            "ImageID,Source,LabelName,Confidence,XMin,XMax,YMin,YMax\n"
            "0000000000000000,xclick,/m/person,1,-0.1,0.8,0.2,0.9\n"
            "0000000000000000,xclick,/m/person,1,1e100,2e100,0.2,0.9\n"
            "0000000000000000,xclick,/m/person,1,0,1e200,0,1e200\n");
 const std::array<StringCategoryMapping, 1> open_mappings{{{"/m/person", 0U, "Person"}}};
 AnnotationParseOptions options;
 options.source = BenchmarkDatasetSource::kOpenImagesV7;
 options.split = "train";
 options.num_workers = 1;
 const auto sequential = parse_open_images_annotations(boxes, classes, digest, open_mappings, options);
 options.num_workers = 4;
 const auto parallel = parse_open_images_annotations(boxes, classes, digest, open_mappings, options);
 REQUIRE(parallel.boxes.size() == 1U);
 CHECK(parallel.boxes[0].x1 == -0.1F);
 CHECK(parallel.rejected.raw_records == 3U);
 CHECK(parallel.rejected.malformed_records == 2U);
 CHECK(reject_json(parallel.rejected) == reject_json(sequential.rejected));
 CHECK(parallel.mask_rle_pairs.empty());
 CHECK(parallel.boxes.front().mask_rle_offset == 0U);
 CHECK(parallel.boxes.front().mask_rle_pairs == 0U);
 store_normalized_annotation_index(root.path() / "open-sequential.index", sequential, {});
 store_normalized_annotation_index(root.path() / "open-parallel.index", parallel, {});
 CHECK(mmltk::common::io::sha256_file(root.path() / "open-sequential.index") == mmltk::common::io::sha256_file(root.path() / "open-parallel.index"));
}
TEST_CASE("normalized benchmark caches require source category presence", "[backend][data][benchmark][annotations]") {
 mmltk::testsupport::ScopedTempDir root("normalized-provenance");
 const std::string digest(64U, '0');
 for (const auto source : {BenchmarkDatasetSource::kCoco2017, BenchmarkDatasetSource::kObjects365V2, BenchmarkDatasetSource::kOpenImagesV7}) {
  NormalizedAnnotationIndex index;
  index.source = source;
  index.split = "train";
  index.annotation_sha256 = digest;
  index.images.push_back(NormalizedImage{0U, 0U, 1U, 1U, 1U, 0U, 0U});
  NormalizedBox box;
  box.x2 = 1.0F;
  box.y2 = 1.0F;
  box.original_area = 1.0;
  box.flags = kAnnotationCategory;
  box.source_category_id = source == BenchmarkDatasetSource::kOpenImagesV7 ? encode_open_images_category("/m/person") : 0U;
  index.boxes.push_back(box);
  const auto path = root.path() / "normalized.index";
  store_normalized_annotation_index(path, index, {});
  REQUIRE(load_normalized_annotation_index(path, source, "train", digest, {}));
  box.flags &= ~kAnnotationCategory;
  box.source_category_id = 0U;
  index.boxes[0] = box;
  CHECK_THROWS(store_normalized_annotation_index(root.path() / "missing.index", index, {}));
  // Version 3 has a 256-byte header followed by this one image record.
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  REQUIRE(file.is_open());
  file.seekp(256U + sizeof(NormalizedImage));
  file.write(reinterpret_cast<const char*>(&box), sizeof(box));
  file.close();
  CHECK_FALSE(load_normalized_annotation_index(path, source, "train", digest, {}));
 }
}
TEST_CASE("benchmark supplied bbox admission precedes mask materialization", "[backend][data][benchmark][annotations]") {
 mmltk::testsupport::ScopedTempDir root("bbox-first-admission");
 const auto path = root.path() / "annotations.json";
 nlohmann::ordered_json document{
  {"images",
   {{{"id", 0}, {"width", 16}, {"height", 8}, {"file_name", "patch0/0.jpg"}}, {{"id", 1}, {"width", 4096}, {"height", 4096}, {"file_name", "patch0/1.jpg"}}}},
  {"categories", {{{"id", 1}, {"name", "person"}}}},
  {"annotations", nlohmann::ordered_json::array()}};
 // Invalid compressed RLE must not override a supplied degenerate bbox.
 document["annotations"].push_back({{"image_id", 0}, {"category_id", 1}, {"bbox", {0, 0, 0, 1}}, {"segmentation", {{"size", {8, 16}}, {"counts", "!"}}}});
 // This valid all-background mask would otherwise allocate 16 MiB.
 document["annotations"].push_back(
  {{"image_id", 1}, {"category_id", 1}, {"bbox", {0, 0, -1, 1}}, {"segmentation", {{"size", {4096, 4096}}, {"counts", {16777216U}}}}});
 // Normalized-float admission also runs before that mask can be allocated.
 document["annotations"].push_back(
  {{"image_id", 1}, {"category_id", 1}, {"bbox", {1e100, 0.0, 1e100, 1.0}}, {"segmentation", {{"size", {4096, 4096}}, {"counts", {16777216U}}}}});
 document["annotations"].push_back(
  {{"image_id", 0}, {"category_id", 1}, {"bbox", {-1, -1, 3, 3}}, {"segmentation", {{"size", {8, 16}}, {"counts", {0, 1, 127}}}}});
 document["annotations"].push_back({{"image_id", 0}, {"category_id", 1}, {"segmentation", {{"size", {8, 16}}, {"counts", {0, 1, 127}}}}});
 // Decode-time failures must remain irrelevant to a degenerate supplied box,
 // even when segmentation is encountered before the box and identities.
 const std::array<nlohmann::ordered_json, 3> invalid_segmentations{nlohmann::ordered_json(false), nlohmann::ordered_json{{"size", {8, 16}}},
                                                                   nlohmann::ordered_json::array({nlohmann::ordered_json::array({0, 0, 1})})};
 for (const auto& segmentation : invalid_segmentations) {
  for (const bool segmentation_first : {false, true}) {
   for (const int bbox_width : {0, 1}) {
    nlohmann::ordered_json record = nlohmann::ordered_json::object();
    if (segmentation_first) record["segmentation"] = segmentation;
    record["image_id"] = 0;
    record["category_id"] = 1;
    record["bbox"] = {0, 0, bbox_width, 1};
    if (!segmentation_first) record["segmentation"] = segmentation;
    document["annotations"].push_back(std::move(record));
   }
  }
 }
 document["annotations"].push_back({{"image_id", 0}, {"category_id", 1}, {"bbox", {0, 0, 1, 1}}, {"segmentation", nullptr}});
 document["annotations"].push_back({{"segmentation", nlohmann::ordered_json::array()}, {"image_id", 0}, {"category_id", 1}, {"bbox", {0, 0, 1, 1}}});
 write_text(path, document.dump());
 const auto digest = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(path));
 const std::array<NumericCategoryMapping, 1> mappings{{{1U, 0U, "person"}}};
 for (const auto source : {BenchmarkDatasetSource::kCoco2017, BenchmarkDatasetSource::kObjects365V2}) {
  AnnotationParseOptions options;
  options.source = source;
  options.split = "train";
  options.num_workers = 1;
  const auto sequential = parse_coco_style_annotations(path, digest, mappings, options);
  options.num_workers = 4;
  const auto parallel = parse_coco_style_annotations(path, digest, mappings, options);
  CHECK(parallel.rejected.raw_records == 19U);
  CHECK(parallel.rejected.degenerate_boxes == 8U);
  CHECK(parallel.rejected.malformed_records == 7U);
  CHECK(reject_json(parallel.rejected) == reject_json(sequential.rejected));
  REQUIRE(parallel.boxes.size() == 4U);
  REQUIRE(sequential.boxes.size() == parallel.boxes.size());
  CHECK(std::memcmp(parallel.boxes.data(), sequential.boxes.data(), parallel.boxes.size() * sizeof(NormalizedBox)) == 0);
  CHECK(parallel.boxes[0].x1 == -1.0F / 16.0F);
  CHECK(parallel.boxes[0].x2 == 2.0F / 16.0F);
  CHECK(parallel.boxes[1].x1 == 0.0F);
  CHECK(parallel.boxes[1].x2 == 1.0F / 16.0F);
  REQUIRE(parallel.mask_rle_pairs.size() == sequential.mask_rle_pairs.size());
  CHECK(std::memcmp(parallel.mask_rle_pairs.data(), sequential.mask_rle_pairs.data(), parallel.mask_rle_pairs.size() * sizeof(RLEPair)) == 0);
  CHECK((parallel.boxes[2].flags & kAnnotationMask) == 0U);
  CHECK(parallel.boxes[2].original_area == 1.0);
  CHECK((parallel.boxes[3].flags & kAnnotationMask) != 0U);
  CHECK(parallel.boxes[3].original_area == 0.0);
  CHECK(parallel.boxes[2].mask_rle_pairs == 0U);
  CHECK(parallel.boxes[3].mask_rle_pairs == 0U);
  for (const auto& box : std::span(parallel.boxes).first(2U)) {
   CHECK(box.original_area == 1.0);
   CHECK((box.flags & kAnnotationMask) != 0U);
   REQUIRE(box.mask_rle_pairs == 1U);
   CHECK(parallel.mask_rle_pairs[box.mask_rle_offset].start == 0U);
   CHECK(parallel.mask_rle_pairs[box.mask_rle_offset].length == 1U);
  }
 }
}
TEST_CASE("benchmark polygon raster bounds handle extreme and half-open coordinates", "[backend][data][benchmark][annotations]") {
 mmltk::testsupport::ScopedTempDir root("polygon-bounds");
 const auto path = root.path() / "annotations.json";
 nlohmann::json document{
  {"images", {{{"id", 0}, {"width", 4}, {"height", 4}}}}, {"categories", {{{"id", 1}, {"name", "person"}}}}, {"annotations", nlohmann::json::array()}};
 const std::array<std::vector<double>, 4> polygons{{
  {-1e308, -1e308, 1e308, -1e308, 1e308, 1e308, -1e308, 1e308},
  {1e308, 1e308, 1e308, 9e307, 9e307, 9e307},
  {-0.5, -0.5, 3.5, -0.5, 3.5, 3.5, -0.5, 3.5},
  {0.5, 0.5, 4.5, 0.5, 4.5, 4.5, 0.5, 4.5},
 }};
 for (const auto& polygon : polygons)
  document["annotations"].push_back({{"image_id", 0}, {"category_id", 1}, {"bbox", {0, 0, 4, 4}}, {"segmentation", {polygon}}});
 write_text(path, document.dump());
 const std::array<NumericCategoryMapping, 1> mappings{{{1U, 0U, "person"}}};
 const auto digest = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(path));
 for (const unsigned workers : {1U, 4U}) {
  AnnotationParseOptions options;
  options.source = BenchmarkDatasetSource::kCoco2017;
  options.split = "train";
  options.num_workers = workers;
  const auto index = parse_coco_style_annotations(path, digest, mappings, options);
  REQUIRE(index.boxes.size() == polygons.size());
  CHECK(index.rejected.malformed_records == 0U);
  for (std::size_t ordinal = 0; ordinal < polygons.size(); ++ordinal) {
   std::array<bool, 16> actual{};
   const auto& box = index.boxes[ordinal];
   for (std::size_t run = 0; run < box.mask_rle_pairs; ++run) {
    const auto pair = index.mask_rle_pairs[box.mask_rle_offset + run];
    REQUIRE(pair.start + pair.length <= actual.size());
    std::fill(actual.begin() + pair.start, actual.begin() + pair.start + pair.length, true);
   }
   for (unsigned pixel = 0; pixel < actual.size(); ++pixel)
    CHECK(actual[pixel] == (ordinal == 0U || ordinal == 3U || (ordinal == 2U && pixel % 4U < 3U && pixel / 4U < 3U)));
  }
 }
}
TEST_CASE("generic and benchmark writers share complete format headers", "[backend][data][benchmark][compiler]") {
 namespace fixtures = mmltk::backend::data::testsupport;
 namespace resize = mmltk::backend::imaging::resample;
 const mmltk::testsupport::ScopedTempDir root("shared-compiled-header");
 const fixtures::FixtureSpec fixture{.root_dir = root.path().string(), .width = 16, .height = 8, .num_images = 1};
 fixtures::create_synthetic_dataset(fixture);
 const auto image_root = root.path() / "cached";
 prepare_cached_image_directory(image_root);
 write_cached_image_atomically(cached_image_path(image_root, 1U), make_jpeg(128U, 64U, 32U), {});
 for (const auto mode : {resize::ImageResizeMode::Stretch, resize::ImageResizeMode::Letterbox}) {
  for (const std::string& annotation :
       {std::string{}, std::string{R"({"class":"person","bbox_xyxy":[1,1,4,4],"mask_rle_encoding":"row_major_start_length","mask_rle":""})"},
        std::string{R"({"class":"person","bbox_xyxy":[1,1,4,4],"mask_rle_encoding":"row_major_start_length","mask_rle":"17:3 33:3 49:3"})"}}) {
   write_text(fs::path(fixtures::dataset_dir(fixture)) / "train/000001.jsonl", annotation);
   auto config = fixtures::compiler_config(fixture);
   config.num_workers = 1;
   config.target_width = 16U;
   config.target_height = 16U;
   config.resize_mode = mode;
   DatasetCompiler::compile(DatasetCompiler::prepare(config, {"train"}), 0U);
   const auto generic = CompiledDataset::open(fixtures::compiled_bin_path(fixture));
   PreparedBenchmarkSplit split;
   split.class_names.assign(generic.class_names().begin(), generic.class_names().end());
   split.sources.push_back({image_root});
   split.images.push_back({1U, 16U, 8U, 0U, static_cast<std::uint16_t>(generic.labels().size()), 0U});
   split.labels.assign(generic.labels().begin(), generic.labels().end());
   split.rle_pairs.assign(generic.rle_pairs().begin(), generic.rle_pairs().end());
   const auto output = root.path() / "benchmark.bin";
   auto request = benchmark_write_request(split, output, 16U);
   request.overwrite = true;
   request.resize_mode = mode;
   write_benchmark_split(request);
   const auto benchmark = CompiledDataset::open(output);
   CHECK(std::memcmp(&generic.header(), &benchmark.header(), sizeof(FileHeader)) == 0);
   const auto sections = validate_compiled_file_sections(benchmark.header(), fs::file_size(output));
   CHECK(sections.label_count == generic.labels().size());
   CHECK(sections.rle_region_bytes == generic.rle_pairs().size_bytes());
  }
 }
}
TEST_CASE("segmented downloads retain durable ranges through failure cancellation and unobserved resume", "[backend][data][benchmark][download]") {
 mmltk::testsupport::ScopedTempDir root("segmented-download");
 constexpr std::size_t bytes = 512U * 1024U * 1024U;
 HttpServer server(bytes);
 DownloadRequest request{"segmented", server.url("segmented"), root.path() / "artifact.bin", root.path() / "artifact.lock", bytes, {}, 1U};
 server.TruncateNextTransfer();
 std::mutex progress_mutex;
 std::vector<DownloadProgress> updates;
 REQUIRE_THROWS(download_artifacts({request}, 2U, {}, [&](const DownloadProgress& progress) {
  const std::scoped_lock lock(progress_mutex);
  updates.push_back(progress);
 }));
 REQUIRE_FALSE(updates.empty());
 CHECK(std::ranges::all_of(updates, [](const DownloadProgress& progress) { return progress.total_bytes == bytes && progress.completed_bytes <= bytes; }));
 CHECK(std::ranges::any_of(updates, [](const DownloadProgress& progress) { return progress.completed_bytes > 0U; }));
 REQUIRE_FALSE(fs::exists(request.destination));
 REQUIRE(fs::file_size(request.destination.string() + ".part") == bytes);
 const auto metadata_path = request.destination.string() + ".part.json";
 const auto partial = read_json_file(metadata_path);
 REQUIRE(partial.at("mode") == "segmented");
 REQUIRE(partial.at("segments").size() == 2U);
 std::uint64_t completed = 0U;
 for (const auto& segment : partial.at("segments")) completed += segment.at("completed").get<std::uint64_t>();
 REQUIRE(completed > 0U);
 REQUIRE(completed < bytes);
 const auto first_ranges = server.ranges();
 REQUIRE(std::ranges::find(first_ranges, std::pair<std::size_t, std::size_t>{0U, bytes / 2U - 1U}) != first_ranges.end());
 REQUIRE(std::ranges::find(first_ranges, std::pair<std::size_t, std::size_t>{bytes / 2U, bytes - 1U}) != first_ranges.end());
 // The fixture gates physical response bytes, independently of progress callbacks.
 std::atomic<bool> cancel{false};
 server.GateNextTransfer();
 auto download = std::async(std::launch::async, [&] {
  try {
   (void)download_artifacts({request}, 2U, mmltk::common::concurrency::CancellationObservation::Atomic(cancel));
   return false;
  } catch (const std::exception&) { return true; }
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancel.store(true, std::memory_order_release);
  server.ReleasePartial();
 });
 REQUIRE(server.WaitPartial());
 cancel.store(true, std::memory_order_release);
 REQUIRE(mmltk::testsupport::await_test_future(download, "unobserved segmented cancellation", 5s));
 REQUIRE_FALSE(fs::exists(request.destination));
 REQUIRE(fs::file_size(request.destination.string() + ".part") == bytes);
 REQUIRE(read_json_file(metadata_path).at("mode") == "segmented");
 server.ReleasePartial();
 request.maximum_attempts = 3U;
 server.TruncateNextTransfer();
 std::atomic<bool> traced_retry{false}, traced_complete{false}, traced_bytes{false}, traced_second_attempt{false}, invalid_byte_facts{false};
 const auto resumed = download_artifacts({request}, 2U, {}, {}, [&](const std::string_view event, const nlohmann::json& fields) {
  if (event == "benchmark.download.progress") {
   traced_bytes.store(true, std::memory_order_relaxed);
   if (fields.at("attempt") == 2U) { traced_second_attempt.store(true, std::memory_order_relaxed); }
   if (fields.at("retained_bytes").get<std::uint64_t>() == 0U || fields.at("durable_bytes") > fields.at("completed_bytes")) {
    invalid_byte_facts.store(true, std::memory_order_relaxed);
   }
  }
  if (event == "benchmark.download.segment_retry") traced_retry.store(true, std::memory_order_relaxed);
  if (event == "benchmark.download.segmented_complete") traced_complete.store(true, std::memory_order_relaxed);
 });
 CHECK(traced_retry.load(std::memory_order_relaxed));
 CHECK(traced_complete.load(std::memory_order_relaxed));
 CHECK(traced_bytes.load(std::memory_order_relaxed));
 CHECK(traced_second_attempt.load(std::memory_order_relaxed));
 CHECK_FALSE(invalid_byte_facts.load(std::memory_order_relaxed));
 REQUIRE(resumed.size() == 1U);
 CHECK(resumed.front().resumed);
 CHECK(resumed.front().attempts == 2U);
 REQUIRE(fs::file_size(request.destination) == bytes);
 CHECK_FALSE(fs::exists(metadata_path));
 CHECK_FALSE(fs::exists(request.destination.string() + ".part"));
 CHECK(has_generated_payload(request.destination, bytes));
 server.Check();
}
TEST_CASE("benchmark shrinking images preserve the RGB8 intermediate projection exactly", "[backend][data][benchmark][writer][perceptual]") {
 using namespace mmltk::backend::imaging::resample;
 mmltk::testsupport::ScopedTempDir root("shrinking-image");
 const auto image_root = root.path() / "images";
 prepare_cached_image_directory(image_root);
 constexpr std::uint32_t width = 65U, height = 49U, target = 17U;
 std::vector<std::uint8_t> rgb(width * height * 3U);
 for (std::size_t i = 0U; i < rgb.size(); ++i) rgb[i] = static_cast<std::uint8_t>((i * 17U + i / 13U) & 255U);
 std::vector<std::uint8_t> encoded;
 SECTION("JPEG") { REQUIRE(stbi_write_jpg_to_func(append_bytes, &encoded, width, height, 3, rgb.data(), 95) != 0); }
 SECTION("PNG") { REQUIRE(stbi_write_png_to_func(append_bytes, &encoded, width, height, 3, rgb.data(), width * 3U) != 0); }
 REQUIRE(has_complete_image_markers(encoded));
 CHECK_FALSE(has_complete_image_markers(std::span(encoded).first(encoded.size() - 1U)));
 write_cached_image_atomically(cached_image_path(image_root, 1U), encoded, {});
 BenchmarkImageDecoder decoder;
 std::vector<std::uint8_t> decoded, cmyk;
 const auto header = decoder.read_header(encoded, width, height);
 decoder.decode_rgb(encoded, header, &decoded, &cmyk);
 if (header.encoding == BenchmarkImageEncoding::Png) CHECK(decoded == rgb);
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{image_root}};
 split.images = {{1U, width, height, 0U, 0U, 0U}};
 for (const auto mode : {ImageResizeMode::Stretch, ImageResizeMode::Letterbox}) {
  const auto output = root.path() / (mode == ImageResizeMode::Stretch ? "stretch.bin" : "letterbox.bin");
  auto request = benchmark_write_request(split, output, target);
  request.perceptual_downscale = true;
  request.resize_mode = mode;
  write_benchmark_split(request);
  const auto compiled = CompiledDataset::open(output);
  const auto expected = mmltk::backend::data::testsupport::expected_resized_rgb(decoded, width, height, target, target, mode, true);
  CHECK(std::memcmp(compiled.image_pixels(0), expected.data(), expected.size() * sizeof(float)) == 0);
  CHECK(compiled.header().resize_mode == mode);
 }
}
TEST_CASE("transfer observers are independent of trace-only pixel observers", "[backend][data][benchmark][progress]") {
 const BenchmarkTraceSink quiet;
 ProgressReporter unobserved({}, quiet);
 CHECK_FALSE(unobserved.transfer_observer_enabled());
 CHECK_FALSE(unobserved.pixel_observer_enabled());
 std::size_t traces = 0U;
 const BenchmarkTraceSink trace = [&](std::string_view, const nlohmann::json&) { ++traces; };
 ProgressReporter traced({}, trace);
 CHECK_FALSE(traced.transfer_observer_enabled());
 CHECK(traced.pixel_observer_enabled());
 traced.source_transfer(DownloadProgress{"coco-fixture", 1U, 2U, 2U}, 1U, 2U);
 CHECK(traces == 0U);
 ArtifactProgressTotals unobserved_totals;
 unobserved_totals.update(DownloadProgress{.artifact_id = "unobserved", .completed_bytes = 1U, .source = static_cast<BenchmarkDatasetSource>(255U)}, traced);
 CHECK(traces == 0U);
 traced.pixel_attempt(0U, 1U, "train", 1U);
 traced.pixel_completed();
 CHECK(traces == 2U);
 std::vector<BenchmarkCompileProgress> updates;
 ProgressReporter observed([&](const BenchmarkCompileProgress& update) { updates.push_back(update); }, quiet);
 CHECK(observed.transfer_observer_enabled());
 CHECK(observed.pixel_observer_enabled());
 observed.phase(DatasetCompilePhase::Downloading);
 observed.source_transfer(DownloadProgress{"coco-fixture", 5U, 11U, 3U, true}, 5U, 11U);
 REQUIRE_FALSE(updates.empty());
 CHECK(updates.back().sources[0].completed_bytes == 5U);
 CHECK(updates.back().sources[0].total_bytes == 11U);
 CHECK(updates.back().sources[0].retry_count == 2U);
 CHECK(updates.back().sources[0].resumed);
}
TEST_CASE("parser workers reset segmentation scratch across masks rejections and dimension changes", "[backend][data][benchmark][annotations]") {
 mmltk::testsupport::ScopedTempDir root("segmentation-scratch");
 const auto path = root.path() / "annotations.json";
 nlohmann::json document{{"images",
                          {{{"id", 1}, {"width", 16}, {"height", 8}, {"file_name", "patch0/1.jpg"}},
                           {{"id", 2}, {"width", 4}, {"height", 4}, {"file_name", "patch0/2.jpg"}},
                           {{"id", 3}, {"width", 4}, {"height", 4}, {"file_name", "patch0/3.jpg"}}}},
                         {"categories", {{{"id", 1}, {"name", "person"}}}},
                         {"annotations", nlohmann::json::array()}};
 for (unsigned cycle = 0U; cycle < 256U; ++cycle)
  for (unsigned kind = 0U; kind < 7U; ++kind) {
   const bool small = (cycle + kind) % 2U != 0U;
   nlohmann::json record{{"image_id", small ? 2 : 1}, {"category_id", 1}, {"id", cycle * 7U + kind}, {"bbox", {0, 0, 2, 2}}};
   if (kind == 0U) record["segmentation"] = {{0, 0, 2, 0, 2, 2, 0, 2}};
   if (kind == 1U) record["segmentation"] = {{"size", {small ? 4 : 8, small ? 4 : 16}}, {"counts", {0, 1, small ? 15 : 127}}};
   if (kind == 2U) record["segmentation"] = {{"size", {small ? 4 : 8, small ? 4 : 16}}, {"counts", small ? "01?" : "01o3"}};
   if (kind == 3U) record["segmentation"] = {{"size", {small ? 4 : 8, small ? 4 : 16}}, {"counts", {0, 2, 1}}};
   if (kind == 4U) record["segmentation"] = nlohmann::json::array();
   if (kind == 5U) {
    record["bbox"] = {0, 0, 0, 0};
    record["segmentation"] = {{0, 0, 4, 0, 4, 4, 0, 4}};
   }
   document["annotations"].push_back(std::move(record));
  }
 write_text(path, document.dump());
 const auto digest = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(path));
 const std::array<NumericCategoryMapping, 1> mappings{{{1U, 0U, "person"}}};
 for (const auto source : {BenchmarkDatasetSource::kCoco2017, BenchmarkDatasetSource::kObjects365V2})
  for (const bool keep_empty : {false, true}) {
   AnnotationParseOptions options;
   options.source = source;
   options.split = "train";
   options.num_workers = 1;
   options.keep_images_without_mapped_boxes = keep_empty;
   const auto sequential = parse_coco_style_annotations(path, digest, mappings, options);
   options.num_workers = 4;
   const auto parallel = parse_coco_style_annotations(path, digest, mappings, options);
   CHECK(parallel.images.size() == (keep_empty ? 3U : 2U));
   REQUIRE(parallel.boxes.size() == 256U * 5U);
   CHECK(parallel.rejected.malformed_records == 256U);
   CHECK(parallel.rejected.degenerate_boxes == 256U);
   store_normalized_annotation_index(root.path() / "sequential.index", sequential, {});
   store_normalized_annotation_index(root.path() / "parallel.index", parallel, {});
   CHECK(mmltk::common::io::sha256_file(root.path() / "sequential.index") == mmltk::common::io::sha256_file(root.path() / "parallel.index"));
   for (const auto& box : parallel.boxes) {
    const auto kind = box.annotation_id % 7U;
    CHECK(box.original_area == (kind == 0U || kind == 6U ? 4.0 : kind == 4U ? 0.0 : 1.0));
    CHECK(((box.flags & kAnnotationMask) != 0U) == (kind != 6U));
    if (kind == 4U || kind == 6U) CHECK(box.mask_rle_pairs == 0U);
   }
  }
}
TEST_CASE("benchmark trace failures are lazy and delivered once", "[backend][data][benchmark][trace]") {
 std::size_t deliveries = 0U;
 const BenchmarkTraceSink null_sink = [&](std::string_view event, const nlohmann::json& fields) {
  ++deliveries;
  CHECK(event == "benchmark.test.failure");
  CHECK(fields.is_null());
  throw std::runtime_error("sink failure after receipt");
 };
 CHECK_NOTHROW(trace_benchmark_event(null_sink, "benchmark.test.failure", []() -> nlohmann::json { throw std::runtime_error("builder failure"); }));
 CHECK(deliveries == 1U);
 CHECK_NOTHROW(trace_benchmark_event(throwing_trace, "benchmark.test.success", [] { return nlohmann::json{{"value", 1U}}; }));
 const auto serialized = make_trace_sink([&](std::string_view event, std::string_view fields) {
  ++deliveries;
  CHECK(event == "benchmark.test.failure");
  CHECK(fields.empty());
  throw std::runtime_error("callback failure after receipt");
 });
 CHECK_NOTHROW(trace_benchmark_event(serialized, "benchmark.test.failure", []() -> nlohmann::json { throw std::runtime_error("builder failure"); }));
 CHECK(deliveries == 2U);
 CHECK_NOTHROW(trace_benchmark_event(serialized, "benchmark.test.failure", [] { return nlohmann::json{{"native_path", "\xff"}}; }));
 CHECK(deliveries == 3U);
 std::size_t built = 0U;
 const auto disabled = make_trace_sink({});
 CHECK_FALSE(disabled);
 trace_benchmark_event(disabled, "benchmark.test.disabled", [&] {
  ++built;
  return nlohmann::json::object();
 });
 CHECK(built == 0U);
 std::vector<std::string> events;
 const auto success = make_trace_sink([&](std::string_view event, std::string_view fields) {
  events.emplace_back(event);
  CHECK(nlohmann::json::parse(fields).at("value") == events.size());
 });
 trace_benchmark_event(success, "benchmark.test.first", [] { return nlohmann::json{{"value", 1U}}; });
 trace_benchmark_event(success, "benchmark.test.second", [] { return nlohmann::json{{"value", 2U}}; });
 CHECK((events == std::vector<std::string>{"benchmark.test.first", "benchmark.test.second"}));
}
TEST_CASE("benchmark storage releases reservations after diagnostic rejection", "[backend][data][benchmark][trace]") {
 mmltk::testsupport::ScopedTempDir root{"benchmark-storage-trace"};
 std::vector<std::uint64_t> reservations;
 const BenchmarkTraceSink sink = [&](std::string_view event, const nlohmann::json& fields) {
  if (event == "benchmark.storage.reserved") reservations.push_back(fields.at("reserved_bytes").get<std::uint64_t>());
  throw std::runtime_error("storage diagnostic failure");
 };
 CHECK_NOTHROW(require_storage(root.path(), 1U, "fixture", sink));
 StorageReservationPool pool{root.path(), sink};
 for (unsigned attempt = 0; attempt < 2U; ++attempt) { const auto reservation = pool.reserve(1U, "fixture"); }
 CHECK((reservations == std::vector<std::uint64_t>{1U, 1U}));
 // Admission inspects existing filesystem capacity; no disk filling occurs.
 constexpr auto impossible = std::numeric_limits<std::uint64_t>::max();
 CHECK_THROWS_AS(require_storage(root.path(), impossible, "fixture", {}), std::runtime_error);
 CHECK_THROWS_AS(require_storage(root.path(), impossible, "fixture", sink), std::runtime_error);
 CHECK_THROWS_AS(pool.reserve(impossible, "fixture"), std::runtime_error);
 CHECK(reservations.size() == 2U);
}
TEST_CASE("benchmark sink setup failure reports once without creating a sink", "[backend][data][benchmark][trace]") {
 struct Callback {
  bool* reject_copy;
  std::size_t* calls;
  Callback(bool& reject, std::size_t& count) : reject_copy(&reject), calls(&count) {}
  Callback(const Callback& other) : reject_copy(other.reject_copy), calls(other.calls) {
   if (*reject_copy) throw std::runtime_error("diagnostic setup failure");
  }
  void operator()(std::string_view event, std::string_view fields) const {
   ++*calls;
   CHECK(event.empty());
   CHECK(fields.empty());
   throw std::runtime_error("failure callback throws");
  }
 };
 bool reject_copy = false;
 std::size_t calls = 0U;
 const BenchmarkTraceCallback callback{Callback{reject_copy, calls}};
 reject_copy = true;
 const auto sink = make_trace_sink(callback);
 CHECK_FALSE(sink);
 CHECK(calls == 1U);
}
TEST_CASE("archive write progress orders late batches within one attempt", "[backend][data][benchmark][progress]") {
 std::vector<std::uint64_t> observed;
 CachedImageWriteProgress progress(
  [&](const auto completed, const auto total) {
   CHECK(total == 400U);
   observed.push_back(completed);
  },
  16U, 384U);
 std::promise<void> newer_published;
 auto newer = newer_published.get_future();
 auto old_batch = std::async(std::launch::async, [&] {
  newer.wait();
  progress.completed(128U);
 });
 progress.completed(256U);
 newer_published.set_value();
 mmltk::testsupport::await_test_future(old_batch, "older write batch publication", 5s);
 progress.completed(384U);
 REQUIRE(observed == std::vector<std::uint64_t>{272U, 400U});
}
TEST_CASE("source count additions serialize publication and permit retry withdrawal", "[backend][data][benchmark][progress]") {
 const BenchmarkTraceSink quiet;
 BenchmarkCompileProgress latest;
 std::vector<std::uint64_t> counts;
 mmltk::testsupport::TestGate first("first source publication");
 ProgressReporter progress(
  [&](const auto& update) {
   latest = update;
   const auto count = update.sources[1].completed_images;
   counts.push_back(count);
   if (count == 128U) { first.receipt().ArriveAndWait(); }
  },
  quiet);
 const auto source = BenchmarkDatasetSource::kObjects365V2;
 progress.phase(DatasetCompilePhase::Extracting);
 auto first_add = std::async(std::launch::async, [&] { progress.add_source_images(source, 128U, 512U); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { first.Release(); });
 REQUIRE(first.WaitEntered(3s));
 auto second_add = std::async(std::launch::async, [&] { progress.add_source_images(source, 256U, 512U); });
 first.Release();
 mmltk::testsupport::await_test_future(first_add, "first source addition", 5s);
 mmltk::testsupport::await_test_future(second_add, "second source addition", 5s);
 CHECK(latest.sources[1].completed_images == 384U);
 CHECK(std::ranges::is_sorted(counts));
 progress.rollback_source_images(source, 128U, 512U);
 CHECK(latest.sources[1].completed_images == 256U);
 progress.add_source_images(source, 256U, 512U);
 CHECK(latest.sources[1].completed_images == 512U);
 CHECK_THROWS_AS(progress.rollback_source_images(source, 513U, 512U), std::underflow_error);
 progress.source_images(source, std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max());
 CHECK_THROWS_AS(progress.add_source_images(source, 1U, std::numeric_limits<std::uint64_t>::max()), std::overflow_error);
}
TEST_CASE("archive image reuse reports initial and resolved counts without replacing valid JPEGs", "[backend][data][benchmark][images]") {
 for (const std::size_t workers : {0U, 3U}) {
  mmltk::testsupport::ScopedTempDir root("progress-reuse");
  const auto archive = root.path() / "images.tar";
  const auto images = root.path() / "images";
  const auto jpeg = make_jpeg(10U, 20U, 30U);
  write_single_jpeg_tar(archive, 2U, jpeg);
  prepare_cached_image_directory(images);
  write_cached_image_atomically(cached_image_path(images, 1U), jpeg, {});
  const RetainedArtifact retained{cached_image_path(images, 1U)};
  const std::vector<std::uint64_t> ids{1U, 2U};
  std::vector<std::uint64_t> counts;
  ArchiveExtractionRequest request{.archive_path = archive,
                                   .source_identity = "fixture:reuse",
                                   .output_root = images,
                                   .source = "objects365",
                                   .shard = "patch-0",
                                   .selected_image_ids = ids,
                                   .image_id_parser = [](const std::string_view name) -> std::optional<std::uint64_t> {
                                    return name.ends_with("/2.jpg") ? std::optional<std::uint64_t>{2U} : std::nullopt;
                                   },
                                   .progress =
                                    [&](const auto completed, const auto total) {
                                     CHECK(total == 2U);
                                     counts.push_back(completed);
                                    },
                                   .validator =
                                    [](const auto, const auto encoded) {
                                     if (!has_complete_image_markers(encoded)) { throw std::runtime_error("invalid cached JPEG"); }
                                    },
                                   .decompression_workers = 0U,
                                   .cache_write_workers = workers};
  auto result = extract_selected_archive_images(request);
  REQUIRE_FALSE(counts.empty());
  CHECK(counts.front() == 1U);
  CHECK(counts.back() == 2U);
  CHECK(std::ranges::is_sorted(counts));
  CHECK(result.image_count == 2U);
  CHECK(result.selection_sha256 == cached_image_selection_digest(ids));
  retained.Check();
  const RetainedArtifact second{cached_image_path(images, 2U)};
  fs::remove(images / ".complete.json");
  counts.clear();
  result = extract_selected_archive_images(request);
  CHECK(counts.front() == 2U);
  CHECK(counts.back() == 2U);
  retained.Check();
  second.Check();
  fs::remove(archive);
  counts.clear();
  result = extract_selected_archive_images(request);
  CHECK(result.cache_hit);
  CHECK(counts == std::vector<std::uint64_t>{2U});
  retained.Check();
  second.Check();
  fs::remove(images / ".complete.json");
  write_text(cached_image_path(images, 2U), "invalid");
  write_single_jpeg_tar(archive, 2U, jpeg);
  counts.clear();
  result = extract_selected_archive_images(request);
  CHECK(counts.front() == 1U);
  CHECK(counts.back() == 2U);
  CHECK(mmltk::common::io::sha256_file(cached_image_path(images, 2U)) == mmltk::common::io::sha256_bytes(jpeg));
  retained.Check();
 }
}
TEST_CASE("Open Images local JPEG and complete group reuse preserve dimensions and identities", "[backend][data][benchmark][images]") {
 mmltk::testsupport::ScopedTempDir root("open-images-reuse");
 const auto cache = BenchmarkCacheLayout::create(root.path());
 const auto images = cache.source_images("open-images") / "train";
 prepare_cached_image_directory(images);
 const auto jpeg = make_jpeg(10U, 20U, 30U);
 write_cached_image_atomically(cached_image_path(images, 1U), jpeg, {});
 write_cached_image_atomically(cached_image_path(images, 2U), jpeg, {});
 const RetainedArtifact first{cached_image_path(images, 1U)};
 const RetainedArtifact second{cached_image_path(images, 2U)};
 NormalizedAnnotationIndex index;
 index.source = BenchmarkDatasetSource::kOpenImagesV7;
 index.images.resize(2U);
 index.images[0].source_image_id = 1U;
 index.images[1].source_image_id = 2U;
 std::vector<QuarantinedImage> quarantined;
 const BenchmarkTraceSink quiet;
 BenchmarkCompileProgress latest;
 ProgressReporter progress([&](const auto& update) { latest = update; }, quiet);
 progress.phase(DatasetCompilePhase::Extracting);
 auto acquired = acquire_open_images(cache, index, &quarantined, {}, &progress, 1, 0U, quiet);
 REQUIRE(acquired.available_image_ids == std::vector<std::uint64_t>{1U, 2U});
 CHECK(latest.sources[2].completed_images == 2U);
 const auto width = index.images[0].width;
 const auto height = index.images[0].height;
 CHECK(width > 0U);
 CHECK(height > 0U);
 for (auto& image : index.images) { image.width = image.height = 0U; }
 acquired = acquire_open_images(cache, index, &quarantined, {}, &progress, 1, 0U, quiet);
 CHECK(acquired.directory.cache_hit);
 CHECK(index.images[0].width == width);
 CHECK(index.images[0].height == height);
 const auto proof = images / ".groups" / "group-000000.complete.json";
 auto manifest = read_json_file(proof);
 manifest["identity"] = "stale";
 write_json_atomically(proof, manifest, {});
 acquired = acquire_open_images(cache, index, &quarantined, {}, &progress, 1, 0U, quiet);
 CHECK_FALSE(acquired.directory.cache_hit);
 manifest = read_json_file(proof);
 manifest["selection_sha256"] = "stale";
 write_json_atomically(proof, manifest, {});
 acquired = acquire_open_images(cache, index, &quarantined, {}, &progress, 1, 0U, quiet);
 CHECK_FALSE(acquired.directory.cache_hit);
 manifest = read_json_file(proof);
 index.images.push_back(NormalizedImage{.source_image_id = 3U});
 const std::vector<std::uint64_t> requested{1U, 2U, 3U};
 manifest["requested_image_count"] = 3U;
 manifest["requested_selection_sha256"] = cached_image_selection_digest(requested);
 manifest["quarantined"] = {{{"image_id", 3U}, {"reason", "fixture unavailable image"}}};
 write_json_atomically(proof, manifest, {});
 acquired = acquire_open_images(cache, index, &quarantined, {}, &progress, 1, 0U, quiet);
 CHECK(acquired.directory.cache_hit);
 CHECK(acquired.available_image_ids == std::vector<std::uint64_t>{1U, 2U});
 REQUIRE(quarantined.size() == 1U);
 CHECK(quarantined.front().image_id == 3U);
 CHECK(quarantined.front().reason == "fixture unavailable image");
 CHECK(latest.sources[2].completed_images == 3U);
 first.Check();
 second.Check();
}
TEST_CASE("activity diagnostics use the isolated serialized adapter independently of GUI observation", "[backend][data][benchmark][trace]") {
 std::vector<std::string> activities;
 const auto trace = make_trace_sink([&](const std::string_view event, const std::string_view fields) {
  REQUIRE_FALSE(fields.empty());
  CHECK(fields.size() < 13U * 1024U);
  if (event == "benchmark.progress.activity") { activities.push_back(nlohmann::json::parse(fields).at("activity").get<std::string>()); }
 });
 ProgressReporter reporter({}, trace);
 CHECK_FALSE(reporter.transfer_observer_enabled());
 reporter.phase(DatasetCompilePhase::Extracting);
 reporter.source_activity(BenchmarkDatasetSource::kObjects365V2, "Checking patch-17 cache");
 reporter.source_activity(BenchmarkDatasetSource::kObjects365V2, "Checking patch-17 cache");
 reporter.activity("Preparing labels");
 reporter.activity("Preparing labels");
 REQUIRE(activities == std::vector<std::string>{"Acquiring and extracting source images", "Checking patch-17 cache", "Preparing labels"});
 std::size_t failed_deliveries = 0U;
 const auto throwing = make_trace_sink([&](const auto, const auto) {
  ++failed_deliveries;
  throw std::runtime_error("diagnostic fixture");
 });
 ProgressReporter faulted({}, throwing);
 CHECK_NOTHROW(faulted.source_activity(BenchmarkDatasetSource::kCoco2017, "Checking local cache"));
 CHECK(failed_deliveries == 1U);
}
TEST_CASE("batched image writes settle before cancellation and preserve successful cache writes", "[backend][data][benchmark][images][cancel]") {
 for (const bool cancel_after_batch : {false, true}) {
  mmltk::testsupport::ScopedTempDir root("batched-cache-writes");
  const auto archive = root.path() / "images.tar";
  const auto jpeg = make_jpeg(5U, 10U, 20U);
  std::vector<std::uint64_t> ids;
  {
   std::ofstream output(archive, std::ios::binary);
   for (std::uint64_t id = 1U; id <= 384U; ++id) {
    ids.push_back(id);
    write_jpeg_tar_entry(output, id, jpeg);
   }
   const std::array<char, 1024U> terminator{};
   output.write(terminator.data(), static_cast<std::streamsize>(terminator.size()));
   REQUIRE(output.good());
  }
  std::atomic<bool> cancelled{false};
  std::vector<std::uint64_t> counts;
  ArchiveExtractionRequest request{.archive_path = archive,
                                   .source_identity = "fixture:batch",
                                   .output_root = root.path() / "images",
                                   .source = "objects365",
                                   .shard = "patch-0",
                                   .selected_image_ids = ids,
                                   .image_id_parser = [](const std::string_view name) -> std::optional<std::uint64_t> {
                                    const auto digits = name.substr(name.find_last_of('/') + 1U);
                                    std::uint64_t id = 0U;
                                    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size() - 4U, id);
                                    return parsed.ec == std::errc{} ? std::optional<std::uint64_t>{id} : std::nullopt;
                                   },
                                   .cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled),
                                   .progress =
                                    [&](const auto completed, const auto) {
                                     counts.push_back(completed);
                                     if (cancel_after_batch && completed >= 128U) { cancelled.store(true); }
                                    },
                                   .decompression_workers = 0U,
                                   .cache_write_workers = 3U};
  if (cancel_after_batch) {
   CHECK_THROWS(extract_selected_archive_images(request));
   CHECK_FALSE(fs::exists(request.output_root / ".complete.json"));
  } else {
   CHECK(extract_selected_archive_images(request).image_count == ids.size());
   REQUIRE_FALSE(counts.empty());
   CHECK(counts.back() == ids.size());
  }
  CHECK(std::ranges::is_sorted(counts));
  std::uint64_t retained = 0U;
  for (const auto id : ids) {
   const auto path = cached_image_path(request.output_root, id);
   if (fs::exists(path)) {
    ++retained;
    CHECK(mmltk::common::io::sha256_file(path) == mmltk::common::io::sha256_bytes(jpeg));
   }
  }
  CHECK(retained >= 128U);
  CHECK(retained <= ids.size());
 }
}
TEST_CASE("fresh segmented retries expose newly durable ranges without counting sparse allocation", "[backend][data][benchmark][download]") {
 mmltk::testsupport::ScopedTempDir root("fresh-segmented-retry");
 constexpr std::size_t bytes = 512U * 1024U * 1024U;
 HttpServer server(bytes);
 DownloadRequest request{"objects365-fresh-retry", server.url("retry"), root.path() / "archive.bin", root.path() / "archive.lock", bytes, {}, 3U};
 request.source = BenchmarkDatasetSource::kObjects365V2;
 server.TruncateNextTransfer();
 std::vector<DownloadProgress> updates;
 const auto downloaded = download_artifacts({request}, 2U, {}, [&](const DownloadProgress& update) { updates.push_back(update); });
 CHECK(std::ranges::all_of(updates, [](const auto& update) { return update.source == BenchmarkDatasetSource::kObjects365V2; }));
 REQUIRE(downloaded.size() == 1U);
 REQUIRE_FALSE(updates.empty());
 CHECK(updates.front().completed_bytes == 0U);
 CHECK(updates.front().retained_bytes == 0U);
 CHECK_FALSE(updates.front().resumed);
 bool saw_retry = false;
 std::uint64_t previous = 0U;
 for (const auto& update : updates) {
  CHECK(update.completed_bytes >= previous);
  CHECK(update.completed_bytes <= bytes);
  CHECK(update.total_bytes == bytes);
  CHECK(update.retained_bytes <= update.completed_bytes);
  CHECK_FALSE(update.redownload);
  if (update.attempt > 1U) {
   saw_retry = true;
   CHECK(update.resumed);
   CHECK(update.retained_bytes >= HttpServer::partial_bytes);
  }
  previous = update.completed_bytes;
 }
 CHECK(saw_retry);
 CHECK(updates.back().completed_bytes == bytes);
 CHECK(has_generated_payload(request.destination, bytes));
 server.Check();
}
TEST_CASE("discarded segmented state retains the typed re-download context during ordinary fallback", "[backend][data][benchmark][download]") {
 mmltk::testsupport::ScopedTempDir root("segmented-redownload");
 constexpr std::size_t bytes = 512U * 1024U * 1024U;
 HttpServer server(bytes);
 DownloadRequest request{"objects365-fallback", server.url("fallback"), root.path() / "archive.bin", root.path() / "archive.lock", bytes, {}, 1U};
 server.RestartNextRangedTransfer();
 bool saw_redownload = false;
 bool saw_completion = false;
 bool saw_discarded_state = false;
 const auto trace = make_trace_sink([&](const std::string_view event, const std::string_view fields) {
  if (event == "benchmark.download.segmented_fallback") { saw_discarded_state = nlohmann::json::parse(fields).at("redownload").get<bool>(); }
  if (event == "benchmark.download.complete") { saw_completion = nlohmann::json::parse(fields).at("redownload").get<bool>(); }
 });
 const auto downloaded = download_artifacts(
  {request}, 2U, {},
  [&](const DownloadProgress& update) {
   if (update.redownload) {
    saw_redownload = true;
    CHECK(update.retained_bytes == 0U);
    CHECK_FALSE(update.resumed);
   }
  },
  trace);
 REQUIRE(downloaded.size() == 1U);
 CHECK(saw_discarded_state);
 CHECK(saw_redownload);
 CHECK(saw_completion);
 CHECK(has_generated_payload(request.destination, bytes));
 server.Check();
}
TEST_CASE("artifact acquisition totals replace contributions without inventing unknown totals", "[backend][data][benchmark][progress]") {
 BenchmarkCompileProgress latest;
 const BenchmarkTraceSink trace;
 ProgressReporter reporter([&](const auto& update) { latest = update; }, trace);
 ArtifactProgressTotals totals;
 reporter.phase(DatasetCompilePhase::Downloading);
 const auto observe = [&](const BenchmarkDatasetSource source, const char* artifact, const std::uint64_t completed, const std::uint64_t total,
                          const std::uint64_t expected_completed, const std::uint64_t expected_total) {
  totals.update(DownloadProgress{.artifact_id = artifact, .completed_bytes = completed, .total_bytes = total, .source = source}, reporter);
  CHECK(latest.completed == expected_completed);
  CHECK(latest.total == expected_total);
  CHECK(latest.current_source == source);
 };
 constexpr auto coco = BenchmarkDatasetSource::kCoco2017;
 constexpr auto objects = BenchmarkDatasetSource::kObjects365V2;
 observe(coco, "known", 7U, 10U, 7U, 10U);
 observe(coco, "unknown", 4U, 0U, 11U, 0U);
 CHECK(latest.sources[0].completed_bytes == 11U);
 CHECK(latest.sources[0].total_bytes == 0U);
 CHECK_FALSE(latest.sources[0].byte_total_known);
 observe(objects, "known", 8U, 20U, 19U, 0U);  // Source-qualified equal artifact names.
 observe(coco, "unknown", 2U, 0U, 17U, 0U);    // Explicit retry withdrawal.
 observe(coco, "unknown", 4U, 4U, 19U, 34U);
 CHECK(latest.sources[0].byte_total_known);
 observe(objects, "unknown", 3U, 0U, 22U, 0U);
 observe(objects, "unknown", 3U, 3U, 22U, 37U);
 observe(coco, "known", 0U, 10U, 15U, 37U);  // Restart of a known artifact.
 observe(coco, "known", 10U, 10U, 25U, 37U);
 totals.update(DownloadProgress{"known", 10U, 10U, 0U, false, true}, reporter);
 CHECK(latest.completed == 25U);
 CHECK(latest.total == 37U);
 CHECK(latest.sources[0].cache_hit);
 // A failed checked replacement leaves its prior contribution intact.
 CHECK_THROWS_AS(totals.update(DownloadProgress{"known", std::numeric_limits<std::uint64_t>::max(), 10U}, reporter), std::overflow_error);
 observe(coco, "known", 10U, 10U, 25U, 37U);
 CHECK_THROWS_AS(totals.update(DownloadProgress{"known", 10U, std::numeric_limits<std::uint64_t>::max()}, reporter), std::overflow_error);
 observe(coco, "known", 10U, 10U, 25U, 37U);
}
TEST_CASE("ordinary transfer observations exclude discarded bodies and settle unknown artifact sizes", "[backend][data][benchmark][download]") {
 mmltk::testsupport::ScopedTempDir root("accepted-transfer-bytes");
 const std::vector<std::uint8_t> payload(4096U, 73U);
 HttpServer server(payload);
 bool unknown = false;
 bool retry = false;
 bool redirect = false;
 bool invalid_request = false, segmented_probe = false;
 std::uint64_t retained = 0U;
 SECTION("invalid local protocol remains fatal") {
  invalid_request = true;
  SECTION("ordinary transfer") {}
  SECTION("segmented identity probe") { segmented_probe = true; }
 }
 SECTION("unknown length") {
  unknown = true;
  server.OmitContentLength();
 }
 SECTION("nonempty retry body exceeds the artifact") {
  retry = true;
  server.fail_next(1, 8192U);
 }
 SECTION("failed response preserves the retained prefix") {
  retry = true;
  retained = 512U;
  server.fail_next(1, 8192U);
 }
 SECTION("redirect body is discarded") {
  redirect = true;
  server.RedirectNextTransfer();
 }
 DownloadRequest request{
  "coco-observed", server.url("artifact"), root.path() / "artifact.bin", root.path() / "artifact.lock", unknown ? 0U : payload.size(), {}, 2U};
 if (invalid_request) {
  request.url = "unsupported-benchmark-protocol://artifact";
  if (segmented_probe) request.expected_size = 512ULL * 1024U * 1024U;
  bool local_failure = false;
  unsigned retries = 0;
  try {
   (void)download_artifacts({request}, segmented_probe ? 2U : 1U, {}, {}, [&](std::string_view event, const auto&) {
    if (event == "benchmark.download.attempt_failed" || event == "benchmark.download.segmented_probe_retry") ++retries;
   });
  } catch (const BenchmarkDownloadUnavailable&) {
   FAIL("invalid local protocol became optional source unavailability");
  } catch (const std::runtime_error& error) {
   local_failure = true;
   CHECK(std::string_view(error.what()).starts_with("local benchmark CURL failure:"));
  }
  CHECK(local_failure);
  CHECK(retries == 0);
  CHECK(server.requests() == 0);
  CHECK_FALSE(fs::exists(request.destination));
  CHECK_FALSE(fs::exists(request.destination.string() + ".download.json"));
  server.Check();
  return;
 }
 if (retained != 0U) {
  std::ofstream partial(request.destination.string() + ".part", std::ios::binary);
  partial.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(retained));
  partial.close();
  write_json_atomically(request.destination.string() + ".part.json",
                        {{"schema_version", kBenchmarkCacheSchemaVersion}, {"url", request.url}, {"etag", "\"benchmark-test-etag\""}}, {});
 }
 std::vector<DownloadProgress> updates;
 const auto downloaded = download_artifacts({request}, 1U, {}, [&](const auto& update) { updates.push_back(update); });
 REQUIRE(downloaded.size() == 1U);
 REQUIRE_FALSE(updates.empty());
 for (const auto& update : updates) {
  CHECK(update.completed_bytes <= payload.size());
  CHECK((update.total_bytes == 0U || update.total_bytes == payload.size()));
  CHECK(update.retained_bytes == retained);
  CHECK(update.completed_bytes >= retained);
  if (retry && update.attempt == 1U) CHECK(update.completed_bytes == retained);
 }
 CHECK(updates.back().completed_bytes == payload.size());
 CHECK(updates.back().total_bytes == payload.size());
 CHECK(downloaded.front().attempts == (retry ? 2U : 1U));
 CHECK(server.requests() == (retry || redirect ? 2U : 1U));
 CHECK(mmltk::common::io::sha256_file(request.destination) == mmltk::common::io::sha256_bytes(payload));
 const auto metadata = read_json_file(request.destination.string() + ".download.json");
 CHECK(metadata.at("size") == payload.size());
 CHECK(metadata.at("identity") == downloaded.front().identity);
 CHECK_FALSE(fs::exists(request.destination.string() + ".part"));
 CHECK_FALSE(fs::exists(request.destination.string() + ".part.json"));
 const auto requests_before = server.requests();
 const auto cached = download_artifacts({request}, 1U, {}, [&](const auto& update) {
  CHECK(update.cache_hit);
  CHECK(update.completed_bytes == payload.size());
  CHECK(update.total_bytes == payload.size());
 });
 CHECK(cached.front().identity == downloaded.front().identity);
 CHECK(server.requests() == requests_before);
 server.Check();
}
TEST_CASE("annotation retries distinguish source corruption from local capacity and cancellation", "[benchmark][cache]") {
 mmltk::testsupport::ScopedTempDir root("annotation-retry");
 const auto source = root.path() / "complete-source";
 const auto completion = root.path() / "complete-source.download.json";
 mmltk::testsupport::write_text_file(source, "retained source");
 mmltk::testsupport::write_text_file(completion, "retained completion");
 const auto source_digest = mmltk::common::io::sha256_file(source);
 const auto completion_digest = mmltk::common::io::sha256_file(completion);
 const auto source_time = fs::last_write_time(source);
 const auto completion_time = fs::last_write_time(completion);
 std::atomic<bool> cancel{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancel);
 unsigned bodies = 0, repairs = 0;
 const auto repair = [&](const std::exception&) {
  ++repairs;
  remove_cache_path(source);
  remove_cache_path(completion);
 };
 SECTION("typed storage exhaustion never invalidates completed source files") {
  CHECK_THROWS_AS(retry_annotation_indexing(
                   cancellation,
                   [&] {
                    ++bodies;
                    throw InsufficientBenchmarkStorage("fixture capacity");
                   },
                   repair),
                  InsufficientBenchmarkStorage);
  CHECK(bodies == 1);
  CHECK(repairs == 0);
 }
 SECTION("cancellation never enters source repair") {
  CHECK_THROWS(retry_annotation_indexing(
   cancellation,
   [&] {
    ++bodies;
    cancel = true;
    throw std::runtime_error("interrupted parse");
   },
   repair));
  CHECK(bodies == 1);
  CHECK(repairs == 0);
 }
 SECTION("source corruption has exactly three bodies and two repairs") {
  CHECK_THROWS(retry_annotation_indexing(
   cancellation,
   [&] {
    ++bodies;
    throw std::runtime_error("malformed source");
   },
   [&](const std::exception&) { ++repairs; }));
  CHECK(bodies == 3);
  CHECK(repairs == 2);
 }
 REQUIRE(fs::is_regular_file(source));
 REQUIRE(fs::is_regular_file(completion));
 CHECK(fs::last_write_time(source) == source_time);
 CHECK(fs::last_write_time(completion) == completion_time);
 CHECK(mmltk::common::io::sha256_file(source) == source_digest);
 CHECK(mmltk::common::io::sha256_file(completion) == completion_digest);
}
