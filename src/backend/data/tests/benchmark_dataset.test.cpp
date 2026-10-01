#include "src/backend/data/benchmark/coconut/detail/coconut_import.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_parquet.h"
#include "src/backend/data/detail/staged_artifact.h"
#include "src/backend/data/detail/compiled_slot_storage.h"
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/file_work_queue.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_input.h"
#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_native_image.h"
#include "src/backend/data/benchmark/detail/benchmark_curl.h"
#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/backend/data/benchmark/detail/benchmark_annotation_cache.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <new>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <simdjson.h>
#include <stb_image_write.h>
#include <stb_image.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "src/test_support/async_test_utils.hpp"
#include "src/common/io/staging_directory.h"
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_compiler.h"
#include "src/backend/data/benchmark/detail/benchmark_download.h"
#include "src/backend/data/benchmark/detail/benchmark_images.h"
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include "src/backend/data/benchmark/detail/benchmark_sampling.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/backend/data/benchmark/detail/benchmark_writer.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/compiled_file.h"
#include "src/backend/data/detail/mask_rle_utils.h"
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/detail/compile_workspace.h"
#include "src/backend/data/detail/compile_pixel_workspace.h"
#include "src/backend/data/detail/compiled_label_workspace.h"
#include "src/common/system/numa_memory.h"
#include "src/common/system/numa_topology.h"
#include "src/backend/data/benchmark/detail/benchmark_image_input.h"
#include "src/backend/data/benchmark/detail/benchmark_progress.h"
#include "src/backend/data/benchmark/detail/open_images_acquisition.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/environment_test_utils.hpp"
#include "src/backend/data/benchmark/benchmark_dataset_compiler.h"
#include "src/backend/data/benchmark/benchmark_hash.h"
#include "src/common/io/file_digest.h"
#include "src/backend/data/compiled/compiled_dataset.h"
#include "src/backend/data/compiler/dataset_compiler.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/backend/data/tests/benchmark_http_fixture.h"
#include "src/backend/data/loading/dataset_loader.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/io/file_memory.h"
#include "src/common/io/scoped_fd.h"
#include "src/common/system/cpu_affinity.h"
namespace {
// One-shot, thread-local allocation failure: only the guarded controller call
// is affected. Completion storage/waits and archive teardown stay unchanged.
thread_local std::optional<std::size_t> allocation_failure_countdown;
class FailNextAllocation final {
public:
 explicit FailNextAllocation(std::size_t allowed) { allocation_failure_countdown = allowed; }
 ~FailNextAllocation() { allocation_failure_countdown.reset(); }
 FailNextAllocation(const FailNextAllocation&) = delete;
 FailNextAllocation& operator=(const FailNextAllocation&) = delete;
 [[nodiscard]] bool fired() const noexcept { return !allocation_failure_countdown; }
};
}  // namespace
[[gnu::noinline]] void* operator new(std::size_t size) {
 if (allocation_failure_countdown) {
  if (!*allocation_failure_countdown) {
   allocation_failure_countdown.reset();
   throw std::bad_alloc{};
  }
  --*allocation_failure_countdown;
 }
 for (;;) {
  if (void* allocation = std::malloc(size ? size : 1)) return allocation;
  if (const auto handler = std::get_new_handler())
   handler();
  else
   throw std::bad_alloc{};
 }
}
[[gnu::noinline]] void* operator new[](std::size_t size) { return ::operator new(size); }
[[gnu::noinline]] void operator delete(void* allocation) noexcept { std::free(allocation); }
[[gnu::noinline]] void operator delete[](void* allocation) noexcept { ::operator delete(allocation); }
[[gnu::noinline]] void operator delete(void* allocation, std::size_t) noexcept { ::operator delete(allocation); }
[[gnu::noinline]] void operator delete[](void* allocation, std::size_t) noexcept { ::operator delete(allocation); }
using mmltk::backend::data::detail::CompileAllowance;
using mmltk::backend::data::detail::CompileExecution;
using mmltk::backend::data::detail::CompileExecutionLimits;
using mmltk::backend::data::detail::CompileResources;
using mmltk::backend::data::detail::CompileTaskKind;
using mmltk::backend::data::detail::CompileWorkClass;
using mmltk::backend::data::detail::InsufficientCompileResources;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace mmltk::backend::data;
using namespace mmltk::backend::data::benchmark_internal;
using mmltk::common::io::FileHandle;
namespace {
class DenyStbHeapAllocations final {
public:
 std::size_t attempts = 0;
 DenyStbHeapAllocations() {
  stbi_set_heap_allocation_guard_thread([](std::size_t, void* user) {
   ++static_cast<DenyStbHeapAllocations*>(user)->attempts;
   return 0;
  }, this);
 }
 ~DenyStbHeapAllocations() { stbi_set_heap_allocation_guard_thread(nullptr, nullptr); }
 DenyStbHeapAllocations(const DenyStbHeapAllocations&) = delete;
 DenyStbHeapAllocations& operator=(const DenyStbHeapAllocations&) = delete;
};
using mmltk::backend::data::testsupport::benchmark_entry;
using mmltk::backend::data::testsupport::bind_benchmark_slots;
using mmltk::backend::data::testsupport::write_benchmark_fixture;
NormalizedAnnotationIndex fixture_index(const NormalizedAnnotationBuilder& builder) { return seal_normalized_annotation_metadata(NormalizedAnnotationBuilder(builder)); }
const NormalizedAnnotationIndex& fixture_index(const NormalizedAnnotationIndex& index) { return index; }
using mmltk::testsupport::require_condition;
// Call only while every available CPU is gated and no other admission mutation is in
// flight. A new scheduler record proves this borrowed work was queued;
// opening its producer can notify admission before the record exists.
// The caller owns the future before waiting, and releases its gates on unwind.
void queue_benchmark_work(BenchmarkCompilePipeline& execution, std::future<void>& future, std::function<void()> work) {
 const auto before = execution.execution().snapshot().records;
 future = std::async(std::launch::async, std::move(work));
 const auto deadline = std::chrono::steady_clock::now() + 2s;
 for (;;) {
  const auto generation = execution.execution().admission_generation();
  if (execution.execution().snapshot().records > before) return;
  require_condition(std::chrono::steady_clock::now() < deadline, "benchmark work did not reach its ready queue");
  execution.execution().wait_for_admission_change(generation, deadline);
 }
}
void wait_benchmark_pressure(BenchmarkCompilePipeline& execution) {
 const auto deadline = std::chrono::steady_clock::now() + 2s;
 for (;;) {
  const auto generation = execution.execution().admission_generation();
  if (execution.execution().resource_pressure()) return;
  require_condition(std::chrono::steady_clock::now() < deadline, "pixel demand did not reach resource admission");
  execution.execution().wait_for_admission_change(generation, deadline);
 }
}
void occupy_benchmark_cpu(BenchmarkCompilePipeline& execution, std::future<void>& work, mmltk::testsupport::TestGate& gate) {
 work = std::async(std::launch::async, [&] { execution.execution().run(CompileTaskKind::Archive, {}, [&](std::size_t) { gate.receipt().ArriveAndWait(); }); });
 REQUIRE(gate.WaitEntered(2s));
}
[[nodiscard]] BenchmarkWriteRequest benchmark_write_request(const PreparedBenchmarkSplit& split, fs::path output, const std::uint32_t resolution,
 const mmltk::common::concurrency::CancellationObservation cancellation = {}, BenchmarkCompilePipeline* execution = nullptr) {
 return {
  .split = split,
  .output_path = std::move(output),
  .resolution = resolution,
  .num_workers = 2,
  .worker_cpus = {},
  .overwrite = false,
  .cancel_requested = cancellation,
  .progress = {},
  .execution = execution,
 };
}
// The attempt retires every pipeline borrower before its referenced writer and
// split storage. Tests may retain direct access to both output and slot owner.
struct LiveBenchmarkWriter {
 PreparedBenchmarkSplit split{.name = "train", .class_names = {"person"}, .sources = {}, .images = {}};
 BenchmarkWriteRequest output;
 BenchmarkSplitWriter writer;
 BenchmarkCompilePipeline::Attempt attempt;
 LiveBenchmarkWriter(BenchmarkCompilePipeline& execution, const fs::path& directory, AnnotationSource source)
     : output(benchmark_write_request(split, directory / "train.bin", 8, {}, &execution)), writer(output, true), attempt(execution) {
  execution.bind_source(directory / "images", writer, source);
 }
};
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
 explicit RetainedArtifact(fs::path path) : path_(std::move(path)), digest_(mmltk::common::io::sha256_file(path_)), before_(mmltk::common::io::FileSnapshot::Read(path_)) {}
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
// Observe real descriptor identities from Curl's socket-options callback. A
// reused descriptor number cannot conceal an overlapping address-race socket.
struct CurlSocketObservation {
 struct Opened {
  curl_socket_t descriptor;
  dev_t device;
  ino_t inode;
 };
 std::mutex mutex;
 std::vector<Opened> opened;
 std::size_t peak = 0, attempts = 0;
 std::exception_ptr failure;
 [[nodiscard]] std::size_t live_locked() {
  std::erase_if(opened, [](const Opened& value) {
   struct stat status{};
   return ::fstat(value.descriptor, &status) != 0 || value.device != status.st_dev || value.inode != status.st_ino;
  });
  return opened.size();
 }
 [[nodiscard]] std::size_t live() {
  const std::lock_guard lock(mutex);
  return live_locked();
 }
 static int observe(void* opaque, curl_socket_t descriptor, curlsocktype) noexcept {
  auto& self = *static_cast<CurlSocketObservation*>(opaque);
  const std::lock_guard lock(self.mutex);
  try {
   struct stat status{};
   require_condition(::fstat(descriptor, &status) == 0 && S_ISSOCK(status.st_mode), "Curl fixture did not receive a physical socket");
   (void)self.live_locked();
   self.opened.push_back({descriptor, status.st_dev, status.st_ino});
   self.peak = std::max(self.peak, self.opened.size());
   ++self.attempts;
   return CURL_SOCKOPT_OK;
  } catch (...) {
   self.failure = std::current_exception();
   return CURL_SOCKOPT_ERROR;
  }
 }
};
struct ObservedCurlTransfer {
 CurlEasy easy{curl_easy_init()};
 CurlHeaders addresses;
 std::string url;
 std::vector<std::uint8_t> bytes;
 std::size_t responses = 0;
 std::function<void()> before_write;
 std::array<char, CURL_ERROR_SIZE> error{};
 std::exception_ptr callback_error;
 mmltk::common::concurrency::CancellationObservation cancel_requested;
 ObservedCurlTransfer(std::string location, CurlSocketObservation& sockets, std::string resolution = {}) : url(std::move(location)) {
  require_condition(static_cast<bool>(easy), "cannot allocate observed Curl fixture");
  configure_curl_transfer(easy.get(),
   {.url = url.c_str(), .error_buffer = error.data(), .owner = this, .write_callback = &write, .header_callback = &header, .progress_callback = &curl_cancel_progress_callback<ObservedCurlTransfer>},
   "observed Curl fixture: ");
  const auto option = [&](CURLoption key, auto value) { set_curl_option_with_prefix(easy.get(), key, value, "observed Curl fixture: ", "option"); };
  option(CURLOPT_PROXY, "");
  option(CURLOPT_SOCKOPTFUNCTION, &CurlSocketObservation::observe);
  option(CURLOPT_SOCKOPTDATA, &sockets);
  if (!resolution.empty()) {
   addresses.reset(curl_slist_append(nullptr, resolution.c_str()));
   require_condition(static_cast<bool>(addresses), "cannot allocate fixture address list");
   option(CURLOPT_RESOLVE, addresses.get());
  }
 }
 static std::size_t header(char* data, std::size_t size, std::size_t count, void* opaque) {
  return curl_run_data_callback<ObservedCurlTransfer>(size, count, opaque, [&](ObservedCurlTransfer& self, std::size_t bytes) {
   if (std::string_view(data, bytes).starts_with("HTTP/")) ++self.responses;
   return bytes;
  });
 }
 static std::size_t write(char* data, std::size_t size, std::size_t count, void* opaque) {
  return curl_run_data_callback<ObservedCurlTransfer>(size, count, opaque, [&](ObservedCurlTransfer& self, std::size_t bytes) {
   if (self.before_write) self.before_write();
   self.bytes.insert(self.bytes.end(), data, data + bytes);
   return bytes;
  });
 }
};
BenchmarkCurl::Completed await_curl(BenchmarkCurl::Channel& channel) {
 const auto deadline = std::chrono::steady_clock::now() + 5s;
 while (std::chrono::steady_clock::now() < deadline) {
  if (auto result = channel.next()) return *result;
  channel.wait_until(deadline);
 }
 throw std::runtime_error("benchmark Curl fixture did not settle");
}
DownloadRequest request_for(const fs::path& root, const std::string& id, const std::string& url, const std::vector<std::uint8_t>& payload) {
 return DownloadRequest{
  id,
  url,
  root / (id + ".bin"),
  root / "locks" / (id + ".lock"),
  payload.size(),
  mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(payload)),
  3U,
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
 CHECK(std::ranges::any_of(retry_updates, [](const auto& update) { return update.transfer.attempt == 2U && update.transfer.completed_bytes > 0U; }));
 DownloadRequest resume = request_for(root.path(), "resume", server.url("resume"), payload);
 std::atomic<bool> cancel{false};
 server.GateNextTransfer();
 mmltk::testsupport::TestGate received("HTTP partial bytes persisted");
 auto download = std::async(std::launch::async, [&] {
  try {
   (void)download_artifacts({resume}, 1U, mmltk::common::concurrency::CancellationObservation::Atomic(cancel), [&](const DownloadProgress& progress) {
    if (progress.transfer.completed_bytes >= HttpServer::partial_bytes) received.receipt().ArriveAndWait();
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
 const auto resumed = download_artifacts({resume}, 1U, mmltk::common::concurrency::CancellationObservation::Atomic(cancel), [&](const auto& update) { resumed_updates.push_back(update); });
 REQUIRE_FALSE(resumed_updates.empty());
 CHECK(std::ranges::any_of(resumed_updates,
  [](const auto& update) { return update.transfer.resumed && update.transfer.retained_bytes == HttpServer::partial_bytes && update.transfer.completed_bytes > update.transfer.retained_bytes; }));
 for (const auto& update : resumed_updates) {
  CHECK(update.transfer.completed_bytes >= HttpServer::partial_bytes);
  CHECK(update.transfer.completed_bytes <= payload.size());
  CHECK(update.transfer.total_bytes == payload.size());
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
 parsed.rejected = {.raw_records = 101U, .unmapped_categories = 23U, .unknown_images = 37U, .malformed_records = 41U, .degenerate_boxes = 53U, .duplicate_boxes = 67U};
 const fs::path index_path = root.path() / "mini-coco.index";
 store_normalized_annotation_index(index_path, fixture_index(parsed), {});
 const RetainedArtifact retained_index{index_path};
 auto loaded = load_normalized_annotation_index(index_path, options.source, options.split, digest, {}, throwing_trace);
 retained_index.Check();
 if (!loaded.has_value()) { throw std::runtime_error("stored normalized annotation index did not reload"); }
 REQUIRE(loaded.value().images.size() == 2U);
 REQUIRE(loaded.value().boxes.size() == 2U);
 CHECK(image_ids(NormalizedAnnotationReadView(*loaded)) == image_ids(NormalizedAnnotationReadView(parsed)));
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
 std::array<std::uint64_t, 4> layout{};
 persisted.seekg(36);
 persisted.read(reinterpret_cast<char*>(layout.data()), sizeof(layout));
 REQUIRE(persisted.good());
 CHECK((layout == std::array<std::uint64_t, 4>{256, 320, 448, 448}));
 persisted.seekg(0);
 const std::string valid_bytes{std::istreambuf_iterator<char>(persisted), std::istreambuf_iterator<char>()};
 REQUIRE(valid_bytes.size() == 448);
 struct InvalidHeader {
  std::size_t offset, width;
  std::uint64_t value;
 };
 // Field offsets, record extents and values are independent of the production
 // header/layout implementation. Overflowing corrupt counts remain cache misses.
 const std::array invalid_headers{
  InvalidHeader{16, 4, 0}, InvalidHeader{16, 4, UINT32_MAX}, InvalidHeader{20, 8, UINT64_MAX}, InvalidHeader{28, 8, UINT64_MAX}, InvalidHeader{20, 8, UINT64_MAX / 64},
  InvalidHeader{28, 8, UINT64_MAX / 8}, InvalidHeader{20, 8, 3}, InvalidHeader{28, 8, 1}, InvalidHeader{36, 8, 257}, InvalidHeader{44, 8, 321}, InvalidHeader{52, 8, 449}, InvalidHeader{60, 8, 449},
  InvalidHeader{13, 1, 1}, InvalidHeader{244, 1, 1}
 };
 for (const auto& invalid : invalid_headers) {
  CAPTURE(invalid.offset, invalid.width, invalid.value);
  auto bytes = valid_bytes;
  for (std::size_t i = 0; i < invalid.width; ++i) bytes[invalid.offset + i] = static_cast<char>((invalid.value >> (i * 8)) & 0xffU);
  write_text(index_path, bytes);
  CHECK_FALSE(load_normalized_annotation_index(index_path, options.source, options.split, digest, {}));
 }
 for (const auto size : {valid_bytes.size() - 1, valid_bytes.size() + 1}) {
  auto bytes = valid_bytes;
  bytes.resize(size);
  write_text(index_path, bytes);
  CHECK_FALSE(load_normalized_annotation_index(index_path, options.source, options.split, digest, {}));
 }
 write_text(index_path, valid_bytes);
 REQUIRE(load_normalized_annotation_index(index_path, options.source, options.split, digest, {}));
 const NormalizedAnnotationRowReady capacity_failure = [](const auto&, std::size_t) { throw std::overflow_error("row consumer capacity"); };
 CHECK_THROWS_AS(load_normalized_annotation_index(index_path, options.source, options.split, digest, {}, {}, nullptr, false, 0, capacity_failure), std::overflow_error);
 const NormalizedAnnotationRowReady callback_failure = [](const auto&, std::size_t) { throw std::logic_error("row consumer failure"); };
 CHECK_THROWS_AS(load_normalized_annotation_index(index_path, options.source, options.split, digest, {}, {}, nullptr, false, 0, callback_failure), std::logic_error);
 const nlohmann::json expected_rejections{
  {"raw_records", 101U}, {"unmapped_categories", 23U}, {"unknown_images", 37U}, {"malformed_records", 41U}, {"degenerate_boxes", 53U}, {"duplicate_boxes", 67U}
 };
 const auto manifest_path = root.path() / "manifest" / "rejections.json";
 write_json_atomically(manifest_path, {{"rejected_records", reject_json(parsed.rejected)}}, {});
 CHECK(read_json_file(manifest_path).at("rejected_records") == expected_rejections);
 corrupt_byte(index_path, 0U);
 loaded = load_normalized_annotation_index(index_path, options.source, options.split, digest, {});
 REQUIRE(!loaded);
 store_normalized_annotation_index(index_path, fixture_index(parsed), {});
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
 NormalizedAnnotationIndex open = parse_open_images_annotations(boxes, classes, mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(boxes)), open_mappings, open_options);
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
 NormalizedAnnotationBuilder source;
 source.source = BenchmarkDatasetSource::kObjects365V2;
 source.split = "train";
 source.annotation_sha256 = "synthetic";
 // Every shard contributes 200 images to each class. Three shards meet the
 // 512-image coverage floor, so unequal archive costs change the chosen set.
 constexpr std::size_t kImageCount = 2400U;
 source.images.reserve(kImageCount);
 source.boxes.reserve(kImageCount + kImageCount / 5U);
 std::array<std::uint32_t, kImageCount> original_box_counts{};
 for (std::size_t image_index = 0U; image_index < kImageCount; ++image_index) {
  const std::uint64_t first_box = source.boxes.size();
  source.boxes.push_back(NormalizedBox{0.1F, 0.2F, 0.8F, 0.9F, 0U, 0U, static_cast<std::uint8_t>(image_index % 3U), {}});
  if (image_index % 5U == 0U) { source.boxes.push_back(NormalizedBox{0.2F, 0.3F, 0.7F, 0.8F, 0U, 0U, static_cast<std::uint8_t>(image_index % 3U), {}}); }
  original_box_counts[image_index] = static_cast<std::uint32_t>(source.boxes.size() - first_box);
  source.images.push_back(NormalizedImage{image_index, first_box, original_box_counts[image_index], 640U, 480U, static_cast<std::uint16_t>(image_index % 4U), 0U});
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
  box.source_ordinal = source.boxes.size() + 900 - i;
  if (box.mask_rle_pairs) {
   source.mask_rle_pairs.push_back({static_cast<std::uint32_t>(i), 1});
   source.mask_rle_pairs.push_back({static_cast<std::uint32_t>(i + 100), 2});
  }
 }
 NormalizedAnnotationBuilder coco;
 coco.source = BenchmarkDatasetSource::kCoco2017;
 for (std::uint64_t id = 0; id < 12; ++id) {
  coco.images.push_back(NormalizedImage{id, coco.boxes.size(), 2, 640, 480, 0, 0});
  coco.boxes.push_back(NormalizedBox{0.1F, 0.2F, 0.8F, 0.9F, 0, 0, 0, {}});
  coco.boxes.push_back(NormalizedBox{0.2F, 0.3F, 0.7F, 0.8F, 0, 0, 0, {}});
 }
 NormalizedAnnotationBuilder open_images = source;
 open_images.source = BenchmarkDatasetSource::kOpenImagesV7;
 source.completion = std::make_shared<NormalizedAnnotationCompletion>();
 open_images.completion = std::make_shared<NormalizedAnnotationCompletion>();
 auto object_index = fixture_index(source);
 auto open_index = fixture_index(open_images);
 const auto coco_index = fixture_index(coco);
 const std::array<std::uint64_t, 4U> shard_bytes{1U, 1U, 1U, 1U};
 const std::array<std::uint64_t, 4U> unequal_bytes{9U, 1U, 2U, 3U};
 const CombinedSupplementalSamplingResult first_combined = sample_combined_supplemental_indices(coco_index, object_index, open_index, shard_bytes);
 const CombinedSupplementalSamplingResult cheaper = sample_combined_supplemental_indices(coco_index, object_index, open_index, unequal_bytes);
 CHECK(first_combined.objects365_shards == std::vector<std::uint16_t>{0U, 1U, 2U});
 CHECK(first_combined.objects365_archive_bytes == 3U);
 CHECK(cheaper.objects365_shards == std::vector<std::uint16_t>{1U, 2U, 3U});
 CHECK(cheaper.objects365_archive_bytes == 6U);
 const SupplementalSamplingResult& first = first_combined.objects365;
 REQUIRE(first.stats.full_images == kImageCount);
 REQUIRE(first.stats.full_boxes == source.boxes.size());
 REQUIRE(first_combined.target_images == (kImageCount * 2U) / 6U);
 REQUIRE(first.stats.selected_images + first_combined.open_images.stats.selected_images == first_combined.target_images);
 REQUIRE(first_combined.open_images.stats.selected_images >= first_combined.open_images_floor);
 REQUIRE(first_combined.open_images.stats.selected_images <= first_combined.open_images_ceiling);
 // Repeated boxes of one class count once per image, in both class and shard
 // summaries. This also fixes the baseline independently of worker agreement.
 for (const auto* selected : {&first_combined.objects365, &first_combined.open_images}) {
  for (std::size_t class_id = 0; class_id < selected->stats.available_class_images.size(); ++class_id) {
   CHECK(selected->stats.available_class_images[class_id] == (class_id < 3U ? kImageCount / 3U : 0U));
  }
 }
 const auto check_views = [&](const CombinedSupplementalSamplingResult& sample) {
  for (const auto* selected : {&sample.objects365, &sample.open_images}) {
   const auto& view = selected->view;
   const auto& backing = selected == &sample.objects365 ? object_index : open_index;
   CHECK(view.source == backing.source);
   CHECK(view.split == backing.split);
   CHECK(view.annotation_sha256 == backing.annotation_sha256);
   CHECK(view.completion == backing.completion);
   CHECK(view.storage().backing == backing.backing);
   CHECK(view.storage().images.data() == backing.images.data());
   CHECK(view.storage().boxes.data() == backing.boxes.data());
   CHECK(view.storage().mask_rle_pairs.data() == backing.mask_rle_pairs.data());
   CHECK(reject_json(view.rejected) == reject_json(source.rejected));
   REQUIRE(view.storage().images.size() == source.images.size());
   REQUIRE(view.storage().boxes.size() == source.boxes.size());
   REQUIRE(view.storage().mask_rle_pairs.size() == source.mask_rle_pairs.size());
   CHECK(std::memcmp(view.storage().images.data(), source.images.data(), source.images.size() * sizeof(NormalizedImage)) == 0);
   CHECK(std::memcmp(view.storage().boxes.data(), source.boxes.data(), source.boxes.size() * sizeof(NormalizedBox)) == 0);
   CHECK(std::memcmp(view.storage().mask_rle_pairs.data(), source.mask_rle_pairs.data(), source.mask_rle_pairs.size() * sizeof(RLEPair)) == 0);
   std::size_t boxes = 0, runs = 0;
   std::array<std::uint64_t, 80> class_images{};
   std::uint64_t previous_id = 0;
   for (std::size_t position = 0; position < view.image_count(); ++position) {
    const auto& image = view.image(position);
    CHECK(view.source_position(position) == image.source_image_id);
    CHECK(&image == &backing.images[image.source_image_id]);
    CHECK((position == 0 || image.source_image_id > previous_id));
    previous_id = image.source_image_id;
    REQUIRE(image.box_count == original_box_counts[image.source_image_id]);
    std::array<bool, 80> classes{};
    for (const auto& box : view.storage().boxes.subspan(image.first_box, image.box_count)) {
     classes[box.class_id] = true;
     for (const auto& run : view.storage().mask_rle_pairs.subspan(box.mask_rle_offset, box.mask_rle_pairs)) {
      CHECK(run.length != 0);
      ++runs;
     }
     ++boxes;
    }
    for (std::size_t class_id = 0; class_id < classes.size(); ++class_id) class_images[class_id] += classes[class_id];
   }
   CHECK(class_images == selected->stats.selected_class_images);
   CHECK(boxes == view.box_count());
   CHECK(runs == view.run_count());
   CHECK(selected->stats.selected_images == view.image_count());
   CHECK(selected->stats.selected_boxes == view.box_count());
  }
 };
 const auto check_agreement = [&](const CombinedSupplementalSamplingResult& expected, const CombinedSupplementalSamplingResult& actual) {
  CHECK(actual.target_images == expected.target_images);
  CHECK(actual.open_images_floor == expected.open_images_floor);
  CHECK(actual.open_images_ceiling == expected.open_images_ceiling);
  CHECK(actual.objects365_shards == expected.objects365_shards);
  CHECK(actual.objects365_archive_bytes == expected.objects365_archive_bytes);
  for (const bool objects : {true, false}) {
   const auto& left = objects ? expected.objects365 : expected.open_images;
   const auto& right = objects ? actual.objects365 : actual.open_images;
   CHECK(right.stats.full_images == left.stats.full_images);
   CHECK(right.stats.full_boxes == left.stats.full_boxes);
   CHECK(right.stats.selected_images == left.stats.selected_images);
   CHECK(right.stats.selected_boxes == left.stats.selected_boxes);
   CHECK(right.stats.available_class_images == left.stats.available_class_images);
   CHECK(right.stats.selected_class_images == left.stats.selected_class_images);
   CHECK(image_ids(right.view) == image_ids(left.view));
   CHECK(right.view.run_count() == left.view.run_count());
  }
  check_views(actual);
 };
 check_views(first_combined);
 check_views(cheaper);
 for (const std::size_t workers : {1U, 3U}) {
  BenchmarkCompilePipeline sampling_execution(workers);
  for (const bool unequal : {false, true}) {
   const auto repeated = sample_combined_supplemental_indices(coco_index, object_index, open_index, unequal ? unequal_bytes : shard_bytes, {}, &sampling_execution.execution());
   check_agreement(unequal ? cheaper : first_combined, repeated);
  }
  CHECK(sampling_execution.execution().try_reserve({sampling_execution.execution().transient_target(), 0}).has_value());
  for (const auto invalid_shard : {std::uint16_t{4}, std::numeric_limits<std::uint16_t>::max()}) {
   auto invalid = source;
   invalid.images.back().source_shard = invalid_shard;
   CHECK_THROWS_WITH(
    sample_combined_supplemental_indices(coco_index, fixture_index(invalid), open_index, shard_bytes, {}, &sampling_execution.execution()), "Objects365 annotation references an unknown image shard");
   CHECK(sampling_execution.execution().try_reserve({sampling_execution.execution().transient_target(), 0}).has_value());
  }
  CHECK_THROWS_WITH(sample_combined_supplemental_indices(coco_index, object_index, open_index, {}, {}, &sampling_execution.execution()), "Objects365 sampler requires archive byte identities");
 }
 enum class SelectionOutcome { Complete, CancelWaiting, CancelAdmitted };
 for (const auto outcome : {SelectionOutcome::Complete, SelectionOutcome::CancelWaiting, SelectionOutcome::CancelAdmitted}) {
  std::atomic<bool> cancelled{false};
  std::promise<void> waiting;
  mmltk::testsupport::TestGate admitted("sampler holds its complete workspace");
  mmltk::testsupport::TestGate greedy("greedy sampler retains source selection storage");
  mmltk::testsupport::TestGate positions("selected views retain construction allowance");
  struct SelectionObservation {
   mmltk::testsupport::TestGate& admitted;
   mmltk::testsupport::TestGate& greedy;
   mmltk::testsupport::TestGate& positions;
   const std::atomic<bool>& stop;
   std::thread::id controller;
   mutable std::atomic<bool> observed{false}, building_views{false}, observed_view{false};
   bool cancelled() const noexcept {
    // Source jobs and final views run on the CPU owner. The controller's
    // intervening cancellation point belongs to the dependent greedy loop.
    if (std::this_thread::get_id() == controller) {
     greedy.receipt().ArriveAndWait();
     building_views.store(true);
    } else if (building_views.load()) {
     if (!observed_view.exchange(true)) positions.receipt().ArriveAndWait();
    } else if (!observed.exchange(true))
     admitted.receipt().ArriveAndWait();
    return stop.load();
   }
  } observation{admitted, greedy, positions, cancelled, {}};
  struct AdmissionObservation {
   const std::atomic<bool>& stop;
   std::promise<void>& waiting;
   mutable std::size_t checks = 0;
   static bool& sampler_thread() {
    thread_local bool value = false;
    return value;
   }
   bool cancelled() const noexcept {
    // reserve checks cancellation, records its resource waiter, then checks
    // the wait predicate. Signal that existing causal boundary without
    // blocking while the pipeline mutex is held.
    if (sampler_thread() && ++checks == 2) mmltk::testsupport::release_test_promise(waiting);
    return stop.load();
   }
  } admission{cancelled, waiting};
  BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1}, mmltk::common::concurrency::CancellationObservation::Borrow(admission));
  auto held = execution.execution().reserve({1, 0});
  auto work = std::async(std::launch::async, [&] {
   AdmissionObservation::sampler_thread() = true;
   observation.controller = std::this_thread::get_id();
   return sample_combined_supplemental_indices(coco_index, object_index, open_index, unequal_bytes, mmltk::common::concurrency::CancellationObservation::Borrow(observation), &execution.execution());
  });
  const mmltk::testsupport::ScopedTestCleanup release([&] {
   cancelled.store(true);
   held = {};
   admitted.Release();
   greedy.Release();
   positions.Release();
   execution.execution().notify_admission_change();
  });
  mmltk::testsupport::await_test_promise(waiting, "sampler waits for its complete allowance");
  CHECK(execution.execution().resource_pressure());
  CHECK_FALSE(observation.observed.load());
  CHECK(work.wait_for(0ms) == std::future_status::timeout);
  // The waiting sampler has not taken the only CPU. Independent ready work
  // can run before the competing transient allocation retires.
  bool independent_ran = false;
  execution.execution().run(CompileTaskKind::Metadata, {}, [&](std::size_t) { independent_ran = true; });
  CHECK(independent_ran);
  if (outcome == SelectionOutcome::CancelWaiting) {
   cancelled.store(true);
   execution.execution().notify_admission_change();
  } else {
   held = {};
   REQUIRE(admitted.WaitEntered(2s));
   CHECK_FALSE(execution.execution().try_reserve({1, 0}).has_value());
   cancelled.store(outcome == SelectionOutcome::CancelAdmitted);
   admitted.Release();
  }
  if (outcome == SelectionOutcome::Complete) {
   REQUIRE(greedy.WaitEntered(2s));
   CHECK_FALSE(execution.execution().try_reserve({1, 0}).has_value());
   greedy.Release();
   REQUIRE(positions.WaitEntered(2s));
   CHECK_FALSE(execution.execution().try_reserve({1, 0}).has_value());
   positions.Release();
   const auto sampled = mmltk::testsupport::await_test_future(work, "oversized sampling after resource pressure");
   check_agreement(cheaper, sampled);
   // Retained selected positions/backing outlive transient sampler credits.
   CHECK(execution.execution().try_reserve({1, 0}).has_value());
  } else {
   CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(work, "cancelled sampling"), "benchmark dataset compilation cancelled");
  }
  cancelled.store(false);
  held = {};
  CHECK(execution.execution().try_reserve({1, 0}).has_value());
 }
 REQUIRE(first.view.image_count() != 0);
 const auto nested = first.view.select_images({0});
 CHECK(nested.source_position(0) == first.view.source_position(0));
 CHECK(&nested.image(0) == &first.view.image(0));
 CHECK_THROWS(nested.source_position(nested.image_count()));
 const std::weak_ptr<NormalizedAnnotationBacking> retained = object_index.backing;
 const auto* retained_boxes = object_index.boxes.data();
 object_index = {};
 open_index = {};
 CHECK_FALSE(retained.expired());
 CHECK(first.view.storage().boxes.data() == retained_boxes);
 CHECK(first.view.completion == source.completion);
 CHECK(std::memcmp(first.view.storage().boxes.data(), source.boxes.data(), source.boxes.size() * sizeof(NormalizedBox)) == 0);
 CHECK(std::memcmp(first.view.storage().mask_rle_pairs.data(), source.mask_rle_pairs.data(), source.mask_rle_pairs.size() * sizeof(RLEPair)) == 0);
 std::array<std::uint64_t, 80U> combined_class_images{};
 for (std::size_t class_id = 0U; class_id < combined_class_images.size(); ++class_id) {
  combined_class_images[class_id] = first.stats.selected_class_images[class_id] + first_combined.open_images.stats.selected_class_images[class_id];
 }
 combined_class_images[0] += coco.images.size();
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
std::vector<std::uint8_t> make_large_cached_jpeg() {
 const auto jpeg = make_jpeg(10, 20, 30);
 std::vector<std::uint8_t> encoded(jpeg.begin(), jpeg.begin() + 2);
 // Two legal JPEG comment segments keep the tiny decoded geometry while
 // making its immutable encoded extent larger than one 64 KiB header grant.
 for (unsigned i = 0; i < 2; ++i) {
  encoded.insert(encoded.end(), {0xff, 0xfe, 0xc0, 0x02});
  encoded.insert(encoded.end(), 48U << 10, static_cast<std::uint8_t>('a' + i));
 }
 encoded.insert(encoded.end(), jpeg.begin() + 2, jpeg.end());
 return encoded;
}
void write_tar_octal(std::array<std::uint8_t, 512U>* header, const std::size_t offset, const std::size_t width, const std::uint64_t value) {
 std::array<char, 32U> digits{};
 const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value, 8);
 require_condition(converted.ec == std::errc{}, "failed to format benchmark test tar number");
 const std::size_t digit_count = static_cast<std::size_t>(converted.ptr - digits.data());
 require_condition(digit_count < width, "benchmark test tar number exceeds its field");
 std::fill_n(header->begin() + static_cast<std::ptrdiff_t>(offset), width - 1U, static_cast<std::uint8_t>('0'));
 std::copy_n(reinterpret_cast<const std::uint8_t*>(digits.data()), digit_count, header->begin() + static_cast<std::ptrdiff_t>(offset + width - digit_count - 1U));
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
  .image_id_parser = [](const std::string_view path) -> std::optional<std::uint64_t> { return path.ends_with("/1.jpg") ? std::optional<std::uint64_t>{1U} : std::nullopt; },
  .cancel_requested = {},
  .progress =
   [&](const std::uint64_t completed, const std::uint64_t total) {
  progress_completed = completed;
  progress_total = total;
 },
  .validator =
   [](const std::uint64_t, const std::span<const std::uint8_t> encoded) {
  require_condition(has_complete_image_markers(encoded), "archive test JPEG is incomplete");
  return BenchmarkImageDecoder{}.read_header(encoded);
 },
  .trace = {},
  .quarantine_unavailable = true,
  .decompression_workers = 0U,
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
 BenchmarkEncodedImage::publish(cached_image_path(image_root, 1U), red, {});
 BenchmarkEncodedImage::publish(cached_image_path(image_root, 2U), green, {});
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
 const mmltk::backend::imaging::resample::ImageResizeGeometry letterbox =
  mmltk::backend::imaging::resample::compute_image_resize_geometry(16U, 8U, kNanoResolution, kNanoResolution, mmltk::backend::imaging::resample::ImageResizeMode::Letterbox);
 REQUIRE(letterbox.resized_width == kNanoResolution);
 REQUIRE(letterbox.resized_height == 192U);
 REQUIRE(letterbox.offset_x == 0U);
 REQUIRE(letterbox.offset_y == 96U);
 std::vector<std::shared_ptr<BenchmarkLabelChunk>> chunks;
 const std::array source_labels{
  benchmark_canvas_box(0U, 0.0F, 0.0F, 1.0F, 1.0F, letterbox),
  benchmark_canvas_box(0U, 0.125F, 0.25F, 0.875F, 0.75F, letterbox),
 };
 for (std::size_t index = 0U; index < ids.size(); ++index) {
  split.images.push_back(EncodedImageRecord{
   ids[index],
   16U,
   8U,
   1U,
   0U,
  });
  auto annotations = std::make_shared<BenchmarkLabelChunk>();
  annotations->labels = {source_labels[index]};
  chunks.push_back(std::move(annotations));
 }
 const auto write_labeled = [&](const BenchmarkWriteRequest& request) { write_benchmark_fixture(request, chunks); };
 const fs::path output = root.path() / "validation.bin";
 std::atomic<std::uint64_t> producer_events{0U};
 write_labeled(BenchmarkWriteRequest{
  split,
  output,
  kNanoResolution,
  2,
  {},
  false,
  {},
  {.context = &producer_events, .image_completed = [](void* context) { static_cast<std::atomic<std::uint64_t>*>(context)->fetch_add(1U, std::memory_order_relaxed); }},
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
 (void)validate_compiled_file_sections(header, file.size());
 const auto source = CompiledDataset::open(output);
 const auto index = source.image_entries();
 const std::array labels{source.image_labels(0).front(), source.image_labels(1).front()};
 REQUIRE(index[0].original_width == 16U);
 REQUIRE(index[0].original_height == 8U);
 REQUIRE(source.run_count() == 0U);
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
  REQUIRE(batch.annotation_views != nullptr);
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
 write_labeled(perceptual_request);
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
  const nlohmann::json acquired{
   {"image_cache", nlohmann::json::array({raw_cache_identity})}, {"train", {{"bytes", fs::file_size(output)}}}, {"val", {{"bytes", fs::file_size(perceptual_request.output_path)}}}
  };
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
  write_labeled(benchmark_write_request(split, output, kNanoResolution));
 } catch (const std::exception&) { refused = true; }
 REQUIRE(refused);
 REQUIRE(mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(output)) == original_digest);
 std::atomic<bool> cancel{true};
 const fs::path cancelled_output = root.path() / "cancelled.bin";
 bool cancelled = false;
 try {
  write_labeled(benchmark_write_request(split, cancelled_output, kNanoResolution, mmltk::common::concurrency::CancellationObservation::Atomic(cancel)));
 } catch (const std::exception&) { cancelled = true; }
 REQUIRE(cancelled);
 REQUIRE(!fs::exists(cancelled_output));
 PreparedBenchmarkSplit broken = split;
 broken.images[0].source_width = 15U;
 const fs::path broken_output = root.path() / "broken.bin";
 bool failed = false;
 try {
  write_labeled(benchmark_write_request(broken, broken_output, kNanoResolution));
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
 const fs::path lock_path =
  root.path() / ".cache" / "benchmark-dataset" / "v1" / "locks" /
  ("output-" + mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(normalized_output.data()), normalized_output.size()))) + ".lock");
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
 active.transfer = BenchmarkTransferProgress{.completed_bytes = 4096, .total_bytes = 8192, .retained_bytes = 1024, .attempt = 3, .resumed = true};
 const std::string status = format_benchmark_source_status(active, "extracting");
 REQUIRE(status.starts_with("Downloading objects365-train-patch-4"));
 REQUIRE(status.find("resumed") != std::string::npos);
 REQUIRE(status.find("2 retries") != std::string::npos);
 REQUIRE(status.find("4096 / 8192 bytes · attempt 3 · retained 1024 bytes") != std::string::npos);
 active.activity = "Verifying cached objects365-train-patch-4";
 REQUIRE(format_benchmark_source_status(active, "extracting").starts_with("Verifying cached"));
 active.complete = true;
 REQUIRE(format_benchmark_source_status(active, "extracting") == "Cache hit · resumed · 2 retries");
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
 std::atomic<bool> cancelled{true};
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
 std::atomic<bool> cancelled{true};
 config.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 config.progress = [&](const BenchmarkCompileProgress&) { cancelled.store(true); };
 CHECK_THROWS_WITH(compile_benchmark_dataset(config), overlap ? "benchmark output and cache directories must not overlap" : "benchmark dataset compilation cancelled");
 cancelled.store(true);
 std::size_t path_trace_calls = 0U;
 config.trace = [&](std::string_view event, std::string_view) {
  if (event == "benchmark.compile.paths") ++path_trace_calls;
  throw std::runtime_error("diagnostic callback failure");
 };
 CHECK_THROWS_WITH(compile_benchmark_dataset(config), overlap ? "benchmark output and cache directories must not overlap" : "benchmark dataset compilation cancelled");
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
 std::atomic<bool> cancelled{true};
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
 NormalizedAnnotationBuilder input;
 input.split = "unsorted";
 input.annotation_sha256 = "identity";
 input.rejected = {13, 2, 3, 4, 5, 6};
 input.images = {{30, 0, 2, 8, 4, 7, 0}, {10, 2, 0, 9, 5, 8, 0}, {20, 2, 1, 10, 6, 9, 0}, {40, 3, 1, 11, 7, 10, 0}};
 input.boxes = {
  {0.1F, 0.2F, 0.8F, 0.9F, 0, 2, 2, kAnnotationMask | kAnnotationCategory | kAnnotationId, {}, 4.5, 101, 3, 19},
  {0.2F, 0.3F, 0.7F, 0.8F, 2, 0, 3, kAnnotationMask | kAnnotationCategory, {}, 0, 0, 4, 23},
  {0.3F, 0.4F, 0.6F, 0.7F, 2, 2, 4, kAnnotationMask | kAnnotationCrowd | kAnnotationCategory, {}, 8.25, 0, 5, 29},
  {0.4F, 0.5F, 0.8F, 0.9F, 4, 1, 5, kAnnotationMask | kAnnotationIgnore | kAnnotationCategory, {}, 1, 0, 6, 31}
 };
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
 NormalizedAnnotationBuilder source, destination;
 source.images = {{1, 0, 1, 8, 8, 0, 0}};
 source.boxes.resize(1);
 source.boxes[0].mask_rle_pairs = 131073;
 source.mask_rle_pairs.resize(131073, RLEPair{1, 1});
 SECTION("image position") { CHECK_THROWS(append_normalized_image_slice(destination, fixture_index(source), 1)); }
 SECTION("box offset") {
  source.images[0].first_box = UINT64_MAX;
  CHECK_THROWS(append_normalized_image_slice(destination, fixture_index(source), 0));
 }
 SECTION("box count") {
  source.images[0].box_count = 2;
  CHECK_THROWS(append_normalized_image_slice(destination, fixture_index(source), 0));
 }
 SECTION("mask offset") {
  source.boxes[0].mask_rle_offset = UINT64_MAX;
  CHECK_THROWS(append_normalized_image_slice(destination, fixture_index(source), 0));
 }
 SECTION("mask count") {
  source.boxes[0].mask_rle_pairs++;
  CHECK_THROWS(append_normalized_image_slice(destination, fixture_index(source), 0));
 }
 SECTION("owned immutable source survives builder retirement") {
  const auto stable = seal_normalized_annotation_metadata(std::move(source));
  source = {};
  append_normalized_image_slice(destination, stable, 0);
  REQUIRE(destination.images.size() == 1);
  CHECK(destination.images.front().source_image_id == stable.images.front().source_image_id);
  return;
 }
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
  append_normalized_image_slice(complete, fixture_index(source), 0, mmltk::common::concurrency::CancellationObservation::Borrow(observed));
  REQUIRE(observed.polls >= 5);
  for (std::size_t cut = 0; cut < observed.polls; ++cut) {
   Cancellation stop{0, cut};
   CHECK_THROWS(append_normalized_image_slice(destination, fixture_index(source), 0, mmltk::common::concurrency::CancellationObservation::Borrow(stop)));
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
TEST_CASE("benchmark event cancellation without progress", "[backend][data][benchmark][cancel]") { test_benchmark_event_cancellation_while_waiting_for_lock_without_progress(); }
TEST_CASE("benchmark source status", "[backend][data][benchmark][progress]") { test_benchmark_cli_source_status_preserves_active_transfer_state(); }
TEST_CASE("benchmark trace gate is lazy", "[backend][data][benchmark][trace]") { test_benchmark_trace_gate_is_lazy(); }
TEST_CASE("benchmark annotations retain provenance crowd area masks and deterministic source order", "[backend][data][benchmark][annotations]") {
 mmltk::testsupport::ScopedTempDir root("faithful-benchmark");
 const auto path = root.path() / "annotations.json";
 nlohmann::json document{
  {"images", {{{"id", 0}, {"width", 16}, {"height", 8}}}}, {"categories", {{{"id", 1}, {"name", "person"}}, {{"id", 2}, {"name", "human"}}}}, {"annotations", nlohmann::json::array()}
 };
 for (unsigned ordinal = 0; ordinal != 128U; ++ordinal) {
  document["annotations"].push_back(
   {{"id", 127U - ordinal}, {"image_id", 0}, {"category_id", ordinal % 2U + 1U}, {"bbox", {-0.5, 1.25, 13.0, 5.5}}, {"area", 7.25}, {"iscrowd", ordinal % 2U}, {"ignore", true},
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
 store_normalized_annotation_index(cache, fixture_index(parallel), {});
 const auto loaded = load_normalized_annotation_index(cache, options.source, options.split, digest, {});
 REQUIRE(loaded);
 REQUIRE(loaded->boxes.size() == parallel.boxes.size());
 CHECK(std::memcmp(loaded->boxes.data(), parallel.boxes.data(), parallel.boxes.size() * sizeof(NormalizedBox)) == 0);
 CHECK(decode_open_images_category(encode_open_images_category("/m/0h8my_4")) == "/m/0h8my_4");
 CHECK_THROWS(encode_open_images_category("/m/toolongidentifier"));
 CHECK_FALSE(valid_open_images_category(0U));
 CHECK_FALSE(valid_open_images_category(0x610062U));
}
std::pair<NormalizedAnnotationIndex, NormalizedAnnotationIndex> parse_serial_and_parallel(
 const fs::path& path, const std::string& digest, std::span<const NumericCategoryMapping> mappings, BenchmarkDatasetSource source, const BenchmarkTraceSink& trace = {}) {
 AnnotationParseOptions options;
 options.source = source;
 options.split = "train";
 options.num_workers = 1;
 options.trace = trace;
 auto sequential = parse_coco_style_annotations(path, digest, mappings, options);
 options.num_workers = 4;
 return {std::move(sequential), parse_coco_style_annotations(path, digest, mappings, options)};
}
TEST_CASE("benchmark semantic admission isolates malformed masks and numeric overflow", "[backend][data][benchmark][annotations]") {
 mmltk::testsupport::ScopedTempDir root("benchmark-admission");
 const auto path = root.path() / "annotations.json";
 nlohmann::json document{
  {"images", {{{"id", 0}, {"width", 16}, {"height", 8}, {"file_name", "patch0/0.jpg"}}}}, {"categories", {{{"id", 1}, {"name", "person"}}}}, {"annotations", nlohmann::json::array()}
 };
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
  unsigned assemblies = 0;
  const auto [sequential, parallel] = parse_serial_and_parallel(path, digest, mappings, source, [&](std::string_view event, const nlohmann::json& fields) {
   if (event != "benchmark.annotations.assembled") return;
   ++assemblies;
   CHECK(fields.at("box_capacity") == fields.at("boxes"));
   CHECK(fields.at("mask_rle_capacity") == fields.at("mask_rle_pairs"));
  });
  CHECK(assemblies == 2);
  REQUIRE(sequential.boxes.size() == 3U);
  REQUIRE(parallel.boxes.size() == sequential.boxes.size());
  CHECK(parallel.rejected.raw_records == 9U + rejected_capacity);
  CHECK(parallel.rejected.malformed_records == 6U + rejected_capacity);
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
 store_normalized_annotation_index(root.path() / "open-sequential.index", fixture_index(sequential), {});
 store_normalized_annotation_index(root.path() / "open-parallel.index", fixture_index(parallel), {});
 CHECK(mmltk::common::io::sha256_file(root.path() / "open-sequential.index") == mmltk::common::io::sha256_file(root.path() / "open-parallel.index"));
}
TEST_CASE("normalized benchmark caches require source category presence", "[backend][data][benchmark][annotations]") {
 mmltk::testsupport::ScopedTempDir root("normalized-provenance");
 const std::string digest(64U, '0');
 for (const auto source : {BenchmarkDatasetSource::kCoco2017, BenchmarkDatasetSource::kObjects365V2, BenchmarkDatasetSource::kOpenImagesV7}) {
  NormalizedAnnotationBuilder index;
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
  store_normalized_annotation_index(path, fixture_index(index), {});
  REQUIRE(load_normalized_annotation_index(path, source, "train", digest, {}));
  box.flags &= ~kAnnotationCategory;
  box.source_category_id = 0U;
  index.boxes[0] = box;
  CHECK_THROWS(store_normalized_annotation_index(root.path() / "missing.index", fixture_index(index), {}));
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
  {"images", {{{"id", 0}, {"width", 16}, {"height", 8}, {"file_name", "patch0/0.jpg"}}, {{"id", 1}, {"width", 4096}, {"height", 4096}, {"file_name", "patch0/1.jpg"}}}},
  {"categories", {{{"id", 1}, {"name", "person"}}}}, {"annotations", nlohmann::ordered_json::array()}
 };
 // Invalid compressed RLE must not override a supplied degenerate bbox.
 document["annotations"].push_back({{"image_id", 0}, {"category_id", 1}, {"bbox", {0, 0, 0, 1}}, {"segmentation", {{"size", {8, 16}}, {"counts", "!"}}}});
 // This valid all-background mask would otherwise allocate 16 MiB.
 document["annotations"].push_back({{"image_id", 1}, {"category_id", 1}, {"bbox", {0, 0, -1, 1}}, {"segmentation", {{"size", {4096, 4096}}, {"counts", {16777216U}}}}});
 // Normalized-float admission also runs before that mask can be allocated.
 document["annotations"].push_back({{"image_id", 1}, {"category_id", 1}, {"bbox", {1e100, 0.0, 1e100, 1.0}}, {"segmentation", {{"size", {4096, 4096}}, {"counts", {16777216U}}}}});
 document["annotations"].push_back({{"image_id", 0}, {"category_id", 1}, {"bbox", {-1, -1, 3, 3}}, {"segmentation", {{"size", {8, 16}}, {"counts", {0, 1, 127}}}}});
 document["annotations"].push_back({{"image_id", 0}, {"category_id", 1}, {"segmentation", {{"size", {8, 16}}, {"counts", {0, 1, 127}}}}});
 // Decode-time failures must remain irrelevant to a degenerate supplied box,
 // even when segmentation is encountered before the box and identities.
 const std::array<nlohmann::ordered_json, 3> invalid_segmentations{
  nlohmann::ordered_json(false), nlohmann::ordered_json{{"size", {8, 16}}}, nlohmann::ordered_json::array({nlohmann::ordered_json::array({0, 0, 1})})
 };
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
  const auto [sequential, parallel] = parse_serial_and_parallel(path, digest, mappings, source);
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
 nlohmann::json document{{"images", {{{"id", 0}, {"width", 4}, {"height", 4}}}}, {"categories", {{{"id", 1}, {"name", "person"}}}}, {"annotations", nlohmann::json::array()}};
 const std::array<std::vector<double>, 4> polygons{{
  {-1e308, -1e308, 1e308, -1e308, 1e308, 1e308, -1e308, 1e308},
  {1e308, 1e308, 1e308, 9e307, 9e307, 9e307},
  {-0.5, -0.5, 3.5, -0.5, 3.5, 3.5, -0.5, 3.5},
  {0.5, 0.5, 4.5, 0.5, 4.5, 4.5, 0.5, 4.5},
 }};
 for (const auto& polygon : polygons) document["annotations"].push_back({{"image_id", 0}, {"category_id", 1}, {"bbox", {0, 0, 4, 4}}, {"segmentation", {polygon}}});
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
   for (unsigned pixel = 0; pixel < actual.size(); ++pixel) CHECK(actual[pixel] == (ordinal == 0U || ordinal == 3U || (ordinal == 2U && pixel % 4U < 3U && pixel / 4U < 3U)));
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
 BenchmarkEncodedImage::publish(cached_image_path(image_root, 1U), make_jpeg(128U, 64U, 32U), {});
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
   split.images.push_back({1U, 16U, 8U, static_cast<std::uint16_t>(generic.image_labels(0).size()), 0U});
   auto annotations = std::make_shared<BenchmarkLabelChunk>();
   annotations->labels.assign(generic.image_labels(0).begin(), generic.image_labels(0).end());
   annotations->runs.assign(generic.annotations(0).runs.begin(), generic.annotations(0).runs.end());
   const std::array chunks{annotations};
   const auto output = root.path() / "benchmark.bin";
   auto request = benchmark_write_request(split, output, 16U);
   request.overwrite = true;
   request.resize_mode = mode;
   write_benchmark_fixture(request, chunks);
   const auto benchmark = CompiledDataset::open(output);
   CHECK(std::memcmp(&generic.header(), &benchmark.header(), sizeof(FileHeader)) == 0);
   (void)validate_compiled_file_sections(benchmark.header(), fs::file_size(output));
   CHECK(benchmark.label_count() == generic.image_labels(0).size());
   CHECK(benchmark.annotations(0).runs.size_bytes() == generic.annotations(0).runs.size_bytes());
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
 CHECK(std::ranges::all_of(updates, [](const DownloadProgress& progress) { return progress.transfer.total_bytes == bytes && progress.transfer.completed_bytes <= bytes; }));
 CHECK(std::ranges::any_of(updates, [](const DownloadProgress& progress) { return progress.transfer.completed_bytes > 0U; }));
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
 // The serial fixture can still own cancelled responses and accepted ranges.
 // A fresh HEAD response settles those earlier requests before the next fault.
 {
  CurlSocketObservation sockets;
  ObservedCurlTransfer barrier(server.url("segmented"), sockets);
  REQUIRE(curl_easy_setopt(barrier.easy.get(), CURLOPT_NOBODY, 1L) == CURLE_OK);
  REQUIRE(curl_easy_setopt(barrier.easy.get(), CURLOPT_TIMEOUT_MS, 5000L) == CURLE_OK);
  REQUIRE(curl_easy_perform(barrier.easy.get()) == CURLE_OK);
 }
 request.maximum_attempts = 3U;
 server.TruncateNextTransfer();
 std::atomic<bool> traced_retry{false}, traced_complete{false}, traced_bytes{false}, traced_second_attempt{false}, invalid_byte_facts{false};
 const auto resumed = download_artifacts({request}, 1U, {}, {}, [&](const std::string_view event, const nlohmann::json& fields) {
  if (event == "benchmark.download.progress") {
   traced_bytes.store(true, std::memory_order_relaxed);
   if (fields.at("attempt") == 2U) { traced_second_attempt.store(true, std::memory_order_relaxed); }
   if (fields.at("retained_bytes").get<std::uint64_t>() == 0U || fields.at("durable_bytes") > fields.at("completed_bytes")) { invalid_byte_facts.store(true, std::memory_order_relaxed); }
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
 const bool preloaded = GENERATE(false, true);
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
 BenchmarkEncodedImage::publish(cached_image_path(image_root, 1U), encoded, {});
 BenchmarkImageDecoder decoder;
 const auto header = decoder.read_header(encoded, width, height);
 mmltk::common::system::NumaMemory workspace(mmltk::common::system::NumaTopology::Capture().permitted_nodes.front(), mmltk::common::system::NumaMemory::Lock::Pages);
 workspace.ensure_bytes(BenchmarkImageDecoder::plan(header).bytes());
 const auto decoded_view = decoder.decode_rgb(encoded, header, {static_cast<std::byte*>(workspace.data()), workspace.capacity_bytes()});
 const std::vector<std::uint8_t> decoded(decoded_view.begin(), decoded_view.end());
 // A lane may first receive a header inspected by another decoder. Its JPEG
 // handle starts on that decode and remains reusable on subsequent calls.
 BenchmarkImageDecoder supplied_header_decoder;
 for (unsigned pass = 0; pass < 2; ++pass)
  CHECK(std::ranges::equal(supplied_header_decoder.decode_rgb(encoded, header, {static_cast<std::byte*>(workspace.data()), workspace.capacity_bytes()}), decoded));
 if (header.encoding == BenchmarkImageEncoding::Png) CHECK(decoded == rgb);
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{image_root}};
 split.images = {{1U, width, height, 0U, 0U}};
 for (const auto mode : {ImageResizeMode::Stretch, ImageResizeMode::Letterbox}) {
  const auto output = root.path() / (mode == ImageResizeMode::Stretch ? "stretch.bin" : "letterbox.bin");
  auto request = benchmark_write_request(split, output, target);
  request.perceptual_downscale = true;
  request.resize_mode = mode;
  if (preloaded) {
   BenchmarkCompilePipeline pipeline(1);
   request.execution = &pipeline;
   BenchmarkSplitWriter writer(request);
   BenchmarkCompilePipeline::Attempt attempt(pipeline);
   writer.write_remaining(request);
   writer.finish(request);
  } else
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
 std::uint64_t completed_pixels = 0, total_pixels = 0;
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json& value) {
  ++traces;
  if (event == "benchmark.pixel_compile.throughput") {
   completed_pixels = value.at("completed_images");
   total_pixels = value.at("total_images");
  }
 };
 ProgressReporter traced({}, trace);
 CHECK_FALSE(traced.transfer_observer_enabled());
 CHECK(traced.pixel_observer_enabled());
 traced.transfers().update(DownloadProgress{"coco-fixture", {.completed_bytes = 1U, .total_bytes = 2U, .attempt = 2U}}, traced);
 CHECK(traces == 0U);
 ArtifactProgressTotals unobserved_totals;
 unobserved_totals.update(DownloadProgress{.artifact_id = "unobserved", .transfer = {.completed_bytes = 1U}, .source = static_cast<BenchmarkDatasetSource>(255U)}, traced);
 CHECK(traces == 0U);
 traced.pixels(0U, 1U);
 traced.pixel_completed();
 traced.flush();
 CHECK(completed_pixels == 1U);
 CHECK(total_pixels == 1U);
 std::vector<BenchmarkCompileProgress> updates;
 ProgressReporter observed([&](const BenchmarkCompileProgress& update) { updates.push_back(update); }, quiet);
 CHECK(observed.transfer_observer_enabled());
 CHECK(observed.pixel_observer_enabled());
 observed.phase(DatasetCompilePhase::Downloading);
 observed.transfers().update(DownloadProgress{"coco-fixture", {.completed_bytes = 5U, .total_bytes = 11U, .attempt = 3U, .resumed = true}}, observed);
 observed.flush();
 REQUIRE_FALSE(updates.empty());
 CHECK(updates.back().sources[0].completed_bytes == 5U);
 CHECK(updates.back().sources[0].total_bytes == 11U);
 CHECK(updates.back().sources[0].retry_count == 2U);
 CHECK(updates.back().sources[0].resumed);
}
TEST_CASE("parser workers reset segmentation scratch across masks rejections and dimension changes", "[backend][data][benchmark][annotations]") {
 mmltk::testsupport::ScopedTempDir root("segmentation-scratch");
 const auto path = root.path() / "annotations.json";
 nlohmann::json document{
  {"images", {{{"id", 1}, {"width", 16}, {"height", 8}, {"file_name", "patch0/1.jpg"}}, {{"id", 2}, {"width", 4}, {"height", 4}, {"file_name", "patch0/2.jpg"}},
              {{"id", 3}, {"width", 4}, {"height", 4}, {"file_name", "patch0/3.jpg"}}}},
  {"categories", {{{"id", 1}, {"name", "person"}}}}, {"annotations", nlohmann::json::array()}
 };
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
   store_normalized_annotation_index(root.path() / "sequential.index", fixture_index(sequential), {});
   store_normalized_annotation_index(root.path() / "parallel.index", fixture_index(parallel), {});
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
 detail::StorageReservationPool pool{root.path(), storage_observer(sink)};
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
 CachedImageWriteProgress progress([&](const auto completed, const auto total) {
  CHECK(total == 400U);
  observed.push_back(completed);
 }, 16U, 384U);
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
 ProgressReporter progress([&](const auto& update) {
  latest = update;
  const auto count = update.sources[1].completed_images;
  counts.push_back(count);
  if (count == 128U) { first.receipt().ArriveAndWait(); }
 }, quiet);
 const auto source = BenchmarkDatasetSource::kObjects365V2;
 progress.phase(DatasetCompilePhase::Extracting);
 auto first_add = std::async(std::launch::async, [&] { progress.transfers().images(source, "first", 128U, 512U, progress); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { first.Release(); });
 REQUIRE(first.WaitEntered(3s));
 auto second_add = std::async(std::launch::async, [&] { progress.transfers().images(source, "second", 256U, 512U, progress); });
 first.Release();
 mmltk::testsupport::await_test_future(first_add, "first source addition", 5s);
 mmltk::testsupport::await_test_future(second_add, "second source addition", 5s);
 progress.flush();
 CHECK(latest.sources[1].completed_images == 384U);
 CHECK(std::ranges::is_sorted(counts));
 progress.transfers().images(source, "first", 0U, 512U, progress);
 progress.flush();
 CHECK(latest.sources[1].completed_images == 256U);
 progress.transfers().images(source, "first", 256U, 512U, progress);
 progress.flush();
 CHECK(latest.sources[1].completed_images == 512U);
 CHECK(latest.sources[1].invalidated_images == 128U);
 progress.transfers().images(source, "first", 256U, 512U, progress);
 progress.flush();
 CHECK(latest.sources[1].completed_images == 512U);  // Warm repeat contributes once.
 CHECK_THROWS_AS(progress.transfers().images(source, "first", 513U, 512U, progress), std::overflow_error);
 CHECK_THROWS_AS(progress.transfers().images(source, "first", std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max(), progress), std::overflow_error);
}
TEST_CASE("archive image reuse reports initial and resolved counts without replacing valid JPEGs", "[backend][data][benchmark][images]") {
 for (const std::size_t workers : {0U, 3U, 1U}) {
  mmltk::testsupport::ScopedTempDir root("progress-reuse");
  const auto archive = root.path() / "images.tar";
  const auto images = root.path() / "images";
  const auto jpeg = make_jpeg(10U, 20U, 30U);
  write_single_jpeg_tar(archive, 2U, jpeg);
  prepare_cached_image_directory(images);
  BenchmarkEncodedImage::publish(cached_image_path(images, 1U), jpeg, {});
  const RetainedArtifact retained{cached_image_path(images, 1U)};
  const std::vector<std::uint64_t> ids{1U, 2U};
  std::vector<std::uint64_t> counts;
  std::unique_ptr<BenchmarkCompilePipeline> execution;
  if (workers) execution = std::make_unique<BenchmarkCompilePipeline>(std::min(workers, mmltk::common::system::allowed_cpu_set().size()));
  ArchiveExtractionRequest request{
   .archive_path = archive,
   .source_identity = "fixture:reuse",
   .output_root = images,
   .source = "objects365",
   .shard = "patch-0",
   .selected_image_ids = ids,
   .image_id_parser = [](const std::string_view name) -> std::optional<std::uint64_t> { return name.ends_with("/2.jpg") ? std::optional<std::uint64_t>{2U} : std::nullopt; },
   .progress =
    [&](const auto completed, const auto total) {
   CHECK(total == 2U);
   counts.push_back(completed);
  },
   .validator =
    [&](const auto, const auto encoded) {
   if (!has_complete_image_markers(encoded)) { throw std::runtime_error("invalid cached JPEG"); }
   if (execution) {
    if (workers == 1U) require_condition(execution->execution().current_lane() == 0, "archive repair validation bypassed shared CPU admission");
    BenchmarkImageValidator probe;
    probe.validate_decodable(encoded, 16, 8);
   }
   return BenchmarkImageDecoder{}.read_header(encoded);
  },
   .decompression_workers = 0U,
   .execution = execution.get()
  };
  auto result = extract_selected_archive_images(request);
  REQUIRE_FALSE(counts.empty());
  CHECK(counts.front() == 1U);
  CHECK(counts.back() == 2U);
  CHECK(std::ranges::is_sorted(counts));
  CHECK(result.image_count == 2U);
  CHECK(result.selection_sha256 == cached_image_selection_digest(ids));
  if (execution) CHECK(execution->execution().try_reserve({1U << 20, 0}).has_value());
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
 BenchmarkEncodedImage::publish(cached_image_path(images, 1U), jpeg, {});
 BenchmarkEncodedImage::publish(cached_image_path(images, 2U), jpeg, {});
 const RetainedArtifact first{cached_image_path(images, 1U)};
 const RetainedArtifact second{cached_image_path(images, 2U)};
 NormalizedAnnotationBuilder index;
 index.source = BenchmarkDatasetSource::kOpenImagesV7;
 index.images.resize(2U);
 index.images[0].source_image_id = 1U;
 index.images[1].source_image_id = 2U;
 std::vector<QuarantinedImage> quarantined;
 const BenchmarkTraceSink quiet;
 BenchmarkCompileProgress latest;
 ProgressReporter progress([&](const auto& update) { latest = update; }, quiet);
 progress.phase(DatasetCompilePhase::Extracting);
 auto acquired = acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, {}, &progress, 1, 0U, quiet);
 REQUIRE(acquired.available_image_ids == std::vector<std::uint64_t>{1U, 2U});
 progress.flush();
 CHECK(latest.sources[2].completed_images == 2U);
 const auto first_proof = read_json_file(images / ".groups" / "group-000000.complete.json");
 const auto width = first_proof.at("dimensions").at(1).get<std::uint32_t>();
 const auto height = first_proof.at("dimensions").at(2).get<std::uint32_t>();
 CHECK(width > 0U);
 CHECK(height > 0U);
 for (auto& image : index.images) { image.width = image.height = 0U; }
 acquired = acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, {}, &progress, 1, 0U, quiet);
 CHECK(acquired.directory.cache_hit);
 progress.flush();
 CHECK(latest.sources[2].completed_images == 2U);
 CHECK(latest.sources[2].invalidated_images == 0U);
 const auto reused_proof = read_json_file(images / ".groups" / "group-000000.complete.json");
 CHECK(reused_proof.at("dimensions").at(1) == width);
 CHECK(reused_proof.at("dimensions").at(2) == height);
 CHECK(index.images[0].width == 0);
 CHECK(index.images[0].height == 0);
 const auto proof = images / ".groups" / "group-000000.complete.json";
 auto manifest = read_json_file(proof);
 for (const auto* field : {"identity", "selection_sha256"}) {
  manifest = read_json_file(proof);
  manifest[field] = "stale";
  write_json_atomically(proof, manifest, {});
  acquired = acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, {}, &progress, 1, 0U, quiet);
  CHECK_FALSE(acquired.directory.cache_hit);
 }
 manifest = read_json_file(proof);
 index.images.push_back(NormalizedImage{.source_image_id = 3U});
 const std::vector<std::uint64_t> requested{1U, 2U, 3U};
 manifest["requested_image_count"] = 3U;
 manifest["requested_selection_sha256"] = cached_image_selection_digest(requested);
 manifest["quarantined"] = {{{"image_id", 3U}, {"reason", "fixture unavailable image"}}};
 write_json_atomically(proof, manifest, {});
 acquired = acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, {}, &progress, 1, 0U, quiet);
 CHECK(acquired.directory.cache_hit);
 CHECK(acquired.available_image_ids == std::vector<std::uint64_t>{1U, 2U});
 REQUIRE(quarantined.size() == 1U);
 CHECK(quarantined.front().image_id == 3U);
 CHECK(quarantined.front().reason == "fixture unavailable image");
 progress.flush();
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
 reporter.flush();
 reporter.source_activity(BenchmarkDatasetSource::kObjects365V2, "Checking patch-17 cache");
 reporter.source_activity(BenchmarkDatasetSource::kObjects365V2, "Checking patch-17 cache");
 reporter.activity("Preparing labels");
 reporter.activity("Preparing labels");
 reporter.flush();
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
  BenchmarkCompilePipeline execution(std::min<std::size_t>(3, mmltk::common::system::allowed_cpu_set().size()), {}, {}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
  ArchiveExtractionRequest request{
   .archive_path = archive,
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
   .execution = &execution
  };
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
 CHECK(updates.front().transfer.completed_bytes == 0U);
 CHECK(updates.front().transfer.retained_bytes == 0U);
 CHECK_FALSE(updates.front().transfer.resumed);
 bool saw_retry = false;
 std::uint64_t previous = 0U;
 for (const auto& update : updates) {
  CHECK(update.transfer.completed_bytes >= previous);
  CHECK(update.transfer.completed_bytes <= bytes);
  CHECK(update.transfer.total_bytes == bytes);
  CHECK(update.transfer.retained_bytes <= update.transfer.completed_bytes);
  CHECK_FALSE(update.redownload);
  if (update.transfer.attempt > 1U) {
   saw_retry = true;
   CHECK(update.transfer.resumed);
   CHECK(update.transfer.retained_bytes >= HttpServer::partial_bytes);
  }
  previous = update.transfer.completed_bytes;
 }
 CHECK(saw_retry);
 CHECK(updates.back().transfer.completed_bytes == bytes);
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
 const auto downloaded = download_artifacts({request}, 2U, {}, [&](const DownloadProgress& update) {
  if (update.redownload) {
   saw_redownload = true;
   CHECK(update.transfer.retained_bytes == 0U);
   CHECK_FALSE(update.transfer.resumed);
  }
 }, trace);
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
 auto& totals = reporter.transfers();
 reporter.phase(DatasetCompilePhase::Downloading);
 const auto observe = [&](const BenchmarkDatasetSource source, const char* artifact, const std::uint64_t completed, const std::uint64_t total, const std::uint64_t expected_completed,
                       const std::uint64_t expected_total) {
  totals.update(DownloadProgress{.artifact_id = artifact, .transfer = {.completed_bytes = completed, .total_bytes = total}, .source = source}, reporter);
  reporter.flush();
  CHECK(latest.tracks.acquisition.completed == expected_completed);
  CHECK(latest.tracks.acquisition.total == expected_total);
  CHECK(latest.current_source == source);
 };
 constexpr auto coco = BenchmarkDatasetSource::kCoco2017;
 constexpr auto objects = BenchmarkDatasetSource::kObjects365V2;
 observe(coco, "known", 7U, 10U, 7U, 10U);
 observe(coco, "unknown", 4U, 0U, 11U, 0U);
 reporter.flush();
 CHECK(latest.sources[0].completed_bytes == 11U);
 CHECK(latest.sources[0].total_bytes == 0U);
 CHECK_FALSE(latest.sources[0].byte_total_known);
 observe(objects, "known", 8U, 20U, 19U, 0U);  // Source-qualified equal artifact names.
 observe(coco, "unknown", 2U, 0U, 17U, 0U);    // Explicit retry withdrawal.
 observe(coco, "unknown", 4U, 4U, 19U, 34U);
 reporter.flush();
 CHECK(latest.sources[0].byte_total_known);
 observe(objects, "unknown", 3U, 0U, 22U, 0U);
 observe(objects, "unknown", 3U, 3U, 22U, 37U);
 observe(coco, "known", 0U, 10U, 15U, 37U);  // Restart of a known artifact.
 observe(coco, "known", 10U, 10U, 25U, 37U);
 totals.update(DownloadProgress{"known", {.completed_bytes = 10U, .total_bytes = 10U, .attempt = 0U, .cache_hit = true, .resumed = false}}, reporter);
 reporter.flush();
 CHECK(latest.tracks.acquisition.completed == 25U);
 CHECK(latest.tracks.acquisition.total == 37U);
 CHECK_FALSE(latest.sources[0].cache_hit);  // Only one of this source's artifacts was reused.
 // A failed checked replacement leaves its prior contribution intact.
 CHECK_THROWS_AS(totals.update(DownloadProgress{"known", {.completed_bytes = std::numeric_limits<std::uint64_t>::max(), .total_bytes = 10U}}, reporter), std::overflow_error);
 observe(coco, "known", 10U, 10U, 25U, 37U);
 CHECK_THROWS_AS(totals.update(DownloadProgress{"known", {.completed_bytes = 10U, .total_bytes = std::numeric_limits<std::uint64_t>::max()}}, reporter), std::overflow_error);
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
 DownloadRequest request{"coco-observed", server.url("artifact"), root.path() / "artifact.bin", root.path() / "artifact.lock", unknown ? 0U : payload.size(), {}, 2U};
 if (invalid_request) {
  request.url = "unsupported-benchmark-protocol://artifact";
  if (segmented_probe) request.expected_size = 512ULL * 1024U * 1024U;
  bool local_failure = false;
  unsigned retries = 0;
  try {
   (void)download_artifacts({request}, segmented_probe ? 2U : 1U, {}, {}, [&](std::string_view event, const auto&) {
    if (event == "benchmark.download.attempt_failed" || event == "benchmark.download.segmented_probe_retry") ++retries;
   });
  } catch (const BenchmarkDownloadUnavailable&) { FAIL("invalid local protocol became optional source unavailability"); } catch (const std::runtime_error& error) {
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
  write_json_atomically(request.destination.string() + ".part.json", {{"schema_version", kBenchmarkCacheSchemaVersion}, {"url", request.url}, {"etag", "\"benchmark-test-etag\""}}, {});
 }
 std::vector<DownloadProgress> updates;
 const auto downloaded = download_artifacts({request}, 1U, {}, [&](const auto& update) { updates.push_back(update); });
 REQUIRE(downloaded.size() == 1U);
 REQUIRE_FALSE(updates.empty());
 for (const auto& update : updates) {
  CHECK(update.transfer.completed_bytes <= payload.size());
  CHECK((update.transfer.total_bytes == 0U || update.transfer.total_bytes == payload.size()));
  CHECK(update.transfer.retained_bytes == retained);
  CHECK(update.transfer.completed_bytes >= retained);
  if (retry && update.transfer.attempt == 1U) CHECK(update.transfer.completed_bytes == retained);
 }
 CHECK(updates.back().transfer.completed_bytes == payload.size());
 CHECK(updates.back().transfer.total_bytes == payload.size());
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
  CHECK(update.transfer.cache_hit);
  CHECK(update.transfer.completed_bytes == payload.size());
  CHECK(update.transfer.total_bytes == payload.size());
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
  CHECK_THROWS_AS(retry_annotation_indexing(cancellation,
                   [&] {
   ++bodies;
   throw detail::InsufficientStorage("fixture capacity");
  }, repair),
   detail::InsufficientStorage);
  CHECK(bodies == 1);
  CHECK(repairs == 0);
 }
 SECTION("metadata I/O failure never enters source repair") {
  CHECK_THROWS_AS(retry_annotation_indexing(cancellation,
                   [&] {
   ++bodies;
   (void)read_json_file(root.path());
  }, repair),
   std::system_error);
  CHECK(bodies == 1);
  CHECK(repairs == 0);
 }
 SECTION("allocation failure never enters source repair") {
  CHECK_THROWS_AS(retry_annotation_indexing(cancellation,
                   [&] {
   ++bodies;
   throw std::bad_alloc();
  }, repair),
   std::bad_alloc);
  CHECK(bodies == 1);
  CHECK(repairs == 0);
 }
 SECTION("cancellation never enters source repair") {
  CHECK_THROWS(retry_annotation_indexing(cancellation, [&] {
   ++bodies;
   cancel = true;
   throw std::runtime_error("interrupted parse");
  }, repair));
  CHECK(bodies == 1);
  CHECK(repairs == 0);
 }
 SECTION("source corruption has exactly three bodies and two repairs") {
  CHECK_THROWS(retry_annotation_indexing(cancellation, [&] {
   ++bodies;
   throw std::runtime_error("malformed source");
  }, [&](const std::exception&) { ++repairs; }));
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
TEST_CASE("durable artifact completion releases its lease before unrelated transfers finish", "[backend][data][benchmark][download]") {
 mmltk::testsupport::ScopedTempDir root("independent-artifacts");
 const auto payload = make_payload(1024U * 1024U);
 HttpServer blocked(payload), progressing(payload);
 blocked.GateNextTransfer();
 const std::vector requests{request_for(root.path(), "blocked", blocked.url("blocked"), payload), request_for(root.path(), "ready", progressing.url("ready"), payload)};
 std::promise<DownloadResult> admitted;
 auto acquired = std::async(std::launch::async, [&] {
  return download_artifacts(requests, 2, {}, {}, {}, [&](DownloadReady ready) {
   if (ready.request_index == 1) admitted.set_value(std::move(ready.artifact));
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  blocked.ReleasePartial();
  blocked.Stop();
  progressing.Stop();
 });
 REQUIRE(blocked.WaitPartial());
 const auto ready = mmltk::testsupport::await_test_promise(admitted, "independent artifact admission");
 CHECK(ready.path == requests[1].destination);
 CHECK(fs::file_size(ready.path) == payload.size());
 const mmltk::common::io::ScopedFd descriptor(::open(requests[1].lock_path.c_str(), O_RDWR | O_CLOEXEC));
 REQUIRE(descriptor.get() >= 0);
 REQUIRE(::flock(descriptor.get(), LOCK_EX | LOCK_NB) == 0);
 REQUIRE(::flock(descriptor.get(), LOCK_UN) == 0);
 CHECK(acquired.wait_for(0ms) == std::future_status::timeout);
 blocked.ReleasePartial();
 const auto results = mmltk::testsupport::await_test_future(acquired, "both artifacts");
 REQUIRE(results.size() == 2);
 CHECK(results[0].path == requests[0].destination);
 blocked.Check();
 progressing.Check();
}
TEST_CASE("progressive benchmark pixels survive quarantine without decoding retained sources twice", "[backend][data][benchmark][writer]") {
 mmltk::testsupport::ScopedTempDir root("progressive-sparse-slots");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), make_jpeg(240, 8, 8), {});
 BenchmarkEncodedImage::publish(cached_image_path(images, 2), make_jpeg(8, 240, 8), {});
 PreparedBenchmarkSplit membership;
 membership.name = "train";
 membership.class_names = {"person"};
 membership.sources = {{images}};
 // Cross a physical block boundary while decoding only the two survivors.
 const auto count = COMPILED_BLOCK_SLOTS + 2;
 membership.images.resize(count, EncodedImageRecord{99, 16, 8, 0, 0});
 for (std::size_t slot = 0; slot < membership.images.size(); ++slot) membership.images[slot].source_image_id = slot + 100;
 membership.images.front().source_image_id = 1;
 membership.images.back().source_image_id = 2;
 auto request = benchmark_write_request(membership, root.path() / "progressive.bin", 1);
 request.num_workers = 1;
 BenchmarkSplitWriter writer(request);
 writer.write_pixel(0, 0);
 writer.write_pixel(count - 1, 0);
 CHECK(writer.completed() == 2);
 PreparedBenchmarkSplit final = membership;
 final.images = {membership.images.front(), membership.images.back()};
 auto expected = benchmark_write_request(final, root.path() / "expected.bin", 1);
 expected.num_workers = 1;
 write_benchmark_split(expected);
 fs::remove_all(images);
 auto finish = benchmark_write_request(final, request.output_path, 1);
 finish.num_workers = 1;
 writer.write_remaining(finish);
 writer.finish(finish);
 const auto actual = CompiledDataset::open(request.output_path);
 const auto reference = CompiledDataset::open(expected.output_path);
 CHECK(actual.header().num_images == reference.header().num_images);
 CHECK(actual.header().num_slots == count);
 CHECK_FALSE(actual.eligible(1));
 CHECK(actual.image_entry(0).source_image_id == 1);
 CHECK(actual.image_entry(count - 1).source_image_id == 2);
 CHECK(std::memcmp(actual.image_pixels(0), reference.image_pixels(0), 3 * sizeof(float)) == 0);
 CHECK(std::memcmp(actual.image_pixels(count - 1), reference.image_pixels(1), 3 * sizeof(float)) == 0);
 CHECK(fs::file_size(request.output_path) > fs::file_size(expected.output_path));
}
TEST_CASE("dynamic fixed slots publish labels before pixels and admit exactly the usable capacity", "[backend][data][benchmark][writer]") {
 mmltk::testsupport::ScopedTempDir root("dynamic-fixed-slots");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), make_jpeg(240, 8, 8), {});
 BenchmarkEncodedImage::publish(cached_image_path(images, 2), make_jpeg(8, 240, 8), {});
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{images}};
 auto request = benchmark_write_request(split, root.path() / "result.bin", 512);
 request.num_workers = 1;
 BenchmarkSplitWriter writer(request);
 CHECK(writer.allocated_bytes() == 0);
 const EncodedImageRecord first{1, 16, 8, 1, 0};
 const auto first_slot = writer.allocate_slot(first, split.sources[0]);
 CHECK(first_slot == 0);
 PackedInstance label{};
 label.bbox_x2 = label.bbox_y2 = 512;
 label.flags = kAnnotationMask;
 label.source_ordinal = 17;
 const auto run_count = (COMPILED_LABEL_CAPACITY - annotation_layout(1, 0).run_offset) / sizeof(RLEPair);
 label.mask_rle_pairs = static_cast<std::uint16_t>(run_count);
 std::vector<RLEPair> runs(run_count);
 for (std::size_t i = 0; i < runs.size(); ++i) runs[i] = {static_cast<std::uint32_t>(2 * i), 1};
 ImageEntry entry{};
 entry.has_source_image_id = 1;
 entry.source_image_id = 1;
 entry.original_width = 16;
 entry.original_height = 8;
 writer.write_labels(first_slot, entry, {{&label, 1}, runs}, 100);
 CHECK_FALSE(writer.image_complete(first_slot));
 const auto label_allocation = writer.allocated_bytes();
 CHECK(label_allocation >= COMPILED_LABEL_CAPACITY);
 CHECK(label_allocation < 2 * COMPILED_LABEL_CAPACITY);
 runs.push_back({static_cast<std::uint32_t>(2 * run_count), 1});
 try {
  writer.write_labels(first_slot, entry, {{&label, 1}, runs});
  FAIL("oversized per-image payload was accepted");
 } catch (const std::runtime_error& error) {
  const std::string message = error.what();
  CHECK(message.find("image 1") != std::string::npos);
  CHECK(message.find("required 500008") != std::string::npos);
  CHECK(message.find("capacity 500000") != std::string::npos);
 }
 runs.pop_back();
 writer.write_pixel(first_slot, 0);
 // Settle the written pages and their filesystem extent metadata before
 // comparing physical allocation with a pure sparse-capacity extension.
 FileHandle staged;
 for (const auto& artifact : fs::directory_iterator(root.path()))
  if (artifact.path().filename().string().starts_with("result.bin.tmp.")) {
   staged = FileHandle::open_readonly(artifact.path().string());
   staged.sync_data();
  }
 REQUIRE(staged.get() >= 0);
 const auto extents = [&] {
  constexpr std::size_t capacity = 32;
  std::vector<std::byte> bytes(sizeof(fiemap) + capacity * sizeof(fiemap_extent));
  auto* map = reinterpret_cast<fiemap*>(bytes.data());
  map->fm_length = FIEMAP_MAX_OFFSET;
  map->fm_flags = FIEMAP_FLAG_SYNC;
  map->fm_extent_count = capacity;
  REQUIRE(::ioctl(staged.get(), FS_IOC_FIEMAP, map) == 0);
  REQUIRE(map->fm_mapped_extents < capacity);
  nlohmann::json result = nlohmann::json::array();
  for (std::uint32_t i = 0; i < map->fm_mapped_extents; ++i) {
   const auto& extent = map->fm_extents[i];
   result.push_back({extent.fe_logical, extent.fe_length, extent.fe_flags});
  }
  return result;
 };
 const auto before_extents = extents();
 const auto occupied = writer.allocated_bytes();
 for (std::uint32_t slot = 1; slot < COMPILED_BLOCK_SLOTS + 1; ++slot) CHECK(writer.allocate_slot({slot + 100, 16, 8, 0, 0}, split.sources[0]) == slot);
 const auto after_extents = extents();
 INFO("sparse growth extents before=" << before_extents.dump() << " after=" << after_extents.dump());
 CHECK(writer.allocated_bytes() == occupied);
 const EncodedImageRecord second{2, 16, 8, 0, 0};
 const auto second_slot = writer.allocate_slot(second, split.sources[0]);
 CHECK(second_slot == COMPILED_BLOCK_SLOTS + 1);
 writer.write_pixel(second_slot, 0);
 const bool trailing_unprepared = GENERATE(false, true);
 const auto final_slots = trailing_unprepared ? COMPILED_BLOCK_SLOTS * 2 + 1 : COMPILED_BLOCK_SLOTS + 2;
 for (std::uint32_t slot = COMPILED_BLOCK_SLOTS + 2; slot < final_slots; ++slot) CHECK(writer.allocate_slot({slot + 100, 16, 8, 0, 0}, split.sources[0]) == slot);
 split.images = {second, first};
 writer.finish(request);
 const auto source = CompiledDataset::open(request.output_path);
 REQUIRE(source.header().num_images == 2);
 REQUIRE(source.slot_count() == final_slots);
 CHECK(std::ranges::equal(source.eligible_slots(), std::array{first_slot, second_slot}));
 CHECK_THROWS_AS(source.annotations(1), std::out_of_range);
 CHECK(source.image_entry(first_slot).label_bytes == COMPILED_LABEL_CAPACITY);
 REQUIRE(source.image_labels(first_slot).size() == 1);
 CHECK(source.image_labels(first_slot)[0].source_ordinal == 117);
 CHECK(source.instance_rle(first_slot, source.image_labels(first_slot)[0]).size() == run_count);
 CHECK(source.image_labels(second_slot).empty());
 const auto stride = static_cast<std::size_t>(source.header().image_stride);
 const std::array reads{CompiledImageRead{second_slot, 0}, CompiledImageRead{first_slot, stride}, CompiledImageRead{second_slot, 2 * stride}};
 std::vector<std::byte> gathered(3 * stride);
 const std::atomic<bool> cancelled{false};
 REQUIRE(source.read_images(reads, gathered, cancelled));
 for (const auto& read : reads) CHECK(std::memcmp(gathered.data() + read.destination_offset, source.image_pixels(read.index), stride) == 0);
}
TEST_CASE("one-worker image readiness resizes before archive completion and reuses warm admission", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("image-readiness");
 const auto archive = root.path() / "images.tar";
 const auto images = root.path() / "images";
 const auto jpeg = make_jpeg(240, 8, 8);
 write_single_jpeg_tar(archive, 1, jpeg);
 PreparedBenchmarkSplit membership;
 membership.name = "train";
 membership.class_names = {"person"};
 membership.sources = {{images}};
 membership.images = {{1, 16, 8, 0, 0}};
 auto write = benchmark_write_request(membership, root.path() / "result.bin", 8);
 write.num_workers = 1;
 BenchmarkCompileProgress observed;
 const BenchmarkTraceSink quiet;
 ProgressReporter progress([&](const auto& value) { observed = value; }, quiet);
 progress.pixels(0, 1);
 write.progress = {.context = &progress, .image_completed = [](void* context) {
  static_cast<ProgressReporter*>(context)->pixel_completed();
 }, .images_invalidated = [](void* context, std::uint64_t count) { static_cast<ProgressReporter*>(context)->invalidate_pixels(count); }};
 BenchmarkSplitWriter writer(write);
 BenchmarkCompilePipeline pipeline(1);
 bind_benchmark_slots(pipeline, writer, membership);
 const std::array<std::uint64_t, 1> ids{1};
 std::size_t ready_count = 0;
 ArchiveExtractionRequest acquisition{
  .archive_path = archive,
  .source_identity = "ready-fixture",
  .output_root = images,
  .source = "coco",
  .shard = "train2017",
  .selected_image_ids = ids,
  .image_id_parser = [](std::string_view name) -> std::optional<std::uint64_t> { return name.ends_with("/1.jpg") ? std::optional<std::uint64_t>{1} : std::nullopt; },
  .decompression_workers = 0,
 };
 const auto publication = pipeline.source_publication(images, {});
 acquisition.image_ready = [&](const CachedImageReady& image) {
  publication(image);
  ++ready_count;
  if (ready_count == 1) {
   require_condition(writer.image_complete(0), "one-worker pixels did not progress inline");
   require_condition(!fs::exists(images / ".complete.json"), "image readiness waited for group publication");
  }
 };
 const auto cold = extract_selected_archive_images(acquisition);
 CHECK_FALSE(cold.cache_hit);
 pipeline.drain();
 CHECK(writer.completed() == 1);
 fs::remove(archive);
 const auto warm = extract_selected_archive_images(acquisition);
 CHECK(warm.cache_hit);
 CHECK(ready_count == 2);
 CHECK(writer.completed() == 1);
 progress.flush();
 CHECK(observed.tracks.pixels.completed == 1);
 CHECK(observed.tracks.pixels.invalidated == 0);
 writer.invalidate_source(images);
 progress.flush();
 CHECK(observed.tracks.pixels.completed == 0);
 CHECK(observed.tracks.pixels.invalidated == 1);
 writer.write_pixel(0, 0);
 progress.flush();
 CHECK(observed.tracks.pixels.completed == 1);
 CHECK(observed.tracks.pixels.complete);
 writer.finish(write);
}
TEST_CASE("cache readiness failure joins held publication and retains completed bytes", "[backend][data][benchmark][images][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("concurrent cache publication requires two eligible CPUs");
 mmltk::testsupport::ScopedTempDir root("cache-ready-failure");
 const auto archive = root.path() / "images.tar";
 const auto images = root.path() / "images";
 const auto jpeg = make_jpeg(240, 8, 8);
 {
  std::ofstream output(archive, std::ios::binary);
  for (const auto id : {1U, 2U}) write_jpeg_tar_entry(output, id, jpeg);
  const std::array<char, 1024> terminator{};
  output.write(terminator.data(), static_cast<std::streamsize>(terminator.size()));
  REQUIRE(output.good());
 }
 const std::array<std::uint64_t, 2> ids{1, 2};
 mmltk::testsupport::TestGate failing("failing cache publication"), held("held cache publication");
 std::promise<void> failure_delivered;
 BenchmarkCompilePipeline execution(2);
 ArchiveExtractionRequest request{
  .archive_path = archive,
  .source_identity = "ready-failure-fixture",
  .output_root = images,
  .source = "coco",
  .shard = "train2017",
  .selected_image_ids = ids,
  .image_id_parser = [](std::string_view name) -> std::optional<std::uint64_t> {
  if (name.ends_with("/1.jpg")) return 1;
  if (name.ends_with("/2.jpg")) return 2;
  return std::nullopt;
 },
  .decompression_workers = 0,
  .execution = &execution
 };
 request.image_ready = [&](const CachedImageReady& image) {
  require_condition(fs::file_size(cached_image_path(images, image.image_id)) == jpeg.size(), "readiness preceded durable image publication");
  if (image.image_id == 1) {
   failing.receipt().ArriveAndWait();
   failure_delivered.set_value();
   throw std::runtime_error("injected cache readiness failure");
  }
  held.receipt().ArriveAndWait();
 };
 auto extraction = std::async(std::launch::async, [&] { return extract_selected_archive_images(request); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  failing.Release();
  held.Release();
 });
 REQUIRE(failing.WaitEntered(2s));
 REQUIRE(held.WaitEntered(2s));
 failing.Release();
 mmltk::testsupport::await_test_promise(failure_delivered, "cache readiness failure");
 CHECK(extraction.wait_for(0ms) == std::future_status::timeout);
 CHECK_FALSE(fs::exists(images / ".complete.json"));
 held.Release();
 CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(extraction, "cache writer retirement"), "injected cache readiness failure");
 CHECK_FALSE(fs::exists(images / ".complete.json"));
 for (const auto id : ids) CHECK(fs::file_size(cached_image_path(images, id)) == jpeg.size());
 request.image_ready = {};
 const auto reused = extract_selected_archive_images(request);
 CHECK_FALSE(reused.cache_hit);
 CHECK(fs::is_regular_file(images / ".complete.json"));
 CHECK(extract_selected_archive_images(request).cache_hit);
}
TEST_CASE("cached pixels finish while label preparation is blocked", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("blocked-labels");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), make_jpeg(240, 8, 8), {});
 PreparedBenchmarkSplit membership;
 membership.name = "train";
 membership.class_names = {"person"};
 membership.sources = {{images}};
 membership.images = {{1, 16, 8, 0, 0}};
 std::promise<void> pixels_done;
 auto request = benchmark_write_request(membership, root.path() / "result.bin", 8);
 request.num_workers = 1;
 request.progress = {.context = &pixels_done, .image_completed = [](void* value) { static_cast<std::promise<void>*>(value)->set_value(); }};
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline pipeline(2);
 bind_benchmark_slots(pipeline, writer, membership);
 mmltk::testsupport::TestGate labels_gate("label preparation");
 auto labels = std::async(std::launch::async, [gate = labels_gate.receipt()] { gate.ArriveAndWait(); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { labels_gate.Release(); });
 REQUIRE(labels_gate.WaitEntered(2s));
 pipeline.source_publication(images, {})({1});
 mmltk::testsupport::await_test_promise(pixels_done, "cached pixel completion");
 CHECK(labels.wait_for(0ms) == std::future_status::timeout);
 pipeline.drain();
 CHECK(writer.completed() == 1);
 CHECK_FALSE(fs::exists(request.output_path));
 labels_gate.Release();
 mmltk::testsupport::await_test_future(labels, "label settlement");
 writer.finish(request);
 CHECK(fs::is_regular_file(request.output_path));
}
void add_cached_pixel_source(PreparedBenchmarkSplit& membership, const fs::path& images, std::uint64_t id = 1) {
 prepare_cached_image_directory(images);
 BenchmarkEncodedImage::publish(cached_image_path(images, id), make_jpeg(240, 8, 8), {});
 membership.sources.push_back({images});
}
PreparedBenchmarkSplit cached_pixel_membership(const fs::path& images) {
 PreparedBenchmarkSplit membership;
 membership.class_names = {"person"};
 add_cached_pixel_source(membership, images);
 return membership;
}
void check_preserved_split(const fs::path& output, const mmltk::common::io::Sha256Digest& original) {
 CHECK(mmltk::common::io::sha256_file(output) == original);
 const auto prefix = output.filename().string() + ".tmp.";
 for (const auto& item : fs::directory_iterator(output.parent_path())) CHECK_FALSE(item.path().filename().string().starts_with(prefix));
}
TEST_CASE("queued pixel custody retains the source lease until its reader drains", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("pixel-custody");
 const auto images = root.path() / "images";
 auto membership = cached_pixel_membership(images);
 membership.images = {{1, 16, 8, 0, 0}};
 mmltk::testsupport::TestGate reader("pixel reader retirement");
 auto receipt = reader.receipt();
 auto request = benchmark_write_request(membership, root.path() / "result.bin", 8);
 request.num_workers = 1;
 request.progress = {.context = &receipt, .image_completed = [](void* value) { static_cast<mmltk::testsupport::TestGate::Receipt*>(value)->ArriveAndWait(); }};
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline pipeline(2);
 bind_benchmark_slots(pipeline, writer, membership);
 const auto lock = root.path() / "source.lock";
 auto custody = std::make_shared<ArtifactLease>(ArtifactLease::acquire(lock, {}));
 pipeline.source_publication(images, custody)({1});
 custody.reset();
 const mmltk::testsupport::ScopedTestCleanup release([&] { reader.Release(); });
 REQUIRE(reader.WaitEntered(2s));
 const auto probe = mmltk::testsupport::probe_file_lock(lock);
 CHECK_FALSE(probe.available);
 reader.Release();
 pipeline.drain();
 REQUIRE(::flock(probe.descriptor.get(), LOCK_EX | LOCK_NB) == 0);
 REQUIRE(::flock(probe.descriptor.get(), LOCK_UN) == 0);
}
TEST_CASE("cancelled progressive pixels leave the previously published file intact", "[backend][data][benchmark][writer]") {
 mmltk::testsupport::ScopedTempDir root("pixel-cancellation");
 const auto images = root.path() / "images";
 auto membership = cached_pixel_membership(images);
 membership.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 0}};
 const auto output = root.path() / "result.bin";
 write_text(output, "old generation");
 const auto original = mmltk::common::io::sha256_file(output);
 std::atomic<bool> cancelled{false};
 auto request = benchmark_write_request(membership, output, 8);
 request.num_workers = 1;
 request.overwrite = true;
 request.cancel_requested = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 {
  BenchmarkSplitWriter writer(request);
  writer.write_pixel(0, 0);
  cancelled.store(true);
  CHECK_THROWS(writer.write_pixel(1, 0));
  CHECK_THROWS(writer.finish(request));
 }
 check_preserved_split(output, original);
}
TEST_CASE("pipeline failure retires queued custody and drains active readers before reporting", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires three live class floors and an elastic CPU");
 mmltk::testsupport::ScopedTempDir root("exceptional-pixel-drain");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 for (std::uint64_t id : {1U, 2U, 3U}) BenchmarkEncodedImage::publish(cached_image_path(images, id), make_jpeg(240, 8, 8), {});
 PreparedBenchmarkSplit membership;
 membership.class_names = {"person"};
 membership.sources = {{images}};
 membership.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 0}, {3, 16, 8, 0, 0}};
 mmltk::testsupport::TestGate first("first reader failure"), second("second reader drain");
 struct Completion {
  mmltk::testsupport::TestGate::Receipt first, second;
  std::atomic<unsigned> count{0};
 } completion{first.receipt(), second.receipt()};
 auto request = benchmark_write_request(membership, root.path() / "result.bin", 8);
 request.num_workers = 2;
 request.progress = {.context = &completion, .image_completed = [](void* opaque) {
  auto& state = *static_cast<Completion*>(opaque);
  if (state.count.fetch_add(1) == 0) {
   state.first.ArriveAndWait();
   throw std::runtime_error("injected pixel completion failure");
  }
  state.second.ArriveAndWait();
 }};
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline pipeline(4, {}, {}, {}, mmltk::backend::data::testsupport::benchmark_trace());
 bind_benchmark_slots(pipeline, writer, membership);
 const mmltk::testsupport::ScopedTestCleanup release_readers([&] {
  first.Release();
  second.Release();
 });
 pipeline.source_publication(images, {})({1});
 REQUIRE(first.WaitEntered(2s));
 pipeline.source_publication(images, {})({2});
 REQUIRE(second.WaitEntered(2s));
 auto queued_retired = std::make_shared<std::promise<void>>();
 auto queued = std::shared_ptr<const ArtifactLease>(new ArtifactLease(ArtifactLease::acquire(root.path() / "queued.lock", {})), [queued_retired](const ArtifactLease* lease) {
  delete lease;
  queued_retired->set_value();
 });
 pipeline.source_publication(images, queued)({3});
 queued.reset();
 auto drain = std::async(std::launch::async, [&] { pipeline.drain(); });
 const mmltk::testsupport::ScopedTestCleanup release_before_join([&] {
  first.Release();
  second.Release();
 });
 first.Release();
 mmltk::testsupport::await_test_promise(*queued_retired, "failed attempt queued custody retirement");
 CHECK(drain.wait_for(0ms) == std::future_status::timeout);
 second.Release();
 CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(drain, "exceptional reader drain"), "injected pixel completion failure");
 CHECK(writer.completed() == 2);
 CHECK_FALSE(writer.image_complete(2));
 CHECK_THROWS_WITH(pipeline.source_publication(images, {})({3}), "injected pixel completion failure");
 CHECK_FALSE(fs::exists(request.output_path));
}
TEST_CASE("writer retains readable source geometry when its body fails", "[backend][data][benchmark][writer]") {
 mmltk::testsupport::ScopedTempDir root("readable-header");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 std::vector<std::uint8_t> encoded;
 const std::array<std::uint8_t, 6 * 4 * 3> rgb{};
 REQUIRE(stbi_write_png_to_func(append_bytes, &encoded, 6, 4, 3, rgb.data(), 6 * 3) != 0);
 const auto valid = encoded;
 encoded.resize(41);  // Keep the IDAT header required by stbi_info, but no image data.
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), encoded, {});
 PreparedBenchmarkSplit split;
 split.class_names = {"person"};
 split.sources = {{images}};
 split.images = {{1, 3, 3, 0, 0}};
 auto request = benchmark_write_request(split, root.path() / "result.bin", 3);
 request.num_workers = 1;
 BenchmarkSplitWriter writer(request, true);
 CHECK_THROWS_AS(writer.write_pixel(0, 0), BenchmarkImageReadError);
 CHECK_FALSE(writer.image_complete(0));
 REQUIRE(writer.header_dimensions(0).has_value());
 CHECK(*writer.header_dimensions(0) == std::pair<std::uint32_t, std::uint32_t>{6, 4});
 CHECK_THROWS(writer.dimensions(0));
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), valid, {});
 writer.write_pixel(0, 0);
 CHECK(writer.image_complete(0));
 split.images[0].source_width = 6;
 split.images[0].source_height = 4;
 writer.finish(request);
 CHECK(CompiledDataset::open(request.output_path).image_entry(0).original_width == 6);
}
TEST_CASE("settled writer tail expands from one overlap scratch lane to the full assigned budget", "[backend][data][benchmark][writer]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPU lanes");
 mmltk::testsupport::ScopedTempDir root("full-pixel-tail");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 for (std::uint64_t id : {1U, 2U}) BenchmarkEncodedImage::publish(cached_image_path(images, id), make_jpeg(240, 8, 8), {});
 PreparedBenchmarkSplit split;
 split.class_names = {"person"};
 split.sources = {{images}};
 split.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 0}};
 mmltk::testsupport::TestGate lanes("settled pixel lanes");
 auto receipt = lanes.receipt();
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 request.num_workers = 1;
 request.progress = {.context = &receipt, .image_completed = [](void* opaque) { static_cast<mmltk::testsupport::TestGate::Receipt*>(opaque)->ArriveAndWait(); }};
 BenchmarkSplitWriter writer(request);
 request.num_workers = 2;
 auto tail = std::async(std::launch::async, [&] { writer.write_remaining(request); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { lanes.Release(); });
 REQUIRE(lanes.WaitEntered(2s, 2));
 CHECK(tail.wait_for(0ms) == std::future_status::timeout);
 lanes.Release();
 mmltk::testsupport::await_test_future(tail, "full-budget pixel tail");
 CHECK(writer.completed() == 2);
 writer.finish(request);
}
TEST_CASE("concurrent tracks preserve unique work through repair and settlement", "[backend][data][benchmark][progress]") {
 BenchmarkCompileProgress latest;
 const BenchmarkTraceSink quiet;
 ProgressReporter progress([&](const auto& update) { latest = update; }, quiet);
 progress.phase(DatasetCompilePhase::Indexing, 4, 6);
 progress.phase(DatasetCompilePhase::Extracting);
 progress.transfers().update(DownloadProgress{.artifact_id = "annotations", .transfer = {.completed_bytes = 8, .total_bytes = 10}}, progress);
 progress.pixels(0, 2);
 progress.pixel_completed();
 progress.flush();
 CHECK(latest.tracks.acquisition.active);
 CHECK(latest.tracks.labels.active);
 CHECK(latest.tracks.pixels.active);
 CHECK(latest.tracks.labels.completed == 4);
 CHECK(latest.tracks.pixels.completed == 1);
 progress.transfers().update(DownloadProgress{.artifact_id = "images", .transfer = {.completed_bytes = 3}}, progress);
 progress.flush();
 CHECK(latest.tracks.acquisition.completed == 11);
 CHECK_FALSE(latest.tracks.acquisition.total_known);
 const auto foreground = latest;
 progress.phase(DatasetCompilePhase::Indexing, 3, 6);
 progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Background masks", false);
 progress.flush();
 CHECK(latest.phase == foreground.phase);
 CHECK(latest.activity == foreground.activity);
 CHECK(latest.completed == foreground.completed);
 CHECK(latest.total == foreground.total);
 CHECK(latest.tracks.labels.completed == 4);
 CHECK(latest.tracks.pixels.completed == 1);
 progress.invalidate_pixels(1);
 progress.flush();
 CHECK(latest.tracks.pixels.completed == 0);
 CHECK(latest.tracks.pixels.invalidated == 1);
 progress.pixel_completed();
 progress.pixels(1, 2);  // Retained successful pixels, no attempt or copy contribution.
 progress.pixel_completed();
 progress.flush();
 CHECK(latest.tracks.pixels.completed == 2);
 CHECK(latest.tracks.pixels.complete);
 CHECK(latest.tracks.labels.active);
 progress.phase(DatasetCompilePhase::Indexing, 6, 6);
 progress.label_plans(2);
 progress.label_plan_started(0);
 progress.label_plan_completed(0);
 progress.label_plan_started(1);
 progress.label_plan_completed(1);
 progress.flush();
 CHECK(latest.tracks.labels.completed == 6);
 progress.discard_label_plans();
 progress.label_plans(2);
 progress.flush();
 CHECK(latest.tracks.labels.completed == 6);
 CHECK(latest.tracks.labels.invalidated == 0);
 progress.label_plan_started(0);
 progress.label_plan_completed(0);
 progress.label_plan_completed(0);
 progress.label_plan_started(1);
 progress.label_plan_completed(1);
 progress.flush();
 CHECK(latest.tracks.labels.completed == 6);
 CHECK(latest.tracks.labels.complete);
 CHECK(latest.tracks.acquisition.active);
 progress.acquisition_complete();
 progress.flush();
 CHECK(latest.tracks.acquisition.complete);
 progress.phase(DatasetCompilePhase::Publishing, 1, 1);
 progress.flush();
 CHECK(latest.tracks.valid());
 CHECK(latest.tracks.pixels.invalidated == 1);
}
TEST_CASE("compile-owned observations survive preparation replacement and source interleaving", "[backend][data][benchmark][progress]") {
 BenchmarkCompileProgress latest;
 const BenchmarkTraceSink quiet;
 ProgressReporter reporter([&](const auto& update) { latest = update; }, quiet);
 const std::array<std::uint64_t, 2> rows{100, 100};
 auto* first = reporter.indexing(rows);
 REQUIRE(first != nullptr);
 first->update(0, 100, reporter);
 auto* replacement = reporter.indexing(rows);
 CHECK(replacement == first);
 replacement->update(1, 100, reporter);
 reporter.flush();
 CHECK(latest.tracks.labels.completed == 200);
 replacement->update(0, 100, reporter);
 reporter.flush();
 CHECK(latest.tracks.labels.completed == 200);
 auto& transfers = reporter.transfers();
 transfers.update(DownloadProgress{.artifact_id = "annotations", .transfer = {.completed_bytes = 8, .total_bytes = 8, .attempt = 3, .resumed = true}}, reporter);
 transfers.update(DownloadProgress{.artifact_id = "archive", .transfer = {.completed_bytes = 10, .total_bytes = 20, .attempt = 2}}, reporter);
 reporter.flush();
 CHECK(latest.sources[0].retry_count == 3);
 CHECK(latest.sources[0].resumed);
 CHECK(latest.tracks.acquisition.completed == 18);
 transfers.update(DownloadProgress{.artifact_id = "annotations", .transfer = {.completed_bytes = 8, .total_bytes = 8, .cache_hit = true}}, reporter);
 reporter.flush();
 CHECK(latest.sources[0].retry_count == 3);
 CHECK(latest.tracks.acquisition.completed == 18);
 ProgressReporter disabled({}, quiet);
 CHECK(disabled.indexing(rows) == nullptr);
}
TEST_CASE("sparse pixel staging reserves occupied extents while preserving the published output", "[backend][data][benchmark][storage]") {
 mmltk::testsupport::ScopedTempDir root("pixel-storage-admission");
 const auto output = root.path() / "result.bin";
 write_text(output, "published generation");
 const auto published = mmltk::common::io::sha256_file(output);
 struct statvfs capacity{};
 REQUIRE(::statvfs(root.path().c_str(), &capacity) == 0);
 constexpr std::uint64_t stride = std::uint64_t{MAX_IMAGE_EXTENT} * MAX_IMAGE_EXTENT * 3 * sizeof(float);
 const auto total = static_cast<std::uint64_t>(capacity.f_blocks) * capacity.f_frsize;
 const auto count = total / stride + 1;
 REQUIRE(count < 1'000'000);
 PreparedBenchmarkSplit split;
 split.class_names = {"person"};
 split.sources = {{root.path()}};
 split.images.resize(count, EncodedImageRecord{1, 16, 8, 0, 0});
 auto request = benchmark_write_request(split, output, MAX_IMAGE_EXTENT);
 request.num_workers = 1;
 request.overwrite = true;
 for (std::size_t slot = 0; slot < split.images.size(); ++slot) split.images[slot].source_image_id = slot + 1;
 BenchmarkSplitWriter writer(request);
 CHECK(writer.allocated_bytes() == 0);
 CHECK(mmltk::common::io::sha256_file(output) == published);
}
TEST_CASE("preparation indexing totals preserve successful rows across owner replacement", "[backend][data][benchmark][progress]") {
 std::vector<BenchmarkCompileProgress> updates;
 const BenchmarkTraceSink quiet;
 ProgressReporter reporter([&](const auto& value) { updates.push_back(value); }, quiet);
 const std::array<std::uint64_t, 2> rows{241602, 5000};
 IndexingProgressTotals attempt(rows);
 attempt.update(0, 0, reporter);
 reporter.flush();
 attempt.update(0, 4096, reporter);
 reporter.flush();
 attempt.update(1, 64, reporter);
 reporter.flush();
 attempt.update(0, 4160, reporter);
 reporter.flush();
 attempt.update(1, 0, reporter);  // Release-local parser retry retains admitted work.
 reporter.flush();
 attempt.update(0, rows[0], reporter);
 reporter.flush();
 CHECK_THROWS_AS(attempt.update(1, rows[1] + 1, reporter), std::overflow_error);
 attempt.update(1, rows[1], reporter);
 reporter.flush();
 REQUIRE(updates.size() == 6);
 CHECK(updates.front().completed == 0);
 CHECK(updates[2].completed == 4160);
 CHECK(updates[3].completed == 4224);
 // The unchanged retry count produces no replaceable snapshot.
 std::uint64_t previous = 0;
 for (const auto& value : updates) {
  CHECK(value.tracks.labels.total == rows[0] + rows[1]);
  CHECK(value.tracks.labels.completed >= previous);
  CHECK(value.tracks.labels.completed <= value.tracks.labels.total);
  previous = value.tracks.labels.completed;
 }
 reporter.flush();
 CHECK(updates.back().tracks.labels.completed == updates.back().tracks.labels.total);
 IndexingProgressTotals replacement(rows);
 replacement.update(0, 0, reporter);
 reporter.flush();
 CHECK(updates.back().tracks.labels.completed == rows[0] + rows[1]);
 CHECK(updates.back().tracks.labels.total == rows[0] + rows[1]);
}
TEST_CASE("registered pixel readiness remains nonblocking under capacity pressure and duplicate delivery", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("registered-pixel-readiness");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 PreparedBenchmarkSplit membership;
 membership.class_names = {"person"};
 membership.sources = {{images}};
 for (std::uint64_t id = 1; id <= 18; ++id) {
  BenchmarkEncodedImage::publish(cached_image_path(images, id), make_jpeg(240, 8, 8), {});
  membership.images.push_back({id, 16, 8, 0, 0});
 }
 mmltk::testsupport::TestGate reader("first ready reader");
 const auto receipt = reader.receipt();
 std::atomic<unsigned> reads{0};
 auto request = benchmark_write_request(membership, root.path() / "result.bin", 8);
 request.num_workers = 1;
 request.image_opened = [&](const fs::path&, std::uint64_t id) {
  reads.fetch_add(1);
  if (id <= 2) receipt.ArriveAndWait();
 };
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline pipeline(2);
 bind_benchmark_slots(pipeline, writer, membership);
 auto lease = std::make_shared<ArtifactLease>(ArtifactLease::acquire(root.path() / "source.lock", {}));
 std::weak_ptr<ArtifactLease> custody = lease;
 auto publication = pipeline.source_publication(images, lease);
 publication({1});
 const mmltk::testsupport::ScopedTestCleanup release_reader([&] { reader.Release(); });
 REQUIRE(reader.WaitEntered(2s));
 auto producer = std::async(std::launch::async, [&] {
  for (const auto& image : membership.images) {
   publication({image.source_image_id});
   publication({image.source_image_id});
  }
  publication({999});  // Unselected readiness has no slot.
 });
 const mmltk::testsupport::ScopedTestCleanup release_before_producer([&] { reader.Release(); });
 mmltk::testsupport::await_test_future(producer, "membership-bounded readiness admission");
 publication = {};
 lease.reset();
 CHECK_FALSE(custody.expired());
 CHECK(reads.load() >= 1);
 CHECK(reads.load() <= 2);
 auto drain = std::async(std::launch::async, [&] { pipeline.drain(); });
 const mmltk::testsupport::ScopedTestCleanup release_before_drain([&] { reader.Release(); });
 CHECK(drain.wait_for(0ms) == std::future_status::timeout);
 reader.Release();
 mmltk::testsupport::await_test_future(drain, "registered readiness drain");
 CHECK(custody.expired());
 CHECK(reads.load() == membership.images.size());
 CHECK(writer.completed() == membership.images.size());
 pipeline.source_publication(images, {})({1});
 pipeline.drain();
 CHECK(reads.load() == membership.images.size());
 writer.finish(request);
}
TEST_CASE("pixel consumer startup rejects invalid CPU custody without stranded workers", "[backend][data][benchmark][pipeline]") {
 const auto permitted = mmltk::common::system::allowed_cpu_set();
 REQUIRE_FALSE(permitted.empty());
 const std::array<int, 2> invalid{permitted.front(), -1};
 CHECK_THROWS_AS(BenchmarkCompilePipeline(2, invalid), std::invalid_argument);
 const std::array<int, 1> undersized{permitted.front()};
 CHECK_THROWS_AS(BenchmarkCompilePipeline(2, undersized), std::invalid_argument);
 BenchmarkCompilePipeline serial(1, undersized);
 serial.drain();
}
TEST_CASE("unrelated compile failure discards queued pixels before joining active readers", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires extraction and pixel floors plus one elastic CPU");
 mmltk::testsupport::ScopedTempDir root("non-pixel-unwind");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 PreparedBenchmarkSplit membership;
 membership.class_names = {"person"};
 membership.sources = {{images}};
 for (std::uint64_t id = 1; id <= 5; ++id) {
  BenchmarkEncodedImage::publish(cached_image_path(images, id), make_jpeg(240, 8, 8), {});
  membership.images.push_back({id, 16, 8, 0, 0});
 }
 const auto output = root.path() / "result.bin";
 write_text(output, "previously published generation");
 const auto published = mmltk::common::io::sha256_file(output);
 mmltk::testsupport::TestGate reader("active reader during unrelated failure"), second("second active reader during unrelated failure");
 mmltk::testsupport::TestGate extraction_lane("held extraction floor during scope unwind");
 struct PixelCompletion {
  mmltk::testsupport::TestGate& first;
  mmltk::testsupport::TestGate& second;
  std::atomic<unsigned> count{0};
 } completion{reader, second};
 std::atomic<unsigned> opened{0};
 auto request = benchmark_write_request(membership, output, 8);
 request.num_workers = 1;
 request.overwrite = true;
 // No cancellation observation is supplied: scope unwind alone must stop work.
 request.image_opened = [&](const fs::path&, std::uint64_t) { opened.fetch_add(1); };
 request.progress = {.context = &completion, .image_completed = [](void* opaque) {
  auto& state = *static_cast<PixelCompletion*>(opaque);
  (state.count.fetch_add(1) == 0 ? state.first : state.second).receipt().ArriveAndWait();
 }};
 BenchmarkSplitWriter writer(request);
 auto active = std::make_shared<ArtifactLease>(ArtifactLease::acquire(root.path() / "active.lock", {}));
 const std::weak_ptr<ArtifactLease> active_custody = active;
 auto discarded = std::make_shared<std::promise<void>>();
 auto queued = std::shared_ptr<const ArtifactLease>(new ArtifactLease(ArtifactLease::acquire(root.path() / "queued.lock", {})), [discarded](const ArtifactLease* lease) {
  delete lease;
  discarded->set_value();
 });
 const std::weak_ptr<const ArtifactLease> queued_custody = queued;
 auto failed = std::async(std::launch::async, [&, active = std::move(active), queued = std::move(queued)]() mutable {
  BenchmarkCompilePipeline pipeline(3);
  bind_benchmark_slots(pipeline, writer, membership);
  pipeline.source_publication(images, active)({1});
  require_condition(reader.WaitEntered(2s), "active pixel reader did not open");
  pipeline.source_publication(images, active)({2});
  active.reset();
  require_condition(second.WaitEntered(2s), "second pixel reader did not open");
  auto held_extraction = pipeline.execution().submit({.kind = CompileTaskKind::Header, .work = [&](std::size_t, const CompileAllowance&) { extraction_lane.receipt().ArriveAndWait(); }});
  require_condition(extraction_lane.WaitEntered(2s), "extraction floor did not enter its physical hold");
  for (std::uint64_t id = 3; id <= 5; ++id) pipeline.source_publication(images, queued)({id});
  queued.reset();
  throw std::runtime_error("injected non-pixel output admission failure");
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  reader.Release();
  second.Release();
  extraction_lane.Release();
 });
 // Both consumers are still held. This receipt therefore proves destruction
 // has begun and discarded every queued lease without needing that consumer.
 mmltk::testsupport::await_test_promise(*discarded, "exception-time queued custody retirement");
 CHECK(queued_custody.expired());
 CHECK_FALSE(active_custody.expired());
 CHECK(opened.load() == 2);
 CHECK(failed.wait_for(0ms) == std::future_status::timeout);
 reader.Release();
 second.Release();
 extraction_lane.Release();
 CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(failed, "unrelated exception reader join"), "injected non-pixel output admission failure");
 CHECK(active_custody.expired());
 CHECK(opened.load() == 2);
 CHECK(writer.completed() == 2);
 for (std::size_t slot = 2; slot < membership.images.size(); ++slot) CHECK_FALSE(writer.image_complete(slot));
 CHECK(mmltk::common::io::sha256_file(output) == published);
}
TEST_CASE("source transfer facts follow represented activity independently of aggregate work", "[backend][data][benchmark][progress]") {
 BenchmarkCompileProgress latest;
 const BenchmarkTraceSink quiet;
 ProgressReporter reporter([&](const auto& update) { latest = update; }, quiet);
 reporter.phase(DatasetCompilePhase::Downloading);
 const DownloadProgress download{.artifact_id = "train-patch", .transfer = {.completed_bytes = 4096, .total_bytes = 8192, .retained_bytes = 1024, .attempt = 2, .resumed = true}};
 reporter.transfers().update(download, reporter);
 reporter.flush();
 REQUIRE(latest.sources.front().transfer);
 CHECK(latest.sources.front().transfer->valid());
 CHECK(latest.sources.front().activity == "Resuming train-patch");
 CHECK(*latest.sources.front().transfer == download.transfer);
 reporter.source_images(download.source, 2, 3);
 reporter.flush();
 CHECK(latest.sources.front().transfer == download.transfer);
 reporter.source_activity(download.source, "Resuming train-patch");
 reporter.flush();
 CHECK(latest.sources.front().transfer == download.transfer);
 reporter.source_activity(download.source, "Extracting train-patch");
 reporter.flush();
 CHECK_FALSE(latest.sources.front().transfer);
 reporter.transfers().update(download, reporter);
 reporter.source_images(download.source, 3, 3, "Normalizing annotations");
 reporter.flush();
 CHECK_FALSE(latest.sources.front().transfer);
 reporter.transfers().update(DownloadProgress{.artifact_id = "metadata", .transfer = {.completed_bytes = 512}}, reporter);
 reporter.flush();
 REQUIRE(latest.sources.front().transfer);
 CHECK(latest.sources.front().transfer->total_bytes == 0);
 CHECK(latest.sources.front().transfer->completed_bytes == 512);
 CHECK(latest.sources.front().completed_bytes == 4608);
 CHECK_FALSE(latest.sources.front().byte_total_known);
 CHECK(format_benchmark_source_status(latest.sources.front(), "Acquiring").find("512 bytes (total unknown)") != std::string::npos);
 reporter.source_complete(download.source, true);
 reporter.flush();
 CHECK_FALSE(latest.sources.front().transfer);
 CHECK(latest.sources.front().complete);
 CHECK(latest.sources.front().retry_count == 1);
 CHECK(latest.sources.front().resumed);
}
TEST_CASE("shared benchmark admission preserves consumer capacity and oversized work on one CPU", "[backend][data][benchmark][pipeline]") {
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32, .descriptors = 4});
 // One allowance covers a held input and the scratch/output its consumer needs.
 // Shared custody is one charge; the producer does not reacquire child credits.
 auto input = execution.execution().reserve({24, 1, true});
 auto borrowed = input;
 CHECK_FALSE(execution.execution().try_reserve({9, 0}).has_value());
 bool consumed = false, rejected_wait = false;
 execution.execution().run(CompileTaskKind::Normalize, {}, [&](std::size_t lane) {
  require_condition(lane == 0, "one CPU admission selected a different lane");
  consumed = true;
  try {
   (void)execution.execution().reserve({1, 0});
  } catch (const std::logic_error&) { rejected_wait = true; }
 }, borrowed);
 CHECK(consumed);
 CHECK(rejected_wait);
 input = {};
 CHECK_FALSE(execution.execution().try_reserve({9, 0}).has_value());
 borrowed = {};
 bool oversized = false;
 execution.execution().run(CompileTaskKind::Pixels, {33, 0}, [&](std::size_t) { oversized = true; });
 CHECK(oversized);
 CHECK(execution.execution().try_reserve({32, 4}).has_value());
 // A retained source lease remains charged while its oversized consumer runs.
 auto custody = execution.execution().reserve(CompileResources::handles(1, true));
 CHECK(execution.execution().try_reserve(CompileResources::handles(1, true)).has_value());
 bool extra_workspace = false;
 execution.execution().run(CompileTaskKind::Pixels, {33, 0}, [&](std::size_t) { extra_workspace = execution.execution().try_reserve({1, 0}).has_value(); });
 CHECK_FALSE(extra_workspace);
 custody = {};
 CHECK(execution.execution().try_reserve({32, 4}).has_value());
 execution.drain();
}
TEST_CASE("descriptor producer admission leaves handles for an admitted consumer", "[backend][data][benchmark][pipeline]") {
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32, .descriptors = 4});
 auto source = execution.execution().reserve({0, 3, true});
 CHECK_FALSE(execution.execution().try_reserve({0, 1, true}).has_value());
 auto completion = execution.execution().try_reserve({0, 1});
 REQUIRE(completion.has_value());
 bool consumed = false;
 execution.execution().run(CompileTaskKind::Header, {}, [&](std::size_t) { consumed = true; }, *completion);
 CHECK(consumed);
}
TEST_CASE("same-stage ready work uses remaining capacity beside a held consumer", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 CompileResources held{40, 0}, head{48, 0}, later{16, 0};
 bool preowned = false, external_cpu = false;
 SECTION("byte capacity") {}
 SECTION("descriptor capacity") {
  held = {0, 5};
  head = {0, 6};
  later = {0, 2};
 }
 SECTION("later work already owns a dependent allowance") {
  preowned = true;
  later.descriptors = 2;
 }
 SECTION("oversized head requires exclusive workspace") { head.bytes = 65; }
 SECTION("an external CPU grant leaves only the gated consumer lane") { external_cpu = true; }
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline execution(2, {}, {.transient_bytes = 64, .descriptors = 8}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 auto parent = preowned ? execution.execution().reserve({0, 1, false, 0, false, 2}) : CompileAllowance{};
 auto allowance = preowned ? execution.execution().reserve(later, parent) : CompileAllowance{};
 auto external = external_cpu ? execution.execution().reserve({40, 0, false, 1}) : CompileAllowance{};
 mmltk::testsupport::TestGate holder("independent retained resources"), cpu("remaining CPU");
 std::atomic<bool> head_ran{false}, later_ran{false};
 std::array<std::future<void>, 4> work;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  holder.Release();
  cpu.Release();
  execution.execution().notify_admission_change();
 });
 if (!external_cpu) {
  work[0] = std::async(std::launch::async, [&] { execution.execution().run(CompileTaskKind::Archive, held, [&](std::size_t) { holder.receipt().ArriveAndWait(); }); });
  REQUIRE(holder.WaitEntered(2s));
 }
 work[1] = std::async(std::launch::async, [&] { execution.execution().run(CompileTaskKind::Archive, {}, [&](std::size_t) { cpu.receipt().ArriveAndWait(); }); });
 REQUIRE(cpu.WaitEntered(2s));
 queue_benchmark_work(execution, work[2], [&] {
  execution.execution().run(CompileTaskKind::Normalize, head, [&](std::size_t) {
   head_ran.store(true);
   require_condition(!execution.execution().try_reserve(head), "head execution did not retain its full demand");
  });
 });
 queue_benchmark_work(execution, work[3], [&] {
  execution.execution().run(CompileTaskKind::Normalize, later, [&](std::size_t) {
   require_condition(!head_ran.load(), "blocked head ran while the independent holder remained live");
   later_ran.store(true);
  }, allowance);
 });
 CHECK(work[3].wait_for(0ms) == std::future_status::timeout);
 cpu.Release();
 mmltk::testsupport::await_test_future(work[3], "feasible same-stage work beside retained resources");
 CHECK(later_ran.load());
 CHECK_FALSE(head_ran.load());
 if (external_cpu)
  CHECK(external.bytes() == 40);
 else
  CHECK(work[0].wait_for(0ms) == std::future_status::timeout);
 CHECK(work[2].wait_for(0ms) == std::future_status::timeout);
 holder.Release();
 external = {};
 allowance = {};
 parent = {};
 for (auto& item : work)
  if (item.valid()) mmltk::testsupport::await_test_future(item, "same-stage resource settlement");
 CHECK(head_ran.load());
 CHECK(execution.execution().try_reserve({64, 8}).has_value());
}
TEST_CASE("stage selection reconsiders its head and rotates beyond a rejected prefix", "[backend][data][benchmark][pipeline]") {
 bool rotate = false;
 SECTION("a feasible head regains priority") {}
 SECTION("fallback resumes beyond its last successful bypass") { rotate = true; }
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 64}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 auto held = execution.execution().reserve({40, 0});
 mmltk::testsupport::TestGate cpu("queue all candidates"), bypass("selected later candidate");
 std::vector<unsigned> order;
 std::array<std::future<void>, 5> work;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  held = {};
  cpu.Release();
  bypass.Release();
  execution.execution().notify_admission_change();
 });
 occupy_benchmark_cpu(execution, work[0], cpu);
 for (std::size_t i = 1; i < work.size(); ++i)
  queue_benchmark_work(execution, work[i], [&, i] {
   execution.execution().run(CompileTaskKind::Normalize,
    {i == 1   ? 48U
     : i == 2 ? 32U
              : 16U,
     0},
    [&](std::size_t) {
    order.push_back(static_cast<unsigned>(i));
    if (i == 3) bypass.receipt().ArriveAndWait();
   });
  });
 cpu.Release();
 REQUIRE(bypass.WaitEntered(2s));
 held = {};
 if (rotate) held = execution.execution().reserve({32, 0});
 bypass.Release();
 if (rotate) {
  mmltk::testsupport::await_test_future(work[4], "fallback after the previous bypass");
  mmltk::testsupport::await_test_future(work[2], "fallback wraps to the newly feasible prefix");
  CHECK(work[1].wait_for(0ms) == std::future_status::timeout);
  held = {};
 }
 for (auto& item : work)
  if (item.valid()) mmltk::testsupport::await_test_future(item, "head reconsideration");
 if (rotate)
  CHECK(order == std::vector<unsigned>{3, 4, 2, 1});
 else
  CHECK(order == std::vector<unsigned>{3, 1, 2, 4});
}
TEST_CASE("same-stage fallback preserves suspended scratch and withdraws its cursor safely", "[backend][data][benchmark][pipeline]") {
 bool fail_outer = false;
 SECTION("adjacent chunks reuse the suspended owner's scratch") {}
 SECTION("outer failure withdraws the queued sibling at the cursor") { fail_outer = true; }
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 64}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 mmltk::testsupport::TestGate outer("outer parser holds lane scratch");
 std::vector<unsigned> scratch;
 std::size_t chunks = 0, allocations = 0, retirements = 0;
 bool child_ran = false;
 std::array<std::future<void>, 2> work;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  outer.Release();
  execution.execution().notify_admission_change();
 });
 work[0] = std::async(std::launch::async, [&] {
  execution.execution().for_each(CompileTaskKind::Normalize, 3, {48, 0}, [&](std::size_t index) {
   ++chunks;
   if (index == 0) {
    scratch.assign(8, 47);
    ++allocations;
    outer.receipt().ArriveAndWait();
    execution.execution().cooperate();
    require_condition(child_ran && chunks == 1, "cooperative fallback reused its suspended owner's scratch");
    if (fail_outer) throw AnnotationDocumentRejected("suspended parser failed");
   }
   require_condition(scratch.size() == 8 && scratch.front() == 47 && retirements == 0, "same-stage work lost reusable scratch");
  }, [&](std::size_t) {
   scratch.clear();
   ++retirements;
  });
 });
 REQUIRE(outer.WaitEntered(2s));
 queue_benchmark_work(execution, work[1], [&] {
  execution.execution().run(CompileTaskKind::Normalize, {16, 0}, [&](std::size_t) {
   child_ran = true;
   require_condition(!execution.execution().try_reserve({1, 0}), "child released the suspended frame's allowance");
  });
 });
 outer.Release();
 if (fail_outer)
  CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(work[0], "failed suspended parser"), "suspended parser failed");
 else
  mmltk::testsupport::await_test_future(work[0], "same-owner scratch reuse");
 mmltk::testsupport::await_test_future(work[1], "independent same-stage cooperative child");
 CHECK(chunks == (fail_outer ? 1 : 3));
 CHECK(allocations == 1);
 CHECK(retirements == 1);
 CHECK(scratch.empty());
 // The removed cursor borrowed stack-backed group records which are now gone.
 execution.execution().run(CompileTaskKind::Normalize, {64, 0}, [](std::size_t) {});
 CHECK(execution.execution().try_reserve({64, 0}).has_value());
}
TEST_CASE("cancellation settles blocked and feasible same-stage jobs without new resources", "[backend][data][benchmark][pipeline]") {
 // CLEANUP-IGNORE: Independent scheduling cases share atomic cancellation and a fixed resource limit, not an executable algorithm.
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 64}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 auto held = execution.execution().reserve({40, 0});
 mmltk::testsupport::TestGate cpu("cancel with the remaining CPU held");
 std::atomic<unsigned> calls{0};
 std::array<std::future<void>, 3> work;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  cpu.Release();
  execution.execution().notify_admission_change();
 });
 occupy_benchmark_cpu(execution, work[0], cpu);
 for (std::size_t i = 1; i < work.size(); ++i)
  queue_benchmark_work(execution, work[i], [&, i] { execution.execution().run(CompileTaskKind::Normalize, {i == 1 ? 48U : 16U, 0}, [&](std::size_t) { calls.fetch_add(1); }); });
 cancelled.store(true);
 cpu.Release();
 for (auto& item : work) CHECK_THROWS(mmltk::testsupport::await_test_future(item, "cancelled stage settlement"));
 CHECK(calls.load() == 0);
 CHECK(held.bytes() == 40);
 cancelled.store(false);
 held = {};
 execution.execution().run(CompileTaskKind::Normalize, {64, 0}, [](std::size_t) {});
}
TEST_CASE("source retirement removes the rotating candidate without obstructing unrelated ready work", "[backend][data][benchmark][pipeline]") {
 bool whole_source = false;
 SECTION("source retirement") { whole_source = true; }
 SECTION("image retirement") {}
 mmltk::testsupport::ScopedTempDir root("stage-cursor-source-retirement");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 std::atomic<unsigned> opens{0}, calls{0};
 request.image_opened = [&](const fs::path&, std::uint64_t) { opens.fetch_add(1); };
 BenchmarkSplitWriter writer(request);
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 65536, .descriptors = 4}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 auto held = execution.execution().reserve({0, 2});
 mmltk::testsupport::TestGate cpu("populate header queue"), bypass("hold the selected header bypass");
 std::array<std::future<void>, 4> work;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  cpu.Release();
  bypass.Release();
  execution.execution().notify_admission_change();
 });
 occupy_benchmark_cpu(execution, work[0], cpu);
 queue_benchmark_work(execution, work[1], [&] { execution.execution().run(CompileTaskKind::Header, {0, 3}, [&](std::size_t) { calls.fetch_add(1); }); });
 queue_benchmark_work(execution, work[2], [&] { execution.execution().run(CompileTaskKind::Header, {0, 1}, [&](std::size_t) { bypass.receipt().ArriveAndWait(); }); });
 const auto publication = execution.source_publication(images, {});
 publication({1, {}, true});
 queue_benchmark_work(execution, work[3], [&] { execution.execution().run(CompileTaskKind::Header, {0, 1}, [&](std::size_t) { calls.fetch_add(1); }); });
 cpu.Release();
 REQUIRE(bypass.WaitEntered(2s));
 // The first bypass left the cursor on the queued physical image. Withdrawal
 // unlinks that exact job before the next selection resumes past it.
 if (whole_source)
  execution.retire_source(images);
 else
  execution.retire_image(images, 1);
 bypass.Release();
 mmltk::testsupport::await_test_future(work[3], "unrelated header beyond a withdrawn cursor");
 CHECK(calls.load() == 1);
 CHECK(opens.load() == 0);
 CHECK_FALSE(writer.image_complete(0));
 CHECK(work[1].wait_for(0ms) == std::future_status::timeout);
 held = {};
 for (auto& item : work)
  if (item.valid()) mmltk::testsupport::await_test_future(item, "header cursor settlement");
 publication({1, {}, true});
 execution.drain();
 CHECK(opens.load() == 0);
 execution.source_publication(images, {}, 1)({1});
 execution.drain();
 CHECK(writer.image_complete(0));
 CHECK(opens.load() == 1);
 CHECK(calls.load() == 2);
}
TEST_CASE("independent pixel failure settles blocked and feasible stage work with the first error", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("stage-selection-first-error");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 mmltk::testsupport::TestGate failing("independent header fails before stage dispatch");
 request.image_opened = [&](const fs::path&, std::uint64_t) {
  failing.receipt().ArriveAndWait();
  throw std::bad_alloc();
 };
 BenchmarkSplitWriter writer(request);
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 65536}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 auto held = execution.execution().reserve({40960, 0});
 std::atomic<unsigned> calls{0};
 std::array<std::future<void>, 2> work;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  cancelled.store(true);
  failing.Release();
  execution.execution().notify_admission_change();
 });
 execution.source_publication(images, {})({1, {}, true});
 REQUIRE(failing.WaitEntered(2s));
 for (std::size_t i = 0; i < work.size(); ++i)
  queue_benchmark_work(execution, work[i], [&, i] { execution.execution().run(CompileTaskKind::Normalize, {i == 0 ? 49152U : 16384U, 0}, [&](std::size_t) { calls.fetch_add(1); }); });
 failing.Release();
 for (auto& item : work) CHECK_THROWS_AS(mmltk::testsupport::await_test_future(item, "failed stage settlement"), std::bad_alloc);
 CHECK_THROWS_AS(execution.drain(), std::bad_alloc);
 CHECK(calls.load() == 0);
 CHECK(held.bytes() == 40960);
 held = {};
 execution.retire_attempt();
 execution.execution().run(CompileTaskKind::Normalize, {65536, 0}, [](std::size_t) {});
}
TEST_CASE("geometry is generation bound before placement and independent of an undecodable body", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("generation-geometry");
 BenchmarkCompilePipeline execution(1);
 const auto images = root.path() / "images";
 const auto generation = execution.source_generation(images);
 execution.geometry_ready({images, 9, generation, 17, 11});
 REQUIRE(execution.geometry(images, 9).has_value());
 CHECK(execution.geometry(images, 9)->width == 17);
 execution.retire_source(images);
 CHECK_FALSE(execution.geometry(images, 9).has_value());
 execution.geometry_ready({images, 9, generation, 17, 11});
 CHECK_FALSE(execution.geometry(images, 9).has_value());
 const auto next = execution.source_generation(images);
 CHECK(next != generation);
 execution.geometry_ready({images, 9, next, 19, 13});
 CHECK(execution.geometry(images, 9)->width == 19);
 const auto current = execution.source_publication(images, {});
 CHECK_THROWS_WITH(current.geometry_ready(9, {20, 13}), "benchmark source generation has contradictory geometry");
 CHECK(execution.geometry(images, 9)->width == 19);
}
TEST_CASE("retiring one generation waits for its reader while unrelated pixels remain usable", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("generation-readers");
 const auto first = root.path() / "first", other = root.path() / "other";
 auto split = cached_pixel_membership(first);
 add_cached_pixel_source(split, other, 2);
 split.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 1}};
 mmltk::testsupport::TestGate held("retiring source reader");
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 request.image_opened = [&](const fs::path& path, std::uint64_t) {
  if (path == first) held.receipt().ArriveAndWait();
 };
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline execution(2);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 const auto before_publication = execution.source_publication(first, {});
 execution.source_publication(other, {})({2});
 execution.drain();
 REQUIRE(writer.image_complete(1));
 before_publication({1});
 const mmltk::testsupport::ScopedTestCleanup release([&] { held.Release(); });
 REQUIRE(held.WaitEntered(2s));
 auto retirement = std::async(std::launch::async, [&] { execution.retire_source(first); });
 const mmltk::testsupport::ScopedTestCleanup release_before_wait([&] { held.Release(); });
 CHECK(retirement.wait_for(0ms) == std::future_status::timeout);
 CHECK(writer.image_complete(1));
 held.Release();
 mmltk::testsupport::await_test_future(retirement, "source reader retirement");
 CHECK_FALSE(writer.image_complete(0));
 CHECK(writer.image_complete(1));
 before_publication({1});
 execution.drain();
 CHECK_FALSE(writer.image_complete(0));
 execution.source_publication(first, {})({1});
 execution.drain();
 CHECK(writer.image_complete(0));
 CHECK(writer.image_complete(1));
}
TEST_CASE("an attempt retires writer borrows before an early execution owner", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("early-execution-unwind");
 BenchmarkCompilePipeline execution(1);
 const auto images = root.path() / "images";
 auto membership = cached_pixel_membership(images);
 membership.images = {{1, 16, 8, 0, 0}};
 CHECK_THROWS_WITH([&] {
  auto request = benchmark_write_request(membership, root.path() / "result.bin", 8);
  BenchmarkSplitWriter writer(request);
  BenchmarkCompilePipeline::Attempt attempt(execution);
  bind_benchmark_slots(execution, writer, membership);
  execution.source_publication(images, {})({1});
  throw std::runtime_error("construction after registration");
 }(), "construction after registration");
 // The same execution owner has no dangling writer or slot references.
 execution.execution().run(CompileTaskKind::Labels, {1, 0}, [](std::size_t) {});
 execution.drain();
 CHECK_FALSE(fs::exists(root.path() / "result.bin"));
}
TEST_CASE("filesystem reservations share allocation settlement across destinations", "[backend][data][benchmark][storage]") {
 mmltk::testsupport::ScopedTempDir root("shared-storage-ledger");
 detail::StorageReservationPool first(root.path(), {});
 detail::StorageReservationPool second(root.path() / "output", {}, &first);
 auto left = first.reserve(16384, "first stage");
 auto right = second.reserve(8192, "second stage");
 CHECK(first.outstanding() == 24576);
 CHECK(second.outstanding() == 24576);
 const auto path = root.path() / "allocated";
 auto file = FileHandle::create_output(path.string(), 4096);
 left.reconcile(file.get());
 struct stat status{};
 REQUIRE(::fstat(file.get(), &status) == 0);
 const auto remaining = 16384U - std::min<std::uint64_t>(16384, static_cast<std::uint64_t>(status.st_blocks) * 512);
 CHECK(second.outstanding() == remaining + 8192);
 auto moved = std::move(left);
 right.release();
 CHECK(first.outstanding() == remaining);
 moved.release();
 CHECK(second.outstanding() == 0);
}
TEST_CASE("membership metadata shares one CPU with ready headers and pixels", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("shared-stage-fairness");
 const auto images = root.path() / "images";
 auto membership = cached_pixel_membership(images);
 membership.images = {{1, 16, 8, 0, 0}};
 auto request = benchmark_write_request(membership, root.path() / "result.bin", 8);
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 2U << 20});
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, membership);
 auto pixel = mmltk::backend::data::testsupport::prepare_benchmark_pixel(execution, writer);
 mmltk::testsupport::TestGate first("first metadata chunk");
 const mmltk::testsupport::ScopedTestCleanup release([&] { first.Release(); });
 bool pixels_during_metadata = false;
 auto metadata = std::async(std::launch::async, [&] {
  execution.execution().for_each(CompileTaskKind::Metadata, 12, {64, 0}, [&](std::size_t index) {
   if (index == 0) first.receipt().ArriveAndWait();
   if (index == 8) pixels_during_metadata = writer.image_complete(0);
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release_before_join([&] { first.Release(); });
 REQUIRE(first.WaitEntered(2s));
 // Prepared pixels and metadata are both runnable before the first chunk
 // releases the sole CPU; physical file preparation has already settled.
 auto pixel_completion = execution.execution().submit(std::move(pixel));
 first.Release();
 mmltk::testsupport::await_test_future(metadata, "fair membership and pixel scheduling");
 execution.drain();
 CHECK(pixels_during_metadata);
 CHECK(writer.image_complete(0));
 // A completed lane retains reusable decoder capacity. An oversized request
 // must retire that idle capacity without losing the completed product.
 auto oversized = execution.execution().reserve({(2U << 20) + 1, 0});
 CHECK(oversized.bytes() == (2U << 20) + 1);
 CHECK(writer.image_complete(0));
}
TEST_CASE("cancelled shared admission releases a queued borrow with its input held", "[backend][data][benchmark][pipeline]") {
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32, .descriptors = 4}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 auto held = execution.execution().reserve({32, 0});
 bool invoked = false;
 auto work = std::async(std::launch::async, [&] {
  try {
   execution.execution().run(CompileTaskKind::Normalize, {1, 0}, [&](std::size_t) { invoked = true; });
   return false;
  } catch (const std::exception&) { return true; }
 });
 cancelled.store(true);
 CHECK(mmltk::testsupport::await_test_future(work, "cancelled resource admission"));
 CHECK_FALSE(invoked);
 held = {};
}
TEST_CASE("filesystem reservation follows allocation that is later withdrawn", "[backend][data][benchmark][storage]") {
 mmltk::testsupport::ScopedTempDir root("reservation-growth");
 detail::StorageReservationPool storage(root.path(), {});
 const auto path = root.path() / "staging";
 auto promised = storage.reserve(16384, "staged output");
 CHECK(storage.outstanding() == 16384);
 auto file = FileHandle::create_output(path.string(), 8192);
 promised.reconcile(file.get());
 const auto settled = storage.outstanding();
 CHECK(settled <= 8192);
 promised.withdraw_allocation();
 CHECK(storage.outstanding() == 16384);
 file = {};
 fs::remove(path);
 CHECK(storage.outstanding() == 16384);
 promised.release();
 CHECK(storage.outstanding() == 0);
}
TEST_CASE("descriptor continuations admit a feasible transfer minimum before source work", "[backend][data][benchmark][pipeline]") {
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32, .descriptors = 16});
 auto source = execution.execution().reserve(CompileResources::handles(1, true, 11));
 auto [connections, transfer] = reserve_benchmark_transfers(execution.execution(), 8, {{0, 8}, {64, 3}}, source);
 CHECK(connections == 1);
 bool consumed = false;
 execution.execution().run(CompileTaskKind::Header, {}, [&](std::size_t) { consumed = true; }, transfer);
 CHECK(consumed);
 transfer = {};
 CHECK(execution.execution().try_reserve({33, 0, true}).has_value());
 BenchmarkCompilePipeline impossible(1, {}, {.transient_bytes = 32, .descriptors = 8});
 CHECK_THROWS(impossible.execution().reserve(CompileResources::handles(1, true, 11)));
 CHECK_THROWS(reserve_benchmark_transfers(impossible.execution(), 1, {{0, 8}, {64, 3}}));
}
TEST_CASE("live descriptor commitments follow copied parents and nested dependent draws", "[backend][data][benchmark][pipeline]") {
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32, .descriptors = 16});
 auto source = execution.execution().reserve(CompileResources::handles(1, true, 11));
 auto copied = source;
 CHECK_FALSE(execution.execution().try_reserve({0, 11, true}).has_value());
 // Consumers can occupy reserved headroom without obstructing the promised
 // producer completion. Copies all draw from the same remaining commitment.
 auto consumer = execution.execution().reserve({0, 4});
 auto [connections, transfer] = reserve_benchmark_transfers(execution.execution(), 8, {{0, 8}, {64, 3}}, copied);
 CHECK(connections == 1);
 CHECK_FALSE(execution.execution().try_reserve({0, 1}, source).has_value());
 transfer = {};
 auto directory = execution.execution().reserve(CompileResources::handles(2, true, 2), source);
 auto stream = execution.execution().reserve({0, 2, true}, directory);
 auto rest = execution.execution().reserve({0, 7, true}, copied);
 CHECK_FALSE(execution.execution().try_reserve({0, 1}, copied).has_value());
 CHECK_FALSE(execution.execution().try_reserve({0, 1}, directory).has_value());
 // Returning a grandchild makes only that parent's promise available again.
 stream = {};
 CHECK_FALSE(execution.execution().try_reserve({0, 1}, copied).has_value());
 CHECK(execution.execution().try_reserve({0, 2, true}, directory).has_value());
 directory = {};
 CHECK(execution.execution().try_reserve({0, 4, true}, source).has_value());
 source = {};
 copied = {};
 CHECK_FALSE(execution.execution().try_reserve({0, 6}).has_value());
 rest = {};
 consumer = {};
 CHECK(execution.execution().try_reserve({32, 16}).has_value());
}
TEST_CASE("dependent transfer concurrency can also use uncommitted capacity", "[backend][data][benchmark][pipeline]") {
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1024, .descriptors = 32});
 auto source = execution.execution().reserve(CompileResources::handles(1, true, 11));
 auto [connections, transfer] = reserve_benchmark_transfers(execution.execution(), 8, {{0, 8}, {64, 3}}, source);
 CHECK(connections == 5);
 CHECK(transfer.bytes() == 320);
 CHECK_FALSE(execution.execution().try_reserve({0, 1, true}).has_value());
 transfer = {};
 CHECK(execution.execution().try_reserve({0, 12, true}).has_value());
 CHECK_FALSE(execution.execution().try_reserve({0, 13, true}).has_value());
}
TEST_CASE("transferred allowances enforce producer and available continuation strength", "[backend][data][benchmark][pipeline]") {
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32, .descriptors = 16});
 auto consumer = execution.execution().reserve({16, 1});
 bool invoked = false;
 CHECK_THROWS_AS(execution.execution().run(CompileTaskKind::Metadata, {16, 1, true}, [&](std::size_t) { invoked = true; }, consumer), std::invalid_argument);
 CHECK_FALSE(invoked);
 bool nested_rejected = false;
 execution.execution().run(CompileTaskKind::Metadata, {}, [&](std::size_t) {
  try {
   execution.execution().for_each(CompileTaskKind::Normalize, 1, {0, 0, true}, [&](std::size_t) { invoked = true; });
  } catch (const std::logic_error&) { nested_rejected = true; }
 }, consumer);
 CHECK(nested_rejected);
 CHECK_FALSE(invoked);
 consumer = {};
 auto producer = execution.execution().reserve({16, 1, true, 0, false, 3});
 auto child = execution.execution().reserve({0, 2, true}, producer);
 CHECK_THROWS_AS(execution.execution().run(CompileTaskKind::Metadata, {16, 1, true, 0, false, 2}, [&](std::size_t) { invoked = true; }, producer), std::invalid_argument);
 execution.execution().run(CompileTaskKind::Metadata, {16, 1, true, 0, false, 1}, [&](std::size_t) { invoked = true; }, producer);
 CHECK(invoked);
 child = {};
 bool nested_valid = false;
 execution.execution().run(CompileTaskKind::Metadata, {16, 1, true, 0, false, 3},
  [&](std::size_t) { execution.execution().for_each(CompileTaskKind::Normalize, 1, {16, 1, true, 0, false, 3}, [&](std::size_t) { nested_valid = true; }); }, producer);
 CHECK(nested_valid);
 BenchmarkCompilePipeline other(1, {}, {.descriptors = 16});
 CHECK_THROWS_AS(other.execution().try_reserve({0, 1}, producer), std::invalid_argument);
 CHECK_THROWS_AS(other.execution().reserve({0, 1}, producer), std::invalid_argument);
 CHECK_THROWS_AS(reserve_benchmark_transfers(other.execution(), 1, {{0, 8}, {1, 3}}, producer), std::invalid_argument);
}
TEST_CASE("dependent commitments return on callback error and cancelled admission", "[backend][data][benchmark][pipeline]") {
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32, .descriptors = 16}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 auto source = execution.execution().reserve(CompileResources::handles(1, true, 11));
 CHECK_THROWS_WITH([&] {
  auto child = execution.execution().reserve({32, 11, true}, source);
  execution.execution().run(CompileTaskKind::Normalize, {32, 11, true}, [](std::size_t) { throw AnnotationDocumentRejected("dependent body failed"); }, std::move(child));
 }(), "dependent body failed");
 CHECK_FALSE(execution.execution().try_reserve({0, 11, true}).has_value());
 auto child = execution.execution().reserve({32, 11, true}, source);
 bool invoked = false;
 cancelled.store(true);
 CHECK_THROWS(execution.execution().run(CompileTaskKind::Normalize, {32, 11, true}, [&](std::size_t) { invoked = true; }, std::move(child)));
 CHECK_FALSE(invoked);
 cancelled.store(false);
 CHECK(execution.execution().try_reserve({32, 11, true}, source).has_value());
 source = {};
 CHECK(execution.execution().try_reserve({32, 16}).has_value());
}
TEST_CASE("failed shared batches withdraw queued siblings and preserve independent sources", "[backend][data][benchmark][pipeline]") {
 BenchmarkCompilePipeline execution(1);
 std::size_t invoked = 0;
 CHECK_THROWS_WITH(execution.execution().for_each(CompileTaskKind::Metadata, 12, {32, 0},
                    [&](std::size_t) {
  ++invoked;
  throw AnnotationDocumentRejected("source row rejected");
 }),
  "source row rejected");
 CHECK(invoked == 1);
 bool independent = false;
 execution.execution().run(CompileTaskKind::Normalize, {32, 0}, [&](std::size_t) { independent = true; });
 CHECK(independent);
}
TEST_CASE("shared remaining pixels use independent lanes and retain canonical slots", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires two class floors and one elastic CPU");
 mmltk::testsupport::ScopedTempDir root("shared-pixel-tail");
 auto split = cached_pixel_membership(root.path() / "images");
 BenchmarkEncodedImage::publish(cached_image_path(root.path() / "images", 2), make_jpeg(240, 8, 8), {});
 split.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 0}};
 BenchmarkCompilePipeline execution(3);
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 request.execution = &execution;
 mmltk::testsupport::TestGate first("first late pixel"), second("second late pixel");
 request.image_opened = [&](const fs::path&, std::uint64_t id) { (id == 1 ? first : second).receipt().ArriveAndWait(); };
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 auto work = std::async(std::launch::async, [&] { writer.write_remaining(request); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  first.Release();
  second.Release();
 });
 REQUIRE(first.WaitEntered(2s));
 REQUIRE(second.WaitEntered(2s));
 first.Release();
 second.Release();
 mmltk::testsupport::await_test_future(work, "parallel late pixels");
 CHECK(writer.image_complete(0));
 CHECK(writer.image_complete(1));
}
TEST_CASE("image replacement retires one reader and rejects its earlier geometry", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("image-replacement");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 BenchmarkEncodedImage::publish(cached_image_path(images, 2), make_jpeg(240, 8, 8), {});
 split.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 0}};
 std::vector<std::uint8_t> encoded;
 const std::array<std::uint8_t, 6 * 4 * 3> rgb{};
 REQUIRE(stbi_write_png_to_func(append_bytes, &encoded, 6, 4, 3, rgb.data(), 6 * 3) != 0);
 const auto replacement_bytes = encoded;
 encoded.resize(41);
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), encoded, {});
 BenchmarkCompilePipeline execution(1);
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 mmltk::testsupport::TestGate held("affected image reader");
 request.image_opened = [&](const fs::path&, std::uint64_t id) {
  if (id == 1) held.receipt().ArriveAndWait();
 };
 BenchmarkSplitWriter writer(request, true);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 const auto generation = execution.source_generation(images);
 const auto generation_publication = execution.source_publication(images, {});
 generation_publication({2});
 generation_publication({1, {}, true});
 const mmltk::testsupport::ScopedTestCleanup release([&] { held.Release(); });
 REQUIRE(held.WaitEntered(2s));
 auto replacement = std::async(std::launch::async, [&] { execution.retire_image(images, 1); });
 const mmltk::testsupport::ScopedTestCleanup release_before_wait([&] { held.Release(); });
 held.Release();
 mmltk::testsupport::await_test_future(replacement, "affected image retirement");
 CHECK(writer.image_complete(1));
 CHECK_FALSE(writer.image_complete(0));
 execution.geometry_ready({images, 1, generation, 16, 8});
 CHECK_FALSE(execution.geometry(images, 1));
 generation_publication({1});
 execution.drain();
 CHECK_FALSE(writer.image_complete(0));
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), replacement_bytes, {});
 execution.source_publication(images, {}, 1)({1});
 execution.drain();
 CHECK(writer.image_complete(0));
 CHECK(writer.image_complete(1));
 REQUIRE(execution.geometry(images, 1));
 CHECK(execution.geometry(images, 1)->width == 6);
 CHECK(execution.geometry(images, 1)->height == 4);
}
TEST_CASE("cooperative parsing returns one CPU to ready consumer work", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("cooperative-parser");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 BenchmarkCompilePipeline execution(1);
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 auto pixel = mmltk::backend::data::testsupport::prepare_benchmark_pixel(execution, writer);
 mmltk::testsupport::TestGate parsing("parser bounded progress");
 bool completed_during_parse = false;
 auto parser = std::async(std::launch::async, [&] {
  execution.execution().run(CompileTaskKind::Metadata, {32, 0}, [&](std::size_t) {
   parsing.receipt().ArriveAndWait();
   execution.execution().cooperate();  // Prepared pixel kernel.
   completed_during_parse = writer.image_complete(0);
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] { parsing.Release(); });
 REQUIRE(parsing.WaitEntered(2s));
 auto pixel_completion = execution.execution().submit(std::move(pixel));
 parsing.Release();
 mmltk::testsupport::await_test_future(parser, "cooperative parser");
 CHECK(completed_during_parse);
}
TEST_CASE("stock mask scratch survives adjacent chunks and retires for a waiting allocation", "[backend][data][benchmark][annotations][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("charged-stock-scratch");
 const auto path = root.path() / "annotations.json";
 nlohmann::json document{{"images", {{{"id", 1}, {"width", 16}, {"height", 8}, {"file_name", "1.jpg"}}}}, {"categories", {{{"id", 1}, {"name", "person"}}}}, {"annotations", nlohmann::json::array()}};
 // Equal-width unique IDs keep adjacent full chunks at the same admitted size.
 for (unsigned id = 0; id < 2000; ++id)
  document["annotations"].push_back({{"id", 10000 + id}, {"image_id", 1}, {"category_id", 1}, {"bbox", {0, 0, 2, 2}}, {"segmentation", {{0, 0, 2, 0, 2, 2, 0, 2}}}, {"unused", std::string(256, 'x')}});
 write_text(path, document.dump());
 const std::array<NumericCategoryMapping, 1> mappings{{{1, 0, "person"}}};
 mmltk::testsupport::TestGate waiting("stock scratch allocation is waiting");
 struct PressureObservation {
  mmltk::testsupport::TestGate& waiting;
  mutable std::size_t polls = 0;
  static bool& pressure_thread() {
   thread_local bool value = false;
   return value;
  }
  bool cancelled() const noexcept {
   // The second poll is the reserve wait predicate, after the owner records
   // the waiter. Keep the parser held until that causal boundary is reached.
   if (pressure_thread() && ++polls == 2) waiting.receipt().ArriveAndWait();
   return false;
  }
 } observation{waiting};
 // A full chunk needs 256 KiB * 64 plus parser overhead. Keep it ordinary
 // before introducing the explicit oversized pressure below.
 constexpr std::uint64_t target = 32U << 20;
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = target}, mmltk::common::concurrency::CancellationObservation::Borrow(observation), mmltk::backend::data::testsupport::benchmark_trace());
 AnnotationParseOptions options;
 options.source = BenchmarkDatasetSource::kCoco2017;
 options.split = "train";
 options.execution = &execution.execution();
 mmltk::testsupport::TestGate reused("second stock chunk with retained mask scratch");
 std::size_t chunks = 0;
 bool capacity_reused = false, pressure_retired = false;
 const auto diagnostic_trace = mmltk::backend::data::testsupport::benchmark_trace();
 options.trace = [&](std::string_view event, const nlohmann::json& fields) {
  if (diagnostic_trace) diagnostic_trace(event, fields);
  if (event != "benchmark.annotations.workspace") return;
  if (++chunks == 2) {
   capacity_reused = fields.at("retained_sparse_bytes").get<std::size_t>() >= 128;
   reused.receipt().ArriveAndWait();
  }
  if (chunks == 3) pressure_retired = fields.at("retained_sparse_bytes").get<std::size_t>() == 0;
 };
 auto parse = std::async(std::launch::async, [&] { return parse_coco_style_annotations(path, std::string(64, 'a'), mappings, options); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  reused.Release();
  waiting.Release();
 });
 REQUIRE(reused.WaitEntered(2s));
 CHECK(capacity_reused);
 CHECK_FALSE(execution.execution().try_reserve({target + 1, 0}).has_value());
 auto pressure = std::async(std::launch::async, [&] {
  PressureObservation::pressure_thread() = true;
  return execution.execution().reserve({target + 1, 0});
 });
 const mmltk::testsupport::ScopedTestCleanup release_before_wait([&] {
  reused.Release();
  waiting.Release();
 });
 REQUIRE(waiting.WaitEntered(2s));
 waiting.Release();
 reused.Release();
 auto grant = mmltk::testsupport::await_test_future(pressure, "stock scratch pressure retirement");
 CHECK(grant.bytes() == target + 1);
 grant = {};
 const auto result = mmltk::testsupport::await_test_future(parse, "stock chunks after workspace pressure");
 CHECK(result.boxes.size() == 2000);
 CHECK(chunks > 2);
 CHECK(pressure_retired);
}
TEST_CASE("a failed batch joins active borrowers while another source keeps progressing", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 BenchmarkCompilePipeline execution(2);
 mmltk::testsupport::TestGate reader("active batch reader"), failure("failing batch reader");
 std::atomic<std::size_t> invoked{0};
 auto batch = std::async(std::launch::async, [&] {
  try {
   execution.execution().for_each(CompileTaskKind::Metadata, 8, {32, 0}, [&](std::size_t index) {
    invoked.fetch_add(1);
    if (index == 0) reader.receipt().ArriveAndWait();
    if (index == 1) {
     failure.receipt().ArriveAndWait();
     throw AnnotationDocumentRejected("initiating source error");
    }
   });
   return std::string{};
  } catch (const AnnotationDocumentRejected& error) { return std::string(error.what()); }
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  reader.Release();
  failure.Release();
 });
 REQUIRE(reader.WaitEntered(2s));
 REQUIRE(failure.WaitEntered(2s));
 failure.Release();
 bool independent = false;
 execution.execution().run(CompileTaskKind::Labels, {32, 0}, [&](std::size_t) { independent = true; });
 CHECK(independent);
 CHECK(batch.wait_for(0ms) == std::future_status::timeout);
 CHECK(invoked.load() == 2);
 reader.Release();
 CHECK(mmltk::testsupport::await_test_future(batch, "failed batch active custody") == "initiating source error");
}
TEST_CASE("a failed body keeps geometry and reaches repair without another failed decode", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("failed-body-readiness");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 std::vector<std::uint8_t> encoded;
 const std::array<std::uint8_t, 6 * 4 * 3> rgb{};
 REQUIRE(stbi_write_png_to_func(append_bytes, &encoded, 6, 4, 3, rgb.data(), 6 * 3) != 0);
 encoded.resize(41);
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), encoded, {});
 BenchmarkCompilePipeline execution(1);
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 request.execution = &execution;
 std::size_t reads = 0;
 request.image_opened = [&](const fs::path&, std::uint64_t) { ++reads; };
 BenchmarkSplitWriter writer(request, true);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 execution.source_publication(images, {})({1});
 CHECK_FALSE(writer.image_complete(0));
 REQUIRE(execution.geometry(images, 1));
 CHECK(execution.geometry(images, 1)->width == 6);
 CHECK_THROWS_AS(writer.write_remaining(request), BenchmarkImageReadError);
 CHECK(reads == 1);
}
TEST_CASE("borrowed callback records recycle while an early member remains held", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 BenchmarkCompilePipeline execution(2, {}, {.transient_bytes = 128});
 mmltk::testsupport::TestGate early("early borrowed member"), later("member beyond two lane windows");
 std::array<std::atomic<unsigned>, 13> visits{};
 auto work = std::async(std::launch::async, [&] {
  execution.execution().for_each(CompileTaskKind::Normalize, visits.size(), {32, 0}, [&](std::size_t index) {
   visits.at(index).fetch_add(1);
   if (index == 0) early.receipt().ArriveAndWait();
   if (index + 1 == visits.size()) later.receipt().ArriveAndWait();
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  early.Release();
  later.Release();
 });
 REQUIRE(early.WaitEntered(2s));
 REQUIRE(later.WaitEntered(2s));
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 for (const auto& count : visits) CHECK(count.load() == 1);
 later.Release();
 early.Release();
 mmltk::testsupport::await_test_future(work, "individually recycled borrowed members");
 CHECK(execution.execution().try_reserve({128, 0}).has_value());
}
TEST_CASE("remaining pixel slots advance beyond a held early reader", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires two class floors and one elastic CPU");
 mmltk::testsupport::ScopedTempDir root("recycled-pixel-members");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images.clear();
 for (std::uint64_t id = 1; id <= 9; ++id) {
  BenchmarkEncodedImage::publish(cached_image_path(images, id), make_jpeg(240, 8, 8), {});
  split.images.push_back({id, 16, 8, 0, 0});
 }
 // Extraction and Pixels each retain a floor while the held reader lives.
 BenchmarkCompilePipeline execution(3);
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 request.execution = &execution;
 mmltk::testsupport::TestGate early("early pixel reader"), later("all independent pixels completed");
 request.image_opened = [&](const fs::path&, std::uint64_t id) {
  if (id == 1) early.receipt().ArriveAndWait();
 };
 struct Completion {
  std::atomic<unsigned> count{0};
  mmltk::testsupport::TestGate& later;
 } completion{{0}, later};
 request.progress = {.context = &completion, .image_completed = [](void* opaque) {
  auto& state = *static_cast<Completion*>(opaque);
  if (state.count.fetch_add(1) == 7) state.later.receipt().ArriveAndWait();
 }};
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 auto work = std::async(std::launch::async, [&] { writer.write_remaining(request); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  early.Release();
  later.Release();
 });
 REQUIRE(early.WaitEntered(2s));
 REQUIRE(later.WaitEntered(2s));
 CHECK_FALSE(writer.image_complete(0));
 for (std::size_t index = 1; index < split.images.size(); ++index) CHECK(writer.image_complete(index));
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 later.Release();
 early.Release();
 mmltk::testsupport::await_test_future(work, "recycled canonical pixel slots");
 CHECK(writer.completed() == split.images.size());
}
TEST_CASE("cooperative frames retain outer scratch and its grant through child retirement", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("suspended-scratch");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 2U << 20});
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 auto pixel = mmltk::backend::data::testsupport::prepare_benchmark_pixel(execution, writer);
 mmltk::testsupport::TestGate outer("reused outer scratch");
 std::vector<unsigned> scratch;
 std::size_t retirements = 0;
 bool retained_after_child = false, outer_charged = false;
 auto work = std::async(std::launch::async, [&] {
  execution.execution().for_each(CompileTaskKind::Metadata, 2, {1U << 20, 0}, [&](std::size_t index) {
   if (index == 0) {
    scratch.assign(8, 47);
    return;
   }
   require_condition(scratch.size() == 8 && scratch.front() == 47, "adjacent callback lost its retained scratch");
   outer.receipt().ArriveAndWait();
   execution.execution().cooperate();
   require_condition(writer.image_complete(0), "cooperative pixel work did not finish");
   // A larger new consumer would fit if the child had released or replaced the
   // outer allowance. Suspended storage remains live through both frames.
   outer_charged = !execution.execution().try_reserve({(1U << 20) + 1, 0}).has_value();
   retained_after_child = scratch.size() == 8 && scratch.front() == 47 && retirements == 0;
  }, [&](std::size_t lane) {
   require_condition(lane == 0, "scratch retired on another lane");
   scratch.clear();
   ++retirements;
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] { outer.Release(); });
 REQUIRE(outer.WaitEntered(2s));
 auto pixel_completion = execution.execution().submit(std::move(pixel));
 outer.Release();
 mmltk::testsupport::await_test_future(work, "suspended outer scratch custody");
 CHECK(retained_after_child);
 CHECK(outer_charged);
 CHECK(scratch.empty());
 CHECK(retirements == 1);
 auto entire_target = execution.execution().reserve({2U << 20, 0});
 CHECK(entire_target.bytes() == (2U << 20));
 CHECK(writer.image_complete(0));
}
TEST_CASE("last shared backing releases credits after borrower and execution retirement", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("detached-allowance-custody");
 const auto lock_path = root.path() / "source.lock";
 std::shared_ptr<ArtifactLease> last_reader;
 {
  BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32, .descriptors = 4});
  auto allowance = execution.execution().reserve({32, 1, true, 0, false, 2});
  auto child = execution.execution().reserve({0, 2, true}, allowance);
  auto backing = ArtifactLease::acquire_charged(lock_path, {}, child);
  last_reader = backing;
  execution.execution().run(CompileTaskKind::Normalize, {32, 1}, [](std::size_t) {}, allowance);
  allowance = {};
  child = {};
  backing.reset();
  CHECK_FALSE(execution.execution().try_reserve({1, 0}).has_value());
  execution.retire_attempt();
  CHECK_FALSE(execution.execution().try_reserve({1, 0}).has_value());
  const std::weak_ptr<ArtifactLease> observer = last_reader;
  last_reader.reset();
  CHECK(observer.expired());
  CHECK(execution.execution().try_reserve({32, 4}).has_value());
  // A new physical lease then outlives the workers themselves. The previous
  // weak observer remains alive while its returned charge is reused.
  auto next = execution.execution().reserve({32, 1, true, 0, false, 2});
  auto next_child = execution.execution().reserve({0, 2, true}, next);
  last_reader = ArtifactLease::acquire_charged(lock_path, {}, next_child);
 }
 const auto probe = mmltk::testsupport::probe_file_lock(lock_path);
 CHECK_FALSE(probe.available);
 last_reader.reset();
 REQUIRE(::flock(probe.descriptor.get(), LOCK_EX | LOCK_NB) == 0);
 REQUIRE(::flock(probe.descriptor.get(), LOCK_UN) == 0);
}
TEST_CASE("borrowed scratch failure settles its group before the callback owner leaves", "[backend][data][benchmark][pipeline]") {
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32});
 bool released = false;
 CHECK_THROWS_WITH(execution.execution().for_each(CompileTaskKind::Normalize, 1, {32, 0}, [](std::size_t) {},
                    [&](std::size_t) {
  released = true;
  throw AnnotationDocumentRejected("scratch retirement failed");
 }),
  "scratch retirement failed");
 CHECK(released);
 bool healthy = false;
 execution.execution().for_each(CompileTaskKind::Labels, 5, {32, 0}, [&](std::size_t) { healthy = true; });
 CHECK(healthy);
 CHECK(execution.execution().try_reserve({32, 0}).has_value());
}
TEST_CASE("cancelled work groups settle queued members and retained scratch", "[backend][data][benchmark][pipeline]") {
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 mmltk::testsupport::TestGate active("active borrower at cancellation");
 std::size_t calls = 0, retirements = 0;
 auto work = std::async(std::launch::async, [&] {
  execution.execution().for_each(CompileTaskKind::Normalize, 12, {32, 0}, [&](std::size_t) {
   ++calls;
   active.receipt().ArriveAndWait();
  }, [&](std::size_t) { ++retirements; });
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] { active.Release(); });
 REQUIRE(active.WaitEntered(2s));
 cancelled.store(true);
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 active.Release();
 CHECK_THROWS(mmltk::testsupport::await_test_future(work, "cancelled group settlement"));
 CHECK(calls == 1);
 CHECK(retirements == 1);
 cancelled.store(false);
 CHECK(execution.execution().try_reserve({32, 0}).has_value());
}
TEST_CASE("checked resource admission leaves no partial charge after overflow", "[backend][data][benchmark][pipeline]") {
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32, .descriptors = 4});
 CompileResources maximal{std::numeric_limits<std::uint64_t>::max(), 1, false, 0, true};
 auto retained = execution.execution().reserve(maximal);
 CHECK_THROWS_AS(execution.execution().try_reserve(CompileResources::handles(1)), std::overflow_error);
 // Failed byte charging must not consume the remaining descriptors. A legal
 // oversized data job still coexists with its separately retained control.
 auto completion = execution.execution().reserve({33, 3});
 CHECK(completion.bytes() == 33);
 completion = {};
 retained = {};
 CHECK(execution.execution().try_reserve({32, 4}).has_value());
}
TEST_CASE("writer group admission exceptions join earlier borrowed readers", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("pixel-group-admission-unwind");
 auto split = cached_pixel_membership(root.path() / "images");
 split.images = {{1, 16, 8, 0, 0}};
 BenchmarkCompilePipeline execution(2);
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 mmltk::testsupport::TestGate reader("reader borrowed before malformed tail admission");
 request.image_opened = [&](const fs::path&, std::uint64_t) { reader.receipt().ArriveAndWait(); };
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 execution.source_publication(split.sources.front().root, {})({1, {}, true});
 const mmltk::testsupport::ScopedTestCleanup release([&] { reader.Release(); });
 REQUIRE(reader.WaitEntered(2s));
 const std::array<std::size_t, 2> slots{0, split.images.size()};
 auto work = std::async(std::launch::async, [&] { execution.write_remaining(writer, split, slots); });
 const mmltk::testsupport::ScopedTestCleanup release_before_join([&] { reader.Release(); });
 bool independent = false;
 execution.execution().run(CompileTaskKind::Labels, {32, 0}, [&](std::size_t) { independent = true; });
 CHECK(independent);
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 reader.Release();
 CHECK_THROWS_AS(mmltk::testsupport::await_test_future(work, "exceptional writer group settlement"), std::out_of_range);
 execution.drain();
 CHECK(writer.image_complete(0));
}
TEST_CASE("local retirement preserves another source queued in the same writer group", "[backend][data][benchmark][pipeline]") {
 bool whole_source = false;
 SECTION("source generation") { whole_source = true; }
 SECTION("one image generation") {}
 mmltk::testsupport::ScopedTempDir root("writer-group-local-retirement");
 const auto first = root.path() / "first", other = root.path() / "other";
 auto split = cached_pixel_membership(first);
 add_cached_pixel_source(split, other, 2);
 split.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 1}};
 mmltk::testsupport::TestGate held("affected active reader"), joining("retirement has withdrawn the generation");
 struct RetirementObservation {
  mmltk::testsupport::TestGate& joining;
  static bool& retirement_thread() {
   thread_local bool value = false;
   return value;
  }
  bool cancelled() const noexcept {
   // Retirement observes cancellation only in its join wait, after withdrawing
   // the generation. This receipt proves the sibling is still queued then.
   if (retirement_thread()) joining.receipt().ArriveAndWait();
   return false;
  }
 } observation{joining};
 BenchmarkCompilePipeline execution(1, {}, {}, mmltk::common::concurrency::CancellationObservation::Borrow(observation));
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 request.execution = &execution;
 request.image_opened = [&](const fs::path& path, std::uint64_t) {
  if (path == first) held.receipt().ArriveAndWait();
 };
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 const auto before = execution.image_generation(first, 1);
 const auto before_publication = execution.source_publication(first, {});
 execution.geometry_ready({first, 1, before, 16, 8});
 auto work = std::async(std::launch::async, [&] { writer.write_remaining(request); });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  held.Release();
  joining.Release();
 });
 REQUIRE(held.WaitEntered(2s));
 CHECK_FALSE(writer.image_complete(1));
 auto retirement = std::async(std::launch::async, [&] {
  RetirementObservation::retirement_thread() = true;
  if (whole_source)
   execution.retire_source(first);
  else
   execution.retire_image(first, 1);
 });
 const mmltk::testsupport::ScopedTestCleanup release_before_join([&] {
  held.Release();
  joining.Release();
 });
 REQUIRE(joining.WaitEntered(2s));
 CHECK(retirement.wait_for(0ms) == std::future_status::timeout);
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 joining.Release();
 held.Release();
 mmltk::testsupport::await_test_future(retirement, "local generation joins its reader");
 CHECK_THROWS(mmltk::testsupport::await_test_future(work, "writer joins unrelated queued pixels"));
 CHECK_FALSE(writer.image_complete(0));
 CHECK(writer.image_complete(1));
 CHECK_FALSE(execution.geometry(first, 1));
 before_publication({1, std::pair{16U, 8U}});
 execution.drain();
 CHECK_FALSE(writer.image_complete(0));
 CHECK_FALSE(execution.geometry(first, 1));
 const auto repaired = execution.image_generation(first, 1);
 const auto repaired_publication = execution.source_publication(first, {}, 1);
 CHECK(repaired != before);
 repaired_publication({1});
 execution.drain();
 CHECK(writer.image_complete(0));
 CHECK(writer.image_complete(1));
}
TEST_CASE("a rejected pixel body leaves admitted writer siblings available for repair", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("writer-body-local-failure");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 0}};
 std::vector<std::uint8_t> encoded;
 const std::array<std::uint8_t, 6 * 4 * 3> rgb{};
 REQUIRE(stbi_write_png_to_func(append_bytes, &encoded, 6, 4, 3, rgb.data(), 6 * 3) != 0);
 encoded.resize(41);
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), encoded, {});
 BenchmarkEncodedImage::publish(cached_image_path(images, 2), make_jpeg(240, 8, 8), {});
 BenchmarkCompilePipeline execution(1);
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 request.execution = &execution;
 std::size_t first_reads = 0;
 request.image_opened = [&](const fs::path&, std::uint64_t id) {
  if (id == 1) ++first_reads;
 };
 BenchmarkSplitWriter writer(request, true);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 CHECK_THROWS_AS(writer.write_remaining(request), BenchmarkImageReadError);
 CHECK_FALSE(writer.image_complete(0));
 CHECK(writer.image_complete(1));
 REQUIRE(execution.geometry(images, 1));
 CHECK(execution.geometry(images, 1)->width == 6);
 CHECK_THROWS_AS(writer.write_remaining(request), BenchmarkImageReadError);
 CHECK(first_reads == 1);
 execution.retire_image(images, 1);
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), make_jpeg(240, 8, 8), {});
 writer.write_remaining(request);
 CHECK(writer.image_complete(0));
 CHECK(writer.image_complete(1));
 CHECK(first_reads == 2);
}
TEST_CASE("charged lease acquisition owns its descriptor through cancellation and last reader", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("charged-lease-factory");
 std::atomic<bool> cancelled{false};
 const auto observation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = 4}, observation);
 const auto path = root.path() / "source.lock";
 auto lease = ArtifactLease::acquire_charged(path, observation, &execution.execution(), CompileResources::handles(1, true, 2));
 auto reader = lease;
 const std::weak_ptr<ArtifactLease> observer = reader;
 lease.reset();
 CHECK_FALSE(execution.execution().try_reserve({0, 4}).has_value());
 mmltk::testsupport::TestGate lock_wait("charged lease reached contended flock");
 struct LeaseCancellation {
  const std::atomic<bool>& cancelled_flag;
  mmltk::testsupport::TestGate::Receipt reached;
  mutable unsigned observations = 0;
  bool cancelled() const noexcept {
   // The factory observes once before open, then after a failed flock.
   if (++observations == 2) reached.ArriveAndWait();
   return cancelled_flag.load();
  }
 } contended{cancelled, lock_wait.receipt()};
 auto waiter = std::async(std::launch::async, [&] { return ArtifactLease::acquire_charged(path, mmltk::common::concurrency::CancellationObservation::Borrow(contended), &execution.execution()); });
 const mmltk::testsupport::ScopedTestCleanup stop([&] {
  cancelled.store(true);
  lock_wait.Release();
 });
 REQUIRE(lock_wait.WaitEntered(2s));
 CHECK(execution.execution().try_reserve({0, 1}).has_value());
 cancelled.store(true);
 lock_wait.Release();
 CHECK_THROWS(mmltk::testsupport::await_test_future(waiter, "cancelled charged lock acquisition"));
 reader.reset();
 CHECK(observer.expired());
 cancelled.store(false);
 CHECK(execution.execution().try_reserve({0, 4}).has_value());
 CHECK(mmltk::testsupport::probe_file_lock(path).available);
}
TEST_CASE("direct storage settlement preserves sparse replacement promises and inode aliases", "[backend][data][benchmark][storage]") {
 mmltk::testsupport::ScopedTempDir root("direct-download-storage");
 detail::StorageReservationPool storage(root.path(), {});
 const auto final = root.path() / "artifact";
 const auto partial = root.path() / "artifact.part";
 auto file = FileHandle::create_output(partial.string(), 0);
 REQUIRE(::ftruncate(file.get(), 65536) == 0);
 auto reservation = reserve_download(storage, final, 65536, "sparse download");
 struct stat status{};
 REQUIRE(::fstat(file.get(), &status) == 0);
 const auto outstanding_for = [&] { return 65536U - std::min<std::uint64_t>(65536, static_cast<std::uint64_t>(status.st_blocks) * 512); };
 CHECK(storage.outstanding() == outstanding_for());
 const std::array<std::uint8_t, 4096> bytes{};
 file.pwrite_all(bytes.data(), bytes.size(), 32768);
 reservation.reconcile(file.get());
 REQUIRE(::fstat(file.get(), &status) == 0);
 CHECK(storage.outstanding() == outstanding_for());
 fs::create_hard_link(partial, final);
 reconcile_download(reservation, final);
 CHECK(storage.outstanding() == outstanding_for());
 reservation.withdraw_allocation();
 CHECK(storage.outstanding() == 65536);
 REQUIRE(::ftruncate(file.get(), 0) == 0);
 reservation.reconcile(file.get());
 CHECK(storage.outstanding() == 65536);
 // This reservation's directory anchor survives replacement of both names.
 fs::remove(final);
 fs::remove(partial);
 reservation.grow(69632, "replacement growth");
 CHECK(storage.outstanding() == 69632);
 reservation.resize(65536, "replacement size");
 file = FileHandle::create_output(partial.string(), 0);
 file.preallocate(65536);
 reservation.reconcile(file.get());
 fs::rename(partial, final);
 reconcile_download(reservation, final);
 REQUIRE(::fstat(file.get(), &status) == 0);
 CHECK(storage.outstanding() == outstanding_for());
 reservation.release();
 CHECK(storage.outstanding() == 0);
 CHECK(fs::file_size(final) == 65536);
}
TEST_CASE("storage admission never reopens unrelated live backing paths", "[backend][data][benchmark][storage]") {
 mmltk::testsupport::ScopedTempDir root("constant-storage-settlement");
 detail::StorageReservationPool storage(root.path(), {});
 fs::create_directory(root.path() / "old");
 auto file = FileHandle::create_output((root.path() / "old" / "file").string(), 4096);
 auto first = storage.reserve(16384, "first");
 first.reconcile(file.get());
 const auto retained = storage.outstanding();
 fs::rename(root.path() / "old", root.path() / "moved");
 auto obstruction = FileHandle::create_output((root.path() / "old").string(), 0);
 // The former backing path now raises ENOTDIR. Its live descriptor and credited
 // allocation remain valid, and unrelated admission must not inspect that path.
 auto second = storage.reserve(8192, "unrelated");
 CHECK(storage.outstanding() == retained + 8192);
 first.reconcile(file.get());
 CHECK(storage.outstanding() == retained + 8192);
 first = std::move(second);
 CHECK(storage.outstanding() == 8192);
 first.release();
 CHECK(storage.outstanding() == 0);
}
TEST_CASE("staged artifact owns cleanup across file-operation and publication failures", "[backend][data][benchmark][storage]") {
 mmltk::testsupport::ScopedTempDir root("staged-artifact-failures");
 detail::StorageReservationPool storage(root.path(), {});
 const auto target = root.path() / "published";
 {
  auto original = FileHandle::create_output(target.string(), 3);
  original.pwrite_all("old", 3, 0);
 }
 SECTION("creation") { CHECK_THROWS(detail::StagedArtifact::create(storage, root.path() / "absent" / "file", 4096, "failed creation")); }
 SECTION("preallocation") {
  fs::path temporary;
  {
   auto stage = detail::StagedArtifact::create(storage, target, 4096, "failed preallocation");
   temporary = stage.path();
   stage.file() = {};
   CHECK_THROWS(stage.preallocate(4096));
  }
  CHECK_FALSE(fs::exists(temporary));
 }
 SECTION("write and sync") {
  fs::path temporary;
  {
   auto stage = detail::StagedArtifact::create(storage, target, 4096, "failed write");
   temporary = stage.path();
   stage.file() = {};
   CHECK_THROWS(stage.file().pwrite_all("new", 3, 0));
   CHECK_THROWS(stage.file().sync_data());
  }
  CHECK_FALSE(fs::exists(temporary));
 }
 SECTION("rename and replacement") {
  fs::path temporary;
  {
   auto stage = detail::StagedArtifact::create(storage, target, 3, "failed replacement");
   temporary = stage.path();
   stage.file().pwrite_all("new", 3, 0);
   stage.file().sync_data();
   CHECK_THROWS(stage.publish(target, {}, detail::StagedArtifact::Publication::DurableReplace, false));
   CHECK(fs::exists(temporary));
  }
  CHECK_FALSE(fs::exists(temporary));
 }
 SECTION("rename syscall") {
  fs::path temporary;
  {
   auto stage = detail::StagedArtifact::create(storage, target, 3, "failed rename");
   temporary = stage.path();
   stage.file().pwrite_all("new", 3, 0);
   CHECK_THROWS(stage.publish(root.path() / "absent" / "value", {}, detail::StagedArtifact::Publication::Rename));
  }
  CHECK_FALSE(fs::exists(temporary));
 }
 SECTION("move assignment abandons the preceding stage") {
  auto first = detail::StagedArtifact::create(storage, target, 4096, "first stage");
  const auto previous = first.path();
  auto second = detail::StagedArtifact::create(storage, target, 8192, "second stage");
  first = std::move(second);
  CHECK_FALSE(fs::exists(previous));
  CHECK(storage.outstanding() == 8192);
 }
 SECTION("cancelled JSON publication") {
  std::atomic<bool> cancelled{true};
  CHECK_THROWS(write_json_atomically(target, nlohmann::json{{"replacement", true}}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), &storage));
 }
 CHECK(storage.outstanding() == 0);
 auto original = FileHandle::open_readonly(target.string());
 std::array<char, 3> bytes{};
 original.pread_all(bytes.data(), bytes.size(), 0);
 CHECK(std::string_view(bytes.data(), bytes.size()) == "old");
 CHECK(std::distance(fs::directory_iterator(root.path()), fs::directory_iterator{}) == 1);
}
TEST_CASE("independent staged writers retain separate promises and publish after moves", "[backend][data][benchmark][storage]") {
 mmltk::testsupport::ScopedTempDir root("independent-staged-writers");
 detail::StorageReservationPool storage(root.path(), {});
 mmltk::testsupport::TestGate staged("two owned stages");
 const auto write = [&](std::size_t index) {
  auto stage = detail::StagedArtifact::create(storage, root.path() / std::to_string(index), 16384, "independent stage");
  staged.receipt().ArriveAndWait();
  auto moved = std::move(stage);
  moved.preallocate(16384);
  moved.file().pwrite_all("data", 4, 0);
  moved.file().sync_data();
  moved.publish(root.path() / std::to_string(index), {});
 };
 auto first = std::async(std::launch::async, [&] { write(0); });
 auto second = std::async(std::launch::async, [&] { write(1); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { staged.Release(); });
 REQUIRE(staged.WaitEntered(2s, 2));
 CHECK(storage.outstanding() == 32768);
 staged.Release();
 mmltk::testsupport::await_test_future(first, "first staged writer");
 mmltk::testsupport::await_test_future(second, "second staged writer");
 CHECK(storage.outstanding() == 0);
 CHECK(fs::file_size(root.path() / "0") == 16384);
 CHECK(fs::file_size(root.path() / "1") == 16384);
}
TEST_CASE("mapped artifacts retain fixed backing after descriptor and producer retirement", "[benchmark][storage][resources]") {
 mmltk::testsupport::ScopedTempDir root("mapped-artifact-custody");
 detail::StorageReservationPool storage(root.path(), {});
 auto owner = std::make_shared<BenchmarkMappedArtifact>(storage, root.path() / "payload", 4096, "mapped fixture");
 const auto path = owner->path();
 REQUIRE(fs::exists(path));
 CHECK(fs::file_size(path) == 4096);
 owner->write("data", 4, 0);
 const char* bytes = nullptr;
 SECTION("readonly private snapshot") { bytes = static_cast<const char*>(owner->map_readonly()); }
 SECTION("writable shared assembly") {
  auto* writable = static_cast<char*>(owner->map_writable());
  writable[0] = 'D';
  bytes = writable;
  std::array<char, 4> persisted{};
  FileHandle::open_readonly(path.string()).pread_all(persisted.data(), persisted.size(), 0);
  CHECK(std::string_view(persisted.data(), persisted.size()) == "Data");
 }
 REQUIRE(bytes != nullptr);
 CHECK_THROWS_AS(owner->write("x", 1, 0), std::logic_error);
 CHECK_THROWS_AS(owner->map_readonly(), std::logic_error);
 CHECK_THROWS_AS(owner->map_writable(), std::logic_error);
 for (const auto& descriptor : fs::directory_iterator("/proc/self/fd")) {
  std::error_code error;
  const auto target = fs::read_symlink(descriptor.path(), error);
  if (!error) CHECK(target != path);
 }
 const std::weak_ptr<const BenchmarkMappedArtifact> lifetime = owner;
 std::shared_ptr<const BenchmarkMappedArtifact> reader = owner;
 owner.reset();
 CHECK_FALSE(lifetime.expired());
 CHECK(fs::exists(path));
 CHECK(std::string_view(bytes + 1, 3) == "ata");
 reader.reset();
 CHECK(lifetime.expired());
 CHECK_FALSE(fs::exists(path));
 CHECK(storage.outstanding() == 0);
}
TEST_CASE("mapped payload construction unwinds storage and empty extents need no file", "[benchmark][storage][resources]") {
 mmltk::testsupport::ScopedTempDir root("mapped-artifact-failures");
 detail::StorageReservationPool storage(root.path(), {});
 SECTION("file creation failure releases its promise") { CHECK_THROWS(BenchmarkMappedArtifact(storage, root.path() / "absent" / "payload", 4096, "missing directory")); }
 SECTION("fixed extent write rejects overflow and unwinds the producer") {
  const auto construct = [&] {
   BenchmarkMappedArtifact owner(storage, root.path() / "payload", 4096, "invalid payload");
   owner.write("x", 1, SIZE_MAX);
  };
  CHECK_THROWS_AS(construct(), std::out_of_range);
 }
 SECTION("metadata construction failure after mapping releases all custody") {
  const auto construct = [&] {
   BenchmarkMappedArtifact owner(storage, root.path() / "payload", 4096, "failed metadata");
   (void)owner.map_readonly();
   throw std::bad_alloc();
  };
  CHECK_THROWS_AS(construct(), std::bad_alloc);
 }
 SECTION("empty readonly and writable mappings") {
  BenchmarkMappedArtifact readonly(storage, root.path() / "readonly", 0, "empty readonly");
  CHECK(readonly.path().empty());
  REQUIRE_NOTHROW(readonly.write(nullptr, 0, 0));
  CHECK(readonly.map_readonly() == nullptr);
  BenchmarkMappedArtifact writable(storage, root.path() / "writable", 0, "empty writable");
  CHECK(writable.map_writable() == nullptr);
 }
 CHECK(storage.outstanding() == 0);
 CHECK(fs::is_empty(root.path()));
}
TEST_CASE("cached images and ordinary stages retain their modes and promises through publication", "[backend][data][benchmark][storage]") {
 mmltk::testsupport::ScopedTempDir root("staged-output-modes");
 detail::StorageReservationPool storage(root.path(), {});
 mode_t mask = 0022;
 SECTION("ordinary process mask") {}
 SECTION("private creation honors an owner permission mask") { mask = 0200; }
 const auto previous_mask = ::umask(mask);
 const mmltk::testsupport::ScopedTestCleanup restore_mask([&] { (void)::umask(previous_mask); });
 const auto encoded = make_jpeg(240, 8, 8);
 struct PublicationObservation {
  detail::StorageReservationPool& storage;
  std::uint64_t promised;
  mutable bool retained = false;
  bool cancelled() const noexcept {
   retained = storage.outstanding() == promised;
   return false;
  }
 } observation{storage, encoded.size()};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(observation);
 const auto image = root.path() / "image.jpg";
 BenchmarkEncodedImage::publish(image, encoded, cancellation, storage);
 CHECK(observation.retained);
 CHECK(storage.outstanding() == 0);
 struct stat status{};
 REQUIRE(::stat(image.c_str(), &status) == 0);
 CHECK((status.st_mode & 0777) == (0600 & ~mask));
 const auto ordinary = root.path() / "ordinary.bin";
 auto stage = detail::StagedArtifact::create(storage, ordinary, encoded.size(), "ordinary stage modes");
 REQUIRE(::fstat(stage.file().get(), &status) == 0);
 CHECK((status.st_mode & 0777) == 0644);
 CHECK(status.st_size == 0);
 stage.file().pwrite_all(encoded.data(), encoded.size(), 0);
 stage.file().sync_data();
 observation.retained = false;
 const auto temporary = stage.path();
 stage.publish(ordinary, cancellation);
 CHECK(observation.retained);
 CHECK(storage.outstanding() == 0);
 CHECK_FALSE(fs::exists(temporary));
 REQUIRE(::stat(ordinary.c_str(), &status) == 0);
 CHECK((status.st_mode & 0777) == 0644);
 for (const auto& path : {image, ordinary}) {
  auto file = FileHandle::open_readonly(path.string());
  REQUIRE(file.size() == encoded.size());
  std::vector<std::uint8_t> persisted(encoded.size());
  file.pread_all(persisted.data(), persisted.size(), 0);
  CHECK(persisted == encoded);
 }
}
TEST_CASE("Curl owns checked fixed and per-transfer resource composition", "[backend][data][benchmark][pipeline]") {
 const auto envelope = benchmark_curl_envelope(2, 4096, 8192);
 const auto single = envelope.demand(1);
 const auto doubled = envelope.demand(2);
 CHECK(single.descriptors == envelope.fixed.descriptors + envelope.per_transfer.descriptors);
 CHECK(benchmark_curl_envelope().demand(1).descriptors == 5);
 CHECK(doubled.bytes == single.bytes + envelope.per_transfer.bytes);
 CHECK_THROWS(benchmark_curl_envelope(std::numeric_limits<std::size_t>::max()));
 CHECK_THROWS(benchmark_curl_envelope(0, 0, std::numeric_limits<std::uint64_t>::max()));
 CHECK_THROWS(envelope.demand(std::numeric_limits<std::size_t>::max()));
}
TEST_CASE("captured image publications retain their generation across replacement and attempt closure", "[backend][data][benchmark][pipeline]") {
 enum class Replacement { Image, Source, Attempt };
 auto replacement = Replacement::Image;
 SECTION("one repaired image") {}
 SECTION("physical source") { replacement = Replacement::Source; }
 SECTION("writer registrations") { replacement = Replacement::Attempt; }
 mmltk::testsupport::ScopedTempDir root("captured-image-publication");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 BenchmarkCompilePipeline execution(1);
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 auto writer = std::make_unique<BenchmarkSplitWriter>(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, *writer, split);
 auto lease = ArtifactLease::acquire_charged(root.path() / "images.lock", {}, &execution.execution());
 auto old = execution.source_publication(images, lease);
 const CachedImageReady ready{1, std::pair{16U, 8U}};
 mmltk::testsupport::TestGate delayed("captured publication before delivery");
 auto producer = std::async(std::launch::async, [old, ready, receipt = delayed.receipt()] {
  receipt.ArriveAndWait();
  old.geometry_ready(ready.image_id, *ready.dimensions);
  old(ready);
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] { delayed.Release(); });
 REQUIRE(delayed.WaitEntered(2s));
 if (replacement == Replacement::Image)
  execution.retire_image(images, 1);
 else if (replacement == Replacement::Source)
  execution.retire_source(images);
 else {
  execution.retire_attempt();
  writer.reset();
  writer = std::make_unique<BenchmarkSplitWriter>(request);
  bind_benchmark_slots(execution, *writer, split);
 }
 delayed.Release();
 mmltk::testsupport::await_test_future(producer, "publication after generation withdrawal");
 execution.drain();
 CHECK_FALSE(writer->image_complete(0));
 CHECK_FALSE(execution.geometry(images, 1));
 // An ordinary producer does not acquire the newest per-image replacement on
 // delivery. Only the repairing producer captures that particular generation.
 if (replacement == Replacement::Image) {
  const auto ordinary = execution.source_publication(images, lease);
  ordinary(ready);
  execution.drain();
  CHECK_FALSE(writer->image_complete(0));
 }
 auto repaired = execution.source_publication(images, lease, 1);
 execution.retire_image(images, 1);
 repaired(ready);
 execution.drain();
 CHECK_FALSE(writer->image_complete(0));
 CHECK_FALSE(execution.geometry(images, 1));
 repaired = execution.source_publication(images, lease, 1);
 repaired(ready);
 CHECK(writer->image_complete(0));  // One-CPU immediate ready delivery is retained.
 REQUIRE(execution.geometry(images, 1));
 CHECK(execution.geometry(images, 1)->width == 16);
}
TEST_CASE("owned pixel input retains mapping and charged custody through its last reader", "[backend][data][benchmark][writer]") {
 mmltk::testsupport::ScopedTempDir root("last-pixel-input-reader");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 const auto path = cached_image_path(images, 1);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32, .descriptors = 4});
 const auto file_open = [&] {
  for (const auto& descriptor : fs::directory_iterator("/proc/self/fd")) {
   std::error_code error;
   if (fs::read_symlink(descriptor.path(), error) == path && !error) return true;
  }
  return false;
 };
 const auto file_mapped = [&] {
  std::ifstream mappings("/proc/self/maps");
  std::string line;
  while (std::getline(mappings, line))
   if (line.find(path.string()) != std::string::npos) return true;
  return false;
 };
 bool released_after_backing = false, released_after_credits = false;
 auto physical = ArtifactLease::acquire_charged(root.path() / "source.lock", {}, &execution.execution());
 const auto* physical_pointer = physical.get();
 std::shared_ptr<const ArtifactLease> custody(physical_pointer, [&, physical = std::move(physical)](const ArtifactLease*) mutable {
  released_after_backing = !file_open() && !file_mapped();
  released_after_credits = execution.execution().try_reserve({32, 3}).has_value();
  physical.reset();
 });
 std::weak_ptr<const ArtifactLease> retained = custody;
 std::shared_ptr<BenchmarkPixelInput> input;
 {
  auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
  BenchmarkSplitWriter writer(request);
  auto allowance = execution.execution().reserve({32, 1});
  execution.execution().run(
   CompileTaskKind::Header, {}, [&](std::size_t lane) { input = writer.prepare_pixel(0, lane, execution.source_publication(images, std::move(custody)), std::move(allowance)); });
  REQUIRE(input);
  REQUIRE(writer.header_dimensions(0));
 }
 auto last_reader = input;
 input.reset();
 execution.retire_attempt();
 CHECK_FALSE(file_open());  // The immutable mapping retains the inode without an open descriptor.
 CHECK(file_mapped());
 CHECK_FALSE(retained.expired());
 CHECK_FALSE(execution.execution().try_reserve({1, 0}).has_value());
 last_reader.reset();
 CHECK(released_after_backing);
 CHECK(released_after_credits);
 CHECK(retained.expired());
 CHECK(execution.execution().try_reserve({32, 4}).has_value());
}
TEST_CASE("retained writers withdraw affected pixels and keep healthy slots across attempts", "[backend][data][benchmark][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("detached-pixel-remapping");
 const auto first = root.path() / "first", other = root.path() / "other";
 auto split = cached_pixel_membership(first);
 prepare_cached_image_directory(other);
 BenchmarkEncodedImage::publish(cached_image_path(other, 2), make_jpeg(8, 240, 8), {});
 split.sources.push_back({other});
 split.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 1}};
 BenchmarkCompilePipeline execution(1);
 auto request = benchmark_write_request(split, root.path() / "retained.bin", 8);
 std::vector<std::uint64_t> invalidations;
 request.progress = {.context = &invalidations, .images_invalidated = [](void* value, std::uint64_t count) { static_cast<std::vector<std::uint64_t>*>(value)->push_back(count); }};
 BenchmarkSplitWriter retained(request);
 bind_benchmark_slots(execution, retained, split);
 const auto old = execution.source_publication(first, {});
 old({1});
 execution.source_publication(other, {})({2});
 execution.retire_attempt();
 execution.retire_source(first);
 // Detached writer ownership remains with the compiler between attempts.
 retained.invalidate_source(first);
 CHECK_FALSE(retained.image_complete(0));
 CHECK(retained.image_complete(1));
 CHECK(invalidations == std::vector<std::uint64_t>{1});
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, retained, split);
 old({1, std::pair{16U, 8U}});
 CHECK_FALSE(retained.image_complete(0));
 execution.source_publication(first, {})({1});
 CHECK(retained.image_complete(0));
 CHECK(retained.image_complete(1));
}
namespace {
void seed_segmented_tail(const DownloadRequest& request, std::uint64_t remaining = 2U << 20, std::size_t count = 2) {
 const mmltk::common::io::ScopedFd partial(::open((request.destination.string() + ".part").c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
 require_condition(partial.get() >= 0 && ::ftruncate(partial.get(), static_cast<off_t>(request.expected_size)) == 0, "cannot create sparse segmented fixture");
 const auto width = request.expected_size / count;
 nlohmann::json segments = nlohmann::json::array();
 for (std::size_t i = 0; i < count; ++i) {
  const auto begin = width * i, end = i + 1 == count ? request.expected_size : begin + width;
  segments.push_back({{"begin", begin}, {"end", end - 1}, {"completed", end - begin - remaining}, {"attempts", 1}});
 }
 write_json_atomically(request.destination.string() + ".part.json",
  nlohmann::json{
   {"schema_version", kBenchmarkCacheSchemaVersion}, {"mode", "segmented"}, {"url", request.url}, {"size", request.expected_size}, {"etag", "\"benchmark-test-etag\""},
   {"last_modified", "Thu, 23 Jul 2026 12:00:00 GMT"}, {"segments", std::move(segments)}
  },
  {});
}
}  // namespace
TEST_CASE("a contended artifact lock returns its admission and independent ready files publish", "[backend][data][benchmark][download]") {
 mmltk::testsupport::ScopedTempDir root("independent-lock-admission");
 const auto payload = make_payload(16384);
 HttpServer server(payload);
 const std::vector requests{request_for(root.path(), "locked", server.url("locked"), payload), request_for(root.path(), "ready", server.url("ready"), payload)};
 auto blocked = ArtifactLease::acquire(requests[0].lock_path, {});
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = 24});
 CHECK_FALSE(ArtifactLease::try_acquire_charged(requests[0].lock_path, {}, &execution.execution()));
 auto attempted = execution.execution().reserve(CompileResources::handles(1, true, 4));
 CHECK_FALSE(ArtifactLease::try_acquire_charged(requests[0].lock_path, {}, std::move(attempted)));
 CHECK(execution.execution().try_reserve({0, 24}).has_value());
 std::atomic<bool> cancelled{false};
 std::promise<DownloadReady> ready;
 auto work = std::async(std::launch::async, [&] {
  return download_artifacts(requests, 1, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), {}, {}, [&](DownloadReady value) {
   if (value.request_index == 1) ready.set_value(std::move(value));
  }, &execution);
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  blocked = {};
 });
 const auto result = mmltk::testsupport::await_test_promise(ready, "independent locked batch result");
 CHECK(result.artifact.path == requests[1].destination);
 CHECK_FALSE(fs::exists(requests[0].destination));
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 blocked = {};
 const auto results = mmltk::testsupport::await_test_future(work, "released artifact lock");
 REQUIRE(results.size() == 2);
 CHECK(results[0].path == requests[0].destination);
 CHECK(results[1].path == requests[1].destination);
 CHECK(execution.execution().try_reserve({0, 24}).has_value());
 server.Check();
}
TEST_CASE("one-connection stored ranges coexist with independent whole artifacts in the compile transport", "[backend][data][benchmark][download]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("mixed-artifact-transport");
 constexpr std::size_t bytes = 512U << 20;
 HttpServer ranged(bytes);
 const auto payload = make_payload(32768);
 HttpServer whole(payload);
 DownloadRequest large{"range", ranged.url("range"), root.path() / "large", root.path() / "large.lock", bytes};
 const auto small = request_for(root.path(), "small", whole.url("small"), payload);
 seed_segmented_tail(large);
 ranged.GateNextTransfer();
 BenchmarkCompilePipeline execution(2);
 std::atomic<bool> cancelled{false};
 auto work = std::async(std::launch::async, [&] { return download_artifacts({large}, 1, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), {}, {}, {}, &execution); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  ranged.ReleasePartial();
 });
 REQUIRE(ranged.WaitPartial());
 auto independent = std::async(std::launch::async, [&] { return download_artifacts({small}, 1, {}, {}, {}, {}, &execution); });
 const auto completed = mmltk::testsupport::await_test_future(independent, "whole artifact while a stored range is held");
 CHECK(completed[0].size == payload.size());
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 ranged.ReleasePartial();
 const auto result = mmltk::testsupport::await_test_future(work, "one-connection two-range resume");
 CHECK(result[0].resumed);
 CHECK(read_json_file(large.destination.string() + ".download.json").at("segments") == 2);
 const auto ranges = ranged.ranges();
 CHECK(std::ranges::find(ranges, std::pair<std::size_t, std::size_t>{bytes / 2 - (2U << 20), bytes / 2 - 1}) != ranges.end());
 CHECK(std::ranges::find(ranges, std::pair<std::size_t, std::size_t>{bytes - (2U << 20), bytes - 1}) != ranges.end());
 ranged.Check();
 whole.Check();
}
TEST_CASE("stored range admission rejects invalid coverage and validators before any payload work", "[backend][data][benchmark][download]") {
 mmltk::testsupport::ScopedTempDir root("range-topology-admission");
 constexpr std::size_t bytes = 512U << 20;
 HttpServer server(bytes);
 DownloadRequest request{"range", server.url("range"), root.path() / "artifact", root.path() / "artifact.lock", bytes};
 seed_segmented_tail(request);
 const auto path = request.destination.string() + ".part.json";
 auto metadata = read_json_file(path);
 SECTION("gap") { metadata["segments"][1]["begin"] = bytes / 2 + 1; }
 SECTION("overlap") { metadata["segments"][1]["begin"] = bytes / 2 - 1; }
 SECTION("overflowing endpoint") { metadata["segments"][1]["end"] = std::numeric_limits<std::uint64_t>::max(); }
 SECTION("completed length") { metadata["segments"][0]["completed"] = bytes; }
 SECTION("changed identity") { metadata["etag"] = "\"older-etag\""; }
 SECTION("overfragmented complete coverage") {
  metadata["segments"] = nlohmann::json::array();
  for (std::size_t i = 0; i < 16; ++i) metadata["segments"].push_back({{"begin", bytes / 16 * i}, {"end", bytes / 16 * (i + 1) - 1}, {"completed", 0}, {"attempts", 1}});
 }
 write_json_atomically(path, metadata, {});
 std::atomic<bool> cancelled{false};
 bool empty_admission = false;
 CHECK_THROWS(download_artifacts({request}, 1, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), [&](const DownloadProgress& update) {
  if (update.transfer.attempt == 0) {
   empty_admission = update.transfer.completed_bytes == 0 && update.transfer.retained_bytes == 0;
   cancelled.store(true);
  }
 }));
 CHECK(empty_admission);
 const auto reset = read_json_file(path);
 REQUIRE(reset.at("segments").size() == 1);
 CHECK(reset.at("segments")[0].at("completed") == 0);
 CHECK(server.requests() == 1);
 server.Check();
}
TEST_CASE("range retries preserve admitted tails and ordinary fallback quiesces their writers", "[backend][data][benchmark][download]") {
 mmltk::testsupport::ScopedTempDir root("range-response-admission");
 constexpr std::size_t bytes = 512U << 20;
 HttpServer server(bytes);
 DownloadRequest request{"range", server.url("range"), root.path() / "artifact", root.path() / "artifact.lock", bytes, {}, 2};
 seed_segmented_tail(request);
 bool fallback = false;
 SECTION("malformed content range") { server.MalformNextRange(); }
 SECTION("changing validator") { server.ChangeNextRangeIdentity(); }
 SECTION("ordinary fallback cancellation") {
  server.RestartNextRangedTransfer();
  fallback = true;
 }
 SECTION("unsupported range probe") {
  server.IgnoreRanges();
  fallback = true;
 }
 std::atomic<bool> cancelled{false};
 std::size_t retries = 0;
 bool fallback_reset = false;
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json&) {
  if (event == "benchmark.download.segment_retry") ++retries;
  if (event == "benchmark.download.segmented_fallback") {
   fallback_reset = true;
   CHECK_FALSE(fs::exists(request.destination.string() + ".part"));
   CHECK_FALSE(fs::exists(request.destination.string() + ".part.json"));
  }
 };
 const auto acquire = [&] {
  return download_artifacts({request}, 1, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), [&](const DownloadProgress& update) {
   if (fallback && update.redownload) cancelled.store(true);
  }, trace);
 };
 if (fallback) {
  CHECK_THROWS(acquire());
  CHECK(fallback_reset);
  const auto checkpoint = read_json_file(request.destination.string() + ".part.json");
  CHECK(checkpoint.value("mode", std::string{}) != "segmented");
  CHECK(checkpoint.at("url") == request.url);
  CHECK(checkpoint.at("bytes") == fs::file_size(request.destination.string() + ".part"));
  CHECK(fs::file_size(request.destination.string() + ".part") < bytes);
 } else {
  const auto result = acquire();
  CHECK(result[0].resumed);
  CHECK(result[0].attempts == 2);
  CHECK(retries == 1);
 }
 server.Check();
}
NormalizedAnnotationBuilder seed_open_images(const fs::path& images, const fs::path& seed, std::span<const std::uint8_t> jpeg, std::size_t count, std::span<const std::uint64_t> missing) {
 prepare_cached_image_directory(images);
 BenchmarkEncodedImage::publish(seed, jpeg, {});
 NormalizedAnnotationBuilder index;
 index.source = BenchmarkDatasetSource::kOpenImagesV7;
 index.images.resize(count);
 for (std::size_t position = 0; position < count; ++position) {
  const auto id = position + 1;
  index.images[position].source_image_id = id;
  if (std::ranges::find(missing, id) != missing.end()) continue;
  const auto path = cached_image_path(images, id);
  fs::create_directories(path.parent_path());
  fs::create_hard_link(seed, path);
 }
 return index;
}
TEST_CASE("Open Images admits a third group while two earlier groups retry and preserves each proof", "[backend][data][benchmark][images]") {
 mmltk::testsupport::ScopedTempDir root("open-images-independent-groups");
 const auto cache = BenchmarkCacheLayout::create(root.path());
 const auto images = cache.source_images("open-images") / "train";
 const auto jpeg = make_jpeg(10, 20, 30);
 constexpr std::size_t count = 8193;
 const std::array<std::uint64_t, 3> missing{1, 4097, 8193};
 auto index = seed_open_images(images, root.path() / "seed.jpg", jpeg, count, missing);
 HttpServer first(jpeg), second(jpeg), third(jpeg);
 first.fail_next(1);
 second.fail_next(1);
 first.GateNextRequest();
 second.GateNextRequest();
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 64U << 20, .descriptors = 64}, cancellation);
 std::promise<bool> later;
 std::vector<std::string> throttle_activities;
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json& fields) {
  if (event == "benchmark.images.complete" && fields.at("identity").get<std::string>().find("group-000002") != std::string::npos) later.set_value(true);
  if (event == "benchmark.progress.activity" && fields.contains("source")) {
   const auto activity = fields.at("activity").get<std::string>();
   if (activity.find("server throttled") != std::string::npos) throttle_activities.push_back(activity);
  }
 };
 std::vector<std::string> throttle_updates;
 ProgressReporter progress([&](const BenchmarkCompileProgress& update) {
  if (update.activity.find("server throttled") != std::string::npos && (throttle_updates.empty() || throttle_updates.back() != update.activity)) throttle_updates.push_back(update.activity);
 }, trace);
 std::vector<QuarantinedImage> quarantined;
 auto work = std::async(std::launch::async, [&] {
  return acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, cancellation, &progress, 1, 0, trace, {}, &execution, [&](std::uint64_t id) {
   return id == 1 ? first.url("first") : id == 4097 ? second.url("second") : third.url("third");
  });
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  first.ReleaseRequest();
  second.ReleaseRequest();
 });
 REQUIRE(mmltk::testsupport::await_test_promise(later, "third group publication while earlier retries are held", 10s));
 CHECK_FALSE(fs::exists(images / ".groups" / "group-000000.complete.json"));
 CHECK_FALSE(fs::exists(images / ".groups" / "group-000001.complete.json"));
 REQUIRE(first.WaitRequest());
 REQUIRE(second.WaitRequest());
 first.ReleaseRequest();
 second.ReleaseRequest();
 const auto result = mmltk::testsupport::await_test_future(work, "independent Open Images group settlement", 10s);
 CHECK(result.available_image_ids.size() == count);
 CHECK(result.directory.image_bytes == count * jpeg.size());
 CHECK(quarantined.empty());
 progress.flush();
 // Source facts retain both throttle decisions; observer snapshots may coalesce.
 REQUIRE(throttle_activities.size() == 2);
 CHECK(throttle_activities[0].find("7 concurrent") != std::string::npos);
 CHECK(throttle_activities[1].find("5 concurrent") != std::string::npos);
 CHECK(std::ranges::all_of(throttle_updates, [&](const auto& update) { return std::ranges::find(throttle_activities, update) != throttle_activities.end(); }));
 for (unsigned group = 0; group < 3; ++group) {
  const auto proof = read_json_file(images / ".groups" / (std::string("group-00000") + std::to_string(group) + ".complete.json"));
  const auto expected = group == 2 ? 1 : 4096;
  CHECK(proof.at("image_count") == expected);
  CHECK(proof.at("image_bytes") == expected * jpeg.size());
  CHECK(proof.at("dimensions").size() == expected * 3U);
 }
 CHECK(execution.execution().try_reserve({64U << 20, 64}).has_value());
 first.Check();
 second.Check();
 third.Check();
}
TEST_CASE("Open Images remote absence is bounded and local capacity failure returns every allowance", "[backend][data][benchmark][images]") {
 mmltk::testsupport::ScopedTempDir root("open-images-failure-admission");
 const auto cache = BenchmarkCacheLayout::create(root.path());
 NormalizedAnnotationBuilder index;
 index.source = BenchmarkDatasetSource::kOpenImagesV7;
 index.images = {{.source_image_id = 1}};
 const auto jpeg = make_jpeg(10, 20, 30);
 HttpServer server(jpeg);
 ProgressReporter progress({}, {});
 std::vector<QuarantinedImage> quarantined;
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32U << 20, .descriptors = 32});
 SECTION("remote 404 retains the complete quarantine proof") {
  server.fail_next(kMaximumAttempts, 0, 404);
  const auto result =
   acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, {}, &progress, 1, 0, {}, {}, &execution, [&](std::uint64_t) { return server.url("missing"); });
  CHECK(result.available_image_ids.empty());
  REQUIRE(quarantined.size() == 1);
  CHECK(server.requests() == kMaximumAttempts);
  const auto cached = acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, {}, &progress, 1, 0, {}, {}, &execution,
   [&](std::uint64_t) -> std::string { throw std::logic_error("cached proof requested HTTP"); });
  CHECK(cached.directory.cache_hit);
 }
 SECTION("local allocation failure is fatal") {
  CHECK_THROWS_AS(
   acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, {}, &progress, 1, 0, {}, {}, &execution, [&](std::uint64_t) -> std::string { throw std::bad_alloc{}; }),
   std::bad_alloc);
  CHECK(quarantined.empty());
 }
 CHECK(execution.execution().try_reserve({32U << 20, 32}).has_value());
 server.Check();
}
TEST_CASE("a sixteen-descriptor artifact uses actual free and parent-committed capacity", "[backend][data][benchmark][download][pipeline]") {
 bool parented = false;
 SECTION("one cold artifact fits below five generic headroom descriptors") {}
 SECTION("a source commitment remains usable beside unrelated producers") { parented = true; }
 mmltk::testsupport::ScopedTempDir root("artifact-feasible-sixteen");
 const auto payload = make_payload(4096);
 HttpServer server(payload);
 const auto request = request_for(root.path(), "small", server.url("small"), payload);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256U << 10, .descriptors = 16});
 std::shared_ptr<ArtifactLease> source;
 CompileAllowance unrelated;
 if (parented) {
  source = ArtifactLease::acquire_charged(root.path() / "source.lock", {}, &execution.execution(), CompileResources::handles(1, true, 3));
  unrelated = execution.execution().reserve(CompileResources::handles(5, true));
 }
 const auto results = download_artifacts({request}, 1, {}, {}, {}, {}, &execution, source ? source->allowance() : CompileAllowance{});
 REQUIRE(results.size() == 1);
 CHECK(results[0].size == payload.size());
 CHECK(results[0].attempts == 1);
 CHECK_FALSE(results[0].cache_hit);
 std::ifstream saved(request.destination, std::ios::binary);
 const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{saved}, std::istreambuf_iterator<char>{}};
 CHECK(bytes == payload);
 const auto metadata = read_json_file(request.destination.string() + ".download.json");
 CHECK(metadata.at("complete") == true);
 CHECK(metadata.at("url") == request.url);
 CHECK(metadata.at("size") == payload.size());
 CHECK(metadata.at("attempts") == 1);
 const auto warm = download_artifacts({request}, 1, {}, {}, {}, {}, &execution, source ? source->allowance() : CompileAllowance{});
 CHECK(warm[0].cache_hit);
 CHECK(server.requests() == 1);
 source.reset();
 unrelated = {};
 CHECK(execution.execution().try_reserve({256U << 10, 16}).has_value());
 auto unlocked = ArtifactLease::try_acquire_charged(request.lock_path, {}, &execution.execution());
 CHECK(static_cast<bool>(unlocked));
 server.Check();
}
TEST_CASE("fixed Curl preparation waits before source locks and cancels without retained custody", "[backend][data][benchmark][download][pipeline]") {
 bool cancel = false;
 SECTION("returned consumer credits wake feasible preparation") {}
 SECTION("cancellation settles preparation while capacity remains held") { cancel = true; }
 mmltk::testsupport::ScopedTempDir root("artifact-fixed-preparation");
 const auto payload = make_payload(128);
 HttpServer server(payload);
 const auto request = request_for(root.path(), "small", server.url("small"), payload);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256U << 10, .descriptors = 16});
 auto pressure = execution.execution().reserve(CompileResources::handles(16));
 std::atomic<bool> cancelled{false};
 mmltk::testsupport::TestGate waiting("source observed cancellation after fixed admission wait");
 struct Cancellation {
  const std::atomic<bool>& cancelled_flag;
  mmltk::testsupport::TestGate::Receipt waiting;
  mutable std::atomic<unsigned> observations{0};
  bool cancelled() const noexcept {
   // This observation belongs only to the source controller. Its first call
   // enters the channel; the second follows the fixed-preparation wait.
   if (observations.fetch_add(1) == 1) waiting.ArriveAndWait();
   return cancelled_flag.load();
  }
 } observation{cancelled, waiting.receipt()};
 auto acquisition = std::async(std::launch::async, [&] { return download_artifacts({request}, 1, mmltk::common::concurrency::CancellationObservation::Borrow(observation), {}, {}, {}, &execution); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  waiting.Release();
  pressure = {};
 });
 REQUIRE(waiting.WaitEntered(5s));
 CHECK_FALSE(fs::exists(request.lock_path));
 CHECK(server.requests() == 0);
 if (cancel)
  cancelled.store(true);
 else
  pressure = {};
 waiting.Release();
 if (cancel)
  CHECK_THROWS(mmltk::testsupport::await_test_future(acquisition, "cancelled fixed transport preparation"));
 else {
  const auto result = mmltk::testsupport::await_test_future(acquisition, "fixed transport credit-return wake");
  REQUIRE(result.size() == 1);
  CHECK(result[0].size == payload.size());
  CHECK(read_json_file(request.destination.string() + ".download.json").at("complete") == true);
 }
 pressure = {};
 CHECK(execution.execution().try_reserve({256U << 10, 16}).has_value());
 if (cancel) CHECK_FALSE(fs::exists(request.lock_path));
 server.Check();
}
TEST_CASE("impossible artifact descriptor demand fails without retaining a lease", "[backend][data][benchmark][download]") {
 mmltk::testsupport::ScopedTempDir root("artifact-descriptor-exhaustion");
 const auto payload = make_payload(64);
 HttpServer server(payload);
 const auto request = request_for(root.path(), "small", server.url("small"), payload);
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = 8});
 CHECK_THROWS_AS(download_artifacts({request}, 1, {}, {}, {}, {}, &execution), InsufficientCompileResources);
 CHECK(execution.execution().try_reserve({0, 8}).has_value());
 CHECK(server.requests() == 0);
 CHECK_FALSE(fs::exists(request.lock_path));
 server.Check();
}
TEST_CASE("Open Images returns idle input backing to an oversized pixel consumer under a tiny target", "[backend][data][benchmark][images][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("open-images-tiny-input-target");
 const auto cache = BenchmarkCacheLayout::create(root.path());
 const auto images = cache.source_images("open-images") / "train";
 NormalizedAnnotationBuilder index;
 index.source = BenchmarkDatasetSource::kOpenImagesV7;
 index.images = {{.source_image_id = 1}, {.source_image_id = 2}};
 const auto jpeg = make_jpeg(10, 20, 30);
 HttpServer first(jpeg), later(jpeg);
 first.GateNextRequest();
 later.fail_next(1, 0, 429);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1, .descriptors = 32}, cancellation);
 ProgressReporter progress({}, {});
 std::vector<QuarantinedImage> quarantined;
 mmltk::testsupport::TestGate consumer("oversized Open Images pixel consumer");
 auto acquisition = std::async(std::launch::async, [&] {
  return acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, cancellation, &progress, 1, 0, {}, {}, &execution,
   [&](std::uint64_t id) { return id == 1 ? first.url("first") : later.url("later"); });
 });
 std::future<void> pixels;
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  first.ReleaseRequest();
  consumer.Release();
 });
 REQUIRE(first.WaitRequest());
 pixels = std::async(std::launch::async, [&] {
  execution.execution().run(CompileTaskKind::Pixels, {32U << 20, 1}, [&](std::size_t) {
   BenchmarkImageValidator validator;
   validator.validate_decodable_file(cached_image_path(images, 1), 16, 8);
   consumer.receipt().ArriveAndWait();
  });
 });
 // No transfer can complete at this gate. The shared event observes the newly
 // queued consumer; its complete grant cannot fit beside the encoded input.
 wait_benchmark_pressure(execution);
 CHECK(execution.execution().resource_pressure());
 first.ReleaseRequest();
 REQUIRE(consumer.WaitEntered(5s));
 // An immediately rejected first request can settle before the CPU consumer
 // wins admission. Its retry cannot retain input beside this oversized job.
 CHECK(later.requests() <= 1);
 CHECK(acquisition.wait_for(0ms) == std::future_status::timeout);
 consumer.Release();
 mmltk::testsupport::await_test_future(pixels, "oversized pixel grant return");
 const auto result = mmltk::testsupport::await_test_future(acquisition, "tiny-target Open Images retry settlement", 10s);
 CHECK(result.available_image_ids == std::vector<std::uint64_t>{1, 2});
 CHECK(quarantined.empty());
 CHECK(later.requests() == 2);
 CHECK(execution.execution().try_reserve({32U << 20, 32}).has_value());
 first.Check();
 later.Check();
}
TEST_CASE("Open Images consumes a repaired saved file before recycling its exclusive input slot", "[backend][data][benchmark][images][pipeline]") {
 enum class Repair { Valid, Dimensions, SavedFile, Missing, Allocation } repair = Repair::Valid;
 SECTION("valid repair precedes the next member") {}
 SECTION("changed dimensions are permanently quarantined") { repair = Repair::Dimensions; }
 SECTION("the saved file is validated instead of the encoded buffer") { repair = Repair::SavedFile; }
 SECTION("a missing saved file remains fatal") { repair = Repair::Missing; }
 SECTION("repair allocation failure remains fatal") { repair = Repair::Allocation; }
 mmltk::testsupport::ScopedTempDir root("open-images-exclusive-repair");
 const auto cache = BenchmarkCacheLayout::create(root.path());
 const auto images = cache.source_images("open-images") / "train";
 const auto jpeg = make_jpeg(10, 20, 30);
 constexpr std::size_t count = 4097;
 const std::array<std::uint64_t, 3> missing{1, 2, count};
 auto index = seed_open_images(images, root.path() / "seed.jpg", jpeg, count, missing);
 HttpServer first(jpeg), member(jpeg), later(jpeg);
 member.GateNextRequest();
 // CLEANUP-IGNORE: Independent repair and consumer cases share throttle/cancellation/resource-limit setup; the transfers and failure oracles differ.
 later.fail_next(1, 0, 429);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1, .descriptors = 32}, cancellation);
 mmltk::testsupport::TestGate validation("repaired file before exclusive full decode");
 std::atomic<unsigned> validations{0};
 ProgressReporter progress({}, {});
 const auto repair_input = [&](std::uint64_t id) {
  if (id != 1 || validations.fetch_add(1) != 0) return;
  validation.receipt().ArriveAndWait();
  if (repair == Repair::Allocation) throw std::bad_alloc{};
 };
 std::vector<QuarantinedImage> quarantined;
 const ImageDecodeProbe probe{1, repair == Repair::Dimensions ? 17U : 16U, 8};
 auto acquisition = std::async(std::launch::async, [&] {
  return acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, cancellation, &progress, 1, 0, {}, probe, &execution, [&](std::uint64_t id) {
   return id == 1 ? first.url("repair") : id == 2 ? member.url("member") : later.url("later");
  }, {}, repair_input);
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  validation.Release();
  member.ReleaseRequest();
 });
 const bool entered = validation.WaitEntered(5s);
 if (!entered) {
  CAPTURE(first.requests(), member.requests(), later.requests(), validations.load());
  if (acquisition.wait_for(0ms) == std::future_status::ready) (void)acquisition.get();
  REQUIRE(entered);
 }
 CHECK(member.requests() == 0);
 CHECK(later.requests() == 0);  // Its group is pending under the original tiny target.
 CHECK_FALSE(execution.geometry(images, 1));
 CHECK_FALSE(execution.execution().try_reserve({1, 0}).has_value());
 const auto repaired_path = cached_image_path(images, 1);
 if (repair == Repair::SavedFile) {
  auto damaged = jpeg;
  damaged[0] = 0;
  BenchmarkEncodedImage::publish(repaired_path, damaged, {});
 } else if (repair == Repair::Missing)
  fs::remove(repaired_path);
 validation.Release();
 if (repair == Repair::Missing || repair == Repair::Allocation) {
  if (repair == Repair::Allocation)
   CHECK_THROWS_AS(mmltk::testsupport::await_test_future(acquisition, "fatal repair allocation"), std::bad_alloc);
  else
   CHECK_THROWS(mmltk::testsupport::await_test_future(acquisition, "missing saved repair file"));
  CHECK(quarantined.empty());
  CHECK(member.requests() == 0);
  CHECK_FALSE(fs::exists(images / ".groups" / "group-000000.complete.json"));
 } else {
  REQUIRE(member.WaitRequest());
  CHECK(validations.load() == 1);
  const bool valid = repair == Repair::Valid;
  CHECK(static_cast<bool>(execution.geometry(images, 1)) == valid);  // Individually admitted geometry precedes group proof publication.
  CHECK(fs::exists(repaired_path) == valid);
  CHECK_FALSE(fs::exists(images / ".groups" / "group-000000.complete.json"));
  member.ReleaseRequest();
  const auto result = mmltk::testsupport::await_test_future(acquisition, "repair slot reuse with a later group", 10s);
  CHECK(result.available_image_ids.size() == count - (valid ? 0 : 1));
  CHECK(result.available_image_ids.front() == (valid ? 1 : 2));
  CHECK(result.directory.image_bytes == result.available_image_ids.size() * jpeg.size());
  CHECK(static_cast<bool>(execution.geometry(images, 1)) == valid);
  const auto geometry = execution.geometry(cache.source_images("open-images") / "train", result.available_image_ids.front());
  REQUIRE(geometry);
  CHECK(geometry->width == 16);
  CHECK(geometry->height == 8);
  CHECK(quarantined.size() == (valid ? 0 : 1));
  if (!valid) {
   CHECK(quarantined[0].image_id == 1);
   CHECK(quarantined[0].reason.starts_with("permanently undecodable after bounded repair:"));
  }
  const auto proof = read_json_file(images / ".groups" / "group-000000.complete.json");
  CHECK(proof.at("image_count") == (valid ? 4096 : 4095));
  CHECK(proof.at("image_bytes") == (valid ? 4096 : 4095) * jpeg.size());
  CHECK(proof.at("dimensions").size() == (valid ? 4096 : 4095) * 3U);
  CHECK(proof.at("quarantined").size() == (valid ? 0 : 1));
  CHECK(read_json_file(images / ".groups" / "group-000001.complete.json").at("image_bytes") == jpeg.size());
  CHECK(later.requests() == 2);
 }
 CHECK(first.requests() == 1);
 CHECK(execution.execution().try_reserve({32U << 20, 32}).has_value());
 first.Check();
 member.Check();
 later.Check();
}
TEST_CASE("range cancellation after a committed checkpoint preserves that snapshot for restart", "[backend][data][benchmark][download]") {
 mmltk::testsupport::ScopedTempDir root("range-checkpoint-cancellation");
 constexpr std::size_t bytes = 512U << 20;
 constexpr std::uint64_t tail = 2U << 20;
 HttpServer server(bytes);
 DownloadRequest request{"range", server.url("range"), root.path() / "artifact", root.path() / "artifact.lock", bytes};
 seed_segmented_tail(request, tail);
 const auto metadata_path = request.destination.string() + ".part.json";
 std::atomic<bool> cancelled{false};
 bool committed = false;
 CHECK_THROWS(download_artifacts({request}, 1, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), [&](const DownloadProgress& update) {
  if (update.transfer.completed_bytes <= bytes - tail * 2) return;
  const auto snapshot = read_json_file(metadata_path);
  if (snapshot.at("segments")[0].at("completed") == bytes / 2) {
   committed = true;
   cancelled.store(true);
  }
 }));
 CHECK(committed);
 const auto checkpoint = read_json_file(metadata_path);
 CHECK(checkpoint.at("segments")[0].at("completed") == bytes / 2);
 CHECK(checkpoint.at("segments")[1].at("completed") == bytes / 2 - tail);
 CHECK_FALSE(fs::exists(request.destination));
 const auto resumed = download_artifacts({request}, 2, {});
 CHECK(resumed[0].resumed);
 CHECK(resumed[0].size == bytes);
 server.Check();
}
TEST_CASE("out-of-order redirected ranges checkpoint only committed writes and resume on one connection", "[backend][data][benchmark][download]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("out-of-order-range-checkpoint");
 constexpr std::size_t bytes = 512U << 20;
 constexpr std::uint64_t tail = 2U << 20;
 HttpServer origin(bytes), earlier(bytes), later(bytes);
 origin.RedirectRanges(earlier.url("earlier"), later.url("later"), bytes / 2);
 earlier.GateNextTransfer();
 DownloadRequest request{"range", origin.url("range"), root.path() / "artifact", root.path() / "artifact.lock", bytes};
 seed_segmented_tail(request, tail);
 const auto metadata_path = request.destination.string() + ".part.json";
 std::atomic<bool> cancelled{false}, published{false};
 BenchmarkCompilePipeline execution(2);
 std::promise<nlohmann::json> later_checkpoint;
 auto work = std::async(std::launch::async, [&] {
  return download_artifacts({request}, 2, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), [&](const DownloadProgress& update) {
   if (published.load() || update.transfer.completed_bytes < bytes - tail) return;
   auto snapshot = read_json_file(metadata_path);
   if (snapshot.at("segments")[1].at("completed") == bytes / 2 && !published.exchange(true)) later_checkpoint.set_value(std::move(snapshot));
  }, {}, {}, &execution);
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  earlier.ReleasePartial();
 });
 REQUIRE(earlier.WaitPartial());
 const auto checkpoint = mmltk::testsupport::await_test_promise(later_checkpoint, "later range committed checkpoint while an earlier write is held");
 // The held first range has written bytes, but its attempt has not committed.
 CHECK(checkpoint.at("segments")[0].at("completed") == bytes / 2 - tail);
 CHECK(checkpoint.at("segments")[1].at("completed") == bytes / 2);
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 cancelled.store(true);
 earlier.ReleasePartial();
 CHECK_THROWS(mmltk::testsupport::await_test_future(work, "cancelled out-of-order ranges"));
 const auto resumed = download_artifacts({request}, 1, {});
 CHECK(resumed[0].resumed);
 CHECK(read_json_file(request.destination.string() + ".download.json").at("segments") == 2);
 CHECK(execution.execution().try_reserve({execution.execution().transient_target(), execution.execution().descriptor_limit()}).has_value());
 origin.Check();
 earlier.Check();
 later.Check();
}
TEST_CASE("many stored ranges leave fair transport admission for an independent whole artifact", "[backend][data][benchmark][download]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("many-range-fair-admission");
 constexpr std::size_t bytes = 512U << 20;
 HttpServer origin(bytes), held(bytes), later(bytes);
 origin.RedirectRanges(held.url("held"), later.url("later"), bytes / 8);
 held.GateNextTransfer();
 const auto payload = make_payload(32768);
 HttpServer whole(payload);
 DownloadRequest large{"ranges", origin.url("ranges"), root.path() / "large", root.path() / "large.lock", bytes};
 const auto small = request_for(root.path(), "whole", whole.url("whole"), payload);
 seed_segmented_tail(large, 2U << 20, 8);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(8, {}, {.transient_bytes = 2U << 20, .descriptors = 24}, cancellation);
 auto ranges = std::async(std::launch::async, [&] { return download_artifacts({large}, 8, cancellation, {}, {}, {}, &execution); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  held.ReleasePartial();
 });
 REQUIRE(held.WaitPartial());
 auto independent = std::async(std::launch::async, [&] { return download_artifacts({small}, 1, cancellation, {}, {}, {}, &execution); });
 CHECK(mmltk::testsupport::await_test_future(independent, "whole-file turn beside eight stored ranges")[0].size == payload.size());
 CHECK(ranges.wait_for(0ms) == std::future_status::timeout);
 held.ReleasePartial();
 CHECK(mmltk::testsupport::await_test_future(ranges, "eight-range settlement")[0].resumed);
 CHECK(read_json_file(large.destination.string() + ".download.json").at("segments") == 8);
 CHECK(execution.execution().try_reserve({2U << 20, 24}).has_value());
 origin.Check();
 held.Check();
 later.Check();
 whole.Check();
}
TEST_CASE("ordinary retry and publication return active work before their source controller continues", "[backend][data][benchmark][download]") {
 bool retry = false;
 SECTION("retry processing before backoff") { retry = true; }
 SECTION("durable publication callback") {}
 mmltk::testsupport::ScopedTempDir root("ordinary-settled-request-work");
 const auto payload = make_payload(32768);
 HttpServer first(payload), second(payload);
 if (retry) first.fail_next(1);
 const auto request = request_for(root.path(), "first", first.url("first"), payload);
 const auto independent = request_for(root.path(), "second", second.url("second"), payload);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256U << 10, .descriptors = 24});
 mmltk::testsupport::TestGate boundary("ordinary source after Curl completion");
 std::atomic<bool> cancelled{false}, once{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json&) {
  if (event == (retry ? "benchmark.download.attempt_failed" : "benchmark.download.complete") && !once.exchange(true)) boundary.receipt().ArriveAndWait();
 };
 auto work = std::async(std::launch::async, [&] { return download_artifacts({request}, 1, cancellation, {}, trace, {}, &execution); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  boundary.Release();
 });
 REQUIRE(boundary.WaitEntered(5s));
 CHECK(execution.execution().try_reserve({256U << 10, 0}).has_value());
 auto ready = std::async(std::launch::async, [&] { return download_artifacts({independent}, 1, cancellation, {}, {}, {}, &execution); });
 CHECK(mmltk::testsupport::await_test_future(ready, "independent source while retry/publication owner is held")[0].size == payload.size());
 CHECK(work.wait_for(0ms) == std::future_status::timeout);
 boundary.Release();
 CHECK(mmltk::testsupport::await_test_future(work, "ordinary source continuation")[0].attempts == (retry ? 2 : 1));
 CHECK(execution.execution().try_reserve({256U << 10, 24}).has_value());
 first.Check();
 second.Check();
}
TEST_CASE("Open Images retired backing admits pixel work while an unrelated ordinary socket remains open", "[backend][data][benchmark][images][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("independent-image-backing-retirement");
 const auto cache = BenchmarkCacheLayout::create(root.path());
 const auto jpeg = make_jpeg(10, 20, 30);
 HttpServer image(jpeg);
 image.GateNextRequest();
 const auto payload = make_payload(1U << 20);
 HttpServer ordinary(payload);
 ordinary.GateNextTransfer();
 const auto request = request_for(root.path(), "ordinary", ordinary.url("ordinary"), payload);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 // The complete pixel grant fits beside the ordinary request's actual Curl
 // workspace, but cannot fit beside even one retained encoded/header slot.
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = (32U << 20) + (256U << 10), .descriptors = 32}, cancellation);
 auto download = std::async(std::launch::async, [&] { return download_artifacts({request}, 1, cancellation, {}, {}, {}, &execution); });
 NormalizedAnnotationBuilder index;
 index.source = BenchmarkDatasetSource::kOpenImagesV7;
 index.images = {{.source_image_id = 1}};
 ProgressReporter progress({}, {});
 std::vector<QuarantinedImage> quarantined;
 std::future<AcquiredOpenImages> acquisition;
 std::future<void> pixels;
 mmltk::testsupport::TestGate consumed("pixel input after image backing retirement");
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  image.ReleaseRequest();
  ordinary.ReleasePartial();
  consumed.Release();
 });
 REQUIRE(ordinary.WaitPartial());
 acquisition = std::async(std::launch::async, [&] {
  return acquire_open_images(
   cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, cancellation, &progress, 1, 0, {}, {}, &execution, [&](std::uint64_t) { return image.url("image"); });
 });
 REQUIRE(image.WaitRequest());
 pixels = std::async(std::launch::async, [&] {
  execution.execution().run(CompileTaskKind::Pixels, {32U << 20, 1}, [&](std::size_t) {
   BenchmarkImageValidator validator;
   validator.validate_decodable_file(cached_image_path(cache.source_images("open-images") / "train", 1), 16, 8);
   consumed.receipt().ArriveAndWait();
  });
 });
 wait_benchmark_pressure(execution);
 CHECK(execution.execution().resource_pressure());
 image.ReleaseRequest();
 REQUIRE(consumed.WaitEntered(5s));
 CHECK(download.wait_for(0ms) == std::future_status::timeout);
 consumed.Release();
 mmltk::testsupport::await_test_future(pixels, "pixel work before unrelated network settlement");
 CHECK(mmltk::testsupport::await_test_future(acquisition, "image acquisition beside held ordinary request").available_image_ids == std::vector<std::uint64_t>{1});
 CHECK(download.wait_for(0ms) == std::future_status::timeout);
 ordinary.ReleasePartial();
 CHECK(mmltk::testsupport::await_test_future(download, "ordinary socket retirement")[0].size == payload.size());
 CHECK(execution.execution().try_reserve({(32U << 20) + (256U << 10), 32}).has_value());
 image.Check();
 ordinary.Check();
}
TEST_CASE("pending and active Curl cancellation detach callbacks and return request custody", "[backend][data][benchmark][download][pipeline]") {
 bool pending = false, idle_parser = false;
 SECTION("pending byte admission") { pending = true; }
 SECTION("admitted physical socket") {}
 SECTION("idle parser yields backing before HTTP admission") { idle_parser = true; }
 mmltk::testsupport::ScopedTempDir root("transport-cancellation-custody");
 const auto payload = make_payload(1U << 20);
 HttpServer server(payload);
 server.GateNextTransfer();
 const auto request = request_for(root.path(), "cancel", server.url("cancel"), payload);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256U << 10, .descriptors = 24});
 std::vector<std::byte> scratch;
 std::atomic<bool> scratch_retired{false};
 CompileExecution::Workspace workspace(execution.execution(), [&](std::size_t) noexcept {
  std::vector<std::byte>().swap(scratch);
  scratch_retired.store(true);
 });
 if (idle_parser) {
  execution.execution().for_each(CompileTaskKind::Metadata, 1, [](std::size_t) { return CompileResources{256U << 10, 0}; }, [&](std::size_t) { scratch.resize(256U << 10); }, workspace);
  REQUIRE(execution.execution().resource_usage().bytes == 256U << 10);
 }
 CompileAllowance pressure;
 if (pending) pressure = execution.execution().reserve({256U << 10, 0});
 mmltk::testsupport::TestGate submitted("source submitted pending request");
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json&) {
  if (pending && event == "benchmark.download.start") submitted.receipt().ArriveAndWait();
 };
 auto work = std::async(std::launch::async, [&] { return download_artifacts({request}, 1, cancellation, {}, trace, {}, &execution); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  submitted.Release();
  server.ReleasePartial();
 });
 if (pending)
  REQUIRE(submitted.WaitEntered(5s));
 else
  REQUIRE(server.WaitPartial());
 if (idle_parser) CHECK(scratch_retired.load());
 cancelled.store(true);
 submitted.Release();
 CHECK_THROWS(mmltk::testsupport::await_test_future(work, "cancelled pending/active request settlement"));
 if (pending) CHECK(server.requests() == 0);
 pressure = {};
 CHECK(execution.execution().try_reserve({256U << 10, 24}).has_value());
 server.ReleasePartial();
 server.Check();
}
TEST_CASE("one logical Curl connection preserves simultaneous native fallback redirects and cancelled borrowers", "[backend][data][benchmark][download][pipeline]") {
 enum class Cancel { None, Pending, Active } cancel = Cancel::None;
 SECTION("both viable requests complete") {}
 SECTION("pending cancellation keeps the active redirect alive") { cancel = Cancel::Pending; }
 SECTION("active cancellation admits the pending request") { cancel = Cancel::Active; }
 const auto payload = make_payload(128);
 HttpServer origin(payload), destination(payload), independent(payload);
 origin.RedirectNextTransfer(destination.url("redirected"));
 destination.GateNextRequest();
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 512U << 10, .descriptors = 24});
 CurlSocketObservation sockets;
 auto channel = execution.curl().channel(BenchmarkCurl::Class::Artifact);
 const auto port = std::to_string(origin.port());
 mmltk::backend::data::testsupport::PendingIpv6Connect pending(origin.port());
 // The IPv6 connect stays pending while the native IPv4 candidate succeeds.
 ObservedCurlTransfer first("http://fixture.invalid:" + port + "/origin", sockets, "fixture.invalid:" + port + ":[::1],127.0.0.1");
 ObservedCurlTransfer second(independent.url("independent"), sockets);
 channel->add(first.easy.get());
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  destination.ReleaseRequest();
  if (channel) {
   channel->remove(first.easy.get());
   channel->remove(second.easy.get());
  }
 });
 REQUIRE(destination.WaitRequest());
 CHECK(sockets.live() == 1);
 channel->add(second.easy.get());
 if (cancel == Cancel::Pending) channel->remove(second.easy.get());
 if (cancel == Cancel::Active)
  channel->remove(first.easy.get());
 else
  destination.ReleaseRequest();
 const auto expected = cancel == Cancel::None ? 2U : 1U;
 unsigned completed = 0;
 const auto deadline = std::chrono::steady_clock::now() + 5s;
 while (completed < expected && std::chrono::steady_clock::now() < deadline) {
  if (auto result = channel->next()) {
   CHECK(result->result == CURLE_OK);
   CHECK(result->handle == (cancel == Cancel::Active ? second.easy.get() : completed == 0 ? first.easy.get() : second.easy.get()));
   ++completed;
  } else
   channel->wait_until(deadline);
 }
 REQUIRE(completed == expected);
 CHECK(first.bytes == (cancel == Cancel::Active ? std::vector<std::uint8_t>{} : payload));
 CHECK(second.bytes == (cancel == Cancel::Pending ? std::vector<std::uint8_t>{} : payload));
 CHECK_FALSE(sockets.failure);
 CHECK(sockets.attempts >= 3);
 CHECK(sockets.peak == 2);
 CHECK(origin.requests() == 1);
 CHECK(destination.requests() == 1);
 CHECK(first.responses == (cancel == Cancel::Active ? 1 : 2));
 channel.reset();
 CHECK(sockets.live() == 0);
 CHECK(execution.execution().try_reserve({512U << 10, 24}).has_value());
 destination.ReleaseRequest();
 origin.Check();
 destination.Check();
 independent.Check();
}
TEST_CASE("warm Curl sockets supply native opportunity through reuse redirect and reconnect", "[backend][data][benchmark][download][pipeline]") {
 const auto payload = make_payload(128);
 HttpServer origin(payload), destination(payload);
 origin.KeepConnectionsAlive();
 destination.KeepConnectionsAlive();
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 512U << 10, .descriptors = 24});
 CurlSocketObservation sockets;
 auto channel = execution.curl().channel(BenchmarkCurl::Class::Artifact);
 std::vector<std::unique_ptr<ObservedCurlTransfer>> transfers;
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  origin.ReleaseRequest();
  destination.ReleaseRequest();
  if (channel)
   for (const auto& transfer : transfers) channel->remove(transfer->easy.get());
 });
 const auto add = [&](std::string url, std::string addresses = {}) -> ObservedCurlTransfer& {
  transfers.push_back(std::make_unique<ObservedCurlTransfer>(std::move(url), sockets, std::move(addresses)));
  auto& transfer = *transfers.back();
  channel->add(transfer.easy.get());
  return transfer;
 };
 const auto complete = [&](ObservedCurlTransfer& transfer, std::size_t responses) {
  const auto result = await_curl(*channel);
  CHECK(result.handle == transfer.easy.get());
  CHECK(result.result == CURLE_OK);
  CHECK(transfer.bytes == payload);
  CHECK(transfer.responses == responses);
 };
 complete(add(origin.url("warm")), 1);
 REQUIRE(sockets.live() == 1);
 CHECK(sockets.attempts == 1);
 // The fixed five-descriptor minimum now includes the cached fd. Only its
 // reusable candidate and resolver promises remain; no redundant fresh pair
 // can be drawn beside this held consumer.
 auto occupied = execution.execution().reserve(CompileResources::handles(19));
 origin.GateNextRequest();
 auto& reused = add(origin.url("reuse"));
 REQUIRE(origin.WaitRequest());
 CHECK(sockets.attempts == 1);
 origin.ReleaseRequest();
 complete(reused, 1);
 const auto port = std::to_string(destination.port());
 mmltk::backend::data::testsupport::PendingIpv6Connect pending(destination.port());
 origin.RedirectNextTransfer("http://redirect.invalid:" + port + "/hop");
 complete(add(origin.url("redirect"), "redirect.invalid:" + port + ":[::1],127.0.0.1"), 2);
 CHECK(sockets.peak == 2);
 CHECK(sockets.attempts == 3);
 const auto origin_port = std::to_string(origin.port());
 complete(add("http://reconnect.invalid:" + origin_port + "/reconnect", "reconnect.invalid:" + origin_port + ":127.0.0.2,127.0.0.1"), 1);
 CHECK(sockets.attempts == 5);
 CHECK(origin.requests() == 4);
 CHECK(destination.requests() == 1);
 CHECK(sockets.live() == 1);
 CHECK_FALSE(sockets.failure);
 channel.reset();
 CHECK(sockets.live() == 0);
 occupied = {};
 CHECK(execution.execution().try_reserve({512U << 10, 24}).has_value());
 origin.Check();
 destination.Check();
}
TEST_CASE("saturated mixed Curl admission guarantees native candidates and returns cancelled custody", "[backend][data][benchmark][download][pipeline]") {
 const auto payload = make_payload(128);
 constexpr std::size_t aggregate = 10, descriptors = 96;
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = aggregate * (256U << 10), .descriptors = descriptors});
 CurlSocketObservation sockets;
 std::vector<std::unique_ptr<HttpServer>> servers;
 std::vector<std::unique_ptr<ObservedCurlTransfer>> transfers;
 auto ordinary = execution.curl().channel(BenchmarkCurl::Class::Artifact);
 auto images = execution.curl().channel(BenchmarkCurl::Class::OpenImages);
 for (std::size_t i = 0; i <= aggregate; ++i) {
  servers.push_back(std::make_unique<HttpServer>(payload));
  servers.back()->GateNextRequest();
 }
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  for (const auto& server : servers) server->ReleaseRequest();
  for (std::size_t i = 0; i < transfers.size(); ++i) {
   auto* channel = i == 0 || i == aggregate ? ordinary.get() : images.get();
   if (channel) channel->remove(transfers[i]->easy.get());
  }
 });
 for (std::size_t i = 0; i < aggregate - 1; ++i) {
  transfers.push_back(std::make_unique<ObservedCurlTransfer>(servers[i]->url("held"), sockets));
  (i ? images : ordinary)->add(transfers.back()->easy.get());
  REQUIRE(servers[i]->WaitRequest());
 }
 REQUIRE(sockets.live() == aggregate - 1);
 mmltk::backend::data::testsupport::PendingIpv6Connect pending(servers[aggregate - 1]->port());
 const auto port = std::to_string(servers[aggregate - 1]->port());
 transfers.push_back(std::make_unique<ObservedCurlTransfer>("http://saturated.invalid:" + port + "/last", sockets, "saturated.invalid:" + port + ":[::1],127.0.0.1"));
 images->add(transfers.back()->easy.get());
 REQUIRE(servers[aggregate - 1]->WaitRequest());
 CHECK(sockets.live() == aggregate);
 CHECK(sockets.peak == aggregate + 1);  // Candidate overlap while all logical turns are occupied.
 CHECK(sockets.peak <= 2 * aggregate);
 const auto charged = benchmark_curl_envelope().demand(aggregate).descriptors;
 CHECK(execution.execution().try_reserve(CompileResources::handles(descriptors - charged)).has_value());
 CHECK_FALSE(execution.execution().try_reserve(CompileResources::handles(descriptors - charged + 1)).has_value());
 transfers.push_back(std::make_unique<ObservedCurlTransfer>(servers[aggregate]->url("pending"), sockets));
 ordinary->add(transfers.back()->easy.get());
 ordinary->remove(transfers.back()->easy.get());
 CHECK(servers[aggregate]->requests() == 0);
 images->remove(transfers[aggregate - 1]->easy.get());
 CHECK(transfers[aggregate - 1]->bytes.empty());
 for (std::size_t i = 0; i < aggregate - 1; ++i) servers[i]->ReleaseRequest();
 const auto first = await_curl(*ordinary);
 CHECK(first.result == CURLE_OK);
 CHECK(first.handle == transfers[0]->easy.get());
 for (std::size_t i = 1; i < aggregate - 1; ++i) CHECK(await_curl(*images).result == CURLE_OK);
 for (std::size_t i = 0; i < aggregate - 1; ++i) {
  CHECK(transfers[i]->bytes == payload);
  CHECK(transfers[i]->responses == 1);
  CHECK(servers[i]->requests() == 1);
 }
 CHECK_FALSE(sockets.failure);
 ordinary.reset();
 images.reset();
 CHECK(sockets.live() == 0);
 CHECK(execution.execution().try_reserve({aggregate * (256U << 10), descriptors}).has_value());
 for (const auto& server : servers) {
  server->ReleaseRequest();
  server->Check();
 }
}
TEST_CASE("idle Curl cache leaves declared archive continuation feasible at thirteen descriptors", "[backend][data][benchmark][download][pipeline]") {
 bool late = false;
 SECTION("archive declaration precedes image work and native eviction rebalances custody") {}
 SECTION("stronger archive declaration contracts an already idle native cache") { late = true; }
 mmltk::testsupport::ScopedTempDir root("idle-cache-source-continuation");
 const auto payload = make_payload(128);
 const auto slot_bytes = (256U << 10) + payload.size();
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 3U * slot_bytes, .descriptors = 13});
 const auto archive_resources = CompileResources::handles(1, true, 4);
 const auto group_resources = CompileResources::handles(1, true, 1);
 CHECK(execution.execution().descriptor_ceiling(archive_resources) == execution.execution().descriptor_limit());  // The dependent promise already protects its consumer capacity.
 CurlSocketObservation sockets;
 std::unique_ptr<BenchmarkCurl::Channel> preparation;
 if (!late) preparation = execution.curl().channel(BenchmarkCurl::Class::Artifact, {}, archive_resources);
 auto images = execution.curl().channel(BenchmarkCurl::Class::OpenImages, {}, group_resources);
 auto group = execution.execution().reserve(group_resources);
 std::vector<CompileAllowance> inputs;
 std::vector<std::unique_ptr<HttpServer>> servers;
 std::vector<std::unique_ptr<ObservedCurlTransfer>> transfers;
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  for (const auto& server : servers) server->ReleaseRequest();
  if (images)
   for (const auto& transfer : transfers) images->remove(transfer->easy.get());
 });
 for (std::size_t i = 0; i < 3; ++i) {
  servers.push_back(std::make_unique<HttpServer>(payload));
  servers.back()->KeepConnectionsAlive();
  servers.back()->GateNextRequest();
  inputs.push_back(execution.execution().reserve(benchmark_curl_input_resources(payload.size()), group));
  transfers.push_back(std::make_unique<ObservedCurlTransfer>(servers.back()->url("image"), sockets));
  images->add(transfers.back()->easy.get(), false, inputs.back());
  REQUIRE(servers.back()->WaitRequest());
 }
 REQUIRE(sockets.live() == 3);
 CHECK(sockets.peak == 3);
 CHECK_FALSE(execution.execution().try_reserve(CompileResources::handles(1)).has_value());
 // Complete the first fixed-child socket while the other two are busy. At the
 // smaller cache limit it is the only evictable connection, so surviving native
 // sockets must receive its fixed custody before independent grants return.
 for (std::size_t i = 0; i < 3; ++i) {
  servers[i]->ReleaseRequest();
  const auto result = await_curl(*images);
  CHECK(result.handle == transfers[i]->easy.get());
  CHECK(result.result == CURLE_OK);
  CHECK(transfers[i]->bytes == payload);
  CHECK(transfers[i]->responses == 1);
  CHECK(servers[i]->requests() == 1);
 }
 inputs.clear();
 group = {};
 if (late) {
  REQUIRE(sockets.live() == 3);
  preparation = execution.curl().channel(BenchmarkCurl::Class::Artifact, {}, archive_resources);
  CHECK(sockets.live() == 0);  // Lowering MAXCONNECTS alone cannot satisfy this.
 } else
  CHECK(sockets.live() == 2);
 images.reset();
 auto archive = ArtifactLease::try_acquire_charged(root.path() / "archive.lock", {}, &execution.execution(), archive_resources);
 REQUIRE(archive);
 // A retained endpoint is reused with the five-credit archive lease still
 // present. After late contraction, warm it once and then prove native reuse.
 HttpServer cancellation_server(payload);
 cancellation_server.GateNextRequest();
 ObservedCurlTransfer warm(servers[2]->url("warm"), sockets), reused(servers[2]->url("reuse"), sockets), cancelled(cancellation_server.url("cancel"), sockets);
 const mmltk::testsupport::ScopedTestCleanup detach([&] {
  cancellation_server.ReleaseRequest();
  if (preparation)
   for (auto* transfer : {&warm, &reused, &cancelled}) preparation->remove(transfer->easy.get());
 });
 const auto before_warm = sockets.attempts;
 preparation->add(warm.easy.get());
 CHECK(await_curl(*preparation).result == CURLE_OK);
 CHECK(warm.bytes == payload);
 CHECK(sockets.attempts == before_warm + (late ? 1 : 0));
 const auto before_reuse = sockets.attempts;
 preparation->add(reused.easy.get());
 CHECK(await_curl(*preparation).result == CURLE_OK);
 CHECK(reused.bytes == payload);
 CHECK(sockets.attempts == before_reuse);
 CHECK(servers[2]->requests() == 3);
 preparation->add(cancelled.easy.get());
 REQUIRE(cancellation_server.WaitRequest());
 preparation->remove(cancelled.easy.get());
 CHECK(cancelled.bytes.empty());
 CHECK(cancellation_server.requests() == 1);
 CHECK_FALSE(sockets.failure);
 archive.reset();
 preparation.reset();
 CHECK(sockets.live() == 0);
 CHECK(execution.execution().try_reserve({3U * slot_bytes, 13}).has_value());
 cancellation_server.ReleaseRequest();
 cancellation_server.Check();
 for (const auto& server : servers) server->Check();
}
TEST_CASE("large ordinary first attempts retain metadata meaning through reset cancellation and retry", "[backend][data][benchmark][download]") {
 enum class Metadata { Valid, Missing, Malformed, Schema, Url, Mode, Validator } metadata_kind = Metadata::Valid;
 SECTION("valid checkpoint resumes and retry reads its newer checkpoint") {}
 SECTION("missing checkpoint resets") { metadata_kind = Metadata::Missing; }
 SECTION("malformed checkpoint resets") { metadata_kind = Metadata::Malformed; }
 SECTION("different schema resets") { metadata_kind = Metadata::Schema; }
 SECTION("different URL resets") { metadata_kind = Metadata::Url; }
 SECTION("invalid mode type resets") { metadata_kind = Metadata::Mode; }
 SECTION("missing validator resets") { metadata_kind = Metadata::Validator; }
 mmltk::testsupport::ScopedTempDir root("large-ordinary-metadata");
 constexpr std::size_t bytes = 512U << 20;
 HttpServer server(bytes);
 DownloadRequest request{"ordinary", server.url("ordinary"), root.path() / "artifact", root.path() / "artifact.lock", bytes};
 const auto partial_path = request.destination.string() + ".part";
 const auto metadata_path = request.destination.string() + ".part.json";
 const auto prefix = make_payload(HttpServer::partial_bytes);
 {
  const mmltk::common::io::ScopedFd partial(::open(partial_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
  REQUIRE(partial.get() >= 0);
  REQUIRE(::write(partial.get(), prefix.data(), prefix.size()) == static_cast<ssize_t>(prefix.size()));
 }
 nlohmann::json metadata{{"schema_version", kBenchmarkCacheSchemaVersion}, {"url", request.url}, {"etag", "\"benchmark-test-etag\""}, {"bytes", prefix.size()}};
 if (metadata_kind == Metadata::Schema) metadata["schema_version"] = 0;
 if (metadata_kind == Metadata::Url) metadata["url"] = "http://different.invalid/artifact";
 if (metadata_kind == Metadata::Mode) metadata["mode"] = 7;
 if (metadata_kind == Metadata::Validator) metadata["etag"] = "";
 if (metadata_kind == Metadata::Malformed)
  write_text(metadata_path, "{broken");
 else if (metadata_kind != Metadata::Missing)
  write_json_atomically(metadata_path, metadata, {});
 const bool valid = metadata_kind == Metadata::Valid;
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256U << 10, .descriptors = 16});
 std::size_t failed_attempts = 0;
 if (valid)
  server.TruncateNextTransfer();
 else
  server.GateNextTransfer();
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json&) {
  if (event != "benchmark.download.attempt_failed") return;
  ++failed_attempts;
  const auto checkpoint = read_json_file(metadata_path);
  CHECK(checkpoint.at("bytes") == 2U * prefix.size());
  CHECK(checkpoint.at("etag") == "\"benchmark-test-etag\"");
 };
 auto work = std::async(std::launch::async, [&] { return download_artifacts({request}, 1, cancellation, {}, trace, {}, &execution); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  server.ReleasePartial();
 });
 if (valid) {
  const auto result = mmltk::testsupport::await_test_future(work, "large ordinary resume after truncated attempt", 20s);
  REQUIRE(result.size() == 1);
  CHECK(result[0].resumed);
  CHECK(result[0].attempts == 2);
  CHECK(result[0].size == bytes);
  CHECK(failed_attempts == 1);
  CHECK(has_generated_payload(request.destination, bytes));
  const auto ranges = server.ranges();
  REQUIRE(ranges.size() == 2);
  CHECK(ranges[0].first == prefix.size());
  CHECK(ranges[1].first == 2U * prefix.size());
 } else {
  REQUIRE(server.WaitPartial());
  cancelled.store(true);
  CHECK_THROWS(mmltk::testsupport::await_test_future(work, "cancelled reset ordinary partial"));
  CHECK(server.ranged_requests() == 0);
  CHECK_FALSE(fs::exists(request.destination));
  const auto checkpoint = read_json_file(metadata_path);
  CHECK(checkpoint.at("url") == request.url);
  CHECK(checkpoint.value("mode", std::string{}) != "segmented");
  CHECK(checkpoint.at("bytes") == fs::file_size(partial_path));
  CHECK(fs::file_size(partial_path) <= prefix.size());
 }
 CHECK(execution.execution().try_reserve({256U << 10, 16}).has_value());
 server.ReleasePartial();
 server.Check();
}
TEST_CASE("Curl class limits come from its reflected inventory and checked CPU budget", "[backend][data][benchmark][download]") {
 BenchmarkCurl normalized(0), selected(3), capped(256);
 CHECK(kBenchmarkCurlClasses.size() == 2);
 CHECK(normalized.limit(BenchmarkCurl::Class::Artifact) == 1);
 CHECK(normalized.limit(BenchmarkCurl::Class::OpenImages) == 10);
 CHECK(selected.limit(BenchmarkCurl::Class::Artifact) == 3);
 CHECK(selected.limit(BenchmarkCurl::Class::OpenImages) == 30);
 CHECK(capped.limit(BenchmarkCurl::Class::Artifact) == 8);
 CHECK(capped.limit(BenchmarkCurl::Class::OpenImages) == 256);
 CHECK_THROWS_AS(BenchmarkCurl(std::numeric_limits<std::size_t>::max()), std::overflow_error);
}
TEST_CASE("standalone artifact batches retain selected controllers across multiple jobs", "[backend][data][benchmark][download]") {
 mmltk::testsupport::ScopedTempDir root("persistent-artifact-controllers");
 const auto payload = make_payload(4096);
 HttpServer first(payload), second(payload);
 first.GateNextRequest();
 second.GateNextRequest();
 std::vector<DownloadRequest> requests;
 for (std::size_t i = 0; i < 6; ++i) requests.push_back(request_for(root.path(), "artifact-" + std::to_string(i), (i % 2 ? second : first).url("body"), payload));
 std::mutex observations;
 std::vector<std::thread::id> controllers;
 std::vector<std::size_t> delivered;
 std::atomic<bool> cancelled{false};
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json&) {
  if (event != "benchmark.download.start") return;
  const std::lock_guard lock(observations);
  controllers.push_back(std::this_thread::get_id());
 };
 auto batch = std::async(std::launch::async, [&] {
  return download_artifacts(requests, 2, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), {}, trace, [&](DownloadReady result) {
   auto lease = ArtifactLease::try_acquire_charged(requests[result.request_index].lock_path, {}, CompileAllowance{});
   require_condition(static_cast<bool>(lease), "ready callback retained its artifact lock");
   require_condition(result.artifact.path == requests[result.request_index].destination, "ready result lost input order");
   delivered.push_back(result.request_index);
  });
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  first.ReleaseRequest();
  second.ReleaseRequest();
 });
 // No CPU-count skip: these are the selected standalone I/O controllers.
 REQUIRE(first.WaitRequest());
 REQUIRE(second.WaitRequest());
 CHECK(batch.wait_for(0ms) == std::future_status::timeout);
 first.ReleaseRequest();
 second.ReleaseRequest();
 const auto results = mmltk::testsupport::await_test_future(batch, "persistent artifact batch");
 REQUIRE(results.size() == requests.size());
 for (std::size_t i = 0; i < results.size(); ++i) {
  CHECK(results[i].path == requests[i].destination);
  CHECK(results[i].size == payload.size());
 }
 REQUIRE(controllers.size() == requests.size());
 std::ranges::sort(controllers);
 controllers.erase(std::unique(controllers.begin(), controllers.end()), controllers.end());
 CHECK(controllers.size() == 2);
 std::ranges::sort(delivered);
 CHECK(delivered == std::vector<std::size_t>{0, 1, 2, 3, 4, 5});
 first.Check();
 second.Check();
}
TEST_CASE("artifact callback and startup failures settle all borrowed batch state", "[backend][data][benchmark][download]") {
 bool startup = false;
 SECTION("a throwing ready callback retires its peer") {}
 SECTION("a source startup error retires an already active peer") { startup = true; }
 mmltk::testsupport::ScopedTempDir root("artifact-batch-failure-custody");
 const auto payload = make_payload(4096);
 HttpServer first(payload), peer(payload);
 peer.GateNextRequest();
 auto request = request_for(root.path(), "first", first.url("first"), payload);
 auto independent = request_for(root.path(), "peer", peer.url("peer"), payload);
 mmltk::testsupport::TestGate callback("borrowed artifact ready result");
 std::atomic<bool> cancelled{false};
 std::promise<void> returned;
 std::shared_ptr<ArtifactLease> blocked;
 if (startup) {
  // Hold selection until the peer is active, then fail ordinary source setup.
  blocked = std::make_shared<ArtifactLease>(ArtifactLease::acquire(request.lock_path, {}));
  write_text(request.destination / "retained-entry", "cannot remove this nonempty destination");
 }
 const std::vector requests{request, independent};
 auto batch = std::async(std::launch::async, [&] {
  return download_artifacts(requests, 2, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), {}, {}, [&](DownloadReady value) {
   if (value.request_index != 0) return;
   callback.receipt().ArriveAndWait();
   require_condition(value.artifact.path == request.destination && value.artifact.size == payload.size(), "borrowed result changed before retirement");
   returned.set_value();
   throw std::runtime_error("artifact ready failure");
  });
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  blocked.reset();
  callback.Release();
  peer.ReleaseRequest();
 });
 REQUIRE(peer.WaitRequest());
 if (startup)
  blocked.reset();
 else {
  REQUIRE(callback.WaitEntered(5s));
  CHECK(batch.wait_for(0ms) == std::future_status::timeout);
  callback.Release();
  mmltk::testsupport::await_test_promise(returned, "borrowed artifact result retirement");
 }
 REQUIRE(batch.wait_for(5s) == std::future_status::ready);
 if (startup)
  CHECK_THROWS(batch.get());
 else
  CHECK_THROWS_WITH(batch.get(), "artifact ready failure");
 for (const auto& value : requests) CHECK(ArtifactLease::try_acquire_charged(value.lock_path, {}, CompileAllowance{}));
 CHECK_FALSE(fs::exists(independent.destination));
 peer.ReleaseRequest();
 first.Check();
 peer.Check();
}
TEST_CASE("channel settlement detaches pending and active work while another channel stays live", "[backend][data][benchmark][download][pipeline]") {
 const auto payload = make_payload(4096);
 HttpServer retired(payload), queued(payload), independent(payload), reused(payload);
 retired.GateNextRequest();
 independent.GateNextRequest();
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = 32});
 CurlSocketObservation sockets;
 auto channel = execution.curl().channel(BenchmarkCurl::Class::Artifact);
 auto other = execution.curl().channel(BenchmarkCurl::Class::OpenImages);
 ObservedCurlTransfer first(retired.url("active"), sockets), pending(queued.url("pending"), sockets), live(independent.url("live"), sockets), later(reused.url("reused"), sockets);
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  retired.ReleaseRequest();
  independent.ReleaseRequest();
  if (channel) channel->remove_all();
  if (other) other->remove_all();
 });
 channel->add(first.easy.get());
 REQUIRE(retired.WaitRequest());
 channel->add(pending.easy.get());
 other->add(live.easy.get());
 REQUIRE(independent.WaitRequest());
 channel->remove_all();
 CHECK_FALSE(channel->next());
 CHECK(first.bytes.empty());
 CHECK(pending.bytes.empty());
 CHECK(queued.requests() == 0);
 CHECK(sockets.live() == 1);
 channel->remove_all();  // Already-empty settlement keeps the channel reusable.
 channel->add(later.easy.get());
 CHECK(await_curl(*channel).handle == later.easy.get());
 CHECK(later.bytes == payload);
 CHECK(live.bytes.empty());
 channel.reset();
 CHECK(sockets.live() == 1);
 independent.ReleaseRequest();
 CHECK(await_curl(*other).handle == live.easy.get());
 CHECK(live.bytes == payload);
 other.reset();
 CHECK(sockets.live() == 0);
 CHECK(execution.execution().try_reserve({execution.execution().transient_target(), 32}).has_value());
 retired.ReleaseRequest();
 retired.Check();
 queued.Check();
 independent.Check();
 reused.Check();
}
TEST_CASE("shared Curl admission changes survive the check to park boundary for actual readers", "[backend][data][benchmark][download][pipeline]") {
 enum class Event { Return, PressureEntry, PressureExit, RetainedReplacement, RecycledInput } event = Event::Return;
 SECTION("returned transient bytes") {}
 SECTION("pressure enters without changing charges") { event = Event::PressureEntry; }
 SECTION("pressure exits without changing charges") { event = Event::PressureExit; }
 SECTION("retained control replaces an equal transient charge") { event = Event::RetainedReplacement; }
 SECTION("source owned input recycling") { event = Event::RecycledInput; }
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 4096, .descriptors = 32});
 std::vector<std::unique_ptr<BenchmarkCurl::Channel>> declarations;
 for (std::size_t i = 0; i < 32; ++i) declarations.push_back(execution.curl().channel(BenchmarkCurl::Class::Artifact));
 auto first = execution.curl().channel(BenchmarkCurl::Class::Artifact);
 auto second = execution.curl().channel(BenchmarkCurl::Class::OpenImages);
 auto occupied = execution.execution().reserve({4096, 0});
 auto pressure = event == Event::PressureExit ? execution.execution().defer_resources() : nullptr;
 CompileAllowance retained;
 std::atomic<bool> recycled{false};
 // Curl's earlier registration has delivered all previous notices before this
 // gate engages. Hold the event owner so equal-total replacement coalesces into
 // one delivery, then let both readers finish checking their actual predicate.
 mmltk::testsupport::TestGate notifier("admission delivery before resource mutation");
 std::atomic<bool> first_notice{true};
 CompileExecution::Readiness delivery(execution.execution(), [&](CompileExecution::Readiness::Changes) {
  if (first_notice.exchange(false)) notifier.receipt().ArriveAndWait();
 });
 const mmltk::testsupport::ScopedTestCleanup release_notifier([&] { notifier.Release(); });
 execution.execution().notify_admission_change();
 REQUIRE(notifier.WaitEntered(5s));
 mmltk::testsupport::TestGate first_checked("first admission predicate checked"), second_checked("second admission predicate checked");
 const auto wait = [&](BenchmarkCurl::Channel& channel, mmltk::testsupport::TestGate& checked) {
  require_condition(!execution.execution().try_reserve({1, 0}), "occupied bytes unexpectedly admitted");
  channel.wait_until(std::chrono::steady_clock::now());
  checked.receipt().ArriveAndWait();
  channel.wait_until(std::chrono::steady_clock::time_point::max());
  if (event == Event::PressureEntry || event == Event::PressureExit) return execution.execution().resource_pressure() == (event == Event::PressureEntry);
  if (event == Event::RecycledInput) return recycled.load();
  return execution.execution().try_reserve({1, 0}).has_value();
 };
 auto a = std::async(std::launch::async, [&] { return wait(*first, first_checked); });
 auto b = std::async(std::launch::async, [&] { return wait(*second, second_checked); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  notifier.Release();
  occupied = {};
  first_checked.Release();
  second_checked.Release();
  first->wake();
  second->wake();
 });
 REQUIRE(first_checked.WaitEntered(5s));
 REQUIRE(second_checked.WaitEntered(5s));
 const auto before = execution.execution().resource_usage();
 switch (event) {
  case Event::Return: occupied = {}; break;
  case Event::PressureEntry: pressure = execution.execution().defer_resources(); break;
  case Event::PressureExit: pressure.reset(); break;
  case Event::RetainedReplacement:
   occupied = {};
   retained = execution.execution().reserve({4096, 0, false, 0, true});
   break;
  case Event::RecycledInput:
   recycled = true;
   execution.curl().admission_wakeup()();
   break;
 }
 if (event != Event::Return) {
  const auto after = execution.execution().resource_usage();
  CHECK(after.bytes == before.bytes);
  CHECK(after.descriptors == before.descriptors);
  CHECK(after.continuation_descriptors == before.continuation_descriptors);
 }
 notifier.Release();
 first_checked.Release();
 second_checked.Release();
 CHECK(mmltk::testsupport::await_test_future(a, "first shared admission reader"));
 CHECK(mmltk::testsupport::await_test_future(b, "second shared admission reader"));
}
TEST_CASE("admission readiness retains external returns across local rollback and delivery acknowledgement", "[backend][data][benchmark][download][pipeline]") {
 enum class Return { None, Rollback, Delivery } returned = Return::None;
 SECTION("unchanged failed demand and CPU grants do not request a retry") {}
 SECTION("external return coalesces with failed partial draw") { returned = Return::Rollback; }
 SECTION("external return survives acknowledgement of the prior delivery") { returned = Return::Delivery; }
 using Changes = CompileExecution::Readiness::Changes;
 using mmltk::backend::data::detail::CompileWorkClass;
 CompileExecution execution(1, {}, {.transient_bytes = 64, .descriptors = 8});
 auto parent = execution.reserve(CompileResources::handles(0, false, 2));
 auto external = execution.reserve(CompileResources::handles(5));
 mmltk::testsupport::TestGate initial("hold readiness before partial admission"), failed("last descriptor draw failed before rollback"), delivering("hold acknowledged readiness batch");
 std::promise<Changes> rolled_back, rearmed;
 std::atomic<unsigned> deliveries{0};
 std::unique_ptr<mmltk::backend::data::detail::CompileResourceWait> pressure;
 std::future<void> draw;
 auto readiness = std::make_unique<CompileExecution::Readiness>(execution, [&](Changes changes) {
  switch (++deliveries) {
   case 1: initial.receipt().ArriveAndWait(); break;
   case 2:
    rolled_back.set_value(changes);
    delivering.receipt().ArriveAndWait();
    break;
   case 3: rearmed.set_value(changes); break;
   default: break;
  }
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  initial.Release();
  failed.Release();
  delivering.Release();
  if (draw.valid()) draw.wait();
  readiness.reset();
 });
 execution.notify_admission_change();
 REQUIRE(initial.WaitEntered(5s));
 draw = std::async(std::launch::async, [&] {
  CompileExecution::Readiness::Attempt attempt(readiness.get());
  auto work = execution.try_reserve({32, 1}, parent);
  auto promised_socket = execution.try_reserve(CompileResources::handles(1), parent);
  auto independent_socket = execution.try_reserve(CompileResources::handles(1));
  require_condition(work && promised_socket && independent_socket, "partial transport draw did not enter its available envelope");
  require_condition(!execution.try_reserve(CompileResources::handles(1)), "exhausted descriptor draw unexpectedly fit");
  pressure = execution.defer_resources();
  failed.receipt().ArriveAndWait();
  // Temporary grants return in reverse order before the uncommitted attempt
  // leaves. Only the racing outside owner may request another pump attempt.
 });
 REQUIRE(failed.WaitEntered(5s));
 if (returned == Return::Rollback) external = {};
 failed.Release();
 mmltk::testsupport::await_test_future(draw, "failed admission rollback");
 initial.Release();
 const auto rollback = mmltk::testsupport::await_test_promise(rolled_back, "coalesced admission changes");
 REQUIRE(delivering.WaitEntered(5s));
 CHECK(rollback.resources);
 CHECK(rollback.pressure);
 CHECK(rollback.retry == (returned == Return::Rollback));
 CHECK_FALSE(rollback.interrupted);
 CHECK(execution.resource_pressure());
 // These are real scheduler grants, independent of diagnostic observations.
 // Their readiness batch has no resource retry even after acknowledgement.
 {
  auto producer = execution.open_class(CompileWorkClass::Acquisition);
  auto cpu = execution.service_grant(CompileWorkClass::Acquisition);
 }
 if (returned == Return::Delivery) external = {};
 delivering.Release();
 const auto next = mmltk::testsupport::await_test_promise(rearmed, "readiness rearmed during callback delivery");
 CHECK(next.retry == (returned == Return::Delivery));
 CHECK(next.resources == (returned == Return::Delivery));
 CHECK_FALSE(next.pressure);
 readiness.reset();
 CHECK(execution.try_reserve(CompileResources::handles(0, false, 2), parent).has_value());
 pressure.reset();
 CHECK_FALSE(execution.resource_pressure());
}
TEST_CASE("readiness retirement joins an executing admission callback", "[backend][data][benchmark][pipeline]") {
 CompileExecution execution(1);
 mmltk::testsupport::TestGate callback("admission callback owns borrowed state"), retiring("readiness destruction started");
 auto readiness = std::make_unique<CompileExecution::Readiness>(execution, [&](CompileExecution::Readiness::Changes) { callback.receipt().ArriveAndWait(); });
 std::future<void> retirement;
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  callback.Release();
  retiring.Release();
 });
 execution.notify_admission_change();
 REQUIRE(callback.WaitEntered(5s));
 retirement = std::async(std::launch::async, [&] {
  retiring.receipt().ArriveAndWait();
  readiness.reset();
 });
 REQUIRE(retiring.WaitEntered(5s));
 retiring.Release();
 CHECK(retirement.wait_for(0ms) == std::future_status::timeout);
 callback.Release();
 mmltk::testsupport::await_test_future(retirement, "joined admission callback");
}
TEST_CASE("pending Curl admission parks through CPU notices and resumes on external credit return", "[backend][data][benchmark][download][pipeline]") {
 using mmltk::backend::data::detail::CompileWorkClass;
 using Changes = CompileExecution::Readiness::Changes;
 const auto payload = make_payload(128);
 HttpServer server(payload);
 CompileExecution execution(1, {}, {.transient_bytes = 256U << 10, .descriptors = 24});
 BenchmarkCurl curl(1, &execution);
 auto channel = curl.channel(BenchmarkCurl::Class::Artifact);
 auto occupied = execution.reserve({256U << 10, 0});
 CurlSocketObservation sockets;
 ObservedCurlTransfer transfer(server.url("pending"), sockets);
 mmltk::testsupport::TestGate pending("Curl registered its unsuccessful byte demand");
 std::promise<Changes> cpu_notices;
 unsigned stage = 0;
 CompileExecution::Readiness delivery(execution, [&](Changes changes) {
  if (!stage && changes.pressure && execution.resource_pressure()) {
   stage = 1;
   pending.receipt().ArriveAndWait();
  } else if (stage == 1) {
   stage = 2;
   cpu_notices.set_value(changes);
  }
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  pending.Release();
  if (channel) channel->remove_all();
 });
 channel->add(transfer.easy.get());
 REQUIRE(pending.WaitEntered(5s));
 // Taking the shared grant joins the failed pump's CPU-bearing work. Its own
 // wait/release events are then delivered in one gated batch, with no new
 // command, socket work or resource mutation capable of rearming the demand.
 { auto cpu = execution.service_grant(CompileWorkClass::Acquisition); }
 execution.notify_admission_change();
 pending.Release();
 const auto changes = mmltk::testsupport::await_test_promise(cpu_notices, "CPU-only readiness batch for pending Curl");
 CHECK_FALSE(changes.resources);
 CHECK_FALSE(changes.pressure);
 CHECK_FALSE(changes.retry);
 CHECK(execution.resource_pressure());
 CHECK(server.requests() == 0);
 occupied = {};
 const auto completion = await_curl(*channel);
 CHECK(completion.handle == transfer.easy.get());
 CHECK(completion.result == CURLE_OK);
 CHECK(transfer.bytes == payload);
 channel.reset();
 CHECK(execution.try_reserve({execution.transient_target(), 24}).has_value());
 server.Check();
}
TEST_CASE("Open Images starts ready HTTP before later cold cache chunks finish", "[backend][data][benchmark][images][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("open-images-incremental-cache-scan");
 const auto cache = BenchmarkCacheLayout::create(root.path());
 const auto images = cache.source_images("open-images") / "train";
 prepare_cached_image_directory(images);
 const auto jpeg = make_jpeg(10, 20, 30);
 HttpServer server(jpeg);
 server.GateNextRequest();
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 64U << 20, .descriptors = 32});
 NormalizedAnnotationBuilder index;
 index.source = BenchmarkDatasetSource::kOpenImagesV7;
 for (std::uint64_t id = 1; id <= 65; ++id) {
  index.images.push_back({.source_image_id = id});
  if (id > 1 && id < 65) BenchmarkEncodedImage::publish(cached_image_path(images, id), jpeg, {});
 }
 write_text(cached_image_path(images, 65), "invalid cached JPEG");
 mmltk::testsupport::TestGate scan("later cold cache header");
 std::atomic<bool> cancelled{false};
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json& fields) {
  if (event == "benchmark.images.cache_invalid" && fields.at("image_id") == 65) scan.receipt().ArriveAndWait();
 };
 std::vector<QuarantinedImage> quarantined;
 ProgressReporter progress({}, {});
 auto acquisition = std::async(std::launch::async, [&] {
  return acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), &progress, 1, 0, trace,
   {}, &execution, [&](std::uint64_t) { return server.url("image"); });
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  scan.Release();
  server.ReleaseRequest();
 });
 REQUIRE(scan.WaitEntered(5s));
 REQUIRE(server.WaitRequest());  // The only CPU is still in the later scan chunk.
 CHECK(acquisition.wait_for(0ms) == std::future_status::timeout);
 CHECK_FALSE(fs::exists(images / ".groups" / "group-000000.complete.json"));
 scan.Release();
 server.ReleaseRequest();
 const auto result = mmltk::testsupport::await_test_future(acquisition, "incremental cache scan acquisition");
 CHECK(result.available_image_ids.size() == 65);
 CHECK(result.directory.image_bytes == jpeg.size() * 65);
 CHECK(quarantined.empty());
 CHECK(server.requests() == 2);
 const auto proof = read_json_file(images / ".groups" / "group-000000.complete.json");
 CHECK(proof.at("image_count") == 65);
 CHECK(proof.at("image_bytes") == jpeg.size() * 65);
 CHECK(proof.at("dimensions").size() == 195);
 CHECK(execution.execution().try_reserve({64U << 20, 32}).has_value());
 server.Check();
}
TEST_CASE("an already settled channel retires without waiting for another channel callback", "[backend][data][benchmark][download][pipeline]") {
 const auto payload = make_payload(128);
 HttpServer first(payload), second(payload);
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = 32});
 CurlSocketObservation sockets;
 auto retired = execution.curl().channel(BenchmarkCurl::Class::Artifact);
 auto live = execution.curl().channel(BenchmarkCurl::Class::OpenImages);
 ObservedCurlTransfer finished(first.url("finished"), sockets), held(second.url("held"), sockets);
 mmltk::testsupport::TestGate callback("other channel's physical callback");
 held.before_write = [&] { callback.receipt().ArriveAndWait(); };
 retired->add(finished.easy.get());
 CHECK(await_curl(*retired).result == CURLE_OK);
 retired->remove_all();
 std::future<void> retirement;
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  callback.Release();
  if (retirement.valid()) retirement.wait();
  if (retired) retired->remove_all();
  if (live) live->remove_all();
 });
 live->add(held.easy.get());
 REQUIRE(callback.WaitEntered(5s));
 retirement = std::async(std::launch::async, [&] {
  retired->remove_all();
  retired.reset();
 });
 mmltk::testsupport::await_test_future(retirement, "already-empty channel destruction");
 callback.Release();
 CHECK(await_curl(*live).result == CURLE_OK);
 CHECK(held.bytes == payload);
 live.reset();
 CHECK(sockets.live() == 0);
 CHECK(execution.execution().try_reserve({execution.execution().transient_target(), 32}).has_value());
 first.Check();
 second.Check();
}
TEST_CASE("transport failure wakes channel readers and permits request settlement", "[backend][data][benchmark][download][pipeline]") {
 const auto payload = make_payload(128);
 HttpServer first(payload), second(payload);
 first.GateNextRequest();
 second.GateNextRequest();
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = 32});
 CurlSocketObservation sockets;
 auto channel = execution.curl().channel(BenchmarkCurl::Class::Artifact);
 auto peer = execution.curl().channel(BenchmarkCurl::Class::OpenImages);
 ObservedCurlTransfer duplicate(first.url("duplicate"), sockets), held(second.url("held"), sockets);
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  first.ReleaseRequest();
  second.ReleaseRequest();
  if (channel) channel->remove_all();
  if (peer) peer->remove_all();
 });
 channel->add(duplicate.easy.get());
 REQUIRE(first.WaitRequest());
 peer->add(held.easy.get());
 REQUIRE(second.WaitRequest());
 // A second registration of a still-active easy handle is a transport error.
 // Its admission waits for an Artifact turn; remove the class restriction by
 // submitting the duplicate through the independent image declaration.
 peer->add(duplicate.easy.get());
 CHECK_THROWS_WITH(await_curl(*channel), "benchmark easy handle already active");
 CHECK_THROWS_WITH(peer->next(), "benchmark easy handle already active");
 auto receipt = std::async(std::launch::async, [&] { peer->remove_all(); });
 mmltk::testsupport::await_test_future(receipt, "failed transport removal receipt");
 channel->remove_all();
 peer.reset();
 channel.reset();
 CHECK(sockets.live() == 0);
 CHECK(execution.execution().try_reserve({execution.execution().transient_target(), 32}).has_value());
 first.ReleaseRequest();
 second.ReleaseRequest();
 first.Check();
 second.Check();
}
TEST_CASE("published image mappings retain their inode and header without a cache reopen", "[backend][data][benchmark][images][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("retained-pixel-input");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 const auto encoded = make_jpeg(120, 16, 8);
 BenchmarkImageDecoder decoder;
 const auto header = decoder.read_header(encoded);
 auto payload = BenchmarkEncodedImage::publish(cached_image_path(images, 1), encoded, {}, nullptr, header);
 REQUIRE(payload);
 std::filesystem::remove(cached_image_path(images, 1));
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{images}};
 split.images = {{1, 16, 8, 0, 0}};
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1024});
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8);
 request.execution = &execution;
 request.num_workers = 1;
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 const auto publication = execution.source_publication(images, {});
 CHECK(publication.consume({1, std::pair{16U, 8U}, false, payload}));
 CHECK(writer.image_complete(0));
 CHECK(publication.consume({1, std::pair{16U, 8U}, false, payload}));
 CHECK(writer.completed() == 1);
 execution.drain();
 writer.finish(request);
 CHECK(CompiledDataset::open(request.output_path).header().num_images == 1);
}
TEST_CASE("warm image admission owns the opened generation across replacement", "[backend][data][benchmark][images]") {
 mmltk::testsupport::ScopedTempDir root("warm-owned-input");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 const auto first = make_jpeg(17, 16, 8), second = make_jpeg(90, 16, 8);
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), first, {});
 FileHandle directory(::open(images.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
 REQUIRE(directory.get() >= 0);
 const auto admitted = BenchmarkEncodedImage::open(directory.get(), 1, {}, {});
 REQUIRE(admitted);
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), second, {});
 CHECK(std::ranges::equal(admitted->encoded(), first));
 const auto replacement = BenchmarkEncodedImage::open(directory.get(), 1, {}, {});
 REQUIRE(replacement);
 CHECK(std::ranges::equal(replacement->encoded(), second));
}
TEST_CASE("PNG decoder views retain the supplied pinned arena through pixel use", "[backend][data][benchmark][writer]") {
 std::vector<std::uint8_t> encoded;
 const std::array<std::uint8_t, 12> expected{0, 20, 40, 60, 80, 100, 120, 140, 160, 180, 200, 220};
 REQUIRE(stbi_write_png_to_func(append_bytes, &encoded, 2, 2, 3, expected.data(), 6));
 BenchmarkImageDecoder decoder;
 std::vector<std::uint8_t> rgb{251, 252, 253};
 const auto header = decoder.read_header(encoded);
 mmltk::common::system::NumaMemory workspace(mmltk::common::system::NumaTopology::Capture().permitted_nodes.front(), mmltk::common::system::NumaMemory::Lock::Pages);
 workspace.ensure_bytes(BenchmarkImageDecoder::plan(header).bytes());
 const auto view = decoder.decode_rgb(encoded, header, {static_cast<std::byte*>(workspace.data()), workspace.capacity_bytes()});
 CHECK(reinterpret_cast<std::uintptr_t>(view.data()) >= reinterpret_cast<std::uintptr_t>(workspace.data()));
 CHECK(view.data() + view.size() <= static_cast<std::uint8_t*>(workspace.data()) + workspace.capacity_bytes());
 CHECK(std::ranges::equal(view, expected));
 CHECK(rgb == std::vector<std::uint8_t>{251, 252, 253});
 encoded.clear();
 encoded.shrink_to_fit();
 CHECK(std::ranges::equal(view, expected));
}
TEST_CASE("registered Open Images repair accepts its one retained pixel consumption", "[backend][data][benchmark][images][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("open-images-pixel-repair");
 const auto cache = BenchmarkCacheLayout::create(root.path());
 const auto images = cache.source_images("open-images") / "train";
 prepare_cached_image_directory(images);
 const auto jpeg = make_jpeg(10, 20, 30);
 HttpServer server(jpeg);
 NormalizedAnnotationBuilder index;
 index.source = BenchmarkDatasetSource::kOpenImagesV7;
 index.images.push_back({.source_image_id = 1, .width = 16, .height = 8});
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{images}};
 split.images = {{1, 16, 8, 0, 0}};
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1, .descriptors = 32}, cancellation);
 std::atomic<unsigned> reads{0}, completions{0};
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8);
 request.execution = &execution;
 request.image_opened = [&](const fs::path&, std::uint64_t) { ++reads; };
 request.progress = {.context = &completions, .image_completed = [](void* value) { ++*static_cast<std::atomic<unsigned>*>(value); }};
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 bool unlinked = false;
 ProgressReporter progress({}, {});
 const auto repair_input = [&](std::uint64_t id) {
  if (id == 1) unlinked = fs::remove(cached_image_path(images, 1));
 };
 std::vector<QuarantinedImage> quarantined;
 auto acquisition = std::async(std::launch::async, [&] {
  return acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, cancellation, &progress, 1, 0, {}, ImageDecodeProbe{1, 16, 8}, &execution,
   [&](std::uint64_t) { return server.url("repair"); }, {}, repair_input);
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] { cancelled.store(true); });
 const auto result = mmltk::testsupport::await_test_future(acquisition, "repair consumes admitted bytes without reopening its saved path");
 REQUIRE(unlinked);
 CHECK(quarantined.empty());
 CHECK(result.available_image_ids == std::vector<std::uint64_t>{1});
 CHECK(writer.image_complete(0));
 CHECK(reads.load() == 1);
 CHECK(completions.load() == 1);
 execution.drain();
 writer.finish(request);
 CHECK(CompiledDataset::open(request.output_path).header().num_images == 1);
}
TEST_CASE("compact image facts reuse only their admitted file generation", "[backend][data][benchmark][images][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("compact-image-fact");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 const auto first = make_jpeg(17, 16, 8);
 BenchmarkImageDecoder decoder;
 auto fact = BenchmarkEncodedImage::publish(cached_image_path(images, 1), first, {}, nullptr, decoder.read_header(first), {}, false);
 REQUIRE(fact);
 CHECK(fact->storage() == BenchmarkEncodedImage::Storage::HeaderOnly);
 CHECK_FALSE(fact->backing());
 CHECK(fact->encoded().empty());
 FileHandle directory(::open(images.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
 REQUIRE(directory.get() >= 0);
 std::size_t headers = 0;
 const CachedImageValidator admit = [&](std::uint64_t, std::span<const std::uint8_t> encoded) {
  ++headers;
  return decoder.read_header(encoded);
 };
 auto same = BenchmarkEncodedImage::open(directory.get(), 1, admit, {}, nullptr, {}, fact);
 REQUIRE(same);
 CHECK(headers == 0);
 CHECK(std::ranges::equal(same->encoded(), first));
 const auto second = make_jpeg(90, 16, 8);
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), second, {});
 auto changed = BenchmarkEncodedImage::open(directory.get(), 1, admit, {}, nullptr, {}, fact);
 REQUIRE(changed);
 CHECK(headers == 1);
 CHECK(std::ranges::equal(changed->encoded(), second));
 CHECK(std::ranges::equal(same->encoded(), first));
 fs::remove(cached_image_path(images, 1));
 CHECK_FALSE(BenchmarkEncodedImage::open(directory.get(), 1, admit, {}, nullptr, {}, fact));
}
TEST_CASE("preplacement image facts remain compact and withdraw with their source", "[backend][data][benchmark][images][pipeline]") {
 bool pressure = false;
 SECTION("mapped publication") {}
 SECTION("descriptor pressure publication") { pressure = true; }
 mmltk::testsupport::ScopedTempDir root("preplacement-image-facts");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1, .descriptors = 13});
 const auto encoded = make_jpeg(40, 16, 8);
 BenchmarkImageDecoder decoder;
 std::shared_ptr<const BenchmarkEncodedImage> payload;
 if (pressure) {
  auto occupied = execution.execution().reserve(CompileResources::handles(13));
  FileHandle directory(::open(images.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  REQUIRE(directory.get() >= 0);
  detail::StorageReservationPool storage(images, {});
  payload = BenchmarkEncodedImage::publish(directory.get(), 1, encoded, decoder.read_header(encoded), {}, storage, &execution.execution());
  REQUIRE(payload);
  CHECK(payload->storage() == BenchmarkEncodedImage::Storage::HeaderOnly);
 } else
  payload = BenchmarkEncodedImage::publish(cached_image_path(images, 1), encoded, {}, nullptr, decoder.read_header(encoded));
 const std::weak_ptr<const void> mapping = payload->backing();
 const auto publication = execution.source_publication(images, {});
 publication({1, std::pair{16U, 8U}, true, payload});
 payload.reset();
 CHECK(mapping.expired());
 auto fact = execution.image_input(images, 1);
 REQUIRE(fact);
 CHECK(fact->storage() == BenchmarkEncodedImage::Storage::HeaderOnly);
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{images}};
 split.images = {{1, 16, 8, 0, 0}};
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8);
 request.execution = &execution;
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 CHECK(publication.consume({1}));
 CHECK(writer.image_complete(0));
 execution.retire_image(images, 1);
 CHECK_FALSE(execution.image_input(images, 1));
 publication({1, std::pair{16U, 8U}, true, fact});
 CHECK_FALSE(execution.image_input(images, 1));
 CHECK_FALSE(writer.image_complete(0));
}
TEST_CASE("held warm image reads allow HTTP results and pixels to finish on one CPU", "[backend][data][benchmark][images][pipeline]") {
 bool cancel = false;
 SECTION("complete group proof") {}
 SECTION("cancel with a pending warm result") { cancel = true; }
 mmltk::testsupport::ScopedTempDir root("open-images-independent-warm");
 const auto cache = BenchmarkCacheLayout::create(root.path());
 const auto images = cache.source_images("open-images") / "train";
 prepare_cached_image_directory(images);
 const auto jpeg = make_jpeg(10, 20, 30);
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), jpeg, {});
 BenchmarkEncodedImage::publish(cached_image_path(images, 3), jpeg, {});
 HttpServer server(jpeg);
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 64U << 20, .descriptors = 16}, cancellation);
 NormalizedAnnotationBuilder index;
 index.source = BenchmarkDatasetSource::kOpenImagesV7;
 for (std::uint64_t id : {1U, 2U, 3U}) index.images.push_back({.source_image_id = id});
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{images}};
 for (std::uint64_t id : {1U, 2U, 3U}) split.images.push_back({id, 16, 8, 0, 0});
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8, cancellation);
 request.execution = &execution;
 mmltk::testsupport::TestGate held("warm read before filesystem acquisition");
 std::promise<void> ready_pixels;
 std::atomic<unsigned> completed{0};
 struct Completion {
  std::atomic<unsigned>& count;
  std::promise<void>& ready;
 } completion{completed, ready_pixels};
 request.progress = {.context = &completion, .image_completed = [](void* opaque) {
  auto& value = *static_cast<Completion*>(opaque);
  if (++value.count == 2) value.ready.set_value();
 }};
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 ProgressReporter progress({}, {});
 std::vector<QuarantinedImage> quarantined;
 auto acquisition = std::async(std::launch::async, [&] {
  return acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, cancellation, &progress, 1, 0, {}, {}, &execution,
   [&](std::uint64_t) { return server.url("image"); }, [&](std::uint64_t id) {
   if (id == 1) held.receipt().ArriveAndWait();
  });
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  held.Release();
 });
 REQUIRE(held.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(ready_pixels, "independent warm and HTTP pixels finish while the first read is held");
 CHECK_FALSE(writer.image_complete(0));
 CHECK(writer.image_complete(1));
 CHECK(writer.image_complete(2));
 CHECK(server.requests() == 1);
 CHECK_FALSE(fs::exists(images / ".groups" / "group-000000.complete.json"));
 if (cancel) cancelled.store(true);
 held.Release();
 if (cancel) {
  CHECK_THROWS(mmltk::testsupport::await_test_future(acquisition, "pending warm reader cancellation"));
  CHECK_FALSE(fs::exists(images / ".groups" / "group-000000.complete.json"));
 } else {
  const auto result = mmltk::testsupport::await_test_future(acquisition, "independent warm group completion");
  execution.drain();
  CHECK(result.available_image_ids == std::vector<std::uint64_t>{1, 2, 3});
  CHECK(result.directory.image_bytes == jpeg.size() * 3);
  CHECK(quarantined.empty());
  CHECK(writer.completed() == 3);
  const auto proof = read_json_file(images / ".groups" / "group-000000.complete.json");
  CHECK(proof.at("image_count") == 3);
  CHECK(proof.at("dimensions").size() == 9);
 }
 server.Check();
}
TEST_CASE("small-target warm cache inputs complete after simultaneous admission", "[backend][data][benchmark][images][pipeline]") {
 std::size_t descriptors = 13;
 SECTION("thirteen descriptors") {}
 SECTION("sixteen descriptors") { descriptors = 16; }
 mmltk::testsupport::ScopedTempDir root("open-images-small-warm");
 const auto cache = BenchmarkCacheLayout::create(root.path());
 const auto images = cache.source_images("open-images") / "train";
 prepare_cached_image_directory(images);
 const auto jpeg = make_large_cached_jpeg();
 REQUIRE(jpeg.size() > (64U << 10));
 REQUIRE(jpeg.size() < (8U << 20));
 for (std::uint64_t id : {1U, 2U, 3U}) BenchmarkEncodedImage::publish(cached_image_path(images, id), jpeg, {});
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256U << 10, .descriptors = descriptors}, cancellation);
 NormalizedAnnotationBuilder index;
 index.source = BenchmarkDatasetSource::kOpenImagesV7;
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{images}};
 for (std::uint64_t id : {1U, 2U, 3U}) {
  index.images.push_back({.source_image_id = id});
  split.images.push_back({id, 16, 8, 0, 0});
 }
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8, cancellation);
 request.execution = &execution;
 // At 13 descriptors the three simultaneous opens exercise acquisition alone;
 // 16 also leaves the writer's two handles and canonical pixel jobs admitted.
 std::unique_ptr<BenchmarkSplitWriter> writer;
 if (descriptors == 16) writer = std::make_unique<BenchmarkSplitWriter>(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 if (writer) bind_benchmark_slots(execution, *writer, split);
 ProgressReporter progress({}, {});
 std::vector<QuarantinedImage> quarantined;
 mmltk::testsupport::TestGate warm("three warm workers before mapped admission");
 std::array<std::atomic<unsigned>, 3> reads{};
 auto acquisition = std::async(std::launch::async, [&] {
  return acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, cancellation, &progress, 1, 4, {}, {}, &execution,
   [](std::uint64_t) -> std::string { throw std::runtime_error("valid warm image unexpectedly requested HTTP"); }, [&](std::uint64_t id) {
   ++reads.at(id - 1);
   warm.receipt().ArriveAndWait();
  });
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  warm.Release();
 });
 REQUIRE(warm.WaitEntered(5s, 3));
 // Three old 64 KiB input grants occupied 192 KiB here, leaving no mapping
 // eligible. Release all three readers together against the same 256 KiB target.
 warm.Release();
 const auto result = mmltk::testsupport::await_test_future(acquisition, "small-target simultaneous warm admission", 5s);
 execution.drain();
 if (writer) writer->finish(request);
 CHECK(result.available_image_ids == std::vector<std::uint64_t>{1, 2, 3});
 CHECK(result.directory.image_bytes == jpeg.size() * 3);
 CHECK(quarantined.empty());
 if (writer) CHECK(writer->completed() == 3);
 for (const auto& count : reads) CHECK(count.load() == 1);
 const auto proof = read_json_file(images / ".groups" / "group-000000.complete.json");
 CHECK(proof.at("image_count") == 3);
 CHECK(proof.at("image_bytes") == jpeg.size() * 3);
 CHECK(proof.at("dimensions") == nlohmann::json::array({1, 16, 8, 2, 16, 8, 3, 16, 8}));
 if (writer) CHECK(CompiledDataset::open(request.output_path).header().num_images == 3);
}
TEST_CASE("deferred warm cache cancellation returns opened custody without waiting for byte pressure", "[backend][data][benchmark][images][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("open-images-deferred-cancel");
 const auto cache = BenchmarkCacheLayout::create(root.path());
 const auto images = cache.source_images("open-images") / "train";
 prepare_cached_image_directory(images);
 const auto jpeg = make_large_cached_jpeg();
 for (std::uint64_t id : {1U, 2U, 3U}) BenchmarkEncodedImage::publish(cached_image_path(images, id), jpeg, {});
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256U << 10, .descriptors = 16}, {}, mmltk::backend::data::testsupport::benchmark_trace());
 NormalizedAnnotationBuilder index;
 index.source = BenchmarkDatasetSource::kOpenImagesV7;
 for (std::uint64_t id : {1U, 2U, 3U, 4U}) index.images.push_back({.source_image_id = id});
 ProgressReporter progress({}, {});
 std::vector<QuarantinedImage> quarantined;
 mmltk::testsupport::TestGate warm("three warm requests before opening"), pressure("complete transient target on shared CPU"), next("worker returned an opened-file deferral");
 std::future<void> cpu;
 auto acquisition = std::async(std::launch::async, [&] {
  return acquire_open_images(cache, NormalizedAnnotationReadView(fixture_index(index)), &quarantined, cancellation, &progress, 1, 4, {}, {}, &execution,
   [](std::uint64_t) -> std::string { throw std::runtime_error("cancelled warm input unexpectedly requested HTTP"); }, [&](std::uint64_t id) { (id == 4 ? next : warm).receipt().ArriveAndWait(); });
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  warm.Release();
  next.Release();
  pressure.Release();
 });
 REQUIRE(warm.WaitEntered(5s, 3));
 cpu = std::async(std::launch::async, [&] { execution.execution().run(CompileTaskKind::Metadata, {256U << 10, 0}, [&](std::size_t) { pressure.receipt().ArriveAndWait(); }); });
 REQUIRE(pressure.WaitEntered(5s));
 warm.Release();
 // A fourth request can reach its worker only after one of the first three
 // opened files returned without mapping: the unrelated byte grant is still held.
 REQUIRE(next.WaitEntered(5s));
 cancelled.store(true);
 next.Release();
 REQUIRE(acquisition.wait_for(5s) == std::future_status::ready);
 CHECK_THROWS(acquisition.get());
 CHECK_FALSE(fs::exists(images / ".groups" / "group-000000.complete.json"));
 for (const auto& descriptor : fs::directory_iterator("/proc/self/fd")) {
  std::error_code error;
  const auto target = fs::read_symlink(descriptor.path(), error);
  if (!error)
   for (std::uint64_t id : {1U, 2U, 3U}) CHECK(target != cached_image_path(images, id));
 }
 CHECK_FALSE(execution.execution().resource_pressure());
 pressure.Release();
 mmltk::testsupport::await_test_future(cpu, "independent shared CPU release");
 CHECK(execution.execution().try_reserve({256U << 10, 16}).has_value());
}
TEST_CASE("deferred image input preserves its inspected inode and returns complete byte custody", "[backend][data][benchmark][images][pipeline]") {
 bool cancel = false;
 SECTION("replacement and unlink before admission") {}
 SECTION("cancellation while admission is deferred") { cancel = true; }
 mmltk::testsupport::ScopedTempDir root("deferred-image-inode");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 const auto jpeg = make_large_cached_jpeg();
 const auto path = cached_image_path(images, 1);
 BenchmarkEncodedImage::publish(path, jpeg, {});
 FileHandle directory(::open(images.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
 REQUIRE(directory.get() >= 0);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256U << 10, .descriptors = 13});
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 auto pressure = execution.execution().reserve({256U << 10, 0});
 auto opened = BenchmarkEncodedImage::open_deferred(directory.get(), 1, cancellation, execution.execution().reserve(CompileResources::handles(1, true)));
 REQUIRE(opened);
 CHECK_FALSE(opened->try_read(&execution.execution(), cancellation));
 CHECK(execution.execution().resource_pressure());
 const auto observed = execution.execution().admission_generation();
 BenchmarkEncodedImage::publish(path, make_jpeg(99, 20, 30), {});
 REQUIRE(fs::remove(path));
 if (cancel) {
  cancelled.store(true);
  CHECK_THROWS(opened->try_read(&execution.execution(), cancellation));
  CHECK_FALSE(execution.execution().resource_pressure());
  opened.reset();
  pressure = {};
 } else {
  pressure = {};
  CHECK(execution.execution().admission_generation() != observed);
  auto payload = opened->try_read(&execution.execution(), cancellation);
  REQUIRE(payload);
  opened.reset();
  CHECK_FALSE(execution.execution().resource_pressure());
  CHECK(std::ranges::equal(payload->encoded(), jpeg));
  CHECK(payload->allowance().bytes() == jpeg.size());
  // All header scratch is physically gone; only the mapped extent stays charged.
  auto remaining = execution.execution().try_reserve({(256U << 10) - jpeg.size(), 13});
  REQUIRE(remaining);
  CHECK_FALSE(execution.execution().try_reserve({1, 0}).has_value());
  payload.reset();
  CHECK(execution.execution().try_reserve({jpeg.size(), 0}).has_value());
 }
 CHECK(execution.execution().try_reserve({256U << 10, 13}).has_value());
}
TEST_CASE("deferred mapped input exposes pressure to retained pixel scratch", "[backend][data][benchmark][images][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("deferred-image-idle-pixels");
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), make_jpeg(10, 20, 30), {});
 const auto jpeg = make_large_cached_jpeg();
 BenchmarkEncodedImage::publish(cached_image_path(images, 2), jpeg, {});
 FileHandle directory(::open(images.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
 REQUIRE(directory.get() >= 0);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256U << 10, .descriptors = 13});
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{images}};
 split.images = {{1, 16, 8, 0, 0}};
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 64);
 request.execution = &execution;
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 REQUIRE(execution.source_publication(images, {}).consume({1}));
 REQUIRE(writer.image_complete(0));
 execution.drain();
 // Pixel work has returned its logical arena while the fixed bank stays locked.
 // A live unrelated borrower creates the pressure for this deferred input.
 const auto locked = execution.execution().snapshot().normal_locked;
 auto held = execution.execution().reserve({128U << 10, 0});
 auto opened = BenchmarkEncodedImage::open_deferred(directory.get(), 2, {}, execution.execution().reserve(CompileResources::handles(1, true)));
 REQUIRE(opened);
 const auto observed = execution.execution().admission_generation();
 CHECK_FALSE(opened->try_read(&execution.execution(), {}));
 CHECK(execution.execution().resource_pressure());
 held = {};
 execution.execution().wait_for_admission_change(observed, std::chrono::steady_clock::now() + 2s);
 auto payload = opened->try_read(&execution.execution(), {});
 REQUIRE(payload);
 CHECK_FALSE(execution.execution().resource_pressure());
 CHECK(std::ranges::equal(payload->encoded(), jpeg));
 CHECK(execution.execution().snapshot().normal_locked == locked);
}
TEST_CASE("charged warm input releases byte custody before checked reopening", "[backend][data][benchmark][images][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("charged-warm-reopen");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 const auto path = cached_image_path(images, 1);
 const auto original = make_large_cached_jpeg();
 BenchmarkEncodedImage::publish(path, original, {});
 BenchmarkCompilePipeline pipeline(1, {}, {.transient_bytes = 256U << 10, .descriptors = 13});
 FileHandle directory(::open(images.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
 REQUIRE(directory.get() >= 0);
 auto opened = BenchmarkEncodedImage::open_deferred(directory.get(), 1, {}, pipeline.execution().reserve(CompileResources::handles(1, true)));
 REQUIRE(opened);
 auto mapped = opened->try_read(&pipeline.execution(), {});
 REQUIRE(mapped);
 REQUIRE(mapped->charged_bytes());
 REQUIRE(mapped->storage() == BenchmarkEncodedImage::Storage::Mapped);
 opened.reset();
 auto file = mapped->file_input();
 REQUIRE(file->storage() == BenchmarkEncodedImage::Storage::HeaderOnly);
 CHECK(file->encoded().empty());
 CHECK_FALSE(file->allowance());
 CHECK(file->same_file(*mapped));
 // The full byte target cannot be admitted while the original mapping lives.
 CHECK_FALSE(pipeline.execution().try_reserve({256U << 10, 0}).has_value());
 mapped.reset();
 auto recovered = pipeline.execution().try_reserve({256U << 10, 0});
 REQUIRE(recovered);
 recovered = {};
 BenchmarkEncodedImage::publish(path, make_jpeg(99, 20, 30), {});
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8, {}, &pipeline);
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 bind_benchmark_slots(pipeline, writer, split);
 const auto publication = pipeline.source_publication(images, {});
 publication({1, {}, true, std::move(file)});
 CHECK_THROWS(pipeline.image_completion(images, 1).wait());
 CHECK_FALSE(writer.image_complete(0));
 CHECK_THROWS(pipeline.drain());
}
TEST_CASE("batch workspace loans finish pixels and retire scratch before reader continuation", "[backend][data][benchmark][pipeline]") {
 bool fail = false, allocation_failure = false;
 SECTION("ordinary reader continuation") {}
 SECTION("consumer exception unwinds the offer") { fail = true; }
 SECTION("partial consumer allocation unwinds the offer") { allocation_failure = true; }
 mmltk::testsupport::ScopedTempDir root("batch-workspace-window");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 constexpr std::uint64_t target = 256ULL << 20;
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = target, .descriptors = 13});
 auto source = execution.execution().reserve({target, 1, false, 0, false, 2});
 auto descriptor_alias = source;
 std::promise<void> completed;
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8);
 request.execution = &execution;
 request.progress = {.context = &completed, .image_completed = [](void* value) { static_cast<std::promise<void>*>(value)->set_value(); }};
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 execution.source_publication(images, {})({1, {}, true});
 const auto window = [&] {
  execution.execution().with_unused_workspace(source, 1U << 20, [&] {
   CHECK_FALSE(execution.execution().try_reserve({1, 0, true}));
   CHECK_FALSE(execution.execution().try_reserve({1, 0}));
   mmltk::testsupport::await_test_promise(completed, "pixel consumes unused reader workspace", 5s);
   if (allocation_failure) throw std::bad_alloc{};
   if (fail) throw std::runtime_error("normalizer failed after independent pixels completed");
  });
 };
 if (allocation_failure)
  CHECK_THROWS_AS(window(), std::bad_alloc);
 else if (fail)
  CHECK_THROWS(window());
 else
  window();
 CHECK(writer.image_complete(0));
 CHECK(source.bytes() == target);
 // Reclaim has retired the borrower's real idle decoder/resizer capacity.
 source.retire_workspace();
 CHECK(descriptor_alias.bytes() == 0);
 CHECK(execution.execution().try_reserve({target, 0}));
 source.retire_descriptors();
 CHECK(descriptor_alias.descriptors() == 0);
 source = {};
 descriptor_alias = {};
 CHECK(execution.execution().resource_usage().descriptors == 2);
 CHECK(execution.execution().try_reserve({target, 11}));  // Staging file and cached directory remain live.
}
TEST_CASE("a workspace window retains its producing credits through callback unwind", "[backend][data][benchmark][pipeline]") {
 bool fail = false;
 SECTION("successful callback releases its last external allowance") {}
 SECTION("throwing callback releases its last external allowance") { fail = true; }
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 64, .descriptors = 13});
 std::vector<std::uint8_t> input(16, 0x42);
 auto producer = execution.execution().reserve({64, 1});
 const auto consume = [&] {
  execution.execution().with_unused_workspace(producer, input.capacity(), [&] {
   producer = {};
   CHECK_FALSE(execution.execution().try_reserve({1, 0}));
   CHECK(std::ranges::all_of(input, [](auto byte) { return byte == 0x42; }));
   std::vector<std::uint8_t>().swap(input);
   if (fail) throw std::runtime_error("consumer retains its original failure");
  });
 };
 if (fail)
  CHECK_THROWS_WITH(consume(), "consumer retains its original failure");
 else
  consume();
 CHECK(input.capacity() == 0);
 CHECK(execution.execution().try_reserve({64, 13}));
}
TEST_CASE("physical workspace retirement waits for CPU frames and the stable offer to settle", "[backend][data][benchmark][pipeline]") {
 bool fail = false, cancel = false;
 SECTION("completed CPU and input window") {}
 SECTION("consumer exception settles deferred retirement") { fail = true; }
 SECTION("cancellation settles deferred retirement") { cancel = true; }
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 constexpr auto target = 128U << 10;
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = target, .descriptors = 13}, cancellation);
 std::vector<std::uint8_t> storage(64U << 10, 0x6a);
 auto producer = execution.execution().reserve({target, 1});
 const auto alias = producer;
 mmltk::testsupport::TestGate retired("physical input freed but CPU and offer still live");
 auto work = std::async(std::launch::async, [&] {
  execution.execution().with_unused_workspace(producer, storage.capacity(), [&] {
   execution.execution().run(CompileTaskKind::Metadata, {}, [&](std::size_t) {
    std::vector<std::uint8_t>().swap(storage);
    producer.retire_workspace();
    CHECK(storage.capacity() == 0);
    CHECK(alias.bytes() == target);
    CHECK_THROWS(producer.split_storage(0));
    retired.receipt().ArriveAndWait();
    if (fail) throw std::runtime_error("deferred retirement consumer failed");
    if (cancel) cancelled.store(true);
   }, producer);
   CHECK(alias.bytes() == target);  // CPU ended; the stable input scope still owns its promise.
  });
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] { retired.Release(); });
 REQUIRE(retired.WaitEntered(5s));
 CHECK_FALSE(execution.execution().try_reserve({1, 0}));
 retired.Release();
 REQUIRE(work.wait_for(5s) == std::future_status::ready);
 if (fail)
  CHECK_THROWS_WITH(work.get(), "deferred retirement consumer failed");
 else if (cancel)
  CHECK_THROWS(work.get());
 else
  work.get();
 cancelled.store(false);
 CHECK(alias.bytes() == 0);
 CHECK(alias.descriptors() == 1);
 CHECK(execution.execution().try_reserve({target, 12}));
 producer.retire_descriptors();
 CHECK(execution.execution().try_reserve({target, 13}));
}
TEST_CASE("retiring a reader returns only its closed descriptors while descendants remain", "[backend][data][benchmark][pipeline]") {
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 32, .descriptors = 13});
 auto parent = execution.execution().reserve({0, 1, true, 0, true, 6});
 auto reader = execution.execution().reserve({24, 2, false, 0, false, 3}, parent);
 auto alias = reader;
 auto descendant = execution.execution().reserve(CompileResources::handles(2), reader);
 CHECK_THROWS(reader.split_storage(25));
 CHECK(reader.bytes() == 24);
 auto backing = reader.split_storage(8);
 reader.retire_descriptors();
 CHECK(alias.descriptors() == 0);
 // Parent can reuse the two closed file slots; live children retain theirs.
 auto replacement = execution.execution().reserve(CompileResources::handles(2), parent);
 reader.retire_workspace();
 CHECK(alias.bytes() == 0);
 CHECK(backing.bytes() == 8);
 CHECK_FALSE(execution.execution().try_reserve({25, 0}));
 parent.retire_descriptors();
 CHECK(descendant.descriptors() == 2);
 replacement = {};
 descendant = {};
 reader = {};
 alias = {};
 parent = {};
 CHECK(execution.execution().try_reserve({24, 13}));
 backing = {};
 CHECK(execution.execution().try_reserve({32, 13}));
}
TEST_CASE("long JSON documents admit short chunks beside pixels on one CPU", "[backend][data][benchmark][annotations][pipeline]") {
 mmltk::testsupport::ScopedTempDir root("bounded-json-chunks");
 const auto path = root.path() / "annotations.json";
 nlohmann::json document{{"categories", {{{"id", 1}, {"name", "person"}}}}, {"images", {{{"id", 1}, {"width", 16}, {"height", 8}}}}, {"annotations", nlohmann::json::array()}};
 for (unsigned i = 0; i < 20000; ++i) document["annotations"].push_back({{"image_id", 1}, {"category_id", 1}, {"bbox", {0, 0, 2, 2}}, {"unused", std::string(256, 'x')}});
 write_text(path, document.dump());
 constexpr auto target = 256ULL << 20;
 REQUIRE(fs::file_size(path) > target / 64);
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = target, .descriptors = 13}, {}, mmltk::backend::data::testsupport::benchmark_trace());
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8);
 request.execution = &execution;
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 auto pixel = mmltk::backend::data::testsupport::prepare_benchmark_pixel(execution, writer);
 mmltk::testsupport::TestGate chunk("first bounded annotation chunk");
 bool first = true, overlapped = false;
 AnnotationParseOptions options;
 options.source = BenchmarkDatasetSource::kCoco2017;
 options.split = "train";
 options.execution = &execution.execution();
 options.trace = [&](std::string_view event, const nlohmann::json&) {
  if (event != "benchmark.annotations.workspace" || !std::exchange(first, false)) return;
  chunk.receipt().ArriveAndWait();
  execution.execution().cooperate();
  overlapped = writer.image_complete(0);
 };
 const std::array<NumericCategoryMapping, 1> categories{{{1, 0, "person"}}};
 auto parsing = std::async(std::launch::async, [&] { return parse_coco_style_annotations(path, std::string(64, 'a'), categories, options); });
 const mmltk::testsupport::ScopedTestCleanup settle([&] { chunk.Release(); });
 REQUIRE(chunk.WaitEntered(5s));
 auto pixel_completion = execution.execution().submit(std::move(pixel));
 chunk.Release();
 const auto parsed = mmltk::testsupport::await_test_future(parsing, "bounded JSON with independent pixels", 10s);
 CHECK(overlapped);
 CHECK(parsed.boxes.size() == 20000);
 CHECK(parsed.rejected.raw_records == 20000);
}
TEST_CASE("mapped normalized generations survive replacement and admit masks only when consumed", "[backend][data][benchmark][annotations]") {
 mmltk::testsupport::ScopedTempDir root("immutable-normalized-generation");
 const auto path = root.path() / "annotations.bin";
 NormalizedAnnotationBuilder builder;
 builder.split = "train";
 builder.annotation_sha256 = std::string(64, 'a');
 builder.images.push_back({.source_image_id = 1, .first_box = 0, .box_count = 1, .width = 4, .height = 4});
 NormalizedBox box;
 box.x2 = box.y2 = 1;
 box.flags = kAnnotationMask | kAnnotationCategory;
 box.source_category_id = 1;
 box.mask_rle_pairs = 1;
 builder.boxes.push_back(box);
 builder.mask_rle_pairs.push_back({0, 16});
 auto product = fixture_index(builder);
 const auto proof = store_normalized_annotation_index(path, product, {});
 auto first = load_normalized_annotation_index(path, builder.source, builder.split, builder.annotation_sha256, {});
 REQUIRE(first);
 REQUIRE(first->completion);
 for (const auto& descriptor : fs::directory_iterator("/proc/self/fd")) {
  std::error_code error;
  const auto target = fs::read_symlink(descriptor.path(), error);
  if (!error) CHECK(target != path);
 }
 CHECK(first->completion->proof_bytes == fs::file_size(path.string() + ".complete.json"));
 CHECK(first->completion->identity == proof->identity);
 auto selected = NormalizedAnnotationReadView(*first).select_images({0});
 const auto* selected_runs = selected.storage().mask_rle_pairs.data();
 CHECK(selected.completion == first->completion);
 CHECK(&selected.image(0) == first->images.data());
 builder.images.front().source_image_id = 2;
 (void)store_normalized_annotation_index(path, fixture_index(builder), {});
 auto second = load_normalized_annotation_index(path, builder.source, builder.split, builder.annotation_sha256, {});
 REQUIRE(second);
 CHECK(first->images.front().source_image_id == 1);
 CHECK(second->images.front().source_image_id == 2);
 CHECK(first->mask_rle_pairs.front().length == 16);
 first.reset();
 CHECK(selected.image(0).source_image_id == 1);
 CHECK(selected.storage().mask_rle_pairs.data() == selected_runs);
 CHECK(selected.completion->images == 1);
 // Damage the new file's last RLE length without changing its layout or proof.
 {
  std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
  const std::uint32_t too_long = 17;
  output.seekp(-static_cast<std::streamoff>(sizeof(too_long)), std::ios::end);
  output.write(reinterpret_cast<const char*>(&too_long), sizeof(too_long));
 }
 auto metadata = load_normalized_annotation_index(path, builder.source, builder.split, builder.annotation_sha256, {}, {}, nullptr, true);
 REQUIRE(metadata);
 CHECK(metadata->images.front().source_image_id == 2);
 CHECK_THROWS(admit_normalized_annotations(*metadata));
 CHECK(selected.storage().mask_rle_pairs.front().length == 16);
}
TEST_CASE("sparse full and empty COCO masks do not require a canvas", "[backend][data][benchmark][annotations]") {
 mmltk::testsupport::ScopedTempDir root("sparse-whole-mask");
 const auto path = root.path() / "annotations.json";
 constexpr std::uint32_t extent = 16000, pixels = extent * extent;
 const nlohmann::json document{
  {"images", {{{"id", 1}, {"width", extent}, {"height", extent}}}}, {"categories", {{{"id", 1}, {"name", "person"}}}},
  {"annotations", {{{"id", 1}, {"image_id", 1}, {"category_id", 1}, {"bbox", {0, 0, extent, extent}}, {"segmentation", {{"size", {extent, extent}}, {"counts", {0, pixels}}}}},
                   {{"id", 2}, {"image_id", 1}, {"category_id", 1}, {"bbox", {0, 0, extent, extent}}, {"segmentation", {{"size", {extent, extent}}, {"counts", {pixels}}}}}}}
 };
 write_text(path, document.dump());
 const std::array<NumericCategoryMapping, 1> categories{{{1, 0, "person"}}};
 AnnotationParseOptions options;
 options.split = "train";
 const auto result = parse_coco_style_annotations(path, std::string(64, 'a'), categories, options);
 REQUIRE(result.boxes.size() == 2);
 REQUIRE(result.mask_rle_pairs.size() == 1);
 CHECK(result.mask_rle_pairs[0].start == 0);
 CHECK(result.mask_rle_pairs[0].length == pixels);
 CHECK(result.boxes[0].mask_rle_pairs == 1);
 CHECK(result.boxes[1].mask_rle_pairs == 0);
 CHECK((result.boxes[1].flags & kAnnotationMask) != 0);
}
TEST_CASE("concurrent reader offers join a live pixel loan on cancellation", "[backend][data][benchmark][pipeline]") {
 bool cancel = false;
 SECTION("two readers reclaim together") {}
 SECTION("cancel while a pixel owns both offers") { cancel = true; }
 mmltk::testsupport::ScopedTempDir root("concurrent-workspace-offers");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 constexpr auto target = 256ULL << 20;
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = target, .descriptors = 16}, cancellation);
 auto first_reader = execution.execution().reserve({target / 2, 1});
 auto second_reader = execution.execution().reserve({target / 2, 1});
 mmltk::testsupport::TestGate first_window("first reader allocation window"), second_window("second reader allocation window"), pixels("pixel still owns workspace");
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8, cancellation);
 request.execution = &execution;
 request.progress = {.context = &pixels, .image_completed = [](void* value) { static_cast<mmltk::testsupport::TestGate*>(value)->receipt().ArriveAndWait(); }};
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 // Each offer covers only half the finite consumer. Both physical lenders
 // must participate; unrelated positive offers are exercised separately.
 const auto demand = writer.prepare_pixel_plan(0, {16, 8})->resources().bytes;
 const auto live_reader_bytes = target / 2 - (demand + 1) / 2;
 std::future<void> first, second;
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  cancelled.store(true);
  first_window.Release();
  second_window.Release();
  pixels.Release();
 });
 first = std::async(std::launch::async, [&] { execution.execution().with_unused_workspace(first_reader, live_reader_bytes, [&] { first_window.receipt().ArriveAndWait(); }); });
 second = std::async(std::launch::async, [&] { execution.execution().with_unused_workspace(second_reader, live_reader_bytes, [&] { second_window.receipt().ArriveAndWait(); }); });
 REQUIRE(first_window.WaitEntered(5s));
 REQUIRE(second_window.WaitEntered(5s));
 CHECK_FALSE(execution.execution().try_reserve({1, 0, true}));
 execution.source_publication(images, {})({1, {}, true});
 REQUIRE(pixels.WaitEntered(5s));
 if (cancel) cancelled.store(true);
 first_window.Release();
 second_window.Release();
 CHECK(first.wait_for(0ms) == std::future_status::timeout);
 CHECK(second.wait_for(0ms) == std::future_status::timeout);
 pixels.Release();
 if (cancel) {
  REQUIRE(first.wait_for(5s) == std::future_status::ready);
  CHECK_THROWS(first.get());
  REQUIRE(second.wait_for(5s) == std::future_status::ready);
  CHECK_THROWS(second.get());
 } else {
  mmltk::testsupport::await_test_future(first, "first reader reclaims physical pixel scratch", 5s);
  mmltk::testsupport::await_test_future(second, "second reader reclaims physical pixel scratch", 5s);
  CHECK(writer.image_complete(0));
  first_reader.retire_workspace();
  second_reader.retire_workspace();
  CHECK(execution.execution().try_reserve({target, 0}));
 }
}
TEST_CASE("a zero-byte workspace offer keeps its scope without joining a live pixel loan", "[backend][data][benchmark][pipeline]") {
 bool fail = false;
 SECTION("zero-capacity window returns while positive loan stays active") {}
 SECTION("zero-capacity callback failure keeps its original exception") { fail = true; }
 mmltk::testsupport::ScopedTempDir root("zero-workspace-offer");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 constexpr auto target = 256ULL << 20;
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = target, .descriptors = 13});
 std::vector<std::uint8_t> positive_storage(1U << 20, 0x7a), zero_storage(64U << 10, 0x3b);
 auto positive = execution.execution().reserve({target - zero_storage.capacity(), 1});
 auto zero = execution.execution().reserve({zero_storage.capacity(), 1});
 const auto alias = zero;
 mmltk::testsupport::TestGate positive_window("positive producer stopped"), zero_window("zero producer stopped"), pixels("pixel decoder backing is still live");
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8);
 request.execution = &execution;
 request.progress = {.context = &pixels, .image_completed = [](void* value) { static_cast<mmltk::testsupport::TestGate*>(value)->receipt().ArriveAndWait(); }};
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 std::future<void> positive_call, zero_call;
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  positive_window.Release();
  zero_window.Release();
  pixels.Release();
 });
 positive_call = std::async(std::launch::async, [&] { execution.execution().with_unused_workspace(positive, positive_storage.capacity(), [&] { positive_window.receipt().ArriveAndWait(); }); });
 zero_call = std::async(std::launch::async, [&] {
  execution.execution().with_unused_workspace(zero, zero_storage.capacity(), [&] {
   CHECK_THROWS(execution.execution().with_unused_workspace(alias, alias.bytes(), [] {}));
   CHECK_THROWS(execution.execution().try_resize_workspace(alias, alias.bytes()));
   CHECK_THROWS(zero.split_storage(0));
   zero_window.receipt().ArriveAndWait();
   if (fail) throw std::runtime_error("zero-capacity consumer failed");
  });
 });
 REQUIRE(positive_window.WaitEntered(5s));
 REQUIRE(zero_window.WaitEntered(5s));
 execution.source_publication(images, {})({1, {}, true});
 REQUIRE(pixels.WaitEntered(5s));
 zero_window.Release();
 REQUIRE(zero_call.wait_for(5s) == std::future_status::ready);
 if (fail)
  CHECK_THROWS_WITH(zero_call.get(), "zero-capacity consumer failed");
 else
  zero_call.get();
 CHECK(std::ranges::all_of(zero_storage, [](auto byte) { return byte == 0x3b; }));
 CHECK(alias.bytes() == zero_storage.capacity());
 CHECK(execution.execution().try_resize_workspace(alias, alias.bytes()));  // Scope sentinel has retired.
 positive_window.Release();
 CHECK(positive_call.wait_for(0ms) == std::future_status::timeout);
 pixels.Release();
 mmltk::testsupport::await_test_future(positive_call, "positive offer reclaims physical pixel backing");
 CHECK(writer.image_complete(0));
 CHECK(std::ranges::all_of(positive_storage, [](auto byte) { return byte == 0x7a; }));
 std::vector<std::uint8_t>().swap(positive_storage);
 std::vector<std::uint8_t>().swap(zero_storage);
 positive.retire_workspace();
 zero.retire_workspace();
 CHECK(alias.bytes() == 0);
 CHECK(alias.descriptors() == 1);
 CHECK(execution.execution().resource_usage().descriptors == 4);
 CHECK(execution.execution().try_reserve({target, 9}));  // Two readers, staging file and cached directory remain live.
 positive.retire_descriptors();
 zero.retire_descriptors();
 CHECK(execution.execution().resource_usage().descriptors == 2);
 CHECK(execution.execution().try_reserve({target, 11}));
}
TEST_CASE("Open Images class fields retain escaped text beyond four quoted columns", "[backend][data][benchmark][annotations]") {
 mmltk::testsupport::ScopedTempDir root("open-images-quoted-fields");
 const auto classes = root.path() / "classes.csv", boxes = root.path() / "boxes.csv";
 write_text(classes, "\"/m/person\",\"Person\",\"one\",\"two\",\"three\",\"four\"\"five\"\n");
 write_text(boxes, "ImageID,Source,LabelName,Confidence,XMin,XMax,YMin,YMax,IsOccluded,IsTruncated,IsGroupOf\n0000000000000001,x,/m/person,1,0,1,0,1,0,0,0\n");
 const std::array<StringCategoryMapping, 1> mappings{{{"/m/person", 0, "Person"}}};
 AnnotationParseOptions options;
 options.split = "train";
 const auto result = parse_open_images_annotations(boxes, classes, std::string(64, 'a'), mappings, options);
 REQUIRE(result.images.size() == 1);
 REQUIRE(result.boxes.size() == 1);
 CHECK(result.images[0].source_image_id == 1);
 CHECK(result.boxes[0].source_ordinal == std::string_view("ImageID,Source,LabelName,Confidence,XMin,XMax,YMin,YMax,IsOccluded,IsTruncated,IsGroupOf\n").size());
}
TEST_CASE("progressive JSON semantics precede unrelated envelope tails", "[backend][data][benchmark][annotations][pipeline]") {
 bool reversed = false, malformed = false, cancel = false;
 SECTION("ordered arrays and valid tail") {}
 SECTION("reversed arrays and escaped envelope names") { reversed = true; }
 SECTION("malformed ignored tail after ready annotation") { malformed = true; }
 SECTION("cancel after ready annotation before the tail") { cancel = true; }
 mmltk::testsupport::ScopedTempDir root("progressive-json-tail");
 const auto path = root.path() / "annotations.json";
 const std::string categories = R"("categories":[{"id":1,"name":"person"}])";
 const std::string images = R"("\u0069mages":[{"id":1,"width":16,"height":8}])";
 const std::string row = R"({"image_id":1,"category_id":1,"bbox":[0,0,2,2]})";
 const std::string annotations = "\"annotations\":[" + row + "]";
 const auto prefix = "{" + (reversed ? annotations + "," + images + "," + categories : categories + "," + images + "," + annotations);
 write_text(path, prefix + ",\"tail\":\"" + std::string(2U << 20, 'x') + (malformed ? "" : "\"}"));
 const auto image_root = root.path() / "images";
 auto split = cached_pixel_membership(image_root);
 split.images = {{1, 16, 8, 0, 0}};
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256ULL << 20, .descriptors = 13}, cancellation);
 auto output = benchmark_write_request(split, root.path() / "pixels.bin", 8);
 output.execution = &execution;
 BenchmarkSplitWriter writer(output);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 auto pixel = mmltk::backend::data::testsupport::prepare_benchmark_pixel(execution, writer);
 bool visited = false, pixels_ready = false;
 AnnotationParseOptions options;
 options.split = "train";
 options.execution = &execution.execution();
 options.cancel_requested = cancellation;
 options.trace = [&](std::string_view event, const nlohmann::json&) {
  if (event != "benchmark.annotations.workspace" || std::exchange(visited, true)) return;
  auto pixel_completion = execution.execution().try_submit(std::move(pixel));
  require_condition(pixel_completion.has_value(), "prepared pixel did not obtain a task record");
  execution.execution().cooperate();
  pixels_ready = writer.image_complete(0);
  if (cancel) cancelled.store(true);
 };
 const std::array<NumericCategoryMapping, 1> mappings{{{1, 0, "person"}}};
 if (malformed || cancel)
  CHECK_THROWS(parse_coco_style_annotations(path, std::string(64, 'a'), mappings, options));
 else {
  const auto parsed = parse_coco_style_annotations(path, std::string(64, 'a'), mappings, options);
  REQUIRE(parsed.boxes.size() == 1);
  CHECK(parsed.boxes[0].source_ordinal == prefix.find(row));
  CHECK(parsed.boxes[0].original_area == 4);
 }
 CHECK(visited);
 CHECK(pixels_ready);
}
TEST_CASE("sparse annotation bounds and area retain normalized and compiled meaning", "[benchmark][annotations][masks][writer]") {
 mmltk::testsupport::ScopedTempDir root("sparse-mask-facts");
 nlohmann::json rows = nlohmann::json::array();
 for (unsigned kind = 0; kind != 4; ++kind) {
  nlohmann::json row{{"id", kind}, {"image_id", 1}, {"category_id", 1}, {"segmentation", {{"size", {8, 16}}, {"counts", {9, 2, 6, 2, 109}}}}};
  if (kind & 1U) row["area"] = 7.25;
  if (kind & 2U) row["bbox"] = {1, 1, 2, 2};
  rows.push_back(std::move(row));
 }
 const auto path = root.path() / "annotations.json";
 write_text(path, nlohmann::json{{"images", {{{"id", 1}, {"width", 16}, {"height", 8}}}}, {"categories", {{{"id", 1}, {"name", "person"}}}}, {"annotations", rows}}.dump());
 const std::array<NumericCategoryMapping, 1> categories{{{1, 0, "person"}}};
 const auto index = parse_coco_style_annotations(path, "sparse-facts", categories, {.split = "train"});
 REQUIRE(index.boxes.size() == 4);
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 split.sources = {{root.path() / "images"}};
 split.images = {{1, 16, 8, 4, 0, AnnotationSource::Coco}};
 auto annotations = std::make_shared<BenchmarkLabelChunk>();
 prepare_cached_image_directory(split.sources[0].root);
 BenchmarkEncodedImage::publish(cached_image_path(split.sources[0].root, 1), make_jpeg(10, 20, 30), {});
 const auto geometry = mmltk::backend::imaging::resample::compute_image_resize_geometry(16, 8, 16, 16, mmltk::backend::imaging::resample::ImageResizeMode::Stretch);
 dataset::MaskResizeScratch scratch;
 for (const auto& box : index.boxes) {
  CHECK(box.original_area == (box.annotation_id & 1U ? 7.25 : 4.0));
  CHECK(box.x1 == 1.0F / 16);
  CHECK(box.y1 == 1.0F / 8);
  CHECK(box.x2 == 3.0F / 16);
  CHECK(box.y2 == 3.0F / 8);
  auto label = benchmark_canvas_box(box.class_id, box.x1, box.y1, box.x2, box.y2, geometry);
  label.flags = box.flags;
  label.original_area = box.original_area;
  label.annotation_id = box.annotation_id;
  label.source_category_id = box.source_category_id;
  label.source_ordinal = box.source_ordinal;
  const auto first = annotations->runs.size();
  (void)dataset::append_resized_row_major_mask(index.mask_rle_pairs.subspan(box.mask_rle_offset, box.mask_rle_pairs), {16, 8}, {16, 16}, geometry, &scratch, annotations->runs);
  label.mask_rle_offset = first * sizeof(RLEPair);
  label.mask_rle_pairs = static_cast<std::uint16_t>(annotations->runs.size() - first);
  annotations->labels.push_back(label);
 }
 const auto output = root.path() / "compiled.bin";
 const std::array chunks{annotations};
 write_benchmark_fixture({.split = split, .output_path = output, .resolution = 16, .num_workers = 1, .resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch}, chunks);
 const auto compiled = CompiledDataset::open(output);
 REQUIRE(compiled.image_labels(0).size() == 4);
 for (const auto& label : compiled.image_labels(0)) {
  CHECK(label.original_area == (label.annotation_id & 1U ? 7.25 : 4.0));
  CHECK(label.bbox_x1 == 1.0F);
  CHECK(label.bbox_y1 == 2.0F);
  CHECK(label.bbox_x2 == 3.0F);
  CHECK(label.bbox_y2 == 6.0F);
  std::uint64_t area = 0;
  const auto bounds = dataset::row_major_mask_bounds(compiled.instance_rle(0, label), {16, 16}, &area);
  CHECK(area == 8);
  CHECK(bounds.min_x == 1);
  CHECK(bounds.min_y == 2);
  CHECK(bounds.max_x == 3);
  CHECK(bounds.max_y == 6);
 }
 std::uint64_t empty_area = 99;
 CHECK_FALSE(dataset::row_major_mask_bounds({}, {16, 8}, &empty_area).has_foreground);
 CHECK(empty_area == 0);
 const std::array<RLEPair, 1> malformed{{{127, 2}}};
 CHECK_THROWS(dataset::row_major_mask_bounds(malformed, {16, 8}, &empty_area));
}
TEST_CASE("settled workspace resize preserves aliases descendants and oversized continuation", "[benchmark][pipeline][resources]") {
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256ULL << 20, .descriptors = 13});
 auto producer = execution.execution().reserve({200ULL << 20, 1, false, 0, false, 2});
 const auto alias = producer;
 auto storage = producer.split_storage(64ULL << 20);
 auto handles = execution.execution().reserve(CompileResources::handles(1), producer);
 CHECK(producer.aliases(alias));
 CHECK_FALSE(producer.aliases(storage));
 CHECK_FALSE(execution.execution().try_resize_workspace(producer, 256ULL << 20));  // The live child is independent storage.
 CHECK(producer.bytes() == 136ULL << 20);
 REQUIRE(execution.execution().try_resize_workspace(producer, 128ULL << 20));
 auto independent = execution.execution().try_reserve({64ULL << 20, 0});
 REQUIRE(independent);
 CHECK_FALSE(execution.execution().try_resize_workspace(producer, 192ULL << 20));
 independent.reset();
 storage = {};
 REQUIRE(execution.execution().try_resize_workspace(producer, 512ULL << 20));  // One legal oversized lineage.
 CHECK(alias.bytes() == 512ULL << 20);
 std::promise<void> entered, release;
 auto released = release.get_future();
 auto active = std::async(std::launch::async, [&] {
  execution.execution().run(CompileTaskKind::Metadata, {}, [&](std::size_t) {
   entered.set_value();
   released.wait();
  }, alias);
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] { mmltk::testsupport::release_test_promise(release); });
 mmltk::testsupport::await_test_promise(entered, "active alias CPU frame");
 CHECK_FALSE(execution.execution().try_resize_workspace(producer, 1));
 CHECK(alias.bytes() == 512ULL << 20);
 mmltk::testsupport::release_test_promise(release);
 mmltk::testsupport::await_test_future(active, "retired alias CPU frame");
 CHECK_THROWS(execution.execution().run(CompileTaskKind::Metadata, {}, [&](std::size_t) { (void)execution.execution().try_resize_workspace(producer, 1); }, producer));
 CHECK_THROWS(execution.execution().with_unused_workspace(producer, 1, [&] { (void)execution.execution().try_resize_workspace(producer, 1); }));
 REQUIRE(execution.execution().try_resize_workspace(producer, 512ULL << 20));
 producer.retire_descriptors();
 CHECK(alias.descriptors() == 0);
 handles = {};
 producer.retire_workspace();
 CHECK(execution.execution().try_reserve({256ULL << 20, 13}));
}
TEST_CASE("two failed workspace upgrades retire before complete one CPU reacquisition", "[benchmark][pipeline][resources]") {
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256ULL << 20, .descriptors = 13});
 auto first = execution.execution().reserve({128ULL << 20, 1});
 auto second = execution.execution().reserve({128ULL << 20, 1});
 std::promise<void> first_failed, second_failed;
 const auto first_ready = first_failed.get_future().share(), second_ready = second_failed.get_future().share();
 const auto upgrade = [&](CompileAllowance allowance, std::promise<void>& failed, const std::shared_future<void>& other) {
  const bool grew = execution.execution().try_resize_workspace(allowance, 192ULL << 20);
  failed.set_value();
  other.wait();
  if (grew) throw std::runtime_error("simultaneous retained upgrade exceeded target");
  allowance.retire_descriptors();
  allowance.retire_workspace();
  allowance = {};
  auto complete = execution.execution().reserve({192ULL << 20, 1});
  execution.execution().run(CompileTaskKind::Normalize, {}, [](std::size_t) {}, complete);
 };
 auto a = std::async(std::launch::async, [&] { upgrade(std::move(first), first_failed, second_ready); });
 auto b = std::async(std::launch::async, [&] { upgrade(std::move(second), second_failed, first_ready); });
 mmltk::testsupport::await_test_future(a, "first complete upgrade");
 mmltk::testsupport::await_test_future(b, "second complete upgrade");
 CHECK(execution.execution().try_reserve({256ULL << 20, 13}));
}
TEST_CASE("source parser workspace reuses charged capacity and yields under an unrelated scanner", "[benchmark][pipeline][annotations][resources]") {
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256ULL << 20, .descriptors = 13});
 simdjson::ondemand::parser parser;
 std::atomic<unsigned> retirements{0};
 const auto text = simdjson::padded_string(std::string_view{R"({"value":"retained"})"});
 const auto demand = [](std::size_t) { return CompileResources{192ULL << 20, 0}; };
 {
  CompileExecution::Workspace workspace(execution.execution(), [&](std::size_t) noexcept {
   parser = simdjson::ondemand::parser{};
   ++retirements;
  });
  const auto parse = [&](std::size_t) {
   auto document = parser.iterate(text);
   const std::string_view value = document["value"].get_string().value();
   CHECK(value == "retained");
   CHECK_THROWS(execution.execution().for_each(CompileTaskKind::Metadata, 1, demand, [](std::size_t) {}, workspace));
   execution.execution().cooperate();
   CHECK(value == "retained");
  };
  execution.execution().for_each(CompileTaskKind::Metadata, 1, demand, parse, workspace);
  const auto capacity = parser.capacity();
  REQUIRE(capacity > 0);
  execution.execution().for_each(CompileTaskKind::Metadata, 1, demand, parse, workspace);
  CHECK(parser.capacity() == capacity);
  CHECK(retirements.load() == 0);
  CHECK_FALSE(execution.execution().try_reserve({128ULL << 20, 0}));
  std::promise<void> entered, release, pressure_ready;
  const auto proceed = release.get_future();
  auto scanner = std::async(std::launch::async, [&] {
   execution.execution().run(CompileTaskKind::Metadata, {}, [&](std::size_t) {
    entered.set_value();
    proceed.wait();
    execution.execution().cooperate();  // Idle parser identity differs from this live frame.
    CHECK(parser.capacity() == 0);
   });
  });
  const mmltk::testsupport::ScopedTestCleanup settle([&] { mmltk::testsupport::release_test_promise(release); });
  mmltk::testsupport::await_test_promise(entered, "unrelated scanner owns the sole CPU");
  auto consumer = std::async(std::launch::async, [&] {
   auto pressure = execution.execution().defer_resources();
   pressure_ready.set_value();
   auto allowance = execution.execution().reserve({128ULL << 20, 0});
   CHECK(retirements.load() == 1);
  });
  const mmltk::testsupport::ScopedTestCleanup release_before_consumer([&] { mmltk::testsupport::release_test_promise(release); });
  mmltk::testsupport::await_test_promise(pressure_ready, "consumer requires idle parser capacity");
  mmltk::testsupport::release_test_promise(release);
  mmltk::testsupport::await_test_future(scanner, "scanner retired unrelated parser scratch");
  mmltk::testsupport::await_test_future(consumer, "consumer admitted after physical retirement");
  execution.execution().for_each(CompileTaskKind::Metadata, 1, demand, parse, workspace);
  CHECK(parser.capacity() > 0);
 }
 CHECK(retirements.load() == 2);
 CHECK(parser.capacity() == 0);
 CHECK(execution.execution().try_reserve({256ULL << 20, 13}));
}
TEST_CASE("source parser workspace retires nested borrowing and failed groups before source destruction", "[benchmark][pipeline][annotations][resources]") {
 bool fail = false, cancel = false;
 SECTION("distinct nested parser preserves the outer borrowed string") {}
 SECTION("throwing consumer releases source capacity") { fail = true; }
 SECTION("cancelled consumer releases source capacity") { cancel = true; }
 // CLEANUP-IGNORE: Parser lifetime and JSON-tail tests deliberately use the same constrained execution setup; their operations and assertions differ.
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 256ULL << 20, .descriptors = 13}, cancellation);
 simdjson::ondemand::parser outer, inner;
 unsigned outer_retirements = 0, inner_retirements = 0;
 const auto text = simdjson::padded_string(std::string_view{R"({"value":"outer borrowed value"})"});
 const auto demand = [](std::size_t) { return CompileResources{1ULL << 20, 0}; };
 {
  CompileExecution::Workspace outer_workspace(execution.execution(), [&](std::size_t) noexcept {
   outer = simdjson::ondemand::parser{};
   ++outer_retirements;
  });
  CompileExecution::Workspace inner_workspace(execution.execution(), [&](std::size_t) noexcept {
   inner = simdjson::ondemand::parser{};
   ++inner_retirements;
  });
  const auto consume = [&] {
   execution.execution().for_each(CompileTaskKind::Metadata, 1, demand, [&](std::size_t) {
    auto document = outer.iterate(text);
    const std::string_view borrowed = document["value"].get_string().value();
    execution.execution().for_each(CompileTaskKind::Metadata, 1, demand, [&](std::size_t) {
     auto nested = inner.iterate(text);
     CHECK(nested["value"].get_string().value() == borrowed);
    }, inner_workspace);
    CHECK(inner.capacity() == 0);
    CHECK(inner_retirements == 1);
    CHECK(borrowed == "outer borrowed value");
    if (fail) throw std::runtime_error("parser consumer failed");
    if (cancel) cancelled.store(true);
   }, outer_workspace);
  };
  if (fail || cancel)
   CHECK_THROWS(consume());
  else
   consume();
 }
 CHECK(inner_retirements == 1);
 CHECK(outer_retirements == 1);
 CHECK(outer.capacity() == 0);
 CHECK(inner.capacity() == 0);
}
TEST_CASE("native label readiness and physical pixels settle independently", "[backend][data][benchmark][pipeline][labels]") {
 using namespace std::chrono_literals;
 bool hold_labels = false, cancel_after_labels = false;
 SECTION("labels finish while the pixel producer is held") {}
 SECTION("pixels finish while recovered labels are held") { hold_labels = true; }
 SECTION("cancellation after labels retires the held pixel publication") { cancel_after_labels = true; }
 mmltk::testsupport::ScopedTempDir root("independent-label-readiness");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline execution(1, {}, {}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 execution.label_configuration(8, mmltk::backend::imaging::resample::ImageResizeMode::Stretch);
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8);
 request.execution = &execution;
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 execution.bind_source(images, writer, AnnotationSource::Generic);
 const auto physical = execution.source_publication(images, {}, 1);
 physical.geometry_ready(1, {16, 8});
 NormalizedAnnotationBuilder builder;
 builder.images.push_back({1, 0, 1, 16, 8, 0, 0});
 NormalizedBox box;
 box.x2 = box.y2 = 1;
 box.flags = kAnnotationMask | kAnnotationId | kAnnotationCategory;
 box.annotation_id = 4;
 box.source_category_id = 1;
 box.mask_rle_pairs = 1;
 box.original_area = 128;
 builder.boxes.push_back(box);
 builder.mask_rle_pairs.push_back({0, 128});
 const NormalizedAnnotationReadView index(fixture_index(builder));
 mmltk::testsupport::TestGate held("one independent image product");
 auto delayed = std::async(std::launch::async, [&] {
  held.receipt().ArriveAndWait();
  if (hold_labels)
   execution.labels_ready(physical, 1, BenchmarkLabelInput(index, 0), "annotations/originals-1", 1);
  else if (!physical.consume({1}))
   throw std::runtime_error("selected pixel publication was not consumed");
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] { held.Release(); });
 REQUIRE(held.WaitEntered(5s));
 if (hold_labels) {
  CHECK(physical.consume({1}));
  CHECK(writer.image_complete(0));
  CHECK_FALSE(execution.image_labels(images, 1, "annotations/originals-1"));
 } else {
  execution.labels_ready(physical, 1, BenchmarkLabelInput(index, 0), "annotations/originals-1", 1);
  CHECK_FALSE(writer.image_complete(0));
 }
 if (cancel_after_labels) {
  REQUIRE(execution.wait_image_labels(images, 1, "annotations/originals-1"));
  cancelled.store(true);
  execution.retire_attempt();
  CHECK_THROWS(execution.labels_ready(physical, 1, BenchmarkLabelInput(index, 0), "annotations/originals-1", 1));
  held.Release();
  CHECK_THROWS(mmltk::testsupport::await_test_future(delayed, "cancelled held pixel publication"));
  CHECK_FALSE(writer.image_complete(0));
  CHECK_FALSE(execution.image_labels(images, 1, "annotations/originals-1"));
  return;
 }
 held.Release();
 mmltk::testsupport::await_test_future(delayed, "independent label and pixel products");
 const auto labels = execution.wait_image_labels(images, 1, "annotations/originals-1");
 REQUIRE(labels);
 CHECK(writer.entry(0).num_instances == 1);
 CHECK(writer.image_complete(0));
 // Original withdrawal does not withdraw physical geometry or completed pixels.
 execution.labels_ready(physical, 1, BenchmarkLabelInput(index, 0), "annotations/originals-1", 1);
 execution.original_generation(images, 2, true);
 execution.labels_ready(physical, 1, BenchmarkLabelInput(index, 0), "annotations/originals-1", 1);
 CHECK_FALSE(execution.image_labels(images, 1, "annotations/originals-1"));
 REQUIRE(execution.geometry(images, 1));
 CHECK(writer.image_complete(0));
 execution.labels_ready(physical, 1, BenchmarkLabelInput(index, 0), "annotations/originals-2", 2);
 REQUIRE(execution.wait_image_labels(images, 1, "annotations/originals-2"));
 // A late label task cannot attach its old physical ticket to a replacement.
 execution.retire_image(images, 1);
 execution.labels_ready(physical, 1, BenchmarkLabelInput(index, 0), "annotations/originals-2", 2);
 CHECK_FALSE(execution.image_labels(images, 1, "annotations/originals-2"));
 const auto replacement = execution.source_publication(images, {}, 1);
 replacement.geometry_ready(1, {16, 8});
 execution.labels_ready(replacement, 1, BenchmarkLabelInput(index, 0), "annotations/originals-2", 2);
 REQUIRE(execution.wait_image_labels(images, 1, "annotations/originals-2"));
 CHECK_FALSE(writer.image_complete(0));
 // Replacement annotations allocate their own slot immediately, even before
 // pixels supply geometry for the newly introduced image.
 execution.membership_ready();
 builder.images.front().source_image_id = 2;
 const NormalizedAnnotationReadView added(fixture_index(builder));
 const auto added_publication = execution.source_publication(images, {}, 2);
 auto unselected = std::async(std::launch::async, [&] { execution.labels_ready(added_publication, 2, BenchmarkLabelInput(added, 0), "added-membership", 2); });
 const mmltk::testsupport::ScopedTestCleanup retire([&] {
  cancelled.store(true);
  execution.execution().notify_admission_change();
 });
 mmltk::testsupport::await_test_future(unselected, "new annotation membership enters fixed storage");
 CHECK_FALSE(execution.geometry(images, 2));
 const auto added_labels = execution.wait_image_labels(images, 2, "added-membership");
 REQUIRE(added_labels);
 const auto added_slot = execution.image_slot(images, 2);
 REQUIRE(added_slot);
 CHECK(writer.entry(*added_slot).num_instances == 1);
 CHECK_FALSE(writer.image_complete(*added_slot));
 replacement({1});
 execution.wait_image(images, 1);
 const std::array selected{std::uint32_t{0}};
 auto sealed = writer.seal_slots(request, selected);
 sealed.artifact.publish(request.output_path, {});
 const auto compiled = CompiledDataset::open(request.output_path);
 const auto written = compiled.annotations(0);
 REQUIRE(written.labels.size() == 1);
 REQUIRE(written.runs.size() == 1);
 CHECK(written.runs[0].start == 0);
 CHECK(written.runs[0].length == 64);
 CHECK(written.labels[0].original_area == 128);
}
TEST_CASE("unused continuation release preserves live children open descriptors and upstream promises", "[benchmark][pipeline][resources]") {
 BenchmarkCompilePipeline execution(1, {}, {.descriptors = 13});
 auto source = execution.execution().reserve(CompileResources::handles(1, true, 4));
 auto producer = execution.execution().reserve(CompileResources::handles(1, true, 3), source);
 auto child = execution.execution().reserve(CompileResources::handles(1), producer);
 producer.release_unused_continuation();
 source.release_unused_continuation();
 CHECK(producer.descriptors() == 1);
 CHECK(child.descriptors() == 1);
 CHECK(execution.execution().try_reserve(CompileResources::handles(10)));
 producer.release_unused_continuation();
 CHECK(execution.execution().try_reserve(CompileResources::handles(10)));
 child = {};
 CHECK(execution.execution().try_reserve(CompileResources::handles(11)));
 producer.release_unused_continuation();
 CHECK(producer.descriptors() == 1);
 CHECK(source.descriptors() == 1);
 {
  auto restored = execution.execution().try_reserve(CompileResources::handles(3), source);
  REQUIRE(restored);
 }
 source.release_unused_continuation();
 CHECK(execution.execution().try_reserve(CompileResources::handles(11)));
 producer.retire_descriptors();
 source.release_unused_continuation();
 CHECK(source.descriptors() == 1);
 {
  auto free = execution.execution().try_reserve(CompileResources::handles(12));
  REQUIRE(free);
 }
 source.retire_descriptors();
 CHECK(execution.execution().try_reserve(CompileResources::handles(13)));
}
TEST_CASE("constant polygon slabs preserve full clipped and empty rectangular support", "[benchmark][annotations][masks]") {
 mmltk::testsupport::ScopedTempDir root("polygon-constant-slabs");
 const auto path = root.path() / "annotations.json";
 constexpr std::uint32_t extent = 16000;
 const nlohmann::json polygons = nlohmann::json::array(
  {nlohmann::json::array({-5.0, -5.0, 17000.0, -5.0, 17000.0, 17000.0, -5.0, 17000.0}), nlohmann::json::array({-8.0, 0.0, -2.0, 0.0, -2.0, 16000.0, -8.0, 16000.0}),
   nlohmann::json::array({0.0, 0.0, 16000.0, 0.0, 16000.0, 8000.0, 0.0, 8000.0}), nlohmann::json::array({0.0, 8000.0, 16000.0, 8000.0, 16000.0, 16000.0, 0.0, 16000.0})});
 nlohmann::json annotations = nlohmann::json::array();
 for (std::size_t i = 0; i < polygons.size(); ++i)
  annotations.push_back({{"id", i + 1}, {"image_id", 1}, {"category_id", 1}, {"bbox", {0, 0, extent, extent}}, {"segmentation", nlohmann::json::array({polygons[i]})}});
 write_text(path, nlohmann::json{{"images", {{{"id", 1}, {"width", extent}, {"height", extent}}}}, {"categories", {{{"id", 1}, {"name", "person"}}}}, {"annotations", annotations}}.dump());
 const std::array<NumericCategoryMapping, 1> categories{{{1, 0, "person"}}};
 AnnotationParseOptions options;
 options.split = "train";
 const auto result = parse_coco_style_annotations(path, std::string(64, 'a'), categories, options);
 REQUIRE(result.boxes.size() == 4);
 REQUIRE(result.mask_rle_pairs.size() == 3);
 CHECK(result.mask_rle_pairs[0].start == 0);
 CHECK(result.mask_rle_pairs[0].length == extent * extent);
 CHECK(result.boxes[0].mask_rle_pairs == 1);
 CHECK(result.boxes[1].mask_rle_pairs == 0);
 CHECK((result.boxes[1].flags & kAnnotationMask) != 0);
 CHECK(result.mask_rle_pairs[1].start == 0);
 CHECK(result.mask_rle_pairs[1].length == extent * 8000);
 CHECK(result.mask_rle_pairs[2].start == extent * 8000);
 CHECK(result.mask_rle_pairs[2].length == extent * 8000);
 CHECK(result.boxes[2].mask_rle_pairs == 1);
 CHECK(result.boxes[3].mask_rle_pairs == 1);
}
namespace {
NormalizedAnnotationReadView label_projection_fixture(std::uint64_t id, std::uint32_t width = 8) {
 NormalizedAnnotationBuilder builder;
 builder.images.push_back({id, 0, 1, width, 2, 0, 0});
 NormalizedBox box;
 box.x2 = box.y2 = 1;
 box.flags = kAnnotationMask | kAnnotationId | kAnnotationCategory;
 box.annotation_id = id + 100;
 box.source_category_id = 1;
 box.source_ordinal = id * 3;
 box.original_area = 3;
 box.mask_rle_pairs = 3;
 builder.boxes.push_back(box);
 builder.mask_rle_pairs = {{0, 1}, {2, 1}, {4, 1}};
 return NormalizedAnnotationReadView(fixture_index(builder));
}
}  // namespace
TEST_CASE("normalized assembly backings preserve identical used records and retained readers", "[benchmark][annotations][resources]") {
 mmltk::testsupport::ScopedTempDir root("normalized-assembly-custody");
 detail::StorageReservationPool storage(root.path(), {});
 const auto input = label_projection_fixture(7);
 std::array<NormalizedAnnotationIndex, 2> products;
 for (std::size_t backing = 0; backing < products.size(); ++backing) {
  auto metadata = static_cast<const NormalizedAnnotationMetadata&>(input);
  metadata.split = "assembly";
  metadata.annotation_sha256 = std::string(64, 'a');
  NormalizedAnnotationAssembler assembly(metadata, 4, 4, 9, {}, backing ? root.path() : fs::path{}, &storage);
  auto image = input.image(0);
  image.first_box = UINT64_MAX;  // Assembly owns offsets and used counts.
  image.box_count = UINT32_MAX;
  assembly.begin_image(image);
  assembly.append_box(input.storage().boxes.front(), input.storage().mask_rle_pairs);
  image.source_image_id = 8;
  assembly.begin_image(image);
  assembly.append_box(input.storage().boxes.front(), input.storage().mask_rle_pairs);
  image.source_image_id = 9;
  assembly.begin_image(image);  // A valid background remains present.
  nlohmann::json event;
  products[backing] = assembly.finish([&](std::string_view name, const nlohmann::json& fields) {
   CHECK(name == "benchmark.annotations.assembled");
   event = fields;
  });
  CHECK(event.at("images") == 3);
  CHECK(event.at("boxes") == 2);
  CHECK(event.at("mask_rle_pairs") == 6);
  CHECK(event.at("image_capacity").get<std::size_t>() >= 4);
  CHECK(event.at("box_capacity").get<std::size_t>() >= 4);
  CHECK(event.at("mask_rle_capacity").get<std::size_t>() >= 9);
  CHECK_THROWS_AS(assembly.begin_image(image), std::logic_error);
  CHECK_THROWS_AS(assembly.append_box(input.storage().boxes.front(), {}), std::logic_error);
  CHECK_THROWS_AS(assembly.finish(), std::logic_error);
 }
 for (const auto& product : products) {
  REQUIRE(product.images.size() == 3);
  REQUIRE(product.boxes.size() == 2);
  REQUIRE(product.mask_rle_pairs.size() == 6);
  CHECK(product.images[0].first_box == 0);
  CHECK(product.images[1].first_box == 1);
  CHECK(product.images[2].first_box == 2);
  CHECK(product.images[2].box_count == 0);
  CHECK(product.boxes[1].mask_rle_offset == 3);
  CHECK(product.split == "assembly");
  CHECK(product.annotation_sha256 == std::string(64, 'a'));
  REQUIRE_NOTHROW(admit_normalized_annotations(product));
 }
 CHECK(std::memcmp(products[0].images.data(), products[1].images.data(), products[0].images.size_bytes()) == 0);
 CHECK(std::memcmp(products[0].boxes.data(), products[1].boxes.data(), products[0].boxes.size_bytes()) == 0);
 CHECK(std::memcmp(products[0].mask_rle_pairs.data(), products[1].mask_rle_pairs.data(), products[0].mask_rle_pairs.size_bytes()) == 0);
 REQUIRE(std::distance(fs::directory_iterator(root.path()), fs::directory_iterator{}) == 1);
 const auto staged = fs::directory_iterator(root.path())->path();
 {
  mmltk::testsupport::ScopedTempDir publications("normalized-partial-publications");
  struct stat staging_inode{};
  REQUIRE(::stat(staged.c_str(), &staging_inode) == 0);
  std::array<std::filesystem::path, 2> paths{publications.path() / "vector.bin", publications.path() / "mapped.bin"};
  for (std::size_t i = 0; i < products.size(); ++i) {
   bool copied = false;
   const auto completion = store_normalized_annotation_index(paths[i], products[i], {}, [&](std::string_view event, const nlohmann::json& fields) {
    if (event != "benchmark.annotations.cache_prepare") return;
    CHECK_FALSE(fields.at("same_backing").get<bool>());
    CHECK(fields.at("payload_copy_bytes") == 272);
    copied = true;
   });
   CHECK(copied);
   CHECK(completion->size == 528);  // 256 + 3 * 32 + 2 * 64 + 6 * 8, excluding unused capacity.
   struct stat published{};
   REQUIRE(::stat(paths[i].c_str(), &published) == 0);
   CHECK(published.st_ino != staging_inode.st_ino);
   const auto loaded = load_normalized_annotation_index(paths[i], products[i].source, products[i].split, products[i].annotation_sha256, {});
   REQUIRE(loaded);
   CHECK(loaded->images.size() == 3);
   CHECK(loaded->boxes.size() == 2);
   CHECK(loaded->mask_rle_pairs.size() == 6);
   CHECK(std::memcmp(loaded->mask_rle_pairs.data(), products[i].mask_rle_pairs.data(), products[i].mask_rle_pairs.size_bytes()) == 0);
  }
  CHECK(mmltk::common::io::sha256_file(paths[0]) == mmltk::common::io::sha256_file(paths[1]));
  CHECK(fs::exists(staged));
 }
 auto reader = NormalizedAnnotationReadView(products[1]).select_images({1});
 products = {};
 CHECK(fs::exists(staged));
 CHECK(reader.image(0).source_image_id == 8);
 CHECK(reader.storage().mask_rle_pairs[5].start == 4);
 reader = {};
 CHECK_FALSE(fs::exists(staged));
 CHECK(storage.outstanding() == 0);
}
TEST_CASE("normalized assembler admits records and cancels atomically for both backings", "[benchmark][annotations][resources]") {
 const bool mapped = GENERATE(false, true);
 CAPTURE(mapped);
 mmltk::testsupport::ScopedTempDir root("normalized-assembly-admission");
 const auto input = label_projection_fixture(7);
 std::atomic<bool> cancelled{false};
 NormalizedAnnotationAssembler assembly(input, 2, 2, 6, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), mapped ? root.path() : fs::path{});
 auto image = input.image(0);
 auto box = input.storage().boxes.front();
 CHECK_THROWS_AS(assembly.append_box(box, {}), std::logic_error);
 SECTION("image dimensions and reserved bytes") {
  for (unsigned field = 0; field < 3; ++field) {
   auto invalid = image;
   if (field == 0)
    invalid.width = 0;
   else if (field == 1)
    invalid.height = 0;
   else
    invalid.reserved = 1;
   CHECK_THROWS(assembly.begin_image(invalid));
  }
 }
 assembly.begin_image(image);
 SECTION("sorted unique image identity") {
  CHECK_THROWS(assembly.begin_image(image));
  --image.source_image_id;
  CHECK_THROWS(assembly.begin_image(image));
 }
 SECTION("box values and reserved bytes") {
  for (unsigned field = 0; field < 5; ++field) {
   auto invalid = box;
   if (field == 0)
    invalid.class_id = UINT8_MAX;
   else if (field == 1)
    invalid.x2 = invalid.x1;
   else if (field == 2)
    invalid.original_area = -1;
   else if (field == 3)
    invalid.flags = 0;
   else
    invalid.reserved[1] = 1;
   CHECK_THROWS(assembly.append_box(invalid, input.storage().mask_rle_pairs));
  }
 }
 SECTION("late invalid runs leave no appended prefix") {
  for (const auto invalid : {RLEPair{3, 0}, RLEPair{0, 1}, RLEPair{15, 2}}) {
   const std::array<RLEPair, 2> runs{{{1, 1}, invalid}};
   CHECK_THROWS(assembly.append_box(box, runs));
  }
 }
 SECTION("cancellation includes empty mask append and finish") {
  cancelled.store(true);
  CHECK_THROWS(assembly.append_box(box, {}));
  CHECK_THROWS(assembly.append_box(box, input.storage().mask_rle_pairs));
  CHECK_THROWS(assembly.begin_image({8, 0, 0, 8, 2, 0, 0}));
  CHECK_THROWS(assembly.finish());
  cancelled.store(false);
 }
 assembly.append_box(box, input.storage().mask_rle_pairs);
 const auto product = assembly.finish();
 REQUIRE(product.images.size() == 1);
 CHECK(product.images.front().box_count == 1);
 REQUIRE(product.boxes.size() == 1);
 CHECK(product.boxes.front().mask_rle_offset == 0);
 REQUIRE(product.mask_rle_pairs.size() == input.storage().mask_rle_pairs.size());
 CHECK(std::memcmp(product.mask_rle_pairs.data(), input.storage().mask_rle_pairs.data(), product.mask_rle_pairs.size_bytes()) == 0);
}
TEST_CASE("normalized assembly handles empty extents and mapped capacity boundaries", "[benchmark][annotations][resources]") {
 mmltk::testsupport::ScopedTempDir root("normalized-assembly-limits");
 const auto input = label_projection_fixture(7);
 SECTION("empty vector and mapped products") {
  for (const bool mapped : {false, true}) {
   NormalizedAnnotationAssembler assembly({}, 0, 0, 0, {}, mapped ? root.path() : fs::path{});
   const auto product = assembly.finish();
   CHECK(product.images.empty());
   CHECK(product.boxes.empty());
   CHECK(product.mask_rle_pairs.empty());
   REQUIRE_NOTHROW(admit_normalized_annotations(product));
  }
  CHECK(fs::is_empty(root.path()));
 }
 SECTION("zero mapped capacity admits no image") {
  NormalizedAnnotationAssembler assembly({}, 0, 0, 0, {}, root.path());
  CHECK_THROWS(assembly.begin_image(input.image(0)));
  CHECK(assembly.finish().images.empty());
 }
 SECTION("fixed mapped image box and run limits preserve admitted prefixes") {
  NormalizedAnnotationAssembler assembly({}, 1, 1, 3, {}, root.path());
  assembly.begin_image(input.image(0));
  const std::array<RLEPair, 4> too_many{{{0, 1}, {2, 1}, {4, 1}, {6, 1}}};
  CHECK_THROWS_AS(assembly.append_box(input.storage().boxes.front(), too_many), std::overflow_error);
  assembly.append_box(input.storage().boxes.front(), input.storage().mask_rle_pairs);
  CHECK_THROWS_AS(assembly.append_box(input.storage().boxes.front(), {}), std::overflow_error);
  CHECK_THROWS(assembly.begin_image({8, 0, 0, 8, 2, 0, 0}));
  const auto product = assembly.finish();
  CHECK(product.images.size() == 1);
  CHECK(product.boxes.size() == 1);
  CHECK(product.mask_rle_pairs.size() == 3);
 }
 SECTION("Open Images may omit dimensions") {
  for (const bool mapped : {false, true}) {
   NormalizedAnnotationMetadata metadata;
   metadata.source = BenchmarkDatasetSource::kOpenImagesV7;
   NormalizedAnnotationAssembler assembly(metadata, 1, 0, 0, {}, mapped ? root.path() : fs::path{});
   REQUIRE_NOTHROW(assembly.begin_image({7}));
   CHECK(assembly.finish().images.front().width == 0);
  }
 }
 SECTION("mapped extent overflow fails before storage creation") {
  constexpr auto maximum = std::numeric_limits<std::size_t>::max();
  const std::array<std::array<std::size_t, 3>, 6> counts{{{maximum, 0, 0}, {0, maximum, 0}, {0, 0, maximum}, {maximum / 32, 0, 0}, {0, maximum / 64, 0}, {0, 0, maximum / 8}}};
  for (const auto& count : counts) {
   CAPTURE(count);
   CHECK_THROWS_AS(NormalizedAnnotationAssembler({}, count[0], count[1], count[2], {}, root.path()), std::overflow_error);
  }
  CHECK(fs::is_empty(root.path()));
 }
 SECTION("cancelled construction leaves no staging payload") {
  std::atomic<bool> cancelled{true};
  for (const bool mapped : {false, true})
   CHECK_THROWS(NormalizedAnnotationAssembler({}, 1, 1, 3, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), mapped ? root.path() : fs::path{}));
  CHECK(fs::is_empty(root.path()));
 }
}
TEST_CASE("normalized run admission cancellation preserves the preceding box", "[benchmark][annotations][resources]") {
 const bool mapped = GENERATE(false, true);
 mmltk::testsupport::ScopedTempDir root("normalized-run-cancellation");
 struct Cancellation {
  mutable std::size_t observations = 0;
  std::size_t stop = SIZE_MAX;
  bool cancelled() const noexcept { return observations++ >= stop; }
 } cancellation;
 const auto input = label_projection_fixture(7);
 NormalizedAnnotationAssembler assembly({}, 1, 2, 8196, mmltk::common::concurrency::CancellationObservation::Borrow(cancellation), mapped ? root.path() : fs::path{});
 assembly.begin_image({7, 0, 0, 16386, 1, 0, 0});
 assembly.append_box(input.storage().boxes.front(), input.storage().mask_rle_pairs);
 std::vector<RLEPair> runs(8193);
 for (std::uint32_t i = 0; i < runs.size(); ++i) runs[i] = {i * 2, 1};
 cancellation.stop = cancellation.observations + 2;
 CHECK_THROWS(assembly.append_box(input.storage().boxes.front(), runs));
 cancellation.stop = SIZE_MAX;
 const auto product = assembly.finish();
 REQUIRE(product.images.size() == 1);
 CHECK(product.images.front().box_count == 1);
 REQUIRE(product.boxes.size() == 1);
 CHECK(product.boxes.front().mask_rle_offset == 0);
 REQUIRE(product.mask_rle_pairs.size() == 3);
 CHECK(product.mask_rle_pairs.back().start == 4);
}
TEST_CASE("normalized label custody rejects unrelated backing for detached native spans", "[benchmark][pipeline][labels][resources]") {
 mmltk::testsupport::ScopedTempDir root("empty-native-custody");
 CoconutNativeWorkspace workspace(CoconutImportLimits{});
 CoconutRecord record;
 record.image_id = 1;
 record.width = record.height = 1;
 CoconutPhysicalImage physical;
 physical.source = CoconutImageNamespace::CocoTrain;
 physical.image_id = 1;
 auto lineage = std::make_shared<CoconutNativeLineage>();
 lineage->component.source = physical.source;
 workspace.borrow_support(record, {}, {1, 1});
 auto native = workspace.finish(record, physical, {1, 1}, lineage, {});
 CoconutStaging staging(root.path());
 CHECK(CoconutNativeImage::stage(native, staging) == native);
 CHECK(fs::is_empty(root.path()));
 auto read = CoconutNativeImage::read(native);
 const auto ordinary = label_projection_fixture(1);
 auto detached = read.view.storage();
 detached.backing = ordinary.storage().backing;
 CHECK_THROWS_WITH(BenchmarkLabelInput(NormalizedAnnotationReadView(detached), 0), "normalized annotation view does not belong to its backing");
 REQUIRE_NOTHROW(BenchmarkLabelInput(ordinary, 0));
 auto native_input = CoconutNativeImage::labels(native);
 const std::weak_ptr<const CoconutNativeImage> lifetime = native;
 read.owner.reset();
 native.reset();
 workspace.retire();
 CHECK_FALSE(lifetime.expired());
 CHECK(native_input.index().image(0).source_image_id == 1);
 CHECK_FALSE(native_input.index().storage().backing);
}
TEST_CASE("owned label inputs use their annotation canvas and reuse completed dependencies", "[benchmark][pipeline][labels]") {
 BenchmarkCompilePipeline execution(1);
 std::array<unsigned, 3> conversions{};
 execution.label_configuration(16, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, [&](const auto&, auto id, const auto&) { ++conversions.at(id); });
 const std::filesystem::path root("label-geometry-join");
 const auto publication = execution.source_publication(root, {});
 std::weak_ptr<NormalizedAnnotationBacking> pending;
 {
  auto input = label_projection_fixture(1);
  pending = input.storage().backing;
  execution.labels_ready(publication, 1, BenchmarkLabelInput(input, 0), "first", 1);
 }
 const auto first = execution.wait_image_labels(root, 1, "first");
 REQUIRE(first);
 CHECK(pending.expired());
 CHECK_FALSE(execution.geometry(root, 1));
 CHECK(execution.has_image_labels(root, 1, "first", 1));
 CHECK_FALSE(execution.has_image_labels(root, 1, "first", 2));
 auto second = label_projection_fixture(2);
 publication.geometry_ready(2, {8, 2});
 execution.labels_ready(publication, 2, BenchmarkLabelInput(second, 0), "second", 1);
 auto ready = execution.wait_image_labels(root, 2, "second");
 REQUIRE(ready);
 CHECK(conversions[2] == 1);
 CHECK(conversions[1] == 1);
 CHECK(execution.has_image_labels(root, 2, "second", 1));
 // Compiler fallback joins the exact product even after an earlier consumer read it.
 execution.labels_ready(publication, 2, BenchmarkLabelInput(second, 0), "second", 1);
 CHECK(execution.wait_image_labels(root, 2, "second") == ready);
 CHECK(conversions[2] == 1);
 // Distinct annotation editions can share one physical image without replacing
 // each other's canonical product or forcing the final compiler to convert again.
 execution.labels_ready(publication, 2, BenchmarkLabelInput(second, 0), "alternate-edition", 1);
 const auto alternate = execution.wait_image_labels(root, 2, "alternate-edition");
 REQUIRE(alternate);
 CHECK(alternate != ready);
 CHECK(conversions[2] == 2);
 execution.labels_ready(publication, 2, BenchmarkLabelInput(second, 0), "second", 1);
 CHECK(execution.wait_image_labels(root, 2, "second") == ready);
 CHECK(conversions[2] == 2);
 publication.geometry_ready(1, {8, 2});
 CHECK(execution.wait_image_labels(root, 1, "first") == first);
 CHECK(first->labels[0].annotation_id == 101);
 CHECK(first->labels[0].source_ordinal == 3);
 CHECK(pending.expired());
 CHECK(conversions[1] == 1);
}
TEST_CASE("independent labels share workers while one admitted conversion is held", "[benchmark][pipeline][labels]") {
 const auto cpus = mmltk::common::system::allowed_cpu_set();
 if (cpus.size() < 2) SKIP("requires two assigned CPUs");
 BenchmarkCompilePipeline execution(2, cpus);
 mmltk::testsupport::TestGate first("first admitted label conversion");
 std::array<unsigned, 3> conversions{};
 std::array<std::size_t, 3> lanes{};
 execution.label_configuration(16, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, [&](const auto&, auto id, const auto&) {
  ++conversions.at(id);
  lanes.at(id) = execution.execution().current_lane();
  if (id == 1) first.receipt().ArriveAndWait();
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  first.Release();
  execution.retire_attempt();
 });
 const std::filesystem::path root("parallel-labels");
 const auto publication = execution.source_publication(root, {});
 const auto one = label_projection_fixture(1), two = label_projection_fixture(2);
 publication.geometry_ready(1, {8, 2});
 publication.geometry_ready(2, {8, 2});
 execution.labels_ready(publication, 1, BenchmarkLabelInput(one, 0), "one");
 REQUIRE(first.WaitEntered(5s));
 CHECK(execution.has_image_labels(root, 1, "one"));
 execution.labels_ready(publication, 1, BenchmarkLabelInput(one, 0), "one");
 execution.labels_ready(publication, 2, BenchmarkLabelInput(two, 0), "two");
 REQUIRE(execution.wait_image_labels(root, 2, "two"));
 CHECK_FALSE(execution.image_labels(root, 1, "one"));
 CHECK(lanes[1] != lanes[2]);
 first.Release();
 REQUIRE(execution.wait_image_labels(root, 1, "one"));
 CHECK(conversions[1] == 1);
 CHECK(conversions[2] == 1);
}
TEST_CASE("label withdrawal retains executing input and releases queued custody", "[benchmark][pipeline][labels][resources]") {
 enum class Retire { Original, Image, Source, Attempt, Cancellation };
 auto retirement = Retire::Original;
 bool running = false, fail_retired = false;
 SECTION("queued original withdrawal") {}
 SECTION("running original withdrawal") { running = true; }
 SECTION("a failed retired original consumer cannot fail its replacement") { running = fail_retired = true; }
 SECTION("queued physical image withdrawal") { retirement = Retire::Image; }
 SECTION("running physical image withdrawal") {
  retirement = Retire::Image;
  running = true;
 }
 SECTION("queued source withdrawal") { retirement = Retire::Source; }
 SECTION("running source withdrawal") {
  retirement = Retire::Source;
  running = true;
 }
 SECTION("queued attempt withdrawal") { retirement = Retire::Attempt; }
 SECTION("running attempt withdrawal") {
  retirement = Retire::Attempt;
  running = true;
 }
 SECTION("queued cancellation") { retirement = Retire::Cancellation; }
 SECTION("running cancellation") {
  retirement = Retire::Cancellation;
  running = true;
 }
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline execution(1, {}, {}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 mmltk::testsupport::TestGate held("consumer owns its input");
 std::atomic<unsigned> conversions{0};
 execution.label_configuration(16, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, [&](const auto&, auto, const auto&) {
  ++conversions;
  if (running) held.receipt().ArriveAndWait();
  if (fail_retired) throw std::runtime_error("retired conversion failed");
 });
 const std::filesystem::path root("retired-label-custody");
 const auto publication = execution.source_publication(root, {});
 publication.geometry_ready(1, {8, 2});
 std::future<void> blocker, retiring;
 if (!running) {
  blocker = std::async(std::launch::async, [&] { execution.execution().run(CompileTaskKind::Metadata, {}, [&](std::size_t) { held.receipt().ArriveAndWait(); }); });
 }
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  held.Release();
  execution.retire_attempt();
 });
 if (!running) REQUIRE(held.WaitEntered(5s));
 std::weak_ptr<NormalizedAnnotationBacking> custody;
 {
  auto input = label_projection_fixture(1);
  custody = input.storage().backing;
  execution.labels_ready(publication, 1, BenchmarkLabelInput(input, 0), "old", 1);
 }
 if (running) REQUIRE(held.WaitEntered(5s));
 CHECK_FALSE(custody.expired());
 const auto before_withdrawal = execution.execution().admission_generation();
 std::promise<void> withdrawing;
 retiring = std::async(std::launch::async, [&] {
  withdrawing.set_value();
  switch (retirement) {
   case Retire::Original: execution.original_generation(root, 2, true); break;
   case Retire::Image: execution.retire_image(root, 1); break;
   case Retire::Source: execution.retire_source(root); break;
   case Retire::Attempt: execution.retire_attempt(); break;
   case Retire::Cancellation:
    cancelled.store(true);
    execution.retire_attempt();
    break;
  }
 });
 mmltk::testsupport::await_test_promise(withdrawing, "label withdrawal begins");
 if (retirement == Retire::Original || retirement == Retire::Image || retirement == Retire::Source) execution.execution().wait_for_admission_change(before_withdrawal);
 if (!running && retirement != Retire::Attempt && retirement != Retire::Cancellation) {
  mmltk::testsupport::await_test_future(retiring, "queued input retires independently of unrelated CPU work");
  CHECK(custody.expired());
  CHECK(conversions == 0);
 } else {
  CHECK(retiring.wait_for(0ms) == std::future_status::timeout);
  if (running) CHECK_FALSE(custody.expired());
 }
 held.Release();
 if (retiring.valid()) mmltk::testsupport::await_test_future(retiring, "physical consumer settles before withdrawal returns");
 if (blocker.valid()) {
  try {
   mmltk::testsupport::await_test_future(blocker, "unrelated worker settles");
  } catch (const std::exception&) { REQUIRE((retirement == Retire::Cancellation || retirement == Retire::Attempt)); }
 }
 CHECK(custody.expired());
 CHECK_FALSE(execution.image_labels(root, 1, "old"));
 if (retirement == Retire::Original) {
  REQUIRE(execution.geometry(root, 1));
  execution.execution().run(CompileTaskKind::Metadata, {}, [](std::size_t) {});
  fail_retired = false;
  execution.original_generation(root, 2, false);
  CHECK_FALSE(execution.labels_ready(publication, 1, BenchmarkLabelInput(label_projection_fixture(1), 0), "old", 1));
  REQUIRE(execution.labels_ready(publication, 1, BenchmarkLabelInput(label_projection_fixture(1), 0), "old", 2));
  CHECK_FALSE(execution.wait_image_labels(root, 1, "old", 1));
  const auto replacement = execution.wait_image_labels(root, 1, "old", 2);
  REQUIRE(replacement);
  CHECK(replacement->labels[0].annotation_id == 101);
  CHECK(conversions == (running ? 2 : 1));
 }
 if (retirement == Retire::Image || retirement == Retire::Source) CHECK_FALSE(execution.geometry(root, 1));
}
TEST_CASE("stock original fallback keeps its captured generation across withdrawal", "[benchmark][pipeline][labels]") {
 BenchmarkCompilePipeline execution(1);
 unsigned conversions = 0;
 execution.label_configuration(16, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, [&](const auto&, auto, const auto&) { ++conversions; });
 const std::filesystem::path root("stock-original-labels");
 CocoAnnotationSplit captured{false, 7, true, label_projection_fixture(1).storage()};
 execution.original_generation(root, captured.generation, false);
 auto physical = execution.source_publication(root, {});
 physical.geometry_ready(1, {8, 2});
 const std::string dependency("same-original-bytes");
 REQUIRE_FALSE(execution.has_image_labels(root, 1, dependency, captured.generation));
 // Withdrawal after the missing-input query cannot stamp the captured input
 // with the replacement generation, even when the physical ticket is fresh.
 execution.original_generation(root, 8, true);
 physical = execution.source_publication(root, {});
 CHECK_FALSE(execution.labels_ready(physical, 1, BenchmarkLabelInput(NormalizedAnnotationReadView(*captured.index), 0), dependency, captured.generation));
 CHECK_FALSE(execution.wait_image_labels(root, 1, dependency, captured.generation));
 CHECK(conversions == 0);
 CocoAnnotationSplit replacement{false, 8, true, label_projection_fixture(1).storage()};
 execution.original_generation(root, replacement.generation, false);
 REQUIRE(execution.labels_ready(physical, 1, BenchmarkLabelInput(NormalizedAnnotationReadView(*replacement.index), 0), dependency, replacement.generation));
 const auto ready = execution.wait_image_labels(root, 1, dependency, replacement.generation);
 REQUIRE(ready);
 CHECK(conversions == 1);
 CHECK(execution.has_image_labels(root, 1, dependency, replacement.generation));
 CHECK_FALSE(execution.has_image_labels(root, 1, dependency, captured.generation));
 CHECK_FALSE(execution.wait_image_labels(root, 1, dependency, captured.generation));
 CHECK_FALSE(execution.labels_ready(physical, 1, BenchmarkLabelInput(NormalizedAnnotationReadView(*captured.index), 0), dependency, captured.generation));
 CHECK(execution.wait_image_labels(root, 1, dependency, replacement.generation) == ready);
 CHECK(conversions == 1);
}
TEST_CASE("label interval capacity reuses charged lanes and retires borrowed windows", "[benchmark][pipeline][labels][resources]") {
 bool fail_borrower = false, cancel_borrower = false;
 SECTION("successful borrowed window") {}
 SECTION("exception retires borrowed physical capacity") { fail_borrower = true; }
 SECTION("cancellation retires borrowed physical capacity") { cancel_borrower = true; }
 std::atomic<bool> cancelled{false};
 // The fixed label output arenas are part of each consumer's real envelope.
 constexpr std::uint64_t target = 2U * 1024U * 1024U;
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = target}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 std::array<std::size_t, 6> retained{};
 execution.label_configuration(16, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, [&](const auto&, auto id, const BenchmarkLabelWorkspace& workspace) {
  retained.at(id) = workspace.retained_bytes();
  if (id == 4 && cancel_borrower) cancelled.store(true);
  if (id == 4 && fail_borrower) throw std::runtime_error("label borrower failed");
 });
 const std::filesystem::path root("label-workspace");
 const auto publication = execution.source_publication(root, {});
 const auto publish = [&](std::uint64_t id) {
  publication.geometry_ready(id, {8, 2});
  execution.labels_ready(publication, id, BenchmarkLabelInput(label_projection_fixture(id), 0), std::to_string(id));
 };
 const auto join = [&](std::uint64_t id) { return execution.wait_image_labels(root, id, std::to_string(id)); };
 publish(1);
 REQUIRE(join(1));
 publish(2);
 REQUIRE(join(2));
 CHECK(retained[1] > 0);
 CHECK(retained[2] > 0);
 auto pressure = execution.execution().reserve({target, 0});  // Logical borrowers can fill the bank without retiring physical backing.
 publish(3);
 CHECK_FALSE(execution.image_labels(root, 3, "3"));
 pressure = {};
 REQUIRE(join(3));
 CHECK(retained[3] > 0);
 auto producer = execution.execution().reserve({target, 0});
 const auto borrowed = [&] {
  execution.execution().with_unused_workspace(producer, 16384, [&] {
   publish(4);
   if (!join(4)) throw std::runtime_error("label borrower lost its required product");
  });
 };
 if (fail_borrower || cancel_borrower) {
  CHECK_THROWS(borrowed());
  cancelled.store(false);
  execution.retire_attempt();
 } else
  borrowed();
 // The lending window returned all borrowers while its producer remains charged.
 REQUIRE(execution.execution().try_resize_workspace(producer, 0, false));
 producer = {};
 if (!fail_borrower && !cancel_borrower) {
  publish(5);
  REQUIRE(join(5));
  CHECK(retained[5] > 0);
 }
}
TEST_CASE("reused label output preserves immutable results and bounds complete mask extents", "[benchmark][labels][masks][resources]") {
 const auto input = label_projection_fixture(7);
 BenchmarkLabelWorkspace workspace;
 std::size_t growths = 0;
 workspace.observe_output_growth({&growths, [](void* context, bool, std::size_t, std::size_t) noexcept { ++*static_cast<std::size_t*>(context); }});
 const auto mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
 const auto reference = compile_benchmark_image_labels(input, 0, {8, 2}, 16, mode, workspace);
 const auto start = std::chrono::steady_clock::now();
 const auto& first = compile_benchmark_image_labels_reused(input, 0, {8, 2}, 16, mode, workspace);
 const auto first_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
 REQUIRE(first.labels.size() == reference.labels.size());
 REQUIRE(first.runs.size() == reference.runs.size());
 REQUIRE(growths == 2);
 const auto prepared_growths = growths;
 const auto retained = workspace.retained_bytes();
 const auto active = annotation_layout(first.labels.size(), first.runs.size()).used_bytes;
 for (unsigned repetition = 0; repetition < 16; ++repetition) {
  const auto& reused = compile_benchmark_image_labels_reused(input, 0, {8, 2}, 16, mode, workspace);
  CHECK(reused.width == reference.width);
  CHECK(reused.height == reference.height);
  CHECK(reused.dropped == reference.dropped);
  REQUIRE(reused.labels.size() == reference.labels.size());
  REQUIRE(reused.runs.size() == reference.runs.size());
  CHECK(std::memcmp(reused.labels.data(), reference.labels.data(), reused.labels.size() * sizeof(PackedInstance)) == 0);
  CHECK(std::memcmp(reused.runs.data(), reference.runs.data(), reused.runs.size() * sizeof(RLEPair)) == 0);
 }
 CHECK(growths == prepared_growths);
 CHECK(workspace.retained_bytes() == retained);
 CHECK(retained == mmltk::common::system::page_rounded_bytes(BenchmarkLabelWorkspace::required_bytes(8)));
 std::printf(
  "benchmark_label_fixture first_projection_ns=%lld active_label_bytes=%zu retained_workspace_bytes=%zu output_vector_growths=%zu steady_output_vector_growths=%zu scope=two_output_vectors_only "
  "mapped_backing=excluded production_baseline=unavailable\n",
  static_cast<long long>(first_ns), active, retained, prepared_growths, growths - prepared_growths);
 NormalizedAnnotationBuilder oversized;
 oversized.images.push_back({99, 0, 2, 512, 512, 0, 0});
 NormalizedBox box;
 box.x2 = box.y2 = 1;
 box.flags = kAnnotationMask | kAnnotationCategory;
 box.source_category_id = 1;
 box.mask_rle_pairs = 70000;
 oversized.boxes.push_back(box);
 for (std::uint32_t run = 0; run < 70000; ++run) oversized.mask_rle_pairs.push_back({run * 2, 1});
 box.mask_rle_offset = oversized.mask_rle_pairs.size();
 box.mask_rle_pairs = 3;
 oversized.boxes.push_back(box);
 for (std::uint32_t run = 0; run < 3; ++run) oversized.mask_rle_pairs.push_back({run * 2, 1});
 const NormalizedAnnotationReadView large(fixture_index(oversized));
 try {
  (void)compile_benchmark_image_labels_reused(large, 0, {512, 512}, 512, mode, workspace);
  FAIL("oversized complete mask must fail compilation");
 } catch (const std::runtime_error& error) {
  const std::string reason(error.what());
  CHECK(reason.find("image 99") != std::string::npos);
  CHECK(reason.find("required 560144 bytes") != std::string::npos);
  CHECK(reason.find("capacity 500000") != std::string::npos);
 }
 CHECK(growths == prepared_growths);
 CHECK(workspace.retained_bytes() == mmltk::common::system::page_rounded_bytes(BenchmarkLabelWorkspace::required_bytes(512)));
 const auto& restored = compile_benchmark_image_labels_reused(input, 0, {8, 2}, 16, mode, workspace);
 REQUIRE(restored.runs.size() == reference.runs.size());
 CHECK(std::memcmp(restored.runs.data(), reference.runs.data(), restored.runs.size() * sizeof(RLEPair)) == 0);
 const auto locked = workspace.retained_bytes();
 workspace.clear();
 CHECK(workspace.retained_bytes() == locked);
 (void)compile_benchmark_image_labels_reused(input, 0, {8, 2}, 16, mode, workspace);
 CHECK(growths == prepared_growths);
}
TEST_CASE("label projection preserves geometry provenance and present-empty masks", "[benchmark][labels][masks]") {
 auto input = label_projection_fixture(7);
 BenchmarkLabelWorkspace workspace;
 const auto mismatch = compile_benchmark_image_labels(input, 0, {9, 2}, 16, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, workspace);
 CHECK(mismatch.labels.empty());
 CHECK(mismatch.dropped == 1);
 CHECK(mismatch.width == 9);
 NormalizedAnnotationBuilder builder;
 builder.source = BenchmarkDatasetSource::kOpenImagesV7;
 builder.images.push_back({7, 0, 1, 1, 1, 0, 0});
 NormalizedBox box;
 box.x2 = box.y2 = 1;
 box.flags = kAnnotationMask | kAnnotationId | kAnnotationCategory | kAnnotationCrowd | kAnnotationIgnore;
 box.annotation_id = 43;
 box.source_category_id = 1;
 box.source_ordinal = 92;
 box.original_area = 0.25;
 builder.boxes.push_back(box);
 const auto normalized = NormalizedAnnotationReadView(fixture_index(builder));
 const auto projected = compile_benchmark_image_labels(normalized, 0, {8, 2}, 16, mmltk::backend::imaging::resample::ImageResizeMode::Letterbox, workspace);
 REQUIRE(projected.labels.size() == 1);
 CHECK(projected.runs.empty());
 CHECK(projected.dropped == 0);
 const auto& label = projected.labels.front();
 CHECK(label.flags == box.flags);
 CHECK(label.mask_rle_pairs == 0);
 CHECK(label.original_area == 4);
 CHECK(label.annotation_id == 43);
 CHECK(label.source_category_id == 1);
 CHECK(label.source_ordinal == 92);
 CHECK(label.bbox_y1 == 6.0F);
 CHECK(label.bbox_y2 == 10.0F);
}
TEST_CASE("required label joins surface failed geometry readers without a pixel drain", "[benchmark][pipeline][labels][failure]") {
 mmltk::testsupport::ScopedTempDir root("label-geometry-failure");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 8, 2, 0, 0}};
 fs::remove(cached_image_path(images, 1));
 BenchmarkCompilePipeline execution(1);
 execution.label_configuration(16, mmltk::backend::imaging::resample::ImageResizeMode::Stretch);
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 16);
 request.execution = &execution;
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 const auto publication = execution.source_publication(images, {});
 execution.labels_ready(publication, 1, BenchmarkLabelInput(label_projection_fixture(1), 0), "required");
 publication({1});
 CHECK_THROWS_AS(execution.wait_image_labels(images, 1, "required"), BenchmarkImageReadError);
 CHECK_FALSE(execution.geometry(images, 1));
 // Native annotation geometry permits independent conversion; the failed
 // physical join still prevents this completed label from becoming eligible.
 CHECK(execution.image_labels(images, 1, "required"));
 CHECK_FALSE(writer.image_complete(0));
}
TEST_CASE("failed artifact digest custody follows the retained download generation", "[backend][data][benchmark][download][trace]") {
 mmltk::testsupport::ScopedTempDir root("download-digest-custody");
 const auto path = root.path() / "artifact.bin";
 const auto first_bytes = make_payload(4096);
 const auto second_bytes = make_payload(8192);
 mmltk::testsupport::write_binary_file(path, first_bytes);
 DownloadResult artifact{.path = path};
 const auto expected = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(first_bytes));
 REQUIRE(download_failure_sha256(artifact) == expected);
 REQUIRE(artifact.failure_digest);
 CHECK_FALSE(artifact.failure_digest->error);
 std::filesystem::rename(path, root.path() / "retired.bin");
 // No path remains to reopen; the owner and a retained copy reuse that success.
 CHECK(download_failure_sha256(artifact) == expected);
 auto retained = artifact;
 CHECK(download_failure_sha256(retained) == expected);
 mmltk::testsupport::write_binary_file(path, second_bytes);
 artifact = DownloadResult{.path = path};
 CHECK_FALSE(artifact.failure_digest);
 CHECK(download_failure_sha256(artifact) == mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(second_bytes)));
 CHECK(download_failure_sha256(retained) == expected);
 DownloadResult failed{.path = root.path() / "missing.bin"};
 CHECK_THROWS(download_failure_sha256(failed));
 REQUIRE(failed.failure_digest);
 const auto original_failure = failed.failure_digest->error;
 REQUIRE(original_failure);
 mmltk::testsupport::write_binary_file(failed.path, first_bytes);
 // Optional callers may ignore this error, but the next required use must
 // retain the same cause without another diagnostic read of this generation.
 CHECK_THROWS(download_failure_sha256(failed));
 CHECK(failed.failure_digest->error == original_failure);
 failed = DownloadResult{.path = failed.path};
 CHECK(download_failure_sha256(failed) == expected);
 std::atomic<bool> cancelled{true};
 failed = DownloadResult{.path = path};
 CHECK_THROWS_WITH(download_failure_sha256(failed, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled)), "file digest cancelled");
 cancelled.store(false);
 CHECK_THROWS_WITH(download_failure_sha256(failed), "file digest cancelled");
 // Exception custody also retains the allocation-failure type unchanged.
 failed.failure_digest->error = std::make_exception_ptr(std::bad_alloc{});
 CHECK_THROWS_AS(download_failure_sha256(failed), std::bad_alloc);
}
TEST_CASE("annotation repair reads failed payloads only for enabled diagnostics", "[backend][data][benchmark][download][trace]") {
 mmltk::testsupport::ScopedTempDir root("annotation-diagnostic-reads");
 const auto payload = make_payload(4096);
 HttpServer server(payload);
 const auto request = request_for(root.path(), "metadata", server.url("metadata"), payload);
 bool enabled = false;
 SECTION("disabled diagnostics never open the failed inode") {}
 SECTION("enabled diagnostics hash each replacement generation") { enabled = true; }
 std::vector<std::string> digests;
 const BenchmarkTraceSink trace = enabled ? BenchmarkTraceSink{[&](std::string_view event, const nlohmann::json& fields) {
  if (event == "benchmark.download.failure_sha256") digests.push_back(fields.at("sha256").get<std::string>());
 }}
                                          : BenchmarkTraceSink{};
 ProgressReporter reporter({}, trace);
 for (unsigned generation = 0; generation < 2; ++generation) {
  const std::string failed(8192, static_cast<char>('a' + generation));
  write_text(request.destination, failed);
  const mmltk::common::io::ScopedFd events(::inotify_init1(IN_CLOEXEC | IN_NONBLOCK));
  REQUIRE(events.get() >= 0);
  REQUIRE(::inotify_add_watch(events.get(), request.destination.c_str(), IN_OPEN | IN_ACCESS) >= 0);
  const auto repaired = repair_annotation_artifacts({request}, BenchmarkDatasetSource::kCoco2017, "malformed input", reporter, reporter.transfers(), 1, {}, trace);
  REQUIRE(repaired.size() == 1);
  std::array<char, 4096> buffer{};
  bool opened = false, accessed = false;
  for (;;) {
   const auto bytes = ::read(events.get(), buffer.data(), buffer.size());
   if (bytes < 0) {
    REQUIRE(errno == EAGAIN);
    break;
   }
   REQUIRE(bytes > 0);
   for (std::size_t offset = 0; offset < static_cast<std::size_t>(bytes);) {
    inotify_event event{};
    std::memcpy(&event, buffer.data() + offset, sizeof(event));
    opened = opened || (event.mask & IN_OPEN);
    accessed = accessed || (event.mask & IN_ACCESS);
    offset += sizeof(event) + event.len;
   }
  }
  CHECK(opened == enabled);
  CHECK(accessed == enabled);
  if (enabled) {
   REQUIRE(digests.size() == generation + 1);
   CHECK(digests.back() == mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(failed.data()), failed.size()))));
  }
 }
 CHECK(digests.size() == (enabled ? 2 : 0));
 server.Check();
}
TEST_CASE("split sealing admits caller metadata and preserves publication on cancellation", "[backend][data][benchmark][writer]") {
 mmltk::testsupport::ScopedTempDir root("split-seal-publication");
 auto split = cached_pixel_membership(root.path() / "images");
 split.images = {{1, 16, 8, 1, 0}};
 PackedInstance label{};
 label.bbox_x2 = label.bbox_y2 = 1;
 auto annotations = std::make_shared<BenchmarkLabelChunk>();
 annotations->labels = {label};
 const auto output = root.path() / "result.bin";
 write_text(output, "prior publication");
 const auto prior = mmltk::common::io::sha256_file(output);
 std::atomic<bool> cancelled{false};
 auto request = benchmark_write_request(split, output, 8, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 request.overwrite = true;
 {
  BenchmarkSplitWriter writer(request);
  writer.write_remaining(request);
  SECTION("caller metadata receives full admission") {
   SECTION("flags") { annotations->labels[0].flags = 128; }
   SECTION("class") { annotations->labels[0].class_id = 1; }
   SECTION("source identity") { split.images[0].source_width = 0; }
   SECTION("unreferenced runs") { annotations->runs = {{0, 1}}; }
   SECTION("run bounds") {
    annotations->labels[0].flags = kAnnotationMask;
    annotations->labels[0].mask_rle_pairs = 1;
    annotations->runs = {{63, 2}};
   }
   CHECK_THROWS([&] {
    writer.write_labels(0, benchmark_entry(split.images[0]), {annotations->labels, annotations->runs});
    (void)writer.seal(request);
   }());
  }
  SECTION("cancel before sealing") {
   cancelled.store(true);
   CHECK_THROWS([&] {
    writer.write_labels(0, benchmark_entry(split.images[0]), {annotations->labels, annotations->runs});
    (void)writer.seal(request);
   }());
  }
  SECTION("cancel during compaction preserves the published generation") {
   writer.write_labels(0, benchmark_entry(split.images[0]), {annotations->labels, annotations->runs});
   request.compaction = [&](std::size_t done, std::size_t) {
    if (done) cancelled.store(true);
   };
   CHECK_THROWS(writer.finish(request));
  }
  SECTION("cancel after sync before publishing") {
   annotations->labels[0].flags = kAnnotationMask;  // Untrusted present-empty mask is valid.
   writer.write_labels(0, benchmark_entry(split.images[0]), {annotations->labels, annotations->runs});
   auto sealed = writer.seal(request);
   CHECK(sealed.info.image_count == 1);
   CHECK(std::ranges::equal(sealed.info.class_names(), split.class_names));
   CHECK(mmltk::common::io::sha256_file(output) == prior);
   cancelled.store(true);
   CHECK_THROWS(sealed.artifact.publish(output, request.cancel_requested));
  }
 }
 check_preserved_split(output, prior);
}
TEST_CASE("independent slots settle payloads and release chunks across source families", "[backend][data][benchmark][writer]") {
 mmltk::testsupport::ScopedTempDir root("split-final-placement");
 std::vector<std::uint8_t> baseline;
 for (const auto workers : {1U, 3U}) {
  BenchmarkCompilePipeline execution(workers);
  PreparedBenchmarkSplit final;
  final.name = "train";
  final.class_names = {"person"};
  auto request = benchmark_write_request(final, root.path() / ("placed-" + std::to_string(workers) + ".bin"), 8);
  request.execution = &execution;
  BenchmarkSplitWriter writer(request);
  BenchmarkCompilePipeline::Attempt attempt(execution);
  std::array<std::weak_ptr<const BenchmarkLabelChunk>, 3> custody;
  for (std::size_t source = 0; source < custody.size(); ++source) {
   const auto images = root.path() / ("images-" + std::to_string(source));
   (void)cached_pixel_membership(images);
   final.sources.push_back({images});
   auto chunk = std::make_shared<BenchmarkLabelChunk>();
   chunk->width = 16;
   chunk->height = 8;
   if (source != 1) {
    PackedInstance label{};
    label.bbox_x2 = label.bbox_y2 = 4;
    label.flags = kAnnotationCategory | kAnnotationMask | kAnnotationId;
    label.source_category_id = 1;
    label.annotation_id = 10 + source;
    label.source_ordinal = 3 + source;
    chunk->runs = source == 0 ? std::vector<RLEPair>{{0, 1}, {4, 2}} : std::vector<RLEPair>{{2, 1}};
    label.mask_rle_pairs = static_cast<std::uint16_t>(chunk->runs.size());
    chunk->labels.push_back(label);
    if (source == 0) {
     label.mask_rle_pairs = 0;
     label.mask_rle_offset = chunk->runs.size() * sizeof(RLEPair);
     label.source_ordinal = 7;
     chunk->labels.push_back(label);  // Present-empty mask after nonempty runs.
    }
   }
   custody[source] = chunk;
   const EncodedImageRecord record{1, 16, 8, static_cast<std::uint16_t>(chunk->labels.size()), static_cast<std::uint16_t>(source), AnnotationSource::Coco};
   const auto slot = writer.allocate_slot(record, {images});
   execution.bind_slot(images, 1, writer, slot);
   final.images.push_back(record);
   writer.write_labels(slot, benchmark_entry(record), {chunk->labels, chunk->runs}, source == 0 ? 100 : 200);
   chunk.reset();
   CHECK(custody[source].expired());
  }
  writer.write_remaining(request);
  auto sealed = writer.seal(request);
  sealed.artifact.publish(request.output_path, {});
  const auto dataset = CompiledDataset::open(request.output_path);
  CHECK(dataset.header().max_instances_per_image == 2);
  CHECK(dataset.image_labels(0)[0].source_ordinal == 103);
  CHECK(dataset.image_labels(0)[1].source_ordinal == 107);
  CHECK(dataset.image_labels(2)[0].source_ordinal == 205);
  CHECK(dataset.image_labels(1).empty());
  CHECK(dataset.instance_rle(0, dataset.image_labels(0)[1]).empty());
  auto bytes = mmltk::testsupport::read_binary_file(request.output_path);
  if (baseline.empty())
   baseline = std::move(bytes);
  else
   CHECK(bytes == baseline);
 }
}
TEST_CASE("direct slot publication rejects malformed records and cannot seal partial output", "[backend][data][benchmark][writer]") {
 mmltk::testsupport::ScopedTempDir root("split-placement-admission");
 const auto membership = cached_pixel_membership(root.path() / "images");
 BenchmarkCompilePipeline execution(1);
 auto split = membership;
 split.images = {{1, 16, 8, 1, 0}};
 auto chunk = std::make_shared<BenchmarkLabelChunk>();
 chunk->width = 16;
 chunk->height = 8;
 PackedInstance label{};
 label.bbox_x2 = label.bbox_y2 = 4;
 label.flags = kAnnotationMask | kAnnotationCategory;
 label.source_category_id = 1;
 label.mask_rle_pairs = 2;
 chunk->labels = {label};
 chunk->runs = {{0, 1}, {4, 2}};
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 SECTION("class identity") { chunk->labels[0].class_id = 1; }
 SECTION("source provenance") {
  chunk->labels[0].flags &= ~kAnnotationCategory;
  chunk->labels[0].source_category_id = 0;
 }
 SECTION("unknown flags") { chunk->labels[0].flags |= 128; }
 SECTION("mask offset") { chunk->labels[0].mask_rle_offset = sizeof(RLEPair); }
 SECTION("run extent") { chunk->runs[1] = {63, 2}; }
 SECTION("run ordering") { chunk->runs[1].start = 0; }
 SECTION("unreferenced run") { chunk->labels[0].mask_rle_pairs = 1; }
 SECTION("source ordinal overflow") { chunk->labels[0].source_ordinal = UINT64_MAX; }
 bool cancel_placement = false;
 SECTION("cancelled placement") { cancel_placement = true; }
 split.images[0].annotation_source = AnnotationSource::Coco;
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8, cancellation);
 request.execution = &execution;
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 writer.write_remaining(request);
 cancelled.store(cancel_placement);
 CHECK_THROWS(writer.write_labels(0, benchmark_entry(split.images[0]), {chunk->labels, chunk->runs}, 1));
 CHECK_THROWS(writer.seal(request));
 CHECK_FALSE(fs::exists(request.output_path));
}
TEST_CASE("image retirement keeps admission closed through writer invalidation", "[benchmark][pipeline][labels][writer]") {
 mmltk::testsupport::ScopedTempDir temporary("slot-invalidation-custody");
 const auto images = temporary.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images.clear();
 BenchmarkCompilePipeline execution(1);
 execution.label_configuration(8, mmltk::backend::imaging::resample::ImageResizeMode::Stretch);
 mmltk::testsupport::TestGate invalidating("old pixel invalidation");
 auto request = benchmark_write_request(split, temporary.path() / "train.bin", 8);
 request.execution = &execution;
 request.progress = {.context = &invalidating, .images_invalidated = [](void* context, std::uint64_t) { static_cast<mmltk::testsupport::TestGate*>(context)->receipt().ArriveAndWait(); }};
 BenchmarkSplitWriter writer(request, true);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 execution.bind_source(images, writer, AnnotationSource::Coco);
 const auto original = execution.source_publication(images, {});
 original({1});
 execution.wait_image(images, 1);
 std::atomic<unsigned> completed{0};
 REQUIRE(original.subscribe(1, [&](std::exception_ptr error) {
  if (!error) ++completed;
 }));
 CHECK(completed == 1);
 const auto slot = execution.image_slot(images, 1);
 REQUIRE(slot);
 auto retiring = std::async(std::launch::async, [&] { execution.retire_image(images, 1); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { invalidating.Release(); });
 REQUIRE(invalidating.WaitEntered(5s));
 CHECK_FALSE(original.subscribe(1, [](std::exception_ptr) {}));
 const auto replacement = execution.source_publication(images, {}, 1);
 const auto input = label_projection_fixture(1);
 replacement.geometry_ready(1, {8, 2});
 CHECK_FALSE(execution.geometry(images, 1));
 CHECK_FALSE(execution.labels_ready(replacement, 1, BenchmarkLabelInput(input, 0), "replacement"));
 CHECK_FALSE(writer.image_complete(*slot));
 invalidating.Release();
 mmltk::testsupport::await_test_future(retiring, "writer invalidation settles before replacement admission");
 REQUIRE(execution.labels_ready(replacement, 1, BenchmarkLabelInput(input, 0), "replacement"));
 REQUIRE(execution.wait_image_labels(images, 1, "replacement"));
 replacement({1});
 execution.wait_image(images, 1);
 CHECK(writer.image_complete(*slot));
 CHECK(writer.entry(*slot).num_instances == 1);
 CHECK(execution.image_slot(images, 1) == slot);
 REQUIRE(replacement.subscribe(1, [&](std::exception_ptr error) {
  if (!error) ++completed;
 }));
 CHECK(completed == 2);
 CHECK_FALSE(original.subscribe(1, [](std::exception_ptr) {}));
}
TEST_CASE("dependency withdrawal retains the active label through overlapping generation retirement", "[benchmark][pipeline][labels][resources]") {
 enum class Retire { Image, Source, Original };
 const auto retirement = GENERATE(Retire::Image, Retire::Source, Retire::Original);
 mmltk::testsupport::ScopedTempDir temporary("overlapping-label-retirement");
 BenchmarkCompilePipeline execution(1);
 LiveBenchmarkWriter fixture(execution, temporary.path(), AnnotationSource::Coco);
 mmltk::testsupport::TestGate held("retiring dependency borrower");
 execution.label_configuration(fixture.output.resolution, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, [&](const auto&, auto, const auto&) { held.receipt().ArriveAndWait(); });
 const auto root = temporary.path() / "images";
 const auto source = execution.source_publication(root, {});
 source.geometry_ready(1, {8, 2});
 const auto input = label_projection_fixture(1);
 std::future<void> dependency, generation;
 const mmltk::testsupport::ScopedTestCleanup release([&] { held.Release(); });
 REQUIRE(execution.labels_ready(source, 1, BenchmarkLabelInput(input, 0), "same-dependency", 1));
 REQUIRE(held.WaitEntered(5s));
 std::atomic<unsigned> settled{0}, failed{0};
 REQUIRE(execution.subscribe_labels(source, 1, "same-dependency", 1, [&](std::exception_ptr error) {
  ++settled;
  if (error) ++failed;
 }));
 auto before = execution.execution().admission_generation();
 dependency = std::async(std::launch::async, [&] { execution.retire_labels(root, 1, "same-dependency"); });
 execution.execution().wait_for_admission_change(before);
 CHECK_FALSE(execution.labels_ready(source, 1, BenchmarkLabelInput(input, 0), "same-dependency", 1));
 before = execution.execution().admission_generation();
 generation = std::async(std::launch::async, [&] {
  switch (retirement) {
   case Retire::Image: execution.retire_image(root, 1); break;
   case Retire::Source: execution.retire_source(root); break;
   case Retire::Original: execution.original_generation(root, 2, true); break;
  }
 });
 execution.execution().wait_for_admission_change(before);
 CHECK(dependency.wait_for(0ms) == std::future_status::timeout);
 CHECK(generation.wait_for(0ms) == std::future_status::timeout);
 held.Release();
 mmltk::testsupport::await_test_future(dependency, "exact dependency retires its active borrower");
 mmltk::testsupport::await_test_future(generation, "generation waits for the same borrower");
 CHECK(settled == 1);
 CHECK(failed == 1);
 CHECK_FALSE(execution.subscribe_labels(source, 1, "same-dependency", 1, [](std::exception_ptr) {}));
 const auto replacement = execution.source_publication(root, {}, 1);
 replacement.geometry_ready(1, {8, 2});
 const auto original_generation = retirement == Retire::Original ? 2U : 1U;
 REQUIRE(execution.labels_ready(replacement, 1, BenchmarkLabelInput(input, 0), "same-dependency", original_generation));
 REQUIRE(execution.wait_image_labels(root, 1, "same-dependency", original_generation));
 REQUIRE(execution.subscribe_labels(replacement, 1, "same-dependency", original_generation, [&](std::exception_ptr error) {
  ++settled;
  if (error) ++failed;
 }));
 CHECK(settled == 2);
 CHECK(failed == 1);
 if (retirement == Retire::Original) CHECK_FALSE(execution.subscribe_labels(replacement, 1, "same-dependency", 1, [](std::exception_ptr) {}));
}
TEST_CASE("label output settles an interrupted projection without another file admission", "[benchmark][pipeline][labels][writer][resources]") {
 enum class Rejection { Execution, OpenFileProducer, FileAdmission };
 const auto rejection = GENERATE(Rejection::Execution, Rejection::OpenFileProducer, Rejection::FileAdmission);
 const bool reject_file = rejection == Rejection::FileAdmission;
 const bool held_peer = rejection == Rejection::OpenFileProducer;
 if (held_peer && mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 mmltk::testsupport::ScopedTempDir temporary("label-output-rejection");
 const auto output = temporary.path() / "train.bin";
 write_text(output, "previous published generation");
 const auto previous = mmltk::common::io::sha256_file(output);
 BenchmarkCompilePipeline pipeline(held_peer ? 2 : 1);
 LiveBenchmarkWriter fixture(pipeline, temporary.path(), AnnotationSource::Coco);
 const auto retained_controls = pipeline.execution().resource_usage().bytes;
 const auto root = temporary.path() / "images";
 const auto original = std::make_exception_ptr(std::runtime_error("execution failed during label projection"));
 mmltk::testsupport::TestGate projection("projection owns its input before file admission");
 mmltk::testsupport::TestGate physical("already accepted file peer retains its call");
 CompileExecution::Completion peer;
 std::atomic<unsigned> settled{0}, written{0};
 std::promise<std::exception_ptr> completion;
 std::weak_ptr<NormalizedAnnotationBacking> input_custody;
 std::future<void> draining;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  projection.Release();
  physical.Release();
  pipeline.retire_attempt();
  pipeline.files().join();
 });
 if (held_peer) {
  peer = pipeline.files().submit({.work = [&] { physical.receipt().ArriveAndWait(); }});
  REQUIRE(physical.WaitEntered(5s));
 }
 pipeline.label_configuration(8, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, [&](const auto&, auto, const auto&) {
  projection.receipt().ArriveAndWait();
  if (reject_file)
   pipeline.files().cancel();
  else
   pipeline.execution().fail(original);
 }, [&](const auto&, auto) { ++written; });
 auto publication = pipeline.source_publication(root, {});
 {
  auto input = label_projection_fixture(1);
  input_custody = input.storage().backing;
  REQUIRE(pipeline.labels_ready(publication, 1, BenchmarkLabelInput(input, 0), "required", 1));
 }
 REQUIRE(projection.WaitEntered(5s));
 REQUIRE(pipeline.subscribe_labels(publication, 1, "required", 1, [&](std::exception_ptr error) {
  ++settled;
  completion.set_value(error);
 }));
 CHECK_FALSE(input_custody.expired());
 CHECK(pipeline.execution().resource_usage().bytes > 0);
 CHECK(pipeline.execution().snapshot().live[static_cast<std::size_t>(CompileWorkClass::Output)] == held_peer);
 projection.Release();
 const auto error = mmltk::testsupport::await_test_promise(completion, "interrupted label settles its subscription");
 REQUIRE(error);
 const auto expected = reject_file ? "file queue admission after stop" : "execution failed during label projection";
 CHECK_THROWS_WITH(std::rethrow_exception(error), expected);
 if (!reject_file) CHECK(error == original);
 draining = std::async(std::launch::async, [&] { pipeline.drain(); });
 CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(draining, "failed pipeline drains unaccepted label output"), expected);
 CHECK(settled == 1);
 CHECK(written == 0);
 CHECK(input_custody.expired());
 CHECK(pipeline.execution().resource_usage().bytes == retained_controls);
 CHECK(mmltk::common::io::sha256_file(output) == previous);
 if (held_peer) CHECK_FALSE(peer.ready());
 physical.Release();
 peer.wait();
 pipeline.files().join();
 publication = {};
 pipeline.retire_attempt();
 CHECK_FALSE(pipeline.execution().interruption());
 CHECK(pipeline.execution().snapshot().live[static_cast<std::size_t>(CompileWorkClass::Output)] == 0);
 if (!reject_file) {
  pipeline.bind_source(root, fixture.writer, AnnotationSource::Coco);
  pipeline.label_configuration(8, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, {}, [&](const auto&, auto) { ++written; });
  const auto replacement = pipeline.source_publication(root, {});
  REQUIRE(pipeline.labels_ready(replacement, 2, BenchmarkLabelInput(label_projection_fixture(2), 0), "replacement", 1));
  REQUIRE(pipeline.wait_image_labels(root, 2, "replacement"));
  pipeline.drain();
  CHECK(written == 1);
  CHECK(settled == 1);
  const auto slot = pipeline.image_slot(root, 2);
  REQUIRE(slot);
  CHECK(fixture.writer.entry(*slot).num_instances == 1);
  CHECK(mmltk::common::io::sha256_file(output) == previous);
 }
}
TEST_CASE("deferred label outputs retire independently of a full physical file queue", "[benchmark][pipeline][labels][writer][resources]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires three assigned CPUs");
 enum class Outcome { Write, Dependency, Source, Cancellation, Failure };
 const auto outcome = GENERATE(Outcome::Write, Outcome::Dependency, Outcome::Source, Outcome::Cancellation, Outcome::Failure);
 mmltk::testsupport::ScopedTempDir temporary("full-label-output-queue");
 std::atomic<bool> cancelled{false};
 BenchmarkCompilePipeline pipeline(3, {}, {}, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled));
 LiveBenchmarkWriter fixture(pipeline, temporary.path(), AnnotationSource::Coco);
 const auto retained_controls = pipeline.execution().resource_usage().bytes;
 const auto root = temporary.path() / "images";
 mmltk::testsupport::TestGate physical("accepted file peers retain physical custody");
 std::array<CompileExecution::Completion, 6> files;
 std::array<CompileExecution::Completion, 2> labels;
 std::array<std::weak_ptr<NormalizedAnnotationBacking>, 2> input_custody;
 std::array<std::promise<std::exception_ptr>, 2> completions;
 std::array<std::atomic<unsigned>, 2> settled{};
 std::atomic<unsigned> physical_returns{0}, written{0};
 std::future<void> projecting, retiring;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  physical.Release();
  pipeline.retire_attempt();
  pipeline.files().join();
 });
 pipeline.label_configuration(8, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, {}, [&](const auto&, auto) { ++written; });
 // Both service lanes enter before source producers open their live classes.
 // The remaining CPU can finish projections while all prepared file records
 // stay occupied; no elapsed-time inference establishes the output handoff.
 for (auto& file : files)
  file = pipeline.files().submit({.work = [&] {
   physical.receipt().ArriveAndWait();
   ++physical_returns;
  }});
 REQUIRE(physical.WaitEntered(5s, 2));
 CHECK_FALSE(pipeline.files().try_submit({.work = [] {}}));
 auto publication = pipeline.source_publication(root, {});
 for (std::size_t index = 0; index < labels.size(); ++index) {
  auto input = label_projection_fixture(index + 1);
  input_custody[index] = input.storage().backing;
  REQUIRE(pipeline.labels_ready(publication, index + 1, BenchmarkLabelInput(input, 0), "required", 1));
  labels[index] = pipeline.label_completion(root, index + 1, "required");
  REQUIRE(pipeline.subscribe_labels(publication, index + 1, "required", 1, [&, index](std::exception_ptr error) {
   ++settled[index];
   completions[index].set_value(error);
  }));
 }
 projecting = std::async(std::launch::async, [&] { pipeline.execution().join(); });
 mmltk::testsupport::await_test_future(projecting, "CPU projections hand off both pinned outputs to the full queue");
 for (std::size_t index = 0; index < labels.size(); ++index) {
  CHECK_FALSE(labels[index].ready());
  CHECK_FALSE(input_custody[index].expired());
 }
 CHECK(pipeline.execution().resource_usage().bytes > 0);
 CHECK(written == 0);
 const auto failure = std::make_exception_ptr(std::runtime_error("execution failed behind full file queue"));
 switch (outcome) {
  case Outcome::Write: break;
  case Outcome::Dependency: retiring = std::async(std::launch::async, [&] { pipeline.retire_labels(root, 2, "required"); }); break;
  case Outcome::Source: retiring = std::async(std::launch::async, [&] { pipeline.retire_source(root); }); break;
  case Outcome::Cancellation:
   cancelled = true;
   pipeline.execution().notify_admission_change();
   break;
  case Outcome::Failure: pipeline.execution().fail(failure); break;
 }
 if (outcome != Outcome::Write) {
  const auto error = mmltk::testsupport::await_test_promise(completions[1], "unaccepted output retires while physical peers remain held");
  CHECK(error);
  if (outcome == Outcome::Failure) CHECK(error == failure);
  CHECK(input_custody[1].expired());
  if (retiring.valid()) mmltk::testsupport::await_test_future(retiring, "source or dependency withdrawal does not wait on unrelated physical work");
  if (outcome == Outcome::Dependency) {
   CHECK_FALSE(labels[0].ready());
   CHECK_FALSE(input_custody[0].expired());
  } else {
   const auto first = mmltk::testsupport::await_test_promise(completions[0], "every terminal pending output drains");
   CHECK(first);
   if (outcome == Outcome::Failure) CHECK(first == failure);
   CHECK(input_custody[0].expired());
   CHECK(pipeline.execution().resource_usage().bytes == retained_controls);
  }
  CHECK_FALSE(files[0].ready());
  CHECK_FALSE(files[1].ready());
  CHECK(physical_returns == 0);
  CHECK(written == 0);
 }
 physical.Release();
 if (outcome == Outcome::Write || outcome == Outcome::Dependency) {
  CHECK_FALSE(mmltk::testsupport::await_test_promise(completions[0], "live output resumes when file capacity returns"));
  if (outcome == Outcome::Write) CHECK_FALSE(mmltk::testsupport::await_test_promise(completions[1], "second live output completes"));
  CHECK(written == (outcome == Outcome::Write ? 2 : 1));
 }
 pipeline.files().join();
 if (outcome == Outcome::Cancellation)
  CHECK_THROWS(pipeline.drain());
 else if (outcome == Outcome::Failure)
  CHECK_THROWS_WITH(pipeline.drain(), "execution failed behind full file queue");
 else
  pipeline.drain();
 for (std::size_t index = 0; index < labels.size(); ++index) {
  CHECK(settled[index] == 1);
  CHECK(input_custody[index].expired());
 }
 CHECK(pipeline.execution().resource_usage().bytes == retained_controls);
 CHECK(physical_returns >= 2);
}
TEST_CASE("provenance settlement coalesces used record blocks while preserving masks and identities", "[benchmark][pipeline][labels][writer]") {
 mmltk::testsupport::ScopedTempDir temporary("settled-record-blocks");
 const auto images = temporary.path() / "images";
 prepare_cached_image_directory(images);
 BenchmarkEncodedImage::publish(cached_image_path(images, 7), make_jpeg(20, 30, 40), {});
 BenchmarkCompilePipeline execution(1);
 LiveBenchmarkWriter fixture(execution, temporary.path(), AnnotationSource::CoconutCoco);
 auto& writer = fixture.writer;
 const auto& output = fixture.output;
 const auto publication = execution.source_publication(images, {});
 publication({7});
 execution.wait_image(images, 7);
 const auto slot = execution.image_slot(images, 7);
 REQUIRE(slot);
 constexpr std::size_t count = 519;
 std::vector<PackedInstance> labels(count);
 std::vector<RLEPair> runs(count, RLEPair{0, 1});
 for (std::size_t i = 0; i < count; ++i) {
  auto& label = labels[i];
  label.bbox_x2 = label.bbox_y2 = 8;
  label.flags = kAnnotationMask | kAnnotationId | kAnnotationCategory;
  label.annotation_id = 1000 + i;
  label.source_category_id = 1;
  label.source_ordinal = i;
  label.original_area = 1;
  label.mask_rle_offset = static_cast<decltype(label.mask_rle_offset)>(i * sizeof(RLEPair));
  label.mask_rle_pairs = 1;
 }
 ImageEntry entry{};
 entry.source = AnnotationSource::CoconutCoco;
 entry.has_source_image_id = 1;
 entry.source_image_id = 7;
 const auto dimensions = writer.dimensions(*slot);
 entry.original_width = dimensions.first;
 entry.original_height = dimensions.second;
 writer.write_labels(*slot, entry, {labels, runs}, 19);
 writer.settle_labels(*slot, 100, true);
 const std::array selected{*slot};
 auto sealed = writer.seal_slots(output, selected);
 sealed.artifact.publish(output.output_path, {}, detail::StagedArtifact::Publication::DurableReplace);
 const auto compiled = CompiledDataset::open(output.output_path);
 const auto actual = compiled.annotations(*slot);
 REQUIRE(actual.labels.size() == count);
 REQUIRE(actual.runs.size() == count);
 for (std::size_t i = 0; i < count; ++i) {
  CHECK(actual.labels[i].source_ordinal == i + 119);
  CHECK(actual.labels[i].annotation_id == labels[i].annotation_id);
  CHECK(actual.labels[i].flags == (labels[i].flags | kAnnotationOrdinalIncomplete));
  CHECK(actual.labels[i].mask_rle_offset == labels[i].mask_rle_offset);
  CHECK(actual.labels[i].original_area == labels[i].original_area);
  CHECK(actual.runs[i].start == runs[i].start);
  CHECK(actual.runs[i].length == runs[i].length);
 }
}
TEST_CASE("dynamic benchmark slots write labels before pixels and replace one dependency in place", "[benchmark][pipeline][labels][writer][resources]") {
 mmltk::testsupport::ScopedTempDir temporary("independent-slot-products");
 const auto images = temporary.path() / "images";
 const bool contradiction = GENERATE(false, true);
 const std::string_view selection = GENERATE("retain Large", "restore XL", "active XL");
 const bool incomplete = GENERATE(false, true);
 const bool restore = selection != "retain Large";
 std::array<std::size_t, 2> retained_before_projection{};
 std::size_t projections = 0;
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 1, .descriptors = 13});
 execution.label_configuration(8, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, [&](const auto&, auto, const BenchmarkLabelWorkspace& workspace) {
  if (projections < retained_before_projection.size()) retained_before_projection[projections] = workspace.retained_bytes();
  ++projections;
 });
 LiveBenchmarkWriter fixture(execution, temporary.path(), AnnotationSource::CoconutObjects365V2);
 auto& writer = fixture.writer;
 const auto& output = fixture.output;
 NormalizedAnnotationBuilder annotations;
 annotations.images.push_back({7, 0, 1, contradiction ? 15U : 16U, 8, 0, 0});
 NormalizedBox box;
 box.x2 = box.y2 = 1;
 box.flags = kAnnotationId | kAnnotationCategory | (incomplete ? kAnnotationOrdinalIncomplete : 0);
 box.annotation_id = 101;
 box.source_category_id = 1;
 box.source_ordinal = 5;
 annotations.boxes.push_back(box);
 const NormalizedAnnotationReadView xl(fixture_index(annotations));
 const auto source = execution.source_publication(images, {});
 const auto labels_started = std::chrono::steady_clock::now();
 REQUIRE(execution.labels_ready(source, 7, BenchmarkLabelInput(xl, 0), "XL", 0, 1));
 REQUIRE(execution.wait_image_labels(images, 7, "XL"));
 const auto first_labels_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - labels_started).count();
 const auto slot = execution.image_slot(images, 7);
 REQUIRE(slot);
 CHECK(writer.entry(*slot).num_instances == 1);
 CHECK_FALSE(writer.image_complete(*slot));
 CHECK_FALSE(std::filesystem::exists(images));
 annotations.boxes[0].annotation_id = 202;
 annotations.boxes[0].source_ordinal = 0;
 const NormalizedAnnotationReadView large(fixture_index(annotations));
 REQUIRE(execution.labels_ready(source, 7, BenchmarkLabelInput(large, 0), "Large", 0, 2));
 REQUIRE(execution.wait_image_labels(images, 7, "Large"));
 CHECK(retained_before_projection[0] == BenchmarkLabelWorkspace::required_bytes(annotations.images[0].width));
 CHECK(retained_before_projection[1] >= (COMPILED_LABEL_CAPACITY / sizeof(PackedInstance)) * sizeof(PackedInstance) + COMPILED_LABEL_CAPACITY);
 CHECK_FALSE(execution.has_image_labels(images, 7, "XL"));
 CHECK(execution.has_image_labels(images, 7, "Large"));
 CHECK(writer.entry(*slot).num_instances == 1);
 CHECK(execution.image_slot(images, 7) == slot);
 prepare_cached_image_directory(images);
 BenchmarkEncodedImage::publish(cached_image_path(images, 7), make_jpeg(20, 30, 40), {});
 const auto pixels_started = std::chrono::steady_clock::now();
 source({7});
 execution.wait_image(images, 7);
 const auto first_pixels_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pixels_started).count();
 CHECK(writer.completed() == 1);
 if (!contradiction && selection == "retain Large" && !incomplete) {
  std::printf(
   "benchmark_pipeline_fixture first_label_slot_ns=%lld first_pixel_slot_ns=%lld used_label_bytes=%u reused_label_workspace_bytes=%zu labels_ready_before_JPEG=1 "
   "source_backing=fixture_owned_normalized_input timing_scope=ready_publication_to_slot_completion production_baseline=unavailable\n",
   static_cast<long long>(first_labels_ns), static_cast<long long>(first_pixels_ns), writer.entry(*slot).label_bytes, retained_before_projection[1]);
 }
 if (restore) {
  // Final component input already carries its canonical ordinal. The early XL
  // vectors were released after Large replaced them, so replay must use this input.
  annotations.boxes[0].annotation_id = 101;
  annotations.boxes[0].source_ordinal = 23;
  const NormalizedAnnotationReadView final_xl(fixture_index(annotations));
  mmltk::testsupport::TestGate active("XL borrower while the preferred claim retires");
  const mmltk::testsupport::ScopedTestCleanup release([&] {
   active.Release();
   execution.drain();
  });
  if (selection == "active XL") {
   execution.label_configuration(8, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, [&](const auto&, auto, const auto&) { active.receipt().ArriveAndWait(); });
   REQUIRE(execution.labels_ready(source, 7, BenchmarkLabelInput(final_xl, 0), "XL", 0, 1));
   REQUIRE(active.WaitEntered(5s));
   CHECK(execution.has_image_labels(images, 7, "XL"));
  }
  execution.retire_labels(images, 7, "Large");
  CHECK_FALSE(writer.entry(*slot).eligible());
  active.Release();
  execution.drain();
  if (!execution.has_image_labels(images, 7, "XL")) {
   REQUIRE(execution.labels_ready(source, 7, BenchmarkLabelInput(final_xl, 0), "XL", 0, 1));
   REQUIRE(execution.wait_image_labels(images, 7, "XL"));
  }
  CHECK(execution.has_image_labels(images, 7, "XL"));
  CHECK_FALSE(execution.has_image_labels(images, 7, "Large"));
  execution.label_configuration(8, mmltk::backend::imaging::resample::ImageResizeMode::Stretch);
 } else
  execution.retire_labels(images, 7, "XL");
 execution.drain();
 CHECK(writer.completed() == 1);
 CHECK(execution.image_slot(images, 7) == slot);
 const std::array selected{*slot};
 if (contradiction) {
  CHECK_THROWS(writer.seal_slots(output, selected));
  CHECK_FALSE(std::filesystem::exists(output.output_path));
 } else {
  if (!restore) writer.settle_labels(*slot, 11, incomplete);
  auto sealed = writer.seal_slots(output, selected);
  sealed.artifact.publish(output.output_path, {}, detail::StagedArtifact::Publication::DurableReplace);
  const auto compiled = CompiledDataset::open(output.output_path);
  REQUIRE(compiled.image_labels(*slot).size() == 1);
  CHECK(compiled.image_labels(*slot)[0].annotation_id == (restore ? 101 : 202));
  CHECK(compiled.image_labels(*slot)[0].source_ordinal == (restore ? 23 : 11));
  CHECK(compiled.image_labels(*slot)[0].has_complete_source_ordinal() == !incomplete);
  CHECK(compiled.header().num_images == 1);
  CHECK(compiled.header().num_slots == 1);
 }
}
TEST_CASE("normalized cache row admission checks each payload before borrowing and retains mapping custody", "[backend][data][benchmark][annotations][cache]") {
 const std::string_view outcome = GENERATE("valid", "late record", "cancel", "receiver failure");
 mmltk::testsupport::ScopedTempDir root("normalized-incremental-admission");
 const std::string digest(64U, '0');
 NormalizedAnnotationBuilder builder;
 builder.source = BenchmarkDatasetSource::kCoco2017;
 builder.split = "train";
 builder.annotation_sha256 = digest;
 NormalizedBox box;
 box.x2 = box.y2 = 1.0F;
 box.original_area = 1;
 box.flags = kAnnotationCategory;
 box.source_category_id = 1;
 for (unsigned i = 0; i < 2; ++i) {
  builder.images.push_back({i + 1U, i, 1U, 1U, 1U, 0U, 0U});
  builder.boxes.push_back(box);
 }
 const auto path = root.path() / "normalized.bin";
 store_normalized_annotation_index(path, fixture_index(builder), {});
 if (outcome == "late record") {
  box.flags = 0;
  box.source_category_id = 0;
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekp(256U + 2U * sizeof(NormalizedImage) + sizeof(NormalizedBox));
  file.write(reinterpret_cast<const char*>(&box), sizeof(box));
 }
 std::atomic<bool> cancelled{false};
 std::optional<BenchmarkLabelInput> first;
 unsigned rows = 0;
 const auto load = [&] {
  return load_normalized_annotation_index(path, builder.source, builder.split, digest, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), {}, nullptr, false, 0,
   [&](const NormalizedAnnotationIndex& mapped, std::size_t image) {
   ++rows;
   if (!image) {
    first.emplace(NormalizedAnnotationReadView(mapped), image);
    if (outcome == "cancel") cancelled = true;
    if (outcome == "receiver failure") throw std::runtime_error("row receiver failed");
   }
  });
 };
 if (outcome == "cancel")
  CHECK_THROWS(load());
 else if (outcome == "receiver failure")
  CHECK_THROWS_WITH(load(), "row receiver failed");
 else
  CHECK(load().has_value() == (outcome == "valid"));
 CHECK(rows == (outcome == "valid" ? 2U : 1U));
 REQUIRE(first);
 std::filesystem::remove(path);
 CHECK(first->index().image(first->position()).source_image_id == 1);
 CHECK(first->index().storage().boxes.front().source_category_id == 1);
}
TEST_CASE("compile class producers retain waiting floors and release them after completion", "[backend][data][benchmark][pipeline]") {
 using mmltk::backend::data::detail::CompileWorkClass;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires three assigned CPUs");
 CompileExecution execution(3);
 auto annotations = execution.open_class(CompileWorkClass::Annotations);
 auto pixels = execution.open_class(CompileWorkClass::Pixels);
 CompileExecution::Dependency dependency(execution);
 std::atomic<unsigned> resumed{0};
 auto parked = execution.try_subscribe(dependency, dependency.generation(), {.kind = CompileTaskKind::Normalize, .work = [&](std::size_t, const CompileAllowance&) { ++resumed; }});
 REQUIRE(parked);
 const auto before = execution.snapshot();
 CHECK(before.baseline[static_cast<std::size_t>(CompileWorkClass::Annotations)] == 1);
 CHECK(before.baseline[static_cast<std::size_t>(CompileWorkClass::Pixels)] == 1);
 CHECK(before.elastic == 1);
 mmltk::testsupport::TestGate first("pixel baseline"), second("pixel elastic lane");
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  first.Release();
  second.Release();
  dependency.notify();
 });
 auto a = execution.submit({.kind = CompileTaskKind::Pixels, .work = [&](std::size_t, const CompileAllowance&) { first.receipt().ArriveAndWait(); }});
 auto b = execution.submit({.kind = CompileTaskKind::Pixels, .work = [&](std::size_t, const CompileAllowance&) { second.receipt().ArriveAndWait(); }});
 REQUIRE(first.WaitEntered(2s));
 REQUIRE(second.WaitEntered(2s));
 CHECK(execution.snapshot().active == 2);
 dependency.notify();
 parked->wait();
 CHECK(resumed == 1);
 annotations.close();
 first.Release();
 second.Release();
 a.wait();
 b.wait();
 execution.join();
 CHECK_FALSE(execution.snapshot().live[static_cast<std::size_t>(CompileWorkClass::Annotations)]);
 auto repaired = execution.open_class(CompileWorkClass::Annotations);
 CHECK(execution.snapshot().live[static_cast<std::size_t>(CompileWorkClass::Annotations)]);
 const auto observed = dependency.generation();
 dependency.notify();
 auto raced = execution.try_subscribe(dependency, observed, {.kind = CompileTaskKind::Recovery, .work = [&](std::size_t, const CompileAllowance&) { ++resumed; }});
 REQUIRE(raced);
 raced->wait();
 CHECK(resumed == 2);
}
TEST_CASE("compile bounded records and event cancellation settle parked subscribers once", "[backend][data][benchmark][pipeline]") {
 struct CancellationTag;
 using Source = mmltk::common::concurrency::EventCancellationSource<CancellationTag, false>;
 auto [source, token] = Source::Mint();
 CompileExecution execution(1, {}, {.transient_bytes = 64, .descriptors = 16, .task_records = 2}, mmltk::common::concurrency::CancellationObservation::Borrow(token));
 CompileExecution::Dependency dependency(execution);
 std::atomic<unsigned> called{0}, settled{0};
 const auto task = [&] { return CompileExecution::Task{.kind = CompileTaskKind::Normalize, .work = [&](std::size_t, const CompileAllowance&) { ++called; }}; };
 auto first = execution.try_subscribe(dependency, dependency.generation(), task());
 auto second = execution.try_subscribe(dependency, dependency.generation(), task());
 REQUIRE(first);
 REQUIRE(second);
 CHECK_FALSE(execution.try_submit(task()));
 first->subscribe([&](std::exception_ptr error) {
  if (error) ++settled;
 });
 second->subscribe([&](std::exception_ptr error) {
  if (error) ++settled;
 });
 REQUIRE(source.RequestCancel());
 CHECK_THROWS(first->wait());
 CHECK_THROWS(second->wait());
 execution.retire_attempt();
 CHECK(called == 0);
 CHECK(settled == 2);
 dependency.notify();
 CHECK(settled == 2);
 CHECK(execution.snapshot().records == 0);
}
TEST_CASE("compile fewer-core sharing and resource bypass preserve every admitted class", "[backend][data][benchmark][pipeline]") {
 using mmltk::backend::data::detail::CompileWorkClass;
 CompileExecution execution(1, {}, {.transient_bytes = 64, .descriptors = 16});
 std::vector<CompileExecution::Producer> producers;
 for (auto kind : {CompileWorkClass::Acquisition, CompileWorkClass::Extraction, CompileWorkClass::Annotations, CompileWorkClass::Pixels, CompileWorkClass::Gather, CompileWorkClass::Output})
  producers.push_back(execution.open_class(kind));
 CHECK(execution.snapshot().elastic == 1);
 std::array<unsigned, 6> visits{};
 std::vector<CompileExecution::Completion> completed;
 const std::array kinds{CompileTaskKind::Acquisition, CompileTaskKind::Archive, CompileTaskKind::Normalize, CompileTaskKind::Pixels, CompileTaskKind::Gather, CompileTaskKind::Output};
 for (std::size_t i = 0; i < kinds.size(); ++i) completed.push_back(execution.submit({.kind = kinds[i], .work = [&, i](std::size_t, const CompileAllowance&) { ++visits[i]; }}));
 for (const auto& completion : completed) completion.wait();
 for (auto visits_for_class : visits) CHECK(visits_for_class == 1);
 auto held = execution.reserve({32, 0});
 std::atomic<unsigned> larger{0}, smaller{0};
 auto large = execution.submit({.kind = CompileTaskKind::Recovery, .resources = {64, 0}, .work = [&](std::size_t, const CompileAllowance&) { ++larger; }});
 auto small = execution.submit({.kind = CompileTaskKind::Normalize, .resources = {32, 0}, .work = [&](std::size_t, const CompileAllowance&) { ++smaller; }});
 small.wait();
 CHECK(smaller == 1);
 CHECK(larger == 0);
 held = {};
 large.wait();
 CHECK(larger == 1);
}
TEST_CASE("compile continuation custody separates step callbacks from record retirement", "[backend][data][benchmark][pipeline][resources]") {
 using namespace mmltk::backend::data::detail;
 enum class Outcome { Resume, Cancel, Failure, Abandon };
 const auto outcome = GENERATE(Outcome::Resume, Outcome::Cancel, Outcome::Failure, Outcome::Abandon);
 const bool finite_loan = GENERATE(false, true);
 std::promise<void> parked, retirement_started;
 std::atomic<unsigned> first_callbacks{0}, successor_callbacks{0}, calls{0};
 CompileExecution execution(1, {}, {.transient_bytes = 64, .task_records = 1}, {}, [&](std::string_view, const CompileExecutionSnapshot& snapshot) {
  if (snapshot.parked[static_cast<std::size_t>(CompileParkReason::Continuation)]) mmltk::testsupport::release_test_promise(parked);
 });
 auto lender = finite_loan ? execution.reserve({64, 0}) : CompileAllowance{};
 const auto exercise = [&] {
  mmltk::testsupport::TestGate callback("completed step still owns its callback"), resumed("successor owns the same execution record");
  CompileExecution::Continuation continuation;
  CompileAllowance retained;
  CompileExecution::Completion first;
  std::future<void> retiring;
  const mmltk::testsupport::ScopedTestCleanup release([&] {
   callback.Release();
   resumed.Release();
   execution.cancel();
   if (first) try {
     first.wait();
    } catch (...) {}
   retained = {};
   continuation = {};
   if (retiring.valid()) retiring.wait();
   execution.retire_attempt();
  });
  first = execution.submit({.kind = CompileTaskKind::Header, .resources = {32, 0}, .work = [&](std::size_t, const CompileAllowance& allowance) {
   continuation = execution.retain_current(CompileTaskKind::Pixels);
   retained = allowance;
  }, .finite_consumer = true, .completed = [&](std::exception_ptr) {
   ++first_callbacks;
   callback.receipt().ArriveAndWait();
  }});
  REQUIRE(callback.WaitEntered(5s));
  CHECK(first.ready());
  CHECK(execution.snapshot().active == 0);
  CHECK(execution.snapshot().records == 1);
  CHECK_FALSE(execution.try_submit({.kind = CompileTaskKind::Normalize, .work = [](std::size_t, const CompileAllowance&) {}}));
  const auto successor = [&] {
   return CompileExecution::Task{.kind = CompileTaskKind::Pixels, .resources = {32, 0}, .work = [&, expected = retained](std::size_t, const CompileAllowance& allowance) {
    mmltk::testsupport::require_condition(allowance.aliases(expected), "successor lost its admitted allowance");
    ++calls;
    resumed.receipt().ArriveAndWait();
   }, .allowance = retained, .finite_consumer = true, .completed = [&](std::exception_ptr) { ++successor_callbacks; }};
  };
  CHECK_FALSE(continuation.try_submit(successor()));
  // Even a completed callback still occupying the worker thread has returned
  // its real CPU grant; physical work can use that lane independently.
  {
   auto service = execution.service_grant(CompileWorkClass::Extraction);
   CHECK(service.cpu() >= 0);
   CHECK(execution.snapshot().active == 1);
  }
  first.cancel();
  callback.Release();
  mmltk::testsupport::await_test_promise(parked, "step callback relinquishes the retained record", 5s);
  CHECK_FALSE(execution.snapshot().live[static_cast<std::size_t>(CompileWorkClass::Extraction)]);
  CHECK(execution.snapshot().live[static_cast<std::size_t>(CompileWorkClass::Pixels)]);
  CHECK(execution.resource_usage().bytes == (finite_loan ? 96 : 32));
  if (outcome == Outcome::Resume) {
   auto completion = continuation.try_submit(successor());
   REQUIRE(completion);
   CHECK_FALSE(continuation);
   first.cancel();
   execution.resume(first);
   REQUIRE(resumed.WaitEntered(5s));
   CHECK(execution.snapshot().records == 1);
   resumed.Release();
   completion->wait();
   retained = {};
   execution.join();
   CHECK(calls == 1);
   CHECK(successor_callbacks == 1);
  } else {
   if (outcome == Outcome::Failure) execution.fail(std::make_exception_ptr(std::runtime_error("first retained-record failure")));
   if (outcome != Outcome::Abandon) {
    execution.cancel();
    CHECK_THROWS_WITH(continuation.try_submit(successor()), outcome == Outcome::Failure ? "first retained-record failure" : "compile admission after shutdown");
   }
   retiring = std::async(std::launch::async, [&] {
    mmltk::testsupport::release_test_promise(retirement_started);
    execution.retire_attempt();
   });
   mmltk::testsupport::await_test_promise(retirement_started, "retirement joins external continuation custody", 5s);
   CHECK(retiring.wait_for(0ms) == std::future_status::timeout);
   CHECK(execution.snapshot().records == 1);
   CHECK(first_callbacks == 1);
   retained = {};
   continuation = {};
   mmltk::testsupport::await_test_future(retiring, "physical owner releases the retained record", 5s);
   CHECK(calls == 0);
   CHECK(successor_callbacks == 0);
  }
  CHECK(first_callbacks == 1);
  CHECK(execution.snapshot().records == 0);
  CHECK(execution.resource_usage().bytes == (finite_loan ? 64 : 0));
  CompileExecution::Dependency dependency(execution);
  auto replacement = execution.try_subscribe(dependency, dependency.generation(), {.kind = CompileTaskKind::Normalize, .work = [&](std::size_t, const CompileAllowance&) { ++calls; }});
  REQUIRE(replacement);
  first.cancel();
  execution.resume(first);
  CHECK_FALSE(replacement->ready());
  dependency.notify();
  replacement->wait();
  execution.join();
  CHECK(calls == (outcome == Outcome::Resume ? 2 : 1));
 };
 if (finite_loan)
  execution.with_unused_workspace(lender, 32, exercise);
 else
  exercise();
}
TEST_CASE("aged compile demands leave retained-owner dependencies runnable", "[backend][data][benchmark][pipeline]") {
 const bool parent_custody = GENERATE(false, true);
 CAPTURE(parent_custody);
 CompileExecution execution(1, {}, {.transient_bytes = 64, .descriptors = 16});
 auto retained = execution.reserve({32, 0});
 std::atomic<unsigned> smaller{0}, observed_smaller{0};
 mmltk::testsupport::TestGate first("retained owner needs fitting small work");
 auto large = execution.submit({.kind = CompileTaskKind::Recovery, .resources = {64, 0}, .work = [&](std::size_t, const CompileAllowance&) { observed_smaller = smaller.load(); }});
 auto owner = std::async(std::launch::async, [&, retained = std::move(retained)]() mutable {
  // These completions cross the age threshold before the outside owner can
  // release its bytes. Explicit parent custody must preserve the same progress.
  for (unsigned item = 0; item < 12; ++item)
   execution
    .submit(
     {.kind = CompileTaskKind::Recovery,
      .resources = {32, 0},
      .work =
       [&](std::size_t, const CompileAllowance&) {
    if (!item) first.receipt().ArriveAndWait();
    ++smaller;
   },
      .parent = parent_custody ? retained : CompileAllowance{}})
    .wait();
  retained = {};
  large.wait();
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  first.Release();
  large.cancel();
 });
 REQUIRE(first.WaitEntered(2s));
 first.Release();
 mmltk::testsupport::await_test_future(owner, "aged demand waits for retained owner dependencies", 5s);
 CHECK(smaller == 12);
 CHECK(observed_smaller == 12);
}
// CLEANUP-IGNORE: Independent fairness and alias-lifetime cases only share CPU eligibility and parameter setup.
TEST_CASE("aged compile demands reserve reclaimable workspace ahead of smaller work", "[backend][data][benchmark][pipeline]") {
 using mmltk::backend::data::detail::CompileWorkClass;
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 const bool finite_borrower = GENERATE(false, true);
 const bool finite_stream = GENERATE(false, true);
 const bool parented_stream = GENERATE(false, true);
 CAPTURE(finite_borrower, finite_stream, parented_stream);
 CompileExecution execution(2, {}, {.transient_bytes = 64, .descriptors = 16});
 auto annotations = execution.open_class(CompileWorkClass::Annotations);
 auto pixels = execution.open_class(CompileWorkClass::Pixels);
 auto lender = execution.reserve({32, 0});
 execution.with_unused_workspace(lender, 16, [&] {
  mmltk::testsupport::TestGate holding("running task owns reclaimable bytes"), annotation_turn("large class pauses after aging"), inspected("small class scanned the stream"),
   admitted("large demand admitted");
  std::atomic<unsigned> smaller{0}, handles{0};
  const mmltk::testsupport::ScopedTestCleanup release([&] {
   holding.Release();
   annotation_turn.Release();
   inspected.Release();
   admitted.Release();
   execution.retire_attempt();
  });
  auto active = execution.submit({.kind = CompileTaskKind::Pixels, .resources = {16, 0}, .work = [&](std::size_t, const CompileAllowance&) { holding.receipt().ArriveAndWait(); }});
  REQUIRE(holding.WaitEntered(2s));
  auto large = execution.submit({.kind = CompileTaskKind::Labels, .resources = {finite_borrower ? 48U : 32U, 0}, .work = [&](std::size_t, const CompileAllowance&) {
   admitted.receipt().ArriveAndWait();
  }, .finite_consumer = finite_borrower});
  // Each zero-byte selection scans the blocked head on its own class floor.
  for (unsigned item = 0; item < 12; ++item) execution.run(CompileTaskKind::Labels, {}, [](std::size_t) {});
  const auto floors = execution.snapshot().baseline;
  CHECK(floors[static_cast<std::size_t>(CompileWorkClass::Annotations)] == 1);
  CHECK(floors[static_cast<std::size_t>(CompileWorkClass::Pixels)] == 1);
  auto turn = execution.submit({.kind = CompileTaskKind::Labels, .work = [&](std::size_t, const CompileAllowance&) { annotation_turn.receipt().ArriveAndWait(); }});
  REQUIRE(annotation_turn.WaitEntered(2s));
  std::vector<CompileExecution::Completion> stream;
  for (unsigned item = 0; item < 8; ++item)
   stream.push_back(execution.submit({.kind = CompileTaskKind::Pixels, .resources = {16, 0}, .work = [&](std::size_t, const CompileAllowance&) {
    ++smaller;
   }, .parent = parented_stream ? lender : CompileAllowance{}, .finite_consumer = finite_stream}));
  holding.Release();
  active.wait();
  execution.run(CompileTaskKind::Pixels, {.bytes = 8, .retained_handles = true}, [&](std::size_t) { ++handles; });
  auto inspection = execution.submit({.kind = CompileTaskKind::Pixels, .work = [&](std::size_t, const CompileAllowance&) { inspected.receipt().ArriveAndWait(); }});
  REQUIRE(inspected.WaitEntered(2s));
  // This same-kind zero-byte marker proves the small class considered every
  // preceding stream item while the other floor could not yet run the large job.
  CHECK(smaller == 0);
  CHECK(handles == 1);
  CHECK(execution.resource_usage().bytes == 32);
  annotation_turn.Release();
  turn.wait();
  REQUIRE(admitted.WaitEntered(2s));
  CHECK(smaller == 0);
  CHECK(execution.resource_usage().bytes == (finite_borrower ? 80U : 64U));
  admitted.Release();
  inspected.Release();
  large.wait();
  inspection.wait();
  for (const auto& task : stream) task.wait();
  CHECK(smaller == 8);
 });
}
TEST_CASE("queued credit aliases preserve dependent progress through physical retirement", "[backend][data][benchmark][pipeline]") {
 using mmltk::backend::data::detail::CompileWorkClass;
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 const bool transferred = GENERATE(false, true);
 CAPTURE(transferred);
 CompileExecution execution(2, {}, {.transient_bytes = 64, .descriptors = 16});
 auto annotations = execution.open_class(CompileWorkClass::Annotations);
 auto pixels = execution.open_class(CompileWorkClass::Pixels);
 std::promise<CompileAllowance> custody;
 mmltk::testsupport::TestGate holding("source credit is physically running"), probe("fitting unrelated work bypasses the unfit demand"), dependent("queued alias retains source backing"),
  admitted("large demand follows its physical owners");
 std::atomic<bool> dependent_returned{false}, observed_return{false};
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  holding.Release();
  probe.Release();
  dependent.Release();
  admitted.Release();
  execution.retire_attempt();
 });
 auto active = execution.submit({.kind = CompileTaskKind::Pixels, .resources = {32, 0}, .work = [&](std::size_t, const CompileAllowance& allowance) {
  custody.set_value(allowance);
  holding.receipt().ArriveAndWait();
 }});
 auto credit = mmltk::testsupport::await_test_promise(custody, "running credit published to its dependent");
 REQUIRE(holding.WaitEntered(2s));
 CompileExecution::Task child{.kind = CompileTaskKind::Pixels, .resources = {16, 0}, .work = [&](std::size_t, const CompileAllowance&) {
  dependent.receipt().ArriveAndWait();
  dependent_returned = true;
 }};
 if (transferred)
  child.allowance = std::move(credit);
 else
  child.parent = std::move(credit);
 auto continuation = execution.submit(std::move(child));
 auto large = execution.submit({.kind = CompileTaskKind::Labels, .resources = {64, 0}, .work = [&](std::size_t, const CompileAllowance&) {
  observed_return = dependent_returned.load();
  admitted.receipt().ArriveAndWait();
 }});
 for (unsigned item = 0; item < 12; ++item) execution.run(CompileTaskKind::Labels, {}, [](std::size_t) {});
 auto fitting = execution.submit({.kind = CompileTaskKind::Labels, .resources = {16, 0}, .work = [&](std::size_t, const CompileAllowance&) { probe.receipt().ArriveAndWait(); }});
 REQUIRE(probe.WaitEntered(2s));
 CHECK_FALSE(active.ready());
 CHECK_FALSE(continuation.ready());
 CHECK(execution.resource_usage().bytes == 48);
 probe.Release();
 fitting.wait();
 holding.Release();
 active.wait();
 REQUIRE(dependent.WaitEntered(2s));
 CHECK(execution.resource_usage().bytes == (transferred ? 32U : 48U));
 dependent.Release();
 continuation.wait();
 REQUIRE(admitted.WaitEntered(2s));
 CHECK(observed_return);
 CHECK(execution.resource_usage().bytes == 64);
 admitted.Release();
 large.wait();
}
TEST_CASE("compile service pumps and external helpers share the configured CPU grants", "[backend][data][benchmark][pipeline]") {
 using mmltk::backend::data::detail::CompileWorkClass;
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 CompileExecution execution(2);
 auto acquisition = execution.open_class(CompileWorkClass::Acquisition);
 auto pixels = execution.open_class(CompileWorkClass::Pixels);
 auto service = execution.service_grant(CompileWorkClass::Acquisition);
 CHECK_FALSE(execution.try_reserve({.cpu_workers = 1}));
 mmltk::testsupport::TestGate gate("compute beside acquisition service");
 const mmltk::testsupport::ScopedTestCleanup release([&] { gate.Release(); });
 auto work = execution.submit({.kind = CompileTaskKind::Pixels, .work = [&](std::size_t, const CompileAllowance&) { gate.receipt().ArriveAndWait(); }});
 REQUIRE(gate.WaitEntered(2s));
 const auto during = execution.snapshot();
 CHECK(during.services == 1);
 CHECK(during.active + during.external == 2);
 service = {};
 gate.Release();
 work.wait();
 execution.join();
 acquisition.close();
 pixels.close();
 auto external = execution.reserve({.cpu_workers = 1});
 REQUIRE(external.cpus().size() == 1);
 int compute_cpu = -1;
 execution.run(CompileTaskKind::Pixels, {}, [&](std::size_t lane) { compute_cpu = execution.cpus()[lane]; });
 CHECK(compute_cpu != external.cpus().front());
 external.retire_workspace();
 CHECK(execution.snapshot().external == 0);
}
TEST_CASE("compile one-core services yield their grant to an earlier ready class", "[backend][data][benchmark][pipeline]") {
 using mmltk::backend::data::detail::CompileWorkClass;
 CompileExecution execution(1);
 auto acquisition = execution.open_class(CompileWorkClass::Acquisition);
 auto pixels = execution.open_class(CompileWorkClass::Pixels);
 auto service = execution.service_grant(CompileWorkClass::Acquisition);
 std::atomic<bool> ran{false};
 auto work = execution.submit({.kind = CompileTaskKind::Pixels, .work = [&](std::size_t, const CompileAllowance&) { ran = true; }});
 service = {};
 service = execution.service_grant(CompileWorkClass::Acquisition);
 CHECK(ran);
 service = {};
 work.wait();
}
TEST_CASE("compile service demand retains denied classes without publishing failed probes", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPUs");
 CompileExecution execution(4);
 auto extraction = execution.open_class(CompileWorkClass::Extraction);
 auto annotations = execution.open_class(CompileWorkClass::Annotations);
 auto pixels = execution.open_class(CompileWorkClass::Pixels);
 auto output = execution.open_class(CompileWorkClass::Output);
 auto held = execution.service_grant(CompileWorkClass::Output);
 CompileExecution::ServiceDemand demand(execution);
 CompileExecution::ServiceDemand::Classes classes{};
 classes[static_cast<std::size_t>(CompileWorkClass::Output)] = true;
 demand.update(classes);
 CHECK_FALSE(demand.try_grant());
 const auto observed = execution.admission_generation();
 demand.update(classes);
 CHECK_FALSE(demand.try_grant());
 CHECK(execution.admission_generation() == observed);
 // A newly queued class remains visible after an actual unsuccessful probe.
 classes[static_cast<std::size_t>(CompileWorkClass::Extraction)] = true;
 demand.update(classes);
 auto peer = demand.try_grant();
 REQUIRE(peer);
 CHECK(peer->work_class() == CompileWorkClass::Extraction);
 CHECK(peer->cpu() != held.cpu());
 const auto snapshot = execution.snapshot();
 CHECK(snapshot.services == 2);
 CHECK(snapshot.active + snapshot.external == 2);
 CHECK(snapshot.parked[static_cast<std::size_t>(detail::CompileParkReason::Cpu)] == 0);
 CHECK_FALSE(demand.try_grant());
}
TEST_CASE("borrowed file classes bypass a denied floor and release demand through completion custody", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPUs");
 const bool cancel = GENERATE(false, true);
 const bool backlog = GENERATE(false, true);
 const bool subscription = GENERATE(false, true);
 std::promise<void> waiting;
 std::atomic<bool> invalid_budget{false};
 CompileExecution execution(4, {}, {}, {}, [&](std::string_view, const detail::CompileExecutionSnapshot& snapshot) {
  if (snapshot.active + snapshot.external > 4) invalid_budget = true;
  if (snapshot.services == 1 && snapshot.parked[static_cast<std::size_t>(detail::CompileParkReason::Cpu)]) mmltk::testsupport::release_test_promise(waiting);
 });
 auto extraction = execution.open_class(CompileWorkClass::Extraction);
 auto annotations = execution.open_class(CompileWorkClass::Annotations);
 auto pixels = execution.open_class(CompileWorkClass::Pixels);
 auto output = execution.open_class(CompileWorkClass::Output);
 detail::FileWorkQueue files(execution, 4, 2);
 mmltk::testsupport::TestGate physical_output("Output owns its reserved floor"), physical_input("Extraction bypasses queued Output"), callback("Extraction callback retains its dispatcher");
 std::atomic<unsigned> second_output_calls{0}, second_input_calls{0}, completed{0}, failed{0};
 const auto complete = [&](std::exception_ptr error) {
  ++completed;
  if (error) ++failed;
 };
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  physical_output.Release();
  physical_input.Release();
  callback.Release();
  files.cancel();
  files.join();
 });
 auto first = files.submit({.completed = complete, .work = [&] { physical_output.receipt().ArriveAndWait(); }});
 REQUIRE(physical_output.WaitEntered(2s));
 auto second = files.submit({.completed = complete, .work = [&] { ++second_output_calls; }});
 mmltk::testsupport::await_test_promise(waiting, "queued Output service demand reaches admission");
 auto input = files.submit({.completed = [&](std::exception_ptr error) {
  complete(error);
  if (!subscription) callback.receipt().ArriveAndWait();
 }, .work = [&] { physical_input.receipt().ArriveAndWait(); }, .service_class = CompileWorkClass::Extraction});
 if (subscription) input.subscribe([&](std::exception_ptr) { callback.receipt().ArriveAndWait(); });
 REQUIRE(physical_input.WaitEntered(2s));
 CHECK_FALSE(first.ready());
 CHECK_FALSE(second.ready());
 CHECK(second_output_calls == 0);
 CHECK(execution.snapshot().services == 2);
 std::optional<CompileExecution::Completion> next_input;
 if (backlog) next_input = files.submit({.completed = complete, .work = [&] { ++second_input_calls; }, .service_class = CompileWorkClass::Extraction});
 physical_input.Release();
 REQUIRE(callback.WaitEntered(2s));
 extraction.close();
 output.close();
 const auto during = execution.snapshot();
 CHECK(during.live[static_cast<std::size_t>(CompileWorkClass::Extraction)]);
 CHECK(during.services == 1);
 CHECK(during.active + during.external == 1);
 CHECK(during.parked[static_cast<std::size_t>(detail::CompileParkReason::Cpu)] == 0);
 // Both dispatchers remain occupied. Queued file demand cannot prevent normal
 // CPU work from using the Extraction grant that its callback has returned.
 execution.run(CompileTaskKind::Archive, {}, [](std::size_t) {});
 CHECK(input.ready() == subscription);
 CHECK(second_input_calls == 0);
 if (cancel) {
  files.cancel();
  CHECK_THROWS(second.wait());
  if (next_input) CHECK_THROWS(next_input->wait());
  CHECK_FALSE(first.ready());
  CHECK(input.ready() == subscription);
 }
 // Retain all four class floors while checking the remaining queued records.
 extraction = execution.open_class(CompileWorkClass::Extraction);
 callback.Release();
 input.wait();
 if (!cancel && next_input) {
  next_input->wait();
  CHECK(second_input_calls == 1);
 }
 CHECK(second_output_calls == 0);
 physical_output.Release();
 first.wait();
 if (!cancel) second.wait();
 files.join();
 extraction.close();
 CHECK(completed == (backlog ? 4U : 3U));
 CHECK(failed == (cancel ? (backlog ? 2U : 1U) : 0U));
 CHECK(second_output_calls == (cancel ? 0U : 1U));
 CHECK_FALSE(invalid_budget);
 CHECK_FALSE(execution.snapshot().live[static_cast<std::size_t>(CompileWorkClass::Extraction)]);
 CHECK_FALSE(execution.snapshot().live[static_cast<std::size_t>(CompileWorkClass::Output)]);
}
TEST_CASE("borrowed file dispatch admits multiple eligible records in one class", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 CompileExecution execution(2);
 detail::FileWorkQueue files(execution, 2, 2);
 mmltk::testsupport::TestGate physical("two admitted Output records");
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  physical.Release();
  files.cancel();
  files.join();
 });
 const detail::FileWorkQueue::Operation operation{.work = [&] { physical.receipt().ArriveAndWait(); }};
 auto first = files.submit(operation), second = files.submit(operation);
 REQUIRE(physical.WaitEntered(2s, 2));
 CHECK(execution.snapshot().services == 2);
 CHECK(execution.snapshot().active + execution.snapshot().external == 2);
 CHECK_FALSE(files.try_submit(operation));
 files.cancel();
 CHECK_FALSE(first.ready());
 CHECK_FALSE(second.ready());
 physical.Release();
 first.wait();
 second.wait();
 files.join();
 CHECK(execution.snapshot().services == 0);
}
TEST_CASE("borrowed file classes share the one-core cursor with ready compute", "[backend][data][benchmark][pipeline]") {
 CompileExecution execution(1);
 auto extraction = execution.open_class(CompileWorkClass::Extraction);
 auto pixels = execution.open_class(CompileWorkClass::Pixels);
 auto output = execution.open_class(CompileWorkClass::Output);
 detail::FileWorkQueue files(execution, 3);
 mmltk::testsupport::TestGate first_input("first Extraction pump"), compute("Pixels turn before queued Output"), output_call("Output turn before queued Extraction");
 std::atomic<unsigned> order{0}, pixel_order{0}, output_order{0}, input_order{0};
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  first_input.Release();
  compute.Release();
  output_call.Release();
  files.cancel();
  files.join();
  execution.retire_attempt();
 });
 auto first = files.submit({.work = [&] { first_input.receipt().ArriveAndWait(); }, .service_class = CompileWorkClass::Extraction});
 REQUIRE(first_input.WaitEntered(2s));
 auto write = files.submit({.work = [&] {
  output_order = ++order;
  output_call.receipt().ArriveAndWait();
 }});
 auto read = files.submit({.work = [&] { input_order = ++order; }, .service_class = CompileWorkClass::Extraction});
 auto pixels_ready = execution.submit({.kind = CompileTaskKind::Pixels, .work = [&](std::size_t, const CompileAllowance&) {
  pixel_order = ++order;
  compute.receipt().ArriveAndWait();
 }});
 first_input.Release();
 REQUIRE(compute.WaitEntered(2s));
 CHECK(pixel_order == 1);
 CHECK(output_order == 0);
 CHECK(input_order == 0);
 CHECK(execution.snapshot().active == 1);
 CHECK(execution.snapshot().services == 0);
 compute.Release();
 pixels_ready.wait();
 REQUIRE(output_call.WaitEntered(2s));
 CHECK(output_order == 2);
 CHECK(input_order == 0);
 CHECK(execution.snapshot().active == 1);
 CHECK(execution.snapshot().services == 1);
 output_call.Release();
 first.wait();
 write.wait();
 read.wait();
 files.join();
 CHECK(input_order == 3);
}
TEST_CASE("compile finite loans withdraw only participating offers", "[backend][data][benchmark][pipeline]") {
 CompileExecution execution(1, {}, {.transient_bytes = 128, .descriptors = 16});
 auto first = execution.reserve({64, 0});
 auto second = execution.reserve({64, 0});
 mmltk::testsupport::TestGate first_offer("first stable offer"), borrower("borrower retains first offer");
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  borrower.Release();
  first_offer.Release();
 });
 auto lending = std::async(std::launch::async, [&] { execution.with_unused_workspace(first, 16, [&] { first_offer.receipt().ArriveAndWait(); }); });
 REQUIRE(first_offer.WaitEntered(2s));
 CompileExecution::Completion task;
 execution.with_unused_workspace(second, 16, [&] {
  task = execution.submit({.kind = CompileTaskKind::Pixels, .resources = {32, 0}, .work = [&](std::size_t, const CompileAllowance&) { borrower.receipt().ArriveAndWait(); }, .finite_consumer = true});
  REQUIRE(borrower.WaitEntered(2s));
 });
 // The unrelated second lender has returned while the first loan remains live.
 CHECK_FALSE(task.ready());
 borrower.Release();
 task.wait();
 first_offer.Release();
 mmltk::testsupport::await_test_future(lending, "quantified workspace withdrawal");
 CHECK(first.bytes() == 64);
 CHECK(second.bytes() == 64);
}
TEST_CASE("borrowed file cancellation interrupts a shared CPU grant wait locally", "[backend][data][benchmark][pipeline]") {
 using mmltk::backend::data::detail::CompileExecutionSnapshot;
 using mmltk::backend::data::detail::CompileParkReason;
 using mmltk::backend::data::detail::FileWorkQueue;
 std::promise<void> waiting;
 std::atomic<bool> observed_wait{false};
 std::atomic<unsigned> calls{0}, completed{0}, failed{0}, settled{0};
 CompileExecution execution(1, {}, {}, {}, [&](std::string_view, const CompileExecutionSnapshot& snapshot) {
  if (snapshot.parked[static_cast<std::size_t>(CompileParkReason::Cpu)] && !observed_wait.exchange(true)) mmltk::testsupport::release_test_promise(waiting);
 });
 mmltk::testsupport::TestGate unrelated("unrelated work keeps the only CPU");
 FileWorkQueue files(execution, 2);
 std::future<void> cancellation;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  unrelated.Release();
  files.cancel();
 });
 auto compute = execution.submit({.kind = CompileTaskKind::Pixels, .work = [&](std::size_t, const CompileAllowance&) { unrelated.receipt().ArriveAndWait(); }});
 REQUIRE(unrelated.WaitEntered(2s));
 const FileWorkQueue::Operation operation{.context = &calls, .call = [](void* context) { ++*static_cast<std::atomic<unsigned>*>(context); }, .completed = [&](std::exception_ptr error) {
  ++completed;
  if (error) ++failed;
 }};
 auto first = files.try_submit(operation), second = files.try_submit(operation);
 REQUIRE(first);
 REQUIRE(second);
 for (const auto* record : {&*first, &*second})
  record->subscribe([&](std::exception_ptr error) {
   if (error) ++settled;
  });
 // The existing CPU-parked observation establishes a real service admission
 // wait, independently of a delay or of queued operation scheduling.
 mmltk::testsupport::await_test_promise(waiting, "file service waits for the held CPU");
 cancellation = std::async(std::launch::async, [&] {
  files.cancel();
  files.join();
 });
 mmltk::testsupport::await_test_future(cancellation, "local cancellation interrupts service admission");
 CHECK_FALSE(compute.ready());
 CHECK(calls == 0);
 CHECK(completed == 2);
 CHECK(failed == 2);
 CHECK(settled == 2);
 CHECK_THROWS(first->wait());
 CHECK_THROWS(second->wait());
 CHECK(execution.snapshot().services == 0);
 CHECK(execution.snapshot().active == 1);
 unrelated.Release();
 compute.wait();
 execution.run(CompileTaskKind::Pixels, {}, [](std::size_t) {});
}
TEST_CASE("borrowed file demand observes notifying and legacy cancellation through a held CPU peer", "[backend][data][benchmark][pipeline]") {
 const bool notifying = GENERATE(false, true);
 struct FileCancellationTag {};
 using Source = mmltk::common::concurrency::EventCancellationSource<FileCancellationTag, false>;
 using Observation = mmltk::common::concurrency::CancellationObservation;
 auto [cancel, token] = Source::Mint();
 std::atomic<bool> legacy{false};
 std::promise<void> waiting, settled;
 std::atomic<unsigned> calls{0};
 const auto cause = std::make_exception_ptr(std::runtime_error("file admission cancellation cause"));
 CompileExecution execution(1, {}, {}, notifying ? Observation::Borrow(token) : Observation::Atomic(legacy), [&](std::string_view, const detail::CompileExecutionSnapshot& snapshot) {
  if (snapshot.parked[static_cast<std::size_t>(detail::CompileParkReason::Cpu)]) mmltk::testsupport::release_test_promise(waiting);
 }, cause);
 detail::FileWorkQueue files(execution, 1);
 mmltk::testsupport::TestGate physical("accepted compute retains its sole CPU grant");
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  physical.Release();
  files.cancel();
  files.join();
  execution.retire_attempt();
 });
 auto peer = execution.submit({.kind = CompileTaskKind::Pixels, .work = [&](std::size_t, const CompileAllowance&) { physical.receipt().ArriveAndWait(); }});
 REQUIRE(physical.WaitEntered(2s));
 auto queued = files.submit({.work = [&] { ++calls; }});
 queued.subscribe([&](std::exception_ptr) { mmltk::testsupport::release_test_promise(settled); });
 mmltk::testsupport::await_test_promise(waiting, "file demand parked behind an accepted peer");
 if (notifying)
  REQUIRE(cancel.RequestCancel());
 else
  legacy = true;
 mmltk::testsupport::await_test_promise(settled, "cancellation retires queued custody before peer return");
 CHECK(queued.failure() == cause);
 CHECK_THROWS(queued.wait());
 files.join();
 CHECK(calls == 0);
 CHECK_FALSE(peer.ready());
 CHECK(execution.snapshot().active == 1);
 CHECK(execution.snapshot().services == 0);
 physical.Release();
 peer.wait();
}
TEST_CASE("borrowed file records retain physical completion and share compile service capacity", "[backend][data][benchmark][pipeline]") {
 using mmltk::backend::data::detail::FileWorkQueue;
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 CompileExecution execution(2);
 struct Physical {
  mmltk::testsupport::TestGate gate{"borrowed file operation"};
  std::atomic<bool> completed{false};
  std::atomic<unsigned> succeeded{0}, failed{0};
 } physical;
 FileWorkQueue files(execution, 2);
 const mmltk::testsupport::ScopedTestCleanup release([&] { physical.gate.Release(); });
 const FileWorkQueue::Operation operation{.context = &physical, .call = [](void* context) {
  auto& input = *static_cast<Physical*>(context);
  input.gate.receipt().ArriveAndWait();
  input.completed = true;
 }, .completed = [&](std::exception_ptr error) {
  if (error)
   ++physical.failed;
  else
   ++physical.succeeded;
 }};
 auto pending = files.try_submit(operation);
 REQUIRE(pending);
 REQUIRE(physical.gate.WaitEntered(2s));
 auto queued = files.try_submit(operation);
 REQUIRE(queued);
 CHECK_FALSE(files.try_submit(operation));
 execution.run(CompileTaskKind::Pixels, {}, [](std::size_t) {});
 files.cancel();
 CHECK(queued->ready());
 CHECK_THROWS(queued->wait());
 CHECK(physical.failed == 1);
 CHECK_FALSE(pending->ready());
 CHECK_FALSE(physical.completed);
 physical.gate.Release();
 pending->wait();
 files.join();
 CHECK(physical.completed);
 CHECK(physical.succeeded == 1);
 CHECK(physical.failed == 1);
}
TEST_CASE("borrowed runtime file cancellation retires context before delayed pool dispatch", "[backend][data][benchmark][pipeline]") {
 using mmltk::backend::data::detail::FileWorkQueue;
 mmltk::common::concurrency::WorkerPool pool(1);
 mmltk::testsupport::TestGate unrelated("runtime reader pool is occupied");
 std::atomic<unsigned> calls{0}, failed{0};
 auto files = std::make_unique<FileWorkQueue>(pool, 1);
 auto compute = pool.enqueue([&] { unrelated.receipt().ArriveAndWait(); });
 std::future<void> cancellation;
 const mmltk::testsupport::ScopedTestCleanup release([&] { unrelated.Release(); });
 REQUIRE(unrelated.WaitEntered(2s));
 auto custody = std::make_shared<unsigned>(0);
 const std::weak_ptr<unsigned> retired = custody;
 auto pending = files->try_submit({.context = &calls, .call = [](void* context) { ++*static_cast<std::atomic<unsigned>*>(context); }, .completed = [&, custody](std::exception_ptr error) {
  if (error) {
   ++*custody;
   ++failed;
  }
 }});
 custody.reset();
 REQUIRE(pending);
 cancellation = std::async(std::launch::async, [&] {
  files->cancel();
  files->join();
  files.reset();
 });
 mmltk::testsupport::await_test_future(cancellation, "runtime cancellation retires queued borrowed records");
 CHECK(compute.wait_for(0s) == std::future_status::timeout);
 CHECK_THROWS(pending->wait());
 CHECK(retired.expired());
 CHECK(failed == 1);
 CHECK(calls == 0);
 unrelated.Release();
 mmltk::testsupport::await_test_future(compute, "runtime reader pool drains stale dispatch");
 pool.wait_idle();
 CHECK(failed == 1);
 CHECK(calls == 0);
}
TEST_CASE("rejected file admission leaves no completion custody or output producer", "[backend][data][benchmark][pipeline]") {
 const bool cancellation = GENERATE(false, true);
 CompileExecution execution(1);
 detail::FileWorkQueue files(execution, 1);
 const auto original = std::make_exception_ptr(std::runtime_error("file admission execution failure"));
 std::atomic<unsigned> completed{0}, calls{0};
 auto custody = std::make_shared<unsigned>(7);
 const std::weak_ptr<unsigned> borrowed = custody;
 if (cancellation)
  execution.cancel();
 else {
  execution.fail(original);
  execution.fail(std::make_exception_ptr(std::runtime_error("later file admission failure")));
  execution.cancel();
 }
 REQUIRE(execution.interruption());
 if (!cancellation) CHECK(execution.interruption() == original);
 const auto expected = cancellation ? "compile work retired before completion" : "file admission execution failure";
 CHECK_THROWS_WITH(files.try_submit(
                    {.completed =
                      [&, custody = std::move(custody)](std::exception_ptr) {
  ++*custody;
  ++completed;
 },
                     .work = [&] { ++calls; }}),
  expected);
 files.join();
 CHECK(borrowed.expired());
 CHECK(completed == 0);
 CHECK(calls == 0);
 CHECK_FALSE(execution.snapshot().live[static_cast<std::size_t>(CompileWorkClass::Output)]);
 execution.retire_attempt();
 CHECK_FALSE(execution.interruption());
 const auto accepted = files.submit({.completed = [&](std::exception_ptr error) {
  if (!error) ++completed;
 }, .work = [&] { ++calls; }});
 accepted.wait();
 files.join();
 CHECK(completed == 1);
 CHECK(calls == 1);
 CHECK_FALSE(execution.snapshot().live[static_cast<std::size_t>(CompileWorkClass::Output)]);
}
TEST_CASE("file terminal admission preserves accepted peers with full or free records", "[backend][data][benchmark][pipeline]") {
 const bool cancellation = GENERATE(false, true);
 const bool full = GENERATE(false, true);
 CompileExecution execution(1);
 detail::FileWorkQueue files(execution, full ? 1 : 2);
 mmltk::testsupport::TestGate physical("accepted Output remains physically owned");
 std::atomic<unsigned> completed{0}, rejected_callbacks{0}, rejected_calls{0};
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  physical.Release();
  files.cancel();
  files.join();
 });
 auto accepted = files.submit({.completed = [&](std::exception_ptr error) {
  if (!error) ++completed;
 }, .work = [&] { physical.receipt().ArriveAndWait(); }});
 REQUIRE(physical.WaitEntered(2s));
 const auto original = std::make_exception_ptr(std::runtime_error("first failure with accepted file custody"));
 if (!cancellation) {
  execution.fail(original);
  execution.fail(std::make_exception_ptr(std::runtime_error("later failure")));
 }
 execution.cancel();
 const auto expected = execution.interruption();
 REQUIRE(expected);
 if (!cancellation) CHECK(expected == original);
 auto custody = std::make_shared<unsigned>(7);
 const std::weak_ptr<unsigned> borrowed = custody;
 std::exception_ptr rejected;
 try {
  (void)files.try_submit({.completed = [&, custody = std::move(custody)](std::exception_ptr) {
   ++*custody;
   ++rejected_callbacks;
  }, .work = [&] { ++rejected_calls; }});
 } catch (...) { rejected = std::current_exception(); }
 CHECK(rejected == expected);
 CHECK(borrowed.expired());
 CHECK(rejected_callbacks == 0);
 CHECK(rejected_calls == 0);
 CHECK_FALSE(accepted.ready());
 CHECK(completed == 0);
 CHECK(execution.snapshot().live[static_cast<std::size_t>(CompileWorkClass::Output)]);
 CHECK(execution.snapshot().services == 1);
 physical.Release();
 accepted.wait();
 files.join();
 CHECK(completed == 1);
 CHECK_FALSE(execution.snapshot().live[static_cast<std::size_t>(CompileWorkClass::Output)]);
 CHECK(execution.snapshot().services == 0);
}
TEST_CASE("rejected borrowed runtime dispatch releases its operation without a callback", "[backend][data][benchmark][pipeline]") {
 mmltk::common::concurrency::WorkerPool pool(1);
 detail::FileWorkQueue files(pool, 1);
 pool.enqueue_detached([] { throw std::runtime_error("runtime reader pool failed"); });
 CHECK_THROWS_WITH(pool.wait_idle(), "runtime reader pool failed");
 std::atomic<unsigned> completed{0}, calls{0};
 auto custody = std::make_shared<unsigned>(7);
 const std::weak_ptr<unsigned> borrowed = custody;
 CHECK_THROWS_WITH(files.try_submit(
                    {.completed =
                      [&, custody = std::move(custody)](std::exception_ptr) {
  ++*custody;
  ++completed;
 },
                     .work = [&] { ++calls; }}),
  "runtime reader pool failed");
 files.join();
 CHECK(borrowed.expired());
 CHECK(completed == 0);
 CHECK(calls == 0);
 // A second rejection reaches the same pool boundary, rather than finding a
 // leaked occupied file record or a retained delayed-dispatch owner.
 CHECK_THROWS_WITH(files.try_submit({.work = [] {}}), "runtime reader pool failed");
}
TEST_CASE("compile fixed arenas retain pinning while owned label outputs delay reuse", "[benchmark][pipeline][resources]") {
 using namespace mmltk::backend::data::detail;
 const auto bytes = CompiledLabelWorkspace::required_bytes(8);
 CompileExecution execution(1, {}, {.transient_bytes = bytes}, {}, [](std::string_view, const CompileExecutionSnapshot&) {});
 const auto initial = execution.snapshot().normal_locked;
 CompiledLabelWorkspace workspace;
 CompiledLabelWorkspace::OutputLease output;
 const void* base = nullptr;
 execution
  .submit(
   {.kind = CompileTaskKind::Labels,
    .resources = {.bytes = bytes, .supplied_workspace = true},
    .work =
     [&](std::size_t, const CompileAllowance& allowance) {
  workspace.prepare(8, allowance);
  workspace.labels.push_back({});
  base = workspace.labels.data();
  output = workspace.release_output();
 },
    .retain_workspace = false})
  .wait();
 CHECK(execution.snapshot().normal_locked == initial);
 CHECK(execution.resource_usage().bytes == bytes);
 CHECK(output.labels.data() == base);
 CHECK_FALSE(execution.try_reserve({.bytes = bytes, .supplied_workspace = true}));
 output = {};
 auto next = execution.reserve({.bytes = bytes, .supplied_workspace = true});
 CHECK(next.workspace().data() == base);
 next.retire_workspace();  // Logical retirement cannot return a live arena lease.
 CHECK(execution.resource_usage().bytes == bytes);
 next = {};
 CHECK(execution.resource_usage().bytes == 0);
 CHECK(execution.snapshot().normal_locked == initial);
}
// CLEANUP-IGNORE: Independent arena cases select the same window parameters and page size; their execution and ownership assertions differ.
TEST_CASE("one exceptional arena finishes its retained lineage and returns to the fixed bank", "[benchmark][pipeline][resources]") {
 using namespace mmltk::backend::data::detail;
 const auto window = GENERATE(CompileWorkspaceWindow::Task, CompileWorkspaceWindow::SharedSource, CompileWorkspaceWindow::Compaction);
 CAPTURE(window);
 const auto page = mmltk::common::system::host_page_size();
 CompileExecution execution(1, {}, {.transient_bytes = page});
 const auto locked = execution.snapshot().normal_locked;
 auto parent = execution.reserve({page, 0});
 CompileAllowance output;
 execution
  .submit(
   {.kind = CompileTaskKind::Pixels,
    .resources = {.bytes = page * 3, .supplied_workspace = true, .workspace_window = window},
    .work =
     [&](std::size_t, const CompileAllowance& allowance) {
  auto memory = allowance.workspace();
  REQUIRE(memory.size() == page * 3);
  memory[0] = std::byte{73};
  output = allowance;
 },
    .parent = parent,
    .retain_workspace = false})
  .wait();
 CHECK(execution.snapshot().normal_locked == locked);
 CHECK(execution.snapshot().exceptional == page * 3);
 CHECK_FALSE(execution.try_reserve({.bytes = 1, .supplied_workspace = true}));
 CHECK(output.workspace()[0] == std::byte{73});
 execution.cancel();
 execution.retire_attempt();
 CHECK(output.workspace()[0] == std::byte{73});
 CHECK_FALSE(execution.try_reserve({.bytes = 1, .supplied_workspace = true}));
 output = {};
 parent = {};
 CHECK(execution.snapshot().exceptional == 0);
 CHECK(execution.snapshot().normal_locked == locked);
 CHECK(execution.reserve({.bytes = 1, .supplied_workspace = true}).workspace().size() == 1);
}
TEST_CASE("Directory supplied decoding accepts PNM content independently of path spelling", "[backend][data][writer]") {
 using namespace mmltk::backend::data::detail;
 const std::array<std::uint8_t, 17> bytes{'P', '6', '\n', '2', ' ', '1', '\n', '2', '5', '5', '\n', 1, 2, 3, 4, 5, 6};
 CompileImageDecoder decoder;
 const auto header = decoder.read_header(bytes, CompileImagePolicy::Directory);
 mmltk::common::system::NumaMemory storage(mmltk::common::system::NumaTopology::Capture().permitted_nodes.front(), mmltk::common::system::NumaMemory::Lock::Pages);
 const auto plan = CompileImageDecoder::plan(header);
 storage.ensure_bytes(plan.bytes());
 auto memory = std::span(static_cast<std::byte*>(storage.data()), plan.bytes());
 const auto image = decoder.decode_rgb(bytes, header, memory);
 CHECK(std::ranges::equal(image, std::span(bytes).last(6)));
 CHECK_THROWS_AS(decoder.decode_rgb(bytes, header, memory.first(plan.bytes() - 1)), std::length_error);
}
TEST_CASE("protected compile windows remain available to their completion class", "[benchmark][pipeline][resources]") {
 using namespace mmltk::backend::data::detail;
 const auto page = mmltk::common::system::host_page_size();
 const std::array<CompileWorkspaceProfile, 6> profiles{{{page, page}, {page, page}, {page, page}, {page, page}, {page, page}, {page, page}}};
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(1, profiles, page, page, page * 9));
 CHECK(plan->protected_bytes == page * 8);
 CHECK(plan->contexts == 1);
 CompileExecution execution(1, {}, {.workspace_plan = plan});
 auto normal = execution.reserve({.bytes = page, .supplied_workspace = true});
 auto output = execution.reserve({.bytes = page, .supplied_workspace = true, .completion_workspace = true, .workspace_class = CompileWorkClass::Output});
 CHECK(normal.workspace().data() != output.workspace().data());
 CHECK_THROWS_AS(execution.with_unused_workspace(output, 0, [] {}), std::invalid_argument);
 CHECK_FALSE(execution.try_reserve({.bytes = page, .supplied_workspace = true}));
 CHECK_FALSE(execution.try_reserve({.bytes = page, .supplied_workspace = true, .completion_workspace = true, .workspace_class = CompileWorkClass::Output}));
 auto annotation = execution.reserve({.bytes = page, .supplied_workspace = true, .completion_workspace = true, .workspace_class = CompileWorkClass::Annotations});
 auto shared = execution.reserve({.bytes = page, .supplied_workspace = true, .workspace_window = CompileWorkspaceWindow::SharedSource});
 auto compaction = execution.reserve({.bytes = page, .supplied_workspace = true, .workspace_window = CompileWorkspaceWindow::Compaction});
 CHECK(shared.workspace().data() != compaction.workspace().data());
 CHECK(execution.snapshot().normal_locked == plan->budget);
}
TEST_CASE("a source larger than its reserve waits for ordinary backing without exceptional commitment", "[benchmark][pipeline][resources]") {
 using namespace mmltk::backend::data::detail;
 const bool cancel = GENERATE(false, true);
 CAPTURE(cancel);
 const auto page = mmltk::common::system::host_page_size();
 // One-to-eight models the encoded reserve and the larger Parquet reader window.
 std::array<CompileWorkspaceProfile, 6> profiles{};
 profiles[static_cast<std::size_t>(CompileWorkClass::Extraction)].normal_bytes = page * 8;
 profiles[static_cast<std::size_t>(CompileWorkClass::Output)].completion_bytes = page;
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(1, profiles, page, page, page * 11));
 std::promise<void> parked;
 std::atomic<bool> observed{false};
 std::atomic<unsigned> calls{0};
 const std::byte* resumed = nullptr;
 CompileExecution execution(1, {}, {.workspace_plan = plan}, {}, [&](std::string_view, const CompileExecutionSnapshot& value) {
  if (value.parked[static_cast<std::size_t>(CompileParkReason::Bytes)] && !observed.exchange(true)) mmltk::testsupport::release_test_promise(parked);
 });
 auto input = execution.reserve({.bytes = page * 8, .supplied_workspace = true});
 auto* const base = input.workspace().data();
 base[0] = std::byte{53};
 auto source = execution.submit(
  {.kind = CompileTaskKind::Archive,
   .resources = {.bytes = page * 8, .supplied_workspace = true, .workspace_window = CompileWorkspaceWindow::SharedSource},
   .work =
    [&](std::size_t, const CompileAllowance& allowance) {
  resumed = allowance.workspace().data();
  CHECK(allowance.workspace()[0] == std::byte{53});
  ++calls;
 },
   .retain_workspace = false});
 const mmltk::testsupport::ScopedTestCleanup stop([&] { source.cancel(); });
 mmltk::testsupport::await_test_promise(parked, "reader window waits for the occupied ordinary arena");
 CHECK(calls == 0);
 CHECK(execution.snapshot().exceptional_reserved == 0);
 // A normal wait leaves unrelated protected work eligible; an oversized drain would block it.
 auto encoded = execution.try_reserve({.bytes = page, .supplied_workspace = true, .workspace_window = CompileWorkspaceWindow::SharedSource});
 auto compact = execution.try_reserve({.bytes = page, .supplied_workspace = true, .workspace_window = CompileWorkspaceWindow::Compaction});
 auto completion = execution.try_reserve({.bytes = page, .supplied_workspace = true, .completion_workspace = true, .workspace_class = CompileWorkClass::Output});
 REQUIRE(encoded);
 REQUIRE(compact);
 REQUIRE(completion);
 CHECK(execution.resource_usage().bytes == plan->budget);
 CHECK_FALSE(execution.try_reserve({.bytes = 1, .supplied_workspace = true}));
 if (cancel) {
  source.cancel();
  CHECK_THROWS(source.wait());
  CHECK(calls == 0);
  CHECK(input.workspace()[0] == std::byte{53});
 }
 input = {};
 if (!cancel) {
  source.wait();
  CHECK(calls == 1);
  CHECK(resumed == base);
 }
 auto next = execution.reserve({.bytes = page * 8, .supplied_workspace = true, .workspace_window = CompileWorkspaceWindow::SharedSource});
 CHECK(next.workspace().data() == base);
 CHECK(execution.snapshot().normal_locked == plan->budget);
 CHECK(execution.snapshot().exceptional_reserved == 0);
 CHECK(execution.snapshot().exceptional_peak == 0);
 CHECK(execution.snapshot().physical_retirements == 0);
}
// CLEANUP-IGNORE: Independent concurrent-borrow and lane-reassignment cases only share CPU eligibility and page-size setup.
TEST_CASE("independent source and compaction tasks borrow ordinary arenas simultaneously", "[benchmark][pipeline][resources]") {
 using namespace mmltk::backend::data::detail;
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 const auto page = mmltk::common::system::host_page_size();
 const std::array<CompileWorkspaceProfile, 1> profiles{{{page * 8, 0}}};
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(2, profiles, page, page, page * 18));
 mmltk::testsupport::TestGate source_entered("ordinary source arena"), compaction_entered("ordinary compaction arena");
 std::array<std::byte*, 2> addresses{};
 CompileExecution execution(2, {}, {.workspace_plan = plan});
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  source_entered.Release();
  compaction_entered.Release();
 });
 auto source = execution.submit(
  {.kind = CompileTaskKind::Archive,
   .resources = {.bytes = page * 8, .supplied_workspace = true, .workspace_window = CompileWorkspaceWindow::SharedSource},
   .work =
    [&](std::size_t, const CompileAllowance& allowance) {
  addresses[0] = allowance.workspace().data();
  addresses[0][0] = std::byte{11};
  source_entered.receipt().ArriveAndWait();
  CHECK(allowance.workspace()[0] == std::byte{11});
 },
   .retain_workspace = false});
 auto compaction = execution.submit(
  {.kind = CompileTaskKind::Output,
   .resources = {.bytes = page * 8, .supplied_workspace = true, .workspace_window = CompileWorkspaceWindow::Compaction},
   .work =
    [&](std::size_t, const CompileAllowance& allowance) {
  addresses[1] = allowance.workspace().data();
  addresses[1][0] = std::byte{29};
  compaction_entered.receipt().ArriveAndWait();
  CHECK(allowance.workspace()[0] == std::byte{29});
 },
   .retain_workspace = false});
 REQUIRE(source_entered.WaitEntered(2s));
 REQUIRE(compaction_entered.WaitEntered(2s));
 CHECK(addresses[0] != addresses[1]);
 CHECK(execution.snapshot().active == 2);
 CHECK(execution.snapshot().leased == page * 16);
 CHECK(execution.snapshot().exceptional_reserved == 0);
 auto encoded = execution.try_reserve({.bytes = page, .supplied_workspace = true, .workspace_window = CompileWorkspaceWindow::SharedSource});
 auto compact = execution.try_reserve({.bytes = page, .supplied_workspace = true, .workspace_window = CompileWorkspaceWindow::Compaction});
 REQUIRE(encoded);
 REQUIRE(compact);
 CHECK_FALSE(execution.try_reserve({.bytes = 1, .supplied_workspace = true}));
 source_entered.Release();
 compaction_entered.Release();
 source.wait();
 compaction.wait();
 CHECK(execution.snapshot().leased == page * 2);
 CHECK(execution.snapshot().normal_locked == plan->budget);
 CHECK(execution.snapshot().exceptional == 0);
}
TEST_CASE("cancelled oversized admission leaves live output custody and fixed backing intact", "[benchmark][pipeline][resources]") {
 using namespace mmltk::backend::data::detail;
 const auto page = mmltk::common::system::host_page_size();
 std::promise<void> parked;
 std::atomic<bool> observed{false};
 CompileExecution execution(1, {}, {.transient_bytes = page}, {}, [&](std::string_view, const CompileExecutionSnapshot& value) {
  if (value.parked[static_cast<std::size_t>(CompileParkReason::Bytes)] && !observed.exchange(true)) mmltk::testsupport::release_test_promise(parked);
 });
 auto output = execution.reserve({.bytes = page, .supplied_workspace = true});
 auto* const base = output.workspace().data();
 base[0] = std::byte{42};
 std::atomic<unsigned> calls{0};
 auto oversized = execution.submit(
  {.kind = CompileTaskKind::Pixels, .resources = {.bytes = page * 3, .supplied_workspace = true}, .work = [&](std::size_t, const CompileAllowance&) { ++calls; }, .retain_workspace = false});
 mmltk::testsupport::await_test_promise(parked, "oversized work drains a live output lease");
 CHECK_FALSE(execution.try_reserve({1, 0}));
 oversized.cancel();
 CHECK_THROWS(oversized.wait());
 CHECK(calls == 0);
 CHECK(output.workspace()[0] == std::byte{42});
 CHECK(execution.snapshot().exceptional_reserved == 0);
 output = {};
 auto normal = execution.reserve({.bytes = page, .supplied_workspace = true});
 CHECK(normal.workspace().data() == base);
 CHECK(execution.snapshot().normal_locked == page);
}
// CLEANUP-IGNORE: Independent arena cases select the same window parameters and page size; their execution and ownership assertions differ.
TEST_CASE("exceptional finite descendants partition one pinned extent until last custody", "[benchmark][pipeline][resources]") {
 using namespace mmltk::backend::data::detail;
 const auto window = GENERATE(CompileWorkspaceWindow::Task, CompileWorkspaceWindow::SharedSource, CompileWorkspaceWindow::Compaction);
 CAPTURE(window);
 const auto page = mmltk::common::system::host_page_size();
 CompileExecution execution(1, {}, {.transient_bytes = page}, {}, [](std::string_view, const CompileExecutionSnapshot&) {});
 auto envelope = execution.reserve({.bytes = page * 3, .supplied_workspace = true, .workspace_window = window});
 auto* const base = envelope.workspace().data();
 auto input = envelope.split_storage(page), output = envelope.split_storage(page);
 CHECK(input.workspace().data() == base);
 CHECK(output.workspace().data() == base + page);
 CHECK(envelope.workspace().data() == base + page * 2);
 execution
  .submit(
   {.kind = CompileTaskKind::Output,
    .resources = {.bytes = page, .supplied_workspace = true, .workspace_window = window},
    .work = [&](std::size_t, const CompileAllowance& own) { own.workspace()[0] = std::byte{19}; },
    .allowance = output,
    .retain_workspace = false})
  .wait();
 envelope = {};
 input = {};
 CHECK(execution.snapshot().exceptional == page * 3);
 CHECK(output.workspace()[0] == std::byte{19});
 CHECK_FALSE(execution.try_reserve({.bytes = 1, .supplied_workspace = true}));
 output = {};
 CHECK(execution.snapshot().exceptional == 0);
 CHECK(execution.snapshot().physical_retirements == 1);
 CHECK(execution.reserve({.bytes = page, .supplied_workspace = true}).workspace().data() != nullptr);
}
TEST_CASE("Directory supplied codec decoding uses stable pinned storage and preserves decoded bytes", "[backend][data][writer]") {
 using namespace mmltk::backend::data::detail;
 constexpr int width = 3, height = 2;
 const std::array<std::uint8_t, 18> rgb{1, 2, 3, 4, 5, 6, 7, 8, 9, 19, 20, 21, 40, 50, 60, 70, 80, 90};
 std::vector<std::uint8_t> encoded;
 SECTION("BMP") {
  // BITMAPINFOHEADER, uncompressed BGR rows stored bottom-up with 4-byte padding.
  encoded = {
   'B', 'M', 78, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0, 0, 24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 21, 20, 19, 60, 50, 40,
   90, 80, 70, 0, 0, 0, 3, 2, 1, 6, 5, 4, 9, 8, 7, 0, 0, 0
  };
 }
 SECTION("TGA") {
  // Uncompressed 24-bit truecolor with a top-left origin and tightly packed BGR.
  encoded = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 2, 0, 24, 0x20, 3, 2, 1, 6, 5, 4, 9, 8, 7, 21, 20, 19, 60, 50, 40, 90, 80, 70};
 }
 SECTION("PNG") { REQUIRE(stbi_write_png_to_func(append_bytes, &encoded, width, height, 3, rgb.data(), width * 3)); }
 CompileImageDecoder decoder;
 const auto header = decoder.read_header(encoded, CompileImagePolicy::Directory);
 // CLEANUP-IGNORE: Independent decoder cases construct execution from the checked workspace size; their operations and assertions differ.
 const auto bytes = CompileImageDecoder::plan(header).bytes();
 CompileExecution execution(1, {}, {.transient_bytes = bytes}, {}, [](std::string_view, const CompileExecutionSnapshot&) {});
 const void* base = nullptr;
 for (auto kind : {CompileTaskKind::Pixels, CompileTaskKind::Normalize}) {
  execution
   .submit(
    {.kind = kind,
     .resources = {.bytes = bytes, .supplied_workspace = true},
     .work =
      [&](std::size_t, const CompileAllowance& allowance) {
   const auto memory = allowance.workspace();
   if (base)
    CHECK(memory.data() == base);
   else
    base = memory.data();
   CHECK(std::ranges::equal(decoder.decode_rgb(encoded, header, memory), rgb));
  },
     .retain_workspace = false})
   .wait();
 }
 CHECK(execution.snapshot().workspace_reuses >= 1);
}
TEST_CASE("a prepared arena changes work class and CPU lane without moving its backing", "[benchmark][pipeline][resources]") {
 using namespace mmltk::backend::data::detail;
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 const auto page = mmltk::common::system::host_page_size();
 CompileExecution execution(2, {}, {.transient_bytes = page});
 auto pixels = execution.open_class(CompileWorkClass::Pixels), annotations = execution.open_class(CompileWorkClass::Annotations);
 const void* base = nullptr;
 std::array<std::size_t, 2> lanes{};
 std::size_t turn = 0;
 for (auto kind : {CompileTaskKind::Pixels, CompileTaskKind::Normalize}) {
  execution
   .submit(
    {.kind = kind,
     .resources = {.bytes = page, .supplied_workspace = true},
     .work =
      [&](std::size_t lane, const CompileAllowance& allowance) {
   lanes[turn++] = lane;
   const auto memory = allowance.workspace();
   if (base) {
    CHECK(memory.data() == base);
    CHECK(memory[0] == std::byte{31});
   } else {
    base = memory.data();
    memory[0] = std::byte{31};
   }
  },
     .retain_workspace = false})
   .wait();
 }
 CHECK(lanes[0] != lanes[1]);
 CHECK(execution.snapshot().normal_locked == page);
 CHECK(execution.snapshot().workspace_contexts == 1);
}
TEST_CASE("Directory supplied JPEG GIF and 16-bit PNM match the existing STB codec policy", "[backend][data][writer]") {
 using namespace mmltk::backend::data::detail;
 static_assert(noexcept(CompileImageDecoder{}));
 static_assert(noexcept(BenchmarkImageDecoder{}));
 std::vector<std::uint8_t> encoded;
 SECTION("JPEG") { encoded = make_jpeg(17, 83, 149); }
 SECTION("GIF") { encoded = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0, 0, 0, 255, 255, 255, 0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 1, 0, 0x3b}; }
 SECTION("16-bit PNM") { encoded = {'P', '6', '\n', '1', ' ', '1', '\n', '6', '5', '5', '3', '5', '\n', 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}; }
 int width = 0, height = 0, channels = 0;
 std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> reference(stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 3), stbi_image_free);
 REQUIRE(reference);
 CompileImageDecoder decoder;
 CompileImageHeader header;
 const std::array<std::uint8_t, 4> malformed{0, 1, 2, 3};
 {
  DenyStbHeapAllocations denied;
  int inspected_width = 0, inspected_height = 0, inspected_channels = 0;
  REQUIRE(stbi_info_from_memory(encoded.data(), static_cast<int>(encoded.size()), &inspected_width, &inspected_height, &inspected_channels));
  CHECK(inspected_width == width);
  CHECK(inspected_height == height);
  CHECK(inspected_channels == channels);
  header = decoder.read_header(encoded, CompileImagePolicy::Directory);
  CHECK(header.width == static_cast<std::uint32_t>(width));
  CHECK(header.height == static_cast<std::uint32_t>(height));
  CHECK(header.decoder_bytes == stbi_workspace_required(encoded.data(), static_cast<int>(encoded.size()), 3));
  CHECK_THROWS_AS(decoder.read_header(malformed, CompileImagePolicy::Directory), CompileImageError);
  CHECK(stbi_workspace_required(malformed.data(), static_cast<int>(malformed.size()), 3) == 0);
  CHECK(std::string_view(stbi_failure_reason()) != "workspace capacity");
  CHECK(denied.attempts == 0);
  // Prove the guard is active on this thread's ordinary decoding allocator.
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> rejected(
   stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &inspected_width, &inspected_height, &inspected_channels, 3), stbi_image_free);
  CHECK_FALSE(rejected);
  CHECK(denied.attempts > 0);
 }
 const auto bytes = CompileImageDecoder::plan(header).bytes();
 CompileExecution execution(1, {}, {.transient_bytes = bytes}, {}, [](std::string_view, const CompileExecutionSnapshot&) {});
 execution
  .submit(
   {.kind = CompileTaskKind::Pixels,
    .resources = {.bytes = bytes, .supplied_workspace = true},
    .work =
     [&](std::size_t, const CompileAllowance& allowance) {
  DenyStbHeapAllocations denied;
  const auto workspace = allowance.workspace();
  CHECK_THROWS_AS(decoder.decode_rgb(encoded, header, workspace.first(bytes - 1)), std::length_error);
  CHECK(std::ranges::equal(decoder.decode_rgb(encoded, header, workspace), std::span(reference.get(), static_cast<std::size_t>(width) * height * 3)));
  CHECK_THROWS_AS(decoder.decode_rgb(malformed, header, workspace), CompileImageError);
  CHECK(denied.attempts == 0);
 },
    .retain_workspace = false})
  .wait();
}
TEST_CASE("sealed pixel plans validate custody and geometry before writing the destination", "[backend][data][writer]") {
 using namespace mmltk::backend::data::detail;
 using namespace mmltk::backend::imaging::resample;
 const auto mode = GENERATE(ImageResizeMode::Stretch, ImageResizeMode::Letterbox);
 const auto perceptual = GENERATE(false, true);
 const std::array<std::uint8_t, 24> rgb{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 19, 20, 21, 40, 50, 60, 70, 80, 90, 110, 120, 130};
 std::vector<std::uint8_t> encoded;
 REQUIRE(stbi_write_png_to_func(append_bytes, &encoded, 4, 2, 3, rgb.data(), 12));
 CompileImageDecoder decoder;
 const auto header = decoder.read_header(encoded, CompileImagePolicy::Benchmark);
 const auto plan = compile_pixel_workspace_plan(header, 2, mode, perceptual);
 CompileExecution execution(1, {}, {.transient_bytes = plan.bytes()});
 CompilePixelWorkspace workspace;
 unsigned prepared = 0;
 std::vector<float> output(12, -1.0F), reference(12, -1.0F);
 auto destination = RgbMutableImageView{output.data(), plan.destination()};
 execution
  .submit(
   {.kind = CompileTaskKind::Pixels,
    .resources = plan.resources(),
    .work =
     [&](std::size_t, const CompileAllowance& allowance) {
  const auto memory = allowance.workspace();
  auto wrong_header = header;
  ++wrong_header.width;
  CHECK_THROWS_AS(workspace.write(plan, encoded, wrong_header, memory, destination), CompileImageError);
  CHECK_THROWS_AS(workspace.write(plan, encoded, header, memory.first(plan.bytes() - 1), destination), std::length_error);
  auto wrong_destination = destination;
  ++wrong_destination.layout.width;
  CHECK_THROWS_AS(workspace.write(plan, encoded, header, memory, wrong_destination), std::invalid_argument);
  wrong_destination = destination;
  --wrong_destination.layout.capacity_bytes;
  CHECK_THROWS_AS(workspace.write(plan, encoded, header, memory, wrong_destination), std::invalid_argument);
  std::vector<std::uint8_t> changed;
  REQUIRE(stbi_write_png_to_func(append_bytes, &changed, 2, 4, 3, rgb.data(), 6));
  CHECK_THROWS_AS(workspace.write(plan, changed, header, memory, destination, [&] { ++prepared; }), CompileImageError);
  CHECK(prepared == 0);
  CHECK(std::ranges::all_of(output, [](float value) { return value == -1.0F; }));
  workspace.write(plan, encoded, header, memory, destination, [&] { ++prepared; });
  CHECK(prepared == 1);
 },
    .retain_workspace = false})
  .wait();
 RgbImageResizer resizer(1, perceptual);
 resizer.resize_to_planar({rgb.data(), {4, 2, 12, 0, rgb.size(), RgbPixelFormat::RGB8}}, {reference.data(), plan.destination()}, mode);
 CHECK(output == reference);
}
TEST_CASE("pixel preparation permits independent publication completion and retirement", "[backend][data][benchmark][pipeline]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires three assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("pixel-plan-independent-facts");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 BenchmarkEncodedImage::publish(cached_image_path(images, 2), make_jpeg(8, 240, 8), {});
 split.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 0}};
 BenchmarkCompilePipeline execution(3, {}, {.transient_bytes = 16U << 20}, {}, mmltk::backend::data::testsupport::benchmark_trace());
 FileHandle directory(::open(images.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
 REQUIRE(directory.get() >= 0);
 auto opened = BenchmarkEncodedImage::open_deferred(directory.get(), 1, {}, execution.execution().reserve(CompileResources::handles(1)));
 REQUIRE(opened);
 auto payload = opened->try_read(&execution.execution(), {});
 REQUIRE(payload);
 REQUIRE(payload->charged_bytes());
 opened.reset();
 mmltk::testsupport::TestGate preparation("first image pixel plan");
 std::array<std::atomic<unsigned>, 2> plans{};
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8);
 request.execution = &execution;
 request.pixel_preparing = [&](const auto&, std::uint64_t id) {
  ++plans.at(id - 1);
  if (id == 1) preparation.receipt().ArriveAndWait();
 };
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 std::future<void> independent;
 const mmltk::testsupport::ScopedTestCleanup release([&] { preparation.Release(); });
 const auto publication = execution.source_publication(images, {});
 publication({1, {}, true, std::move(payload)});
 REQUIRE(preparation.WaitEntered(5s));
 independent = std::async(std::launch::async, [&] {
  publication({2, {}, true});
  execution.image_completion(images, 2).wait();
  execution.retire_image(images, 2);
 });
 mmltk::testsupport::await_test_future(independent, "unrelated image completes and retires during pixel planning", 5s);
 CHECK_FALSE(writer.image_complete(0));
 CHECK_FALSE(writer.image_complete(1));
 CHECK(plans[0] == 1);
 CHECK(plans[1] == 1);
 preparation.Release();
 execution.drain();
 CHECK(writer.image_complete(0));
}
TEST_CASE("active pixel preparation drains before cancellation repair or writer retirement", "[backend][data][benchmark][pipeline]") {
 enum class Retire { Cancel, Image, Source, Attempt };
 const auto retirement = GENERATE(Retire::Cancel, Retire::Image, Retire::Source, Retire::Attempt);
 mmltk::testsupport::ScopedTempDir root("pixel-plan-retirement");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 std::atomic<bool> cancelled{false};
 const auto cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(cancelled);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = 16U << 20}, cancellation);
 mmltk::testsupport::TestGate preparation("admitted pixel preparation");
 std::atomic<unsigned> plans{0};
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8, cancellation);
 request.execution = &execution;
 request.pixel_preparing = [&](const auto&, auto) {
  if (++plans == 1) preparation.receipt().ArriveAndWait();
 };
 auto writer = std::make_unique<BenchmarkSplitWriter>(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, *writer, split);
 const auto publication = execution.source_publication(images, {});
 std::future<void> retiring;
 const mmltk::testsupport::ScopedTestCleanup release([&] { preparation.Release(); });
 publication({1, {}, true});
 REQUIRE(preparation.WaitEntered(5s));
 const auto before = execution.execution().admission_generation();
 retiring = std::async(std::launch::async, [&] {
  switch (retirement) {
   case Retire::Cancel:
    cancelled = true;
    execution.execution().notify_admission_change();
    execution.retire_attempt();
    break;
   case Retire::Image: execution.retire_image(images, 1); break;
   case Retire::Source: execution.retire_source(images); break;
   case Retire::Attempt:
    execution.retire_attempt();
    writer.reset();
    break;
  }
 });
 if (retirement == Retire::Cancel)
  CHECK_THROWS_WITH(execution.execution().wait_for_admission_change(before), "benchmark dataset compilation cancelled");
 else if (retirement == Retire::Attempt)
  CHECK_THROWS_WITH(execution.execution().wait_for_admission_change(before), "compile admission after shutdown");
 else
  execution.execution().wait_for_admission_change(before);
 CHECK(retiring.wait_for(0ms) == std::future_status::timeout);
 CHECK_FALSE(writer->image_complete(0));
 preparation.Release();
 mmltk::testsupport::await_test_future(retiring, "retirement joins admitted pixel preparation", 5s);
 if (retirement == Retire::Cancel) {
  CHECK_FALSE(writer->image_complete(0));
  CHECK_FALSE(writer->header_dimensions(0));
  return;
 }
 if (!writer) {
  writer = std::make_unique<BenchmarkSplitWriter>(request);
  bind_benchmark_slots(execution, *writer, split);
 }
 CHECK_FALSE(writer->image_complete(0));
 CHECK_FALSE(writer->header_dimensions(0));
 publication({1, {}, true});
 execution.drain();
 CHECK(plans == 1);
 const auto replacement = execution.source_publication(images, {}, 1);
 CHECK(replacement.consume({1}));
 CHECK(writer->image_complete(0));
 CHECK(plans == 2);
}
TEST_CASE("pixel admission releases pooled input and reuses its plan through file fallback", "[backend][data][benchmark][pipeline][resources]") {
 using namespace mmltk::backend::data::detail;
 const bool mapped_file = GENERATE(false, true);
 const bool record_pressure = GENERATE(false, true);
 mmltk::testsupport::ScopedTempDir root("pixel-plan-file-fallback");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 const auto encoded = make_jpeg(11, 22, 33);
 const auto header = BenchmarkImageDecoder{}.read_header(encoded);
 const auto page = mmltk::common::system::host_page_size();
 REQUIRE(encoded.size() < page);
 REQUIRE(compile_pixel_workspace_plan(header, 8, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, false).bytes() > page);
 BenchmarkCompilePipeline execution(1, {}, {.transient_bytes = page, .descriptors = 8, .task_records = 1});
 auto backing = std::make_shared<CompileAllowance>(execution.execution().reserve({.bytes = page, .supplied_workspace = true}));
 const auto memory = backing->workspace();
 std::memcpy(memory.data(), encoded.data(), encoded.size());
 const std::weak_ptr<CompileAllowance> input_lifetime = backing;
 auto file = BenchmarkEncodedImage::publish(cached_image_path(images, 1), encoded, {}, nullptr, header, {}, mapped_file);
 auto payload = BenchmarkEncodedImage::pooled(*backing, backing, {reinterpret_cast<const std::uint8_t*>(memory.data()), encoded.size()}, std::move(file));
 backing.reset();
 mmltk::testsupport::TestGate preparation("pooled input preparation"), occupied("bounded Header record is occupied");
 std::future<void> header_record;
 std::atomic<unsigned> plans{0}, reads{0};
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8);
 request.execution = &execution;
 request.pixel_preparing = [&](const auto&, auto) {
  ++plans;
  preparation.receipt().ArriveAndWait();
 };
 request.image_opened = [&](const auto&, auto) {
  ++reads;
  CHECK(input_lifetime.expired());
 };
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(execution);
 bind_benchmark_slots(execution, writer, split);
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  preparation.Release();
  occupied.Release();
 });
 if (record_pressure) {
  header_record = std::async(std::launch::async, [&] { execution.execution().run(CompileTaskKind::Header, {}, [&](std::size_t) { occupied.receipt().ArriveAndWait(); }); });
  REQUIRE(occupied.WaitEntered(5s));
 }
 execution.source_publication(images, {})({1, {}, true, std::move(payload)});
 if (record_pressure) {
  CHECK(input_lifetime.expired());
  CHECK(plans == 0);
  if (mapped_file) REQUIRE(fs::remove(cached_image_path(images, 1)));
  occupied.Release();
  mmltk::testsupport::await_test_future(header_record, "bounded Header record becomes available", 5s);
 } else {
  REQUIRE(preparation.WaitEntered(5s));
  CHECK_FALSE(input_lifetime.expired());
  if (mapped_file) REQUIRE(fs::remove(cached_image_path(images, 1)));
 }
 REQUIRE(preparation.WaitEntered(5s));
 preparation.Release();
 execution.image_completion(images, 1).wait();
 CHECK(input_lifetime.expired());
 CHECK(writer.image_complete(0));
 CHECK(plans == 1);
 CHECK(reads == 1);
 CHECK(execution.execution().snapshot().exceptional == 0);
 CHECK(execution.execution().snapshot().normal_locked == page);
}
TEST_CASE("standalone pixel sessions clamp both CPUs and useful images before preparation", "[backend][data][benchmark][writer]") {
 const auto available = mmltk::common::system::allowed_cpu_set();
 REQUIRE_FALSE(available.empty());
 bool cpu_limit = false;
 SECTION("available CPU limit") { cpu_limit = true; }
 SECTION("unfinished image limit") {}
 mmltk::testsupport::ScopedTempDir root("standalone-pixel-worker-limit");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 if (cpu_limit) {
  BenchmarkEncodedImage::publish(cached_image_path(images, 2), make_jpeg(11, 22, 33), {});
  split.images.push_back({2, 16, 8, 0, 0});
 }
 const auto compile_threads = [] {
  std::size_t result = 0;
  for (const auto& task : fs::directory_iterator("/proc/self/task")) {
   std::ifstream input(task.path() / "comm");
   std::string name;
   std::getline(input, name);
   if (name.starts_with("compile") && name.size() > 7 && name[7] >= '0' && name[7] <= '9') ++result;
  }
  return result;
 };
 const auto before = compile_threads();
 std::size_t prepared = 0;
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8);
 request.num_workers = 3;
 request.worker_cpus = cpu_limit ? std::span<const int>(available).first(1) : std::span<const int>(available);
 request.pixel_preparing = [&](const auto&, auto) {
  ++prepared;
  CHECK(compile_threads() == before + 1);
 };
 BenchmarkSplitWriter writer(request);
 writer.write_remaining(request);
 CHECK(writer.completed() == split.images.size());
 CHECK(prepared == split.images.size());
 writer.write_remaining(request);
 CHECK(prepared == split.images.size());
 CHECK(compile_threads() == before);
}
TEST_CASE("standalone pixel failure drains held completion and retains completed slots", "[backend][data][benchmark][writer]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("standalone-pixel-failure-drain");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 BenchmarkEncodedImage::publish(cached_image_path(images, 2), make_jpeg(11, 22, 33), {});
 split.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 0}};
 mmltk::testsupport::TestGate completion("completed standalone slot retains its call");
 std::promise<void> failing;
 std::atomic<bool> fail{true};
 std::array<std::atomic<unsigned>, 2> reads{};
 auto request = benchmark_write_request(split, root.path() / "pixels.bin", 8);
 request.image_opened = [&](const auto&, std::uint64_t id) {
  ++reads.at(id - 1);
  if (id == 2 && fail) {
   if (!completion.WaitEntered(5s)) throw std::runtime_error("first standalone completion did not enter");
   failing.set_value();
   throw std::runtime_error("injected standalone input failure");
  }
 };
 request.progress = {.context = &completion, .image_completed = [](void* context) { static_cast<mmltk::testsupport::TestGate*>(context)->receipt().ArriveAndWait(); }};
 BenchmarkSplitWriter writer(request);
 auto writing = std::async(std::launch::async, [&] { writer.write_remaining(request); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { completion.Release(); });
 mmltk::testsupport::await_test_promise(failing, "second standalone input fails after first completion enters", 5s);
 CHECK(writing.wait_for(0ms) == std::future_status::timeout);
 CHECK(writer.image_complete(0));
 CHECK_FALSE(writer.image_complete(1));
 CHECK_FALSE(fs::exists(request.output_path));
 completion.Release();
 CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(writing, "standalone failure joins physical users", 5s), "injected standalone input failure");
 fail = false;
 writer.write_remaining(request);
 CHECK(writer.completed() == 2);
 CHECK(reads[0] == 1);
 CHECK(reads[1] == 2);
}
TEST_CASE("held occupied extent preparation leaves source claims and unrelated completion available", "[backend][data][benchmark][pipeline][storage]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("prepared-slot-independence");
 auto split = cached_pixel_membership(root.path() / "images");
 split.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 0}};
 BenchmarkEncodedImage::publish(cached_image_path(split.sources.front().root, 1), make_jpeg(128, 64, 32), {});
 BenchmarkEncodedImage::publish(cached_image_path(split.sources.front().root, 2), make_jpeg(32, 64, 128), {});
 BenchmarkCompilePipeline pipeline(4);
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8);
 request.execution = &pipeline;
 mmltk::testsupport::TestGate preparation("first occupied extent awaits physical preparation");
 request.extent_preparing = [&](const fs::path&, std::uint64_t id) {
  if (id == 1) preparation.receipt().ArriveAndWait();
 };
 BenchmarkSplitWriter writer(request, true);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 bind_benchmark_slots(pipeline, writer, split);
 const mmltk::testsupport::ScopedTestCleanup release([&] { preparation.Release(); });
 auto publication = pipeline.source_publication(split.sources.front().root, {});
 publication({1});
 REQUIRE(preparation.WaitEntered(2s));
 CHECK(writer.allocate_slot({.source_image_id = 3}, {split.sources.front().root}) == 2);
 publication({2});
 auto second = pipeline.image_completion(split.sources.front().root, 2);
 std::promise<void> completed;
 second.subscribe([&](std::exception_ptr error) {
  if (error)
   completed.set_exception(error);
  else
   completed.set_value();
 });
 mmltk::testsupport::await_test_promise(completed, "independent pixel completion during held extent preparation");
 CHECK(writer.image_complete(1));
 CHECK_FALSE(writer.image_complete(0));
 preparation.Release();
 pipeline.drain();
 CHECK(writer.image_complete(0));
}
TEST_CASE("occupied allocation distinguishes overwrite and reclaimed growth", "[backend][data][benchmark][storage]") {
 mmltk::testsupport::ScopedTempDir root("occupied-page-mutations");
 detail::StorageReservationPool reservations(root.path());
 auto stage = detail::StagedArtifact::create(reservations, root.path() / "pixels", 0, "occupied fixture");
 stage.resize_sparse(32768);
 stage.allocate_range(4096, 8192);
 const auto before = detail::StorageReservationPool::allocated_bytes(stage.file().get());
 stage.file().pwrite_all("pixels", 6, 4096);
 stage.allocate_range(4096, 4096);
 CHECK(detail::StorageReservationPool::allocated_bytes(stage.file().get()) == before);
 CHECK(reservations.outstanding() == 0);
 const bool punched = stage.reclaim_range(4096, 4096);
 if (punched) CHECK(detail::StorageReservationPool::allocated_bytes(stage.file().get()) < before);
 stage.allocate_range(4096, 4096);
 CHECK(reservations.outstanding() == 0);
 stage.resize_sparse(4096);
 CHECK(stage.file().size() == 4096);
 CHECK(reservations.outstanding() == 0);
 stage.file() = {};
 CHECK_THROWS(stage.allocate_range(16384, 4096));
 CHECK_THROWS(stage.reclaim_range(0, 4096));
 CHECK_THROWS(stage.resize_sparse(0));
}
TEST_CASE("file output retains pinned labels until physical completion through cancellation", "[backend][data][benchmark][storage]") {
 using detail::FileWorkQueue;
 mmltk::testsupport::ScopedTempDir root("label-output-custody");
 detail::StorageReservationPool reservations(root.path());
 auto stage = detail::StagedArtifact::create(reservations, root.path() / "labels", 0, "label output");
 detail::CompiledSlotStorage storage(stage, compute_file_layout(2, 12));
 detail::CompiledLabelWorkspace workspace;
 workspace.prepare(0);
 PackedInstance label{};
 label.bbox_x2 = 1;
 label.bbox_y2 = 1;
 label.source_ordinal = 7;
 workspace.labels.push_back(label);
 auto payload = std::make_shared<detail::CompiledLabelWorkspace::OutputLease>(workspace.release_output());
 const std::weak_ptr<const mmltk::common::system::NumaMemory> backing = payload->owned;
 ImageEntry entry{};
 entry.status = ImageStatus::Eligible;
 entry.original_width = entry.original_height = 1;
 set_annotation_extent(entry, 1, 0, "labels");
 CompileExecution execution(1);
 FileWorkQueue files(execution, 1);
 mmltk::testsupport::TestGate physical("borrowed label write in progress");
 const mmltk::testsupport::ScopedTestCleanup release([&] { physical.Release(); });
 auto output = files.submit({.work = [&, payload] {
  physical.receipt().ArriveAndWait();
  storage.labels(0).write(entry, std::move(*payload));
 }});
 REQUIRE(physical.WaitEntered(2s));
 payload.reset();
 workspace.prepare(0);
 CHECK_FALSE(backing.expired());
 files.cancel();
 CHECK_FALSE(output.ready());
 physical.Release();
 output.wait();
 files.join();
 CHECK(backing.expired());
 storage.labels(0).settle_ordinals(1, 11, true);
 PackedInstance stored{};
 stage.file().pread_all(&stored, sizeof(stored), storage.layout().label_offset(0));
 CHECK(stored.source_ordinal == 18);
 CHECK((stored.flags & kAnnotationOrdinalIncomplete) != 0);
 storage.prepare_pixels(0);
 auto pixel = storage.pixels(0);
 pixel.data()[0] = 0.75F;
 workspace.prepare(0);
 label.source_ordinal = 99;
 label.flags = kAnnotationMask;
 label.mask_rle_pairs = 1;
 workspace.labels.push_back(label);
 workspace.runs.push_back({0, 1});
 set_annotation_extent(entry, 1, 1, "replacement labels");
 storage.labels(0).write(entry, workspace.release_output());
 stage.file().pread_all(&stored, sizeof(stored), storage.layout().label_offset(0));
 CHECK(stored.source_ordinal == 99);
 RLEPair run{};
 stage.file().pread_all(&run, sizeof(run), storage.layout().label_offset(0) + annotation_layout(1, 1).run_offset);
 CHECK(run.start == 0);
 CHECK(run.length == 1);
 CHECK(pixel.data()[0] == 0.75F);
}
TEST_CASE("split sealing joins a physical peer before propagating failure", "[backend][data][benchmark][writer][storage]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("split-seal-peer-custody");
 const auto output = root.path() / "published.bin";
 write_text(output, "previous generation");
 const auto previous = mmltk::common::io::sha256_file(output);
 const std::array<detail::CompileWorkspaceProfile, 1> profiles{{{COMPILED_COMPACTION_SCRATCH_BYTES, 0}}};
 auto plan = std::make_shared<detail::CompileWorkspacePlan>(detail::CompileWorkspacePlan::prepare(2, profiles, 0, COMPILED_COMPACTION_SCRATCH_BYTES, COMPILED_COMPACTION_SCRATCH_BYTES * 3));
 BenchmarkCompilePipeline pipeline(2, {}, {.workspace_plan = plan});
 detail::StorageReservationPool storage(root.path(), {}, &pipeline.storage());
 mmltk::testsupport::TestGate physical("first seal owns its inode and pinned scratch");
 std::promise<void> failed;
 fs::path temporary;
 std::atomic<bool> physically_complete{false};
 auto sealing = std::async(std::launch::async, [&] {
  return seal_benchmark_splits(pipeline, [&](std::size_t split, std::span<std::byte> scratch) -> BenchmarkSealedSplit {
   if (split) {
    if (!physical.WaitEntered(5s)) throw std::runtime_error("first split seal did not enter");
    failed.set_value();
    throw std::runtime_error("injected peer seal failure");
   }
   auto artifact = detail::StagedArtifact::create(storage, output, 4096, "held split seal");
   temporary = artifact.path();
   artifact.preallocate(4096);
   scratch.front() = std::byte{37};
   physical.receipt().ArriveAndWait();
   if (scratch.front() != std::byte{37}) throw std::runtime_error("split scratch custody changed");
   artifact.file().pwrite_all(scratch.data(), 1, 0);
   artifact.close();
   physically_complete = true;
   return {std::move(artifact), {}};
  });
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] { physical.Release(); });
 mmltk::testsupport::await_test_promise(failed, "peer seal fails while first split retains physical custody", 5s);
 CHECK(sealing.wait_for(0ms) == std::future_status::timeout);
 CHECK_FALSE(physically_complete.load());
 CHECK(fs::exists(temporary));
 CHECK(pipeline.execution().snapshot().exceptional == 0);
 physical.Release();
 CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(sealing, "failed split joins its admitted peer", 5s), "injected peer seal failure");
 CHECK(physically_complete.load());
 CHECK_FALSE(fs::exists(temporary));
 CHECK(storage.outstanding() == 0);
 CHECK(mmltk::common::io::sha256_file(output) == previous);
}
TEST_CASE("completion windows drain independent borrowers after a one-time wait hook failure", "[backend][data][benchmark][pipeline]") {
 enum class Finish { Drain, WaitOne, MakeRoom, Destruction };
 const auto finish = GENERATE(Finish::Drain, Finish::WaitOne, Finish::MakeRoom, Finish::Destruction);
 const bool owned = GENERATE(false, true);
 CAPTURE(finish, owned);
 auto execution = owned ? std::make_unique<CompileExecution>(1) : nullptr;
 CompileExecution::CompletionSource held(execution.get()), first(execution.get()), second(execution.get());
 const auto initial = std::make_exception_ptr(std::runtime_error("initial wait hook failure"));
 const auto later = std::make_exception_ptr(std::runtime_error("later physical failure"));
 std::array<std::atomic<unsigned>, 3> callbacks{};
 std::atomic<unsigned> hooks{0};
 std::promise<void> waiting, first_settled, second_settled;
 std::weak_ptr<int> borrowed;
 auto draining = std::async(std::launch::async, [&] {
  BenchmarkCompletionWindow window(3, execution.get(), [&] {
   ++hooks;
   mmltk::testsupport::release_test_promise(waiting);
   std::rethrow_exception(initial);
  });
  auto owner = std::make_shared<int>(41);
  borrowed = owner;
  window.push(held.completion(), [&, owner](std::exception_ptr error) {
   ++callbacks[0];
   require_condition(held.completion().ready() && *owner == 41, "held borrower lost physical custody");
   require_condition(error == later, "physical failure did not reach its settlement callback");
   throw std::runtime_error("later held callback failure");
  });
  owner.reset();
  window.push(first.completion(), [&](std::exception_ptr error) {
   ++callbacks[1];
   require_condition(!error, "healthy independent completion changed");
   first_settled.set_value();
   throw std::runtime_error("later ready callback failure");
  });
  window.push(second.completion(), [&](std::exception_ptr error) {
   ++callbacks[2];
   require_condition(!error, "second independent completion changed");
   second_settled.set_value();
  });
  try {
   switch (finish) {
    case Finish::Drain: window.drain(); break;
    case Finish::WaitOne: window.wait_one(); break;
    case Finish::MakeRoom: window.make_room(); break;
    case Finish::Destruction: return;
   }
  } catch (...) {
   require_condition(window.size() == 0, "error propagated before accepted borrowers settled");
   require_condition(std::current_exception() == initial, "completion window replaced its first failure");
   throw;
  }
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  first.settle();
  second.settle();
  held.settle(later);
 });
 mmltk::testsupport::await_test_promise(waiting, "wait hook fails while physical work is held", 5s);
 first.settle();
 mmltk::testsupport::await_test_promise(first_settled, "first ready borrower bypasses held physical work", 5s);
 second.settle();
 mmltk::testsupport::await_test_promise(second_settled, "second ready borrower survives a peer callback failure", 5s);
 CHECK(draining.wait_for(0ms) == std::future_status::timeout);
 CHECK_FALSE(borrowed.expired());
 CHECK(hooks == 1);
 CHECK(callbacks[0] == 0);
 if (execution) {
  CHECK_FALSE(execution->interruption());
  execution->run(CompileTaskKind::Metadata, {}, [](std::size_t) {});
 }
 held.settle(later);
 if (finish == Finish::Destruction)
  mmltk::testsupport::await_test_future(draining, "destruction joins the held borrower", 5s);
 else
  CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(draining, "first failure follows physical drain", 5s), "initial wait hook failure");
 CHECK(borrowed.expired());
 CHECK(hooks == 1);
 for (const auto& calls : callbacks) CHECK(calls == 1);
}
TEST_CASE("completion subscription allocation failure retains accepted physical custody", "[backend][data][benchmark][pipeline]") {
 // GCC's std::function first owns the wake capture, then Completion::subscribe
 // grows its callback vector. Fail each real allocation after window acceptance.
 const auto allowed = GENERATE(std::size_t{0}, std::size_t{1});
 CAPTURE(allowed);
 CompileExecution::CompletionSource held, unregistered;
 const auto physical = std::make_exception_ptr(std::runtime_error("later physical failure"));
 std::atomic<unsigned> held_calls{0}, unregistered_calls{0};
 std::promise<void> waiting, unregistered_settled;
 std::weak_ptr<int> borrowed;
 auto pushing = std::async(std::launch::async, [&] {
  BenchmarkCompletionWindow window(2, nullptr, [&] { mmltk::testsupport::release_test_promise(waiting); });
  auto owner = std::make_shared<int>(17);
  borrowed = owner;
  window.push(held.completion(), [&, owner](std::exception_ptr error) {
   ++held_calls;
   require_condition(!error && *owner == 17, "held subscription peer lost custody");
  });
  owner.reset();
  std::function<void(std::exception_ptr)> settled = [&](std::exception_ptr error) {
   ++unregistered_calls;
   require_condition(error == physical, "unregistered physical failure changed");
   unregistered_settled.set_value();
   throw std::runtime_error("later subscription peer callback failure");
  };
  const FailNextAllocation allocation(allowed);
  try {
   window.push(unregistered.completion(), std::move(settled));
  } catch (...) {
   require_condition(allocation.fired(), "completion subscription allocation was not exercised");
   require_condition(window.size() == 0, "failed subscription released accepted work before drain");
   throw;
  }
  throw std::runtime_error("completion subscription did not allocate");
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  unregistered.settle(physical);
  held.settle();
 });
 mmltk::testsupport::await_test_promise(waiting, "failed subscription joins an accepted unregistered borrower", 5s);
 unregistered.settle(physical);
 mmltk::testsupport::await_test_promise(unregistered_settled, "unregistered completion uses its physical wait", 5s);
 CHECK(pushing.wait_for(0ms) == std::future_status::timeout);
 CHECK_FALSE(borrowed.expired());
 CHECK(held_calls == 0);
 held.settle();
 CHECK_THROWS_AS(mmltk::testsupport::await_test_future(pushing, "allocation failure follows all accepted completions", 5s), std::bad_alloc);
 CHECK(borrowed.expired());
 CHECK(held_calls == 1);
 CHECK(unregistered_calls == 1);
}
TEST_CASE("archive suspension retires scratch without allocating a member checkpoint", "[backend][data][benchmark][archive]") {
 mmltk::testsupport::ScopedTempDir root("archive-suspension-allocation");
 const auto path = root.path() / "source.tar";
 const auto jpeg = make_jpeg(40, 80, 120);
 write_single_jpeg_tar(path, 123456789, jpeg);
 BenchmarkArchive input(path);
 const auto suspend = [&] {
  bool allocated;
  {
   const FailNextAllocation allocation(0);
   input.suspend();
   input.suspend({});
   allocated = allocation.fired();
  }
  CHECK_FALSE(allocated);
  CHECK(input.retained_workspace_bytes() == 0);
 };
 suspend();
 REQUIRE(input.next());
 const auto expected = input.member_position();
 REQUIRE(input.member().size() > 15);
 CHECK(std::ranges::equal(input.read(jpeg.size()), jpeg));
 suspend();
 std::exception_ptr restore_failure;
 {
  const FailNextAllocation allocation(0);
  try {
   (void)input.read(jpeg.size());
  } catch (...) { restore_failure = std::current_exception(); }
 }
 REQUIRE(restore_failure);
 CHECK_THROWS_AS(std::rethrow_exception(restore_failure), std::bad_alloc);
 CHECK(input.retained_workspace_bytes() == 0);
 // A failed restore keeps the original continuation across another suspension.
 suspend();
 CHECK(std::ranges::equal(input.read(jpeg.size()), jpeg));
 REQUIRE(input.seek(expected));
 suspend();
 CHECK(std::ranges::equal(input.read(jpeg.size()), jpeg));
 CHECK_FALSE(input.next());
}
TEST_CASE("custom extraction retains a bounded pinned window through physical pixel completion", "[backend][data][benchmark][archive]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPUs");
 mmltk::testsupport::ScopedTempDir directory("custom-pinned-image-window");
 const auto archive = directory.path() / "images.tar";
 const auto images = directory.path() / "images";
 const auto jpeg = make_jpeg(80, 40, 20);
 std::vector<std::uint64_t> ids;
 {
  std::ofstream output(archive, std::ios::binary);
  for (unsigned id = 1; id <= 12; ++id) {
   ids.push_back(id);
   write_jpeg_tar_entry(output, id, jpeg);
  }
  const std::array<char, 1024> end{};
  output.write(end.data(), end.size());
 }
 bool cancel = false, cached = false;
 SECTION("cold publication") {}
 SECTION("individually cached peers") { cached = true; }
 SECTION("cancellation joins a held physical completion") { cancel = true; }
 if (cached) {
  prepare_cached_image_directory(images);
  for (auto id : ids) BenchmarkEncodedImage::publish(cached_image_path(images, id), jpeg, {});
 }
 BenchmarkCompilePipeline pipeline(4);
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 struct Observed {
  mmltk::testsupport::TestGate held{"first custom pixel retains its completion"};
  std::atomic<unsigned> completed{0};
  std::promise<void> peers;
 } observed;
 auto request = benchmark_write_request(split, directory.path() / "pixels.bin", 8);
 request.execution = &pipeline;
 request.progress = {.context = &observed, .image_completed = [](void* context) {
  auto& value = *static_cast<Observed*>(context);
  const auto count = value.completed.fetch_add(1);
  if (!count)
   value.held.receipt().ArriveAndWait();
  else if (count == 11)
   value.peers.set_value();
 }};
 BenchmarkSplitWriter writer(request, true);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 pipeline.bind_source(images, writer, AnnotationSource::Coco);
 const auto publication = pipeline.source_publication(images, {}, {}, true);
 auto extracting = std::async(std::launch::async, [&] {
  return extract_selected_archive_images(
   {.archive_path = archive,
    .source_identity = "window",
    .output_root = images,
    .source = "coco",
    .shard = "train2017",
    .selected_image_ids = ids,
    .image_id_parser = benchmark_archive_image_candidate,
    .image_ready = [publication](const CachedImageReady& ready) { publication(ready); },
    .execution = &pipeline});
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] { observed.held.Release(); });
 REQUIRE(observed.held.WaitEntered(5s));
 mmltk::testsupport::await_test_promise(observed.peers, "independent custom pixels complete through the bounded input window", 5s);
 CHECK_FALSE(fs::exists(images / ".complete.json"));
 if (cancel) pipeline.execution().cancel();
 CHECK(extracting.wait_for(0ms) == std::future_status::timeout);
 observed.held.Release();
 if (cancel)
  CHECK_THROWS(mmltk::testsupport::await_test_future(extracting, "cancelled custom source physically drains", 5s));
 else {
  const auto result = mmltk::testsupport::await_test_future(extracting, "custom source proof follows physical completion", 5s);
  CHECK(result.image_count == ids.size());
  CHECK(result.image_bytes == ids.size() * jpeg.size());
  CHECK(writer.completed() == ids.size());
 }
}
TEST_CASE("an exceptional archive pixel releases its normal input and quiesces the source cursor", "[backend][data][benchmark][archive][resources]") {
 using namespace mmltk::backend::data::detail;
 const bool below_window = GENERATE(false, true);
 CAPTURE(below_window);
 mmltk::testsupport::ScopedTempDir directory("exceptional-archive-pixel");
 const auto archive = directory.path() / "images.tar", images = directory.path() / "images";
 const auto jpeg = make_jpeg(24, 80, 160);
 const std::array<std::uint64_t, 2> ids{1, 2};
 {
  std::ofstream output(archive, std::ios::binary);
  for (auto id : ids) write_jpeg_tar_entry(output, id, jpeg);
  const std::array<char, 1024> end{};
  output.write(end.data(), end.size());
 }
 const std::array<CompileWorkspaceProfile, 1> profiles{{{64U << 10, 0}}};
 auto plan = std::make_shared<CompileWorkspacePlan>(below_window ? compile_workspace_plan(1, 128, false, 0, 1, BenchmarkEncodedImage::normal_window_bytes)
                                                                 : CompileWorkspacePlan::prepare(1, profiles, BenchmarkEncodedImage::normal_window_bytes, 0, 40ULL << 20));
 const auto header = BenchmarkImageDecoder{}.read_header(jpeg);
 REQUIRE(compile_pixel_workspace_plan(header, {128, 128, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, false}).bytes() > plan->context_bytes);
 const auto decoder_bytes = BenchmarkArchive::input_requirement(archive, 0).workspace_bytes;
 BenchmarkCompilePipeline pipeline(1, {}, {.workspace_plan = plan});
 const auto locked = pipeline.execution().snapshot().normal_locked;
 PreparedBenchmarkSplit split;
 split.name = "train";
 split.class_names = {"person"};
 struct Observed {
  mmltk::testsupport::TestGate held{"oversized pixel owns its exceptional arena"};
  std::atomic<unsigned> completed{0};
 } observed;
 auto request = benchmark_write_request(split, directory.path() / "pixels.bin", 128);
 request.execution = &pipeline;
 request.progress = {.context = &observed, .image_completed = [](void* context) {
  auto& value = *static_cast<Observed*>(context);
  if (!value.completed.fetch_add(1)) value.held.receipt().ArriveAndWait();
 }};
 BenchmarkSplitWriter writer(request, true);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 pipeline.bind_source(images, writer, AnnotationSource::Coco);
 const auto publication = pipeline.source_publication(images, {}, {}, true);
 std::atomic<unsigned> input_waits{0}, checked_inputs{0};
 auto extracting = std::async(std::launch::async, [&] {
  return extract_selected_archive_images(
   {.archive_path = archive,
    .source_identity = "exceptional",
    .output_root = images,
    .source = "coco",
    .shard = "train2017",
    .selected_image_ids = ids,
    .image_id_parser = benchmark_archive_image_candidate,
    .validator =
     [&](std::uint64_t, std::span<const std::uint8_t> encoded) {
   if (below_window) {
    const auto snapshot = pipeline.execution().snapshot();
    require_condition(snapshot.exceptional >= BenchmarkEncodedImage::normal_window_bytes, "below-window input lacks its admitted exceptional backing");
    require_condition(snapshot.resources.bytes < BenchmarkEncodedImage::normal_window_bytes + decoder_bytes, "restored archive retained decoder ancestry through input publication");
   }
   ++checked_inputs;
   return BenchmarkImageDecoder{}.read_header(encoded);
  },
    .trace =
     [&](std::string_view event, const nlohmann::json&) {
   if (event == "benchmark.images.input_wait") ++input_waits;
  },
    .image_ready = [publication](const CachedImageReady& ready) { publication(ready); },
    .execution = &pipeline});
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  observed.held.Release();
  pipeline.execution().cancel();
 });
 REQUIRE(observed.held.WaitEntered(5s));
 CHECK(pipeline.execution().snapshot().exceptional > 0);
 CHECK(pipeline.execution().snapshot().normal_locked == locked);
 CHECK_FALSE(fs::exists(images / ".complete.json"));
 observed.held.Release();
 const auto result = mmltk::testsupport::await_test_future(extracting, "exceptional pixel returns to the retained forward source", 5s);
 CHECK(result.image_count == ids.size());
 CHECK(writer.completed() == ids.size());
 CHECK(checked_inputs == ids.size());
 if (below_window) CHECK(input_waits >= ids.size());
 CHECK(pipeline.execution().snapshot().exceptional == 0);
 CHECK(pipeline.execution().snapshot().normal_locked == locked);
}
TEST_CASE("custom archive admission retires decoder and unaccepted input through a full physical queue", "[backend][data][benchmark][archive][resources]") {
 using namespace mmltk::backend::data::detail;
 if (mmltk::common::system::allowed_cpu_set().size() < 3) SKIP("requires three assigned CPUs");
 const bool cancel = GENERATE(false, true);
 CAPTURE(cancel);
 mmltk::testsupport::ScopedTempDir directory("archive-full-file-window");
 const auto archive = directory.path() / "images.tar", images = directory.path() / "images";
 const auto jpeg = make_jpeg(24, 80, 160);
 const std::array<std::uint64_t, 3> ids{1, 2, 3};
 {
  std::ofstream output(archive, std::ios::binary);
  for (auto id : ids) write_jpeg_tar_entry(output, id, jpeg);
  const std::array<char, 1024> end{};
  output.write(end.data(), end.size());
 }
 mmltk::testsupport::TestGate input_wait("source failed its own pinned input reservation"), checked("second member has a checked header"), restoring("input admitted before decoder restoration"),
  restoration_peer("new oversized peer precedes decoder restoration"), accepted("accepted source callback retains physical custody"), file_peer("independent accepted file callback"),
  pixel("oversized pixel owns its arena");
 std::atomic<unsigned> validations{0}, callbacks{0}, file_returns{0}, file_waits{0};
 std::atomic<bool> first_input_wait{false}, first_restoration{false}, testing_full_queue{false};
 std::promise<void> restoration_waiting, pixel_waiting, source_full;
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json& fields) {
  if (event == "benchmark.images.input_wait" && !first_input_wait.exchange(true)) input_wait.receipt().ArriveAndWait();
  if (event == "benchmark.images.decoder_admission" && !first_restoration.exchange(true)) restoring.receipt().ArriveAndWait();
  if (event == "benchmark.images.file_wait") {
   const auto count = ++file_waits;
   if (count == 1) mmltk::testsupport::release_test_promise(source_full);
  }
  if (const auto parked = fields.find("parked"); parked != fields.end() && (*parked)[static_cast<std::size_t>(CompileParkReason::Bytes)].get<std::size_t>())
   mmltk::testsupport::release_test_promise(testing_full_queue ? pixel_waiting : restoration_waiting);
 };
 const std::array<CompileWorkspaceProfile, 1> profiles{{{64U << 10, 0}}};
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(3, profiles, BenchmarkEncodedImage::normal_window_bytes, 0, 40ULL << 20));
 BenchmarkCompilePipeline pipeline(3, {}, {.workspace_plan = plan}, {}, trace);
 auto occupied = pipeline.execution().reserve({.bytes = BenchmarkEncodedImage::normal_window_bytes, .supplied_workspace = true, .workspace_window = CompileWorkspaceWindow::SharedSource});
 const auto locked = pipeline.execution().snapshot().normal_locked;
 std::array<CompileExecution::Completion, 5> files;
 const auto pixel_bytes = compile_pixel_workspace_plan(BenchmarkImageDecoder{}.read_header(jpeg), {128, 128, mmltk::backend::imaging::resample::ImageResizeMode::Stretch, false}).bytes();
 REQUIRE(pixel_bytes > plan->context_bytes);
 const auto pixel_task = [&](mmltk::testsupport::TestGate& gate) {
  return pipeline.execution().submit(
   {.kind = CompileTaskKind::Pixels,
    .resources = {.bytes = pixel_bytes, .supplied_workspace = true, .workspace_class = CompileWorkClass::Pixels},
    .work =
     [receipt = gate.receipt()](std::size_t, const CompileAllowance& allowance) {
   allowance.workspace().front() = std::byte{29};
   receipt.ArriveAndWait();
  },
    .retain_workspace = false});
 };
 CompileExecution::Completion oversized, restoring_pixel;
 std::future<CachedImageDirectory> extracting;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  input_wait.Release();
  checked.Release();
  restoring.Release();
  restoration_peer.Release();
  accepted.Release();
  file_peer.Release();
  pixel.Release();
  occupied = {};
  pipeline.execution().cancel();
  pipeline.files().join();
 });
 extracting = std::async(std::launch::async, [&] {
  return extract_selected_archive_images(
   {.archive_path = archive,
    .source_identity = "opened-full-window",
    .output_root = images,
    .source = "coco",
    .shard = "train2017",
    .selected_image_ids = ids,
    .image_id_parser = benchmark_archive_image_candidate,
    .validator =
     [&](std::uint64_t id, std::span<const std::uint8_t> encoded) {
   ++validations;
   const auto header = BenchmarkImageDecoder{}.read_header(encoded);
   if (id == 2) checked.receipt().ArriveAndWait();
   return header;
  },
    .trace = trace,
    .image_ready =
     [&](const CachedImageReady& ready) {
   require_condition(ready.payload && !ready.payload->charged_bytes(), "accepted immutable file callback retained pinned source input");
   if (ready.image_id == 1) accepted.receipt().ArriveAndWait();
   ++callbacks;
  },
    .execution = &pipeline,
    .retain_encoded = false});
 });
 REQUIRE(input_wait.WaitEntered(5s));
 CHECK_FALSE(pipeline.execution().resource_pressure());
 CHECK(pipeline.execution().resource_usage().bytes == BenchmarkEncodedImage::normal_window_bytes + CompileResources::handles(2).bytes);
 // This source's failed try_reserve must release decoder scratch without any
 // unrelated preexisting waiter. Independent CPU work remains selectable.
 auto independent = pipeline.execution().submit({.kind = CompileTaskKind::Metadata, .work = [](std::size_t, const CompileAllowance&) {}});
 independent.wait();
 CHECK(pipeline.execution().snapshot().normal_locked == locked);
 occupied = {};
 input_wait.Release();
 REQUIRE(restoring.WaitEntered(5s));
 restoring_pixel = pixel_task(restoration_peer);
 mmltk::testsupport::await_test_promise(restoration_waiting, "new oversized work closes admission before decoder restoration", 5s);
 restoring.Release();
 REQUIRE(restoration_peer.WaitEntered(5s));
 CHECK(validations == 0);
 CHECK(callbacks == 0);
 CHECK(pipeline.execution().snapshot().leased == 0);
 CHECK(pipeline.execution().snapshot().normal_locked == locked);
 restoration_peer.Release();
 restoring_pixel.wait();
 REQUIRE(accepted.WaitEntered(5s));
 REQUIRE(checked.WaitEntered(5s));
 for (auto& file : files)
  file = pipeline.files().submit({.completed = [&](std::exception_ptr) {
   file_peer.receipt().ArriveAndWait();
   ++file_returns;
  }, .work = [] {}});
 REQUIRE(file_peer.WaitEntered(5s));
 CHECK_FALSE(pipeline.files().has_capacity());
 CHECK_FALSE(pipeline.files().try_submit({.work = [] {}}));
 testing_full_queue = true;
 oversized = pixel_task(pixel);
 mmltk::testsupport::await_test_promise(pixel_waiting, "oversized pixel waits behind the source input and decoder", 5s);
 // The unaccepted second member and later third member must still come from
 // the opened inode, even after the source's pathname names different bytes.
 fs::rename(archive, directory.path() / "opened.tar");
 write_single_jpeg_tar(archive, 2, make_jpeg(200, 10, 30));
 checked.Release();
 mmltk::testsupport::await_test_promise(source_full, "source observes full physical file records", 5s);
 REQUIRE(pixel.WaitEntered(5s));
 CHECK(pipeline.execution().snapshot().exceptional > 0);
 CHECK(pipeline.execution().snapshot().leased == 0);
 CHECK(pipeline.execution().snapshot().normal_locked == locked);
 CHECK(callbacks == 0);
 CHECK(file_returns == 0);
 CHECK(validations == 2);
 CHECK_FALSE(fs::exists(cached_image_path(images, 2)));
 CHECK_FALSE(fs::exists(images / ".complete.json"));
 if (cancel) {
  pipeline.execution().cancel();
  CHECK_THROWS(pipeline.files().has_capacity());
 }
 CHECK(extracting.wait_for(0ms) == std::future_status::timeout);
 CHECK_FALSE(oversized.ready());
 pixel.Release();
 oversized.wait();
 // Completion of the oversized peer returns resources, but neither it nor
 // source-owned release notifications return a held physical file record.
 CHECK(pipeline.execution().snapshot().leased == 0);
 CHECK(file_waits == 1);
 CHECK(extracting.wait_for(0ms) == std::future_status::timeout);
 CHECK(validations == 2);
 accepted.Release();
 file_peer.Release();
 if (cancel)
  CHECK_THROWS(mmltk::testsupport::await_test_future(extracting, "cancelled source joins its accepted callback", 5s));
 else {
  const auto result = mmltk::testsupport::await_test_future(extracting, "source resumes its opened archive generation", 5s);
  CHECK(result.image_count == ids.size());
  CHECK(result.image_bytes == ids.size() * jpeg.size());
  CHECK(callbacks == ids.size());
  CHECK(validations == ids.size());
  const auto expected = mmltk::common::io::sha256_bytes(jpeg);
  for (auto id : ids) CHECK(mmltk::common::io::sha256_file(cached_image_path(images, id)) == expected);
 }
 pipeline.files().join();
 if (!cancel) CHECK(pipeline.files().has_capacity());
 CHECK(file_returns == files.size());
 CHECK(pipeline.execution().snapshot().exceptional == 0);
 CHECK(pipeline.execution().snapshot().normal_locked == locked);
}
TEST_CASE("planned normalized gather admits disjoint chunks on shared lanes", "[benchmark][annotations][resources]") {
 using namespace mmltk::backend::data::detail;
 using namespace std::chrono_literals;
 const auto workers = GENERATE(std::size_t{1}, std::size_t{2});
 if (mmltk::common::system::allowed_cpu_set().size() < workers) SKIP("requires the selected assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("normalized-disjoint-gather");
 // This fixture observes the optional task-completion counter at its phase gate.
 CompileExecution execution(workers, {}, {}, {}, [](std::string_view, const CompileExecutionSnapshot&) {});
 FileWorkQueue files(execution, 2);
 auto allowance = execution.reserve({4096, 1});
 mmltk::testsupport::TestGate planning("admitted gather extent and cursor planning");
 mmltk::testsupport::TestGate entered("independent bounded gather ranges");
 struct Gate {
  CompileExecution& execution;
  mmltk::testsupport::TestGate& planning;
  mmltk::testsupport::TestGate& gate;
  mutable std::atomic<bool> planned{false}, charged{true};
  mutable std::atomic<std::size_t> calls{0};
  bool cancelled() const noexcept {
   if (execution.on_lane()) {
    if (execution.current_allowance().bytes() != 4096) charged = false;
    if (!execution.snapshot().completed) {
     if (!planned.exchange(true)) planning.receipt().ArriveAndWait();
    } else if (calls.fetch_add(1) < execution.workers())
     gate.receipt().ArriveAndWait();
   }
   return false;
  }
 } cancellation{execution, planning, entered};
 const auto source = label_projection_fixture(7);
 std::vector<RLEPair> runs(65537);
 for (std::size_t i = 0; i < runs.size(); ++i) runs[i] = {static_cast<std::uint32_t>(i * 2), 1};
 auto box = source.storage().boxes.front();
 box.mask_rle_offset = 0;
 box.mask_rle_pairs = static_cast<std::uint32_t>(runs.size());
 std::array<NormalizedAnnotationAssembler::Slice, 2> slices;
 for (std::size_t i = 0; i < slices.size(); ++i) slices[i] = {{7 + i, 0, 1, 262144, 1, 0, 0}, {&box, 1}, runs, i * 100, i != 0};
 NormalizedAnnotationAssembler assembly({}, mmltk::common::concurrency::CancellationObservation::Borrow(cancellation), root.path());
 auto gathering = std::async(std::launch::async, [&] {
  assembly.gather(slices, &execution, &files, allowance);
  return assembly.finish();
 });
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  planning.Release();
  entered.Release();
  execution.cancel();
 });
 REQUIRE(planning.WaitEntered(5s));
 CHECK(execution.snapshot().running[static_cast<std::size_t>(CompileWorkClass::Gather)] == 1);
 CHECK(execution.snapshot().completed == 0);
 CHECK(fs::is_empty(root.path()));
 planning.Release();
 REQUIRE(entered.WaitEntered(5s, workers));
 CHECK(execution.snapshot().running[static_cast<std::size_t>(CompileWorkClass::Gather)] == workers);
 entered.Release();
 const auto product = mmltk::testsupport::await_test_future(gathering, "canonical gather completes every disjoint run continuation", 5s);
 CHECK(cancellation.charged.load());
 REQUIRE(product.images.size() == 2);
 REQUIRE(product.boxes.size() == 2);
 CHECK(product.images[0].source_image_id == 7);
 CHECK(product.images[1].first_box == 1);
 CHECK(product.boxes[1].mask_rle_offset == runs.size());
 CHECK(product.boxes[1].source_ordinal == box.source_ordinal + 100);
 CHECK((product.boxes[1].flags & kAnnotationOrdinalIncomplete) != 0);
 CHECK(std::memcmp(product.mask_rle_pairs.data(), runs.data(), runs.size() * sizeof(RLEPair)) == 0);
 CHECK(std::memcmp(product.mask_rle_pairs.data() + runs.size(), runs.data(), runs.size() * sizeof(RLEPair)) == 0);
 REQUIRE_NOTHROW(admit_normalized_annotations(product));
}
TEST_CASE("a Parquet source uses its protected window beside busy fitting contexts", "[benchmark][pipeline][resources]") {
 using namespace mmltk::backend::data::detail;
 auto plan = std::make_shared<CompileWorkspacePlan>(compile_workspace_plan(1, 1, false, kParquetPoolBytes, 0, BenchmarkEncodedImage::normal_window_bytes));
 CompileExecution execution(1, {}, {.workspace_plan = plan});
 const auto locked = execution.snapshot().normal_locked;
 auto normal = execution.reserve({.bytes = plan->context_bytes, .supplied_workspace = true});
 auto ordinary = normal.workspace();
 ordinary.front() = std::byte{19};
 auto source =
  execution.try_reserve({.bytes = kParquetPoolBytes, .supplied_workspace = true, .workspace_class = CompileWorkClass::Extraction, .workspace_window = CompileWorkspaceWindow::SharedSource});
 REQUIRE(source);
 auto shared = source->workspace();
 REQUIRE(shared.size() == kParquetPoolBytes);
 shared.front() = std::byte{23};
 CHECK(shared.data() != ordinary.data());
 CHECK(ordinary.front() == std::byte{19});
 CHECK(execution.snapshot().exceptional == 0);
 CHECK(execution.snapshot().normal_locked == locked);
 source.reset();
 normal = {};
 CHECK(execution.snapshot().normal_locked == locked);
}
TEST_CASE("failed normalized gather cannot seal a partially copied product", "[benchmark][annotations][resources]") {
 const bool mapped = GENERATE(false, true);
 const bool planning_failure = GENERATE(false, true);
 mmltk::testsupport::ScopedTempDir root("normalized-gather-failure");
 const auto source = label_projection_fixture(7);
 auto box = source.storage().boxes.front();
 const std::array<RLEPair, 2> runs{{{0, 1}, {0, 1}}};
 box.mask_rle_offset = 0;
 box.mask_rle_pairs = runs.size();
 auto image = source.image(0);
 image.box_count = 1;
 if (planning_failure) image.box_count = 2;
 const std::array slices{NormalizedAnnotationAssembler::Slice{image, {&box, 1}, runs}};
 NormalizedAnnotationAssembler assembly(source, 1, 1, runs.size(), {}, mapped ? root.path() : fs::path{});
 CHECK_THROWS(assembly.gather(slices));
 CHECK_THROWS_AS(assembly.finish(), std::logic_error);
 CHECK_THROWS_AS(assembly.begin_image(image), std::logic_error);
}
TEST_CASE("preloaded image custody preserves an admitted inode while a ready peer converts", "[benchmark][pipeline][images][preload]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("preload-independent-inode");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 0}};
 const auto first = make_jpeg(240, 8, 8);
 BenchmarkEncodedImage::publish(cached_image_path(images, 2), first, {});
 mmltk::testsupport::TestGate reading("first image owns admitted preload storage");
 std::atomic<unsigned> inputs{0};
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json& fields) {
  if (event != "benchmark.image.stage_started" || fields.at("stage") != "input") return;
  ++inputs;
  if (fields.at("image_id") == 1) reading.receipt().ArriveAndWait();
 };
 BenchmarkCompilePipeline pipeline(4, {}, {}, {}, trace);
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8, {}, &pipeline);
 request.perceptual_downscale = true;
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 bind_benchmark_slots(pipeline, writer, split);
 const mmltk::testsupport::ScopedTestCleanup release([&] { reading.Release(); });
 const auto publication = pipeline.source_publication(images, {});
 publication({1});
 REQUIRE(reading.WaitEntered(5s));
 // The physical read has not copied the body. Replacement and unlink cannot
 // substitute new pixels for the exact mapping admitted by the header stage.
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), make_jpeg(8, 240, 8), {});
 REQUIRE(fs::remove(cached_image_path(images, 1)));
 publication({2});
 std::promise<void> ready;
 pipeline.image_completion(images, 2).subscribe([&](std::exception_ptr error) {
  if (error)
   ready.set_exception(error);
  else
   ready.set_value();
 });
 mmltk::testsupport::await_test_promise(ready, "ready image converts while its peer is preloading", 5s);
 CHECK(writer.image_complete(1));
 CHECK_FALSE(writer.image_complete(0));
 CHECK(inputs == 2);
 CHECK(pipeline.execution().snapshot().active <= 4);
 reading.Release();
 pipeline.drain();
 writer.finish(request);
 const auto compiled = CompiledDataset::open(request.output_path);
 CHECK(std::memcmp(compiled.image_pixels(0), compiled.image_pixels(1), 8 * 8 * 3 * sizeof(float)) == 0);
 CHECK(pipeline.execution().snapshot().leased == 0);
}
TEST_CASE("image file capacity remains useful behind held cache and label output", "[benchmark][pipeline][writer][preload]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("preload-independent-output");
 BenchmarkCompilePipeline pipeline(4);
 LiveBenchmarkWriter fixture(pipeline, root.path(), AnnotationSource::Coco);
 const auto images = root.path() / "images";
 prepare_cached_image_directory(images);
 BenchmarkEncodedImage::publish(cached_image_path(images, 1), make_jpeg(200, 40, 20), {});
 const auto publication = pipeline.source_publication(images, {});
 mmltk::testsupport::TestGate output("cache operations retain all ordinary file records");
 std::array<CompileExecution::Completion, 8> writes;
 const mmltk::testsupport::ScopedTestCleanup release([&] { output.Release(); });
 for (auto& write : writes) write = pipeline.files().submit({.work = [&] { output.receipt().ArriveAndWait(); }});
 REQUIRE(output.WaitEntered(5s, 2));
 CHECK_FALSE(pipeline.files().has_capacity());
 pipeline.label_configuration(8, mmltk::backend::imaging::resample::ImageResizeMode::Stretch);
 REQUIRE(pipeline.labels_ready(publication, 99, BenchmarkLabelInput(label_projection_fixture(99), 0), "labels", 1));
 publication({1});
 std::promise<void> ready;
 pipeline.image_completion(images, 1).subscribe([&](std::exception_ptr error) {
  if (error)
   ready.set_exception(error);
  else
   ready.set_value();
 });
 mmltk::testsupport::await_test_promise(ready, "pixels prepare their extent and finish beside held output", 5s);
 const auto slot = pipeline.image_slot(images, 1);
 REQUIRE(slot);
 CHECK(fixture.writer.image_complete(*slot));
 CHECK_FALSE(pipeline.label_completion(images, 99, "labels").ready());
 CHECK_FALSE(writes.front().ready());
 CHECK(pipeline.execution().snapshot().active <= 4);
 output.Release();
 pipeline.files().join();
 pipeline.drain();
 CHECK(pipeline.label_completion(images, 99, "labels").ready());
}
TEST_CASE("preload and extent cancellation join physical custody before replacement", "[benchmark][pipeline][images][preload][resources]") {
 const bool output = GENERATE(false, true);
 enum class Retire { Cancel, Image, Source };
 const auto retirement = GENERATE(Retire::Cancel, Retire::Image, Retire::Source);
 const bool replace = retirement != Retire::Cancel;
 mmltk::testsupport::ScopedTempDir root("preload-physical-retirement");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 mmltk::testsupport::TestGate physical("admitted image file service owns storage");
 std::atomic<bool> first{true};
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json& fields) {
  if (event == "benchmark.image.stage_started" && fields.at("stage") == (output ? "output" : "input") && first.exchange(false)) physical.receipt().ArriveAndWait();
 };
 BenchmarkCompilePipeline pipeline(1, {}, {.transient_bytes = 4096, .task_records = 1}, {}, trace);
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8, {}, &pipeline);
 write_text(request.output_path, "prior published pixels");
 const auto previous = mmltk::common::io::sha256_file(request.output_path);
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 bind_benchmark_slots(pipeline, writer, split);
 auto publication = pipeline.source_publication(images, {}, {}, true);
 const mmltk::testsupport::ScopedTestCleanup release([&] { physical.Release(); });
 publication({1});
 REQUIRE(physical.WaitEntered(5s));
 const auto locked = pipeline.execution().snapshot().normal_locked;
 CHECK(pipeline.execution().snapshot().leased > 0);
 CHECK(pipeline.execution().snapshot().records == 1);
 auto retiring = std::async(std::launch::async, [&] {
  if (retirement == Retire::Image)
   pipeline.retire_image(images, 1);
  else if (retirement == Retire::Source)
   pipeline.retire_source(images);
  else {
   pipeline.execution().cancel();
   pipeline.drain();
  }
 });
 const mmltk::testsupport::ScopedTestCleanup release_before_wait([&] { physical.Release(); });
 CHECK(retiring.wait_for(0ms) == std::future_status::timeout);
 CHECK_FALSE(pipeline.image_completion(images, 1).ready());
 CHECK(mmltk::common::io::sha256_file(request.output_path) == previous);
 physical.Release();
 if (replace) {
  mmltk::testsupport::await_test_future(retiring, "image generation joins its admitted physical operation", 5s);
  BenchmarkEncodedImage::publish(cached_image_path(images, 1), make_jpeg(30, 60, 200), {});
  publication({1});
  CHECK_FALSE(writer.image_complete(0));
  const auto replacement = pipeline.source_publication(images, {}, 1);
  CHECK(replacement.consume({1}));
  CHECK(writer.image_complete(0));
 } else
  CHECK_THROWS(mmltk::testsupport::await_test_future(retiring, "cancellation settles the admitted image service", 5s));
 CHECK(pipeline.execution().snapshot().leased == 0);
 CHECK(pipeline.execution().snapshot().exceptional == 0);
 CHECK(pipeline.execution().snapshot().normal_locked == locked);
 CHECK(mmltk::common::io::sha256_file(request.output_path) == previous);
 pipeline.retire_attempt();
 CHECK(pipeline.execution().snapshot().records == 0);
}
TEST_CASE("bounded preload services reuse workers and fixed storage on small CPU plans", "[benchmark][pipeline][images][preload][resources]") {
 const auto workers = GENERATE(std::size_t{1}, std::size_t{2});
 if (mmltk::common::system::allowed_cpu_set().size() < workers) SKIP("requires the selected assigned CPUs");
 mmltk::testsupport::ScopedTempDir root("preload-fixed-services");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 constexpr std::size_t count = 12;
 for (std::uint64_t id = 1; id <= count; ++id) {
  BenchmarkEncodedImage::publish(cached_image_path(images, id), make_jpeg(70, 80, 90), {});
  split.images.push_back({id, 16, 8, 0, 0});
 }
 std::mutex observations;
 std::vector<std::thread::id> readers;
 unsigned started = 0, completed = 0;
 bool bounded = true;
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json& fields) {
  const std::lock_guard lock(observations);
  if (event == "file.service.started" && fields.at("service") == "image") ++started;
  if (event == "file.admitted" && fields.at("service") == "image") bounded = bounded && fields.at("records").get<unsigned>() <= 4;
  if (event == "benchmark.image.stage_started" && fields.at("stage") == "input") {
   const auto reader = std::this_thread::get_id();
   if (std::ranges::find(readers, reader) == readers.end()) readers.push_back(reader);
  }
  if (event == "benchmark.image.stage_completed" && fields.at("stage") == "input" && !fields.at("failed").get<bool>()) ++completed;
 };
 BenchmarkCompilePipeline pipeline(workers, {}, {.transient_bytes = 4U << 20}, {}, trace);
 const auto locked = pipeline.execution().snapshot().normal_locked;
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8, {}, &pipeline);
 request.perceptual_downscale = true;
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 bind_benchmark_slots(pipeline, writer, split);
 const auto publication = pipeline.source_publication(images, {}, {}, true);
 for (std::uint64_t id = 1; id <= count; ++id) {
  CHECK(publication.consume({id}));
  CHECK(pipeline.execution().snapshot().normal_locked == locked);
 }
 pipeline.drain();
 CHECK(writer.completed() == count);
 CHECK(pipeline.execution().snapshot().workspace_reuses > 0);
 CHECK(pipeline.execution().snapshot().leased == 0);
 {
  const std::lock_guard lock(observations);
  CHECK(started == 1);
  CHECK(completed == count);
  CHECK_FALSE(readers.empty());
  CHECK(readers.size() <= std::min(workers, std::size_t{2}));
  CHECK(bounded);
 }
}
namespace {
struct PendingImageEvidence {
 std::atomic<bool> bounded{true};
 std::atomic<std::size_t> observations{0};
 void observe(const nlohmann::json& fields) {
  std::size_t pending = 0, denied = 0;
  for (const auto* stage : {"header", "workspace", "input", "output", "pixels"}) pending += fields.at(stage).get<std::size_t>();
  for (const auto& blocked : fields.at("stage_records_blocked")) denied += blocked.get<bool>();
  const auto accepted = fields.at("image_admitted").get<std::size_t>();
  const auto probes = fields.at("image_head_probes").get<std::size_t>();
  if (pending != fields.at("pending_images").get<std::size_t>() || probes != accepted + denied || denied > 5) bounded = false;
  ++observations;
 }
};
struct PipelineImageReceipts {
 struct Image {
  std::atomic<unsigned> settled{0};
  std::exception_ptr error;
 };
 std::vector<Image> images;
 std::atomic<std::size_t> settled{0};
 std::promise<void> completed;
 explicit PipelineImageReceipts(std::size_t count) : images(count) {}
 void subscribe(const CompileExecution::Completion& completion, std::size_t index) {
  REQUIRE(completion);
  completion.subscribe([this, index](std::exception_ptr error) {
   images[index].error = error;
   ++images[index].settled;
   if (++settled == images.size()) mmltk::testsupport::release_test_promise(completed);
  });
 }
 void wait() {
  mmltk::testsupport::await_test_promise(completed, "every image settles its completion receipt", 5s);
  for (const auto& image : images) CHECK(image.settled == 1);
 }
};
}  // namespace
TEST_CASE("image continuations finish ahead of newer arena waiters with a hard record bank", "[benchmark][pipeline][images][preload][resources]") {
 using namespace mmltk::backend::data::detail;
 enum class Input { File, Retained, Exceptional };
 const auto input = GENERATE(Input::File, Input::Retained, Input::Exceptional);
 const auto workers = GENERATE(std::size_t{1}, std::size_t{2});
 if (mmltk::common::system::allowed_cpu_set().size() < workers) SKIP("requires the selected assigned CPUs");
 const auto page = mmltk::common::system::host_page_size();
 const auto budget = input == Input::Exceptional ? page : std::size_t{4U << 20};
 // Keep exactly one normal conversion arena even with two CPU grants.
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(1, {}, input == Input::Retained ? page : 0, 0, budget));
 mmltk::testsupport::ScopedTempDir root("image-continuation-records");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}, {2, 16, 8, 0, 0}};
 const auto encoded = make_jpeg(70, 80, 90);
 const auto header = BenchmarkImageDecoder{}.read_header(encoded);
 auto file = BenchmarkEncodedImage::publish(cached_image_path(images, 1), encoded, {}, nullptr, header);
 BenchmarkEncodedImage::publish(cached_image_path(images, 2), encoded, {});
 mmltk::testsupport::TestGate physical("first image returns its CPU grant before the file callback"), second("newer image cannot finish before its predecessor");
 PipelineImageReceipts receipts(2);
 PendingImageEvidence pending;
 std::promise<void> waiting, first_completed;
 std::atomic<bool> first{true}, bounded{true};
 std::atomic<unsigned> first_workspaces{0};
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json& fields) {
  if (event == "file.physical_completed" && fields.at("service") == "image" && first.exchange(false)) physical.receipt().ArriveAndWait();
  if (event == "benchmark.pipeline.ready") {
   pending.observe(fields);
   if (workers == 1 && fields.at("header") == 1 && fields.at("image_admitted") == 0) mmltk::testsupport::release_test_promise(waiting);
  }
  if (event == "benchmark.image.stage_started") {
   if (fields.at("stage") == "workspace" && fields.at("image_id") == 1) ++first_workspaces;
   if (fields.at("stage") == "pixels" && fields.at("image_id") == 2) second.receipt().ArriveAndWait();
  }
  if (event == "compile.execution") {
   const auto exceptional = fields.at("exceptional_reserved").get<std::uint64_t>();
   if (fields.at("records").get<std::size_t>() > workers || fields.at("active").get<std::size_t>() + fields.at("external").get<std::size_t>() > workers || fields.at("normal_locked") != budget ||
       fields.at("leased").get<std::uint64_t>() > budget + exceptional || fields.at("bytes").get<std::uint64_t>() > budget + exceptional + CompileResources::handles(16).bytes ||
       fields.at("descriptors").get<std::size_t>() + fields.at("descriptor_promises").get<std::size_t>() > 16)
    bounded = false;
   if (workers == 2 && fields.at("parked")[static_cast<std::size_t>(CompileParkReason::Bytes)].get<std::size_t>()) mmltk::testsupport::release_test_promise(waiting);
  }
 };
 BenchmarkCompilePipeline pipeline(workers, {}, {.transient_bytes = budget, .descriptors = 16, .task_records = workers, .workspace_plan = plan}, {}, trace);
 BenchmarkEncodedImage::Ptr payload;
 if (input == Input::Retained) {
  auto backing = std::make_shared<CompileAllowance>(pipeline.execution().reserve({.bytes = page, .supplied_workspace = true, .workspace_window = CompileWorkspaceWindow::SharedSource}));
  const auto memory = backing->workspace();
  REQUIRE(encoded.size() < memory.size());
  std::memcpy(memory.data(), encoded.data(), encoded.size());
  payload = BenchmarkEncodedImage::pooled(*backing, backing, {reinterpret_cast<const std::uint8_t*>(memory.data()), encoded.size()}, std::move(file));
 }
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8, {}, &pipeline);
 request.perceptual_downscale = true;
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 bind_benchmark_slots(pipeline, writer, split);
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  physical.Release();
  second.Release();
  pipeline.retire_attempt();
 });
 const auto publication = pipeline.source_publication(images, {}, {}, true);
 publication({1, {}, true, std::move(payload)});
 receipts.subscribe(pipeline.image_completion(images, 1), 0);
 pipeline.image_completion(images, 1).subscribe([&](std::exception_ptr error) {
  if (error)
   first_completed.set_exception(error);
  else
   first_completed.set_value();
 });
 REQUIRE(physical.WaitEntered(5s));
 CHECK(pipeline.execution().snapshot().active == 0);
 CHECK(pipeline.execution().snapshot().records == 1);
 CHECK(pipeline.execution().snapshot().leased > 0);
 CHECK(first_workspaces == (input == Input::Retained ? 0 : 1));
 if (input == Input::Exceptional) CHECK(pipeline.execution().snapshot().exceptional > 0);
 publication({2});
 receipts.subscribe(pipeline.image_completion(images, 2), 1);
 mmltk::testsupport::await_test_promise(waiting, "new image waits behind retained record or arena custody", 5s);
 CHECK(pipeline.execution().snapshot().records == workers);
 physical.Release();
 mmltk::testsupport::await_test_promise(first_completed, "retained Pixels finishes before the newer image can finish", 5s);
 REQUIRE(second.WaitEntered(5s));
 CHECK(writer.image_complete(0));
 CHECK_FALSE(pipeline.image_completion(images, 2).ready());
 second.Release();
 receipts.wait();
 pipeline.drain();
 pipeline.execution().join();
 for (const auto& receipt : receipts.images) CHECK_FALSE(receipt.error);
 CHECK(pipeline.execution().snapshot().records == 0);
 CHECK(pipeline.execution().snapshot().leased == 0);
 CHECK(pipeline.execution().snapshot().exceptional == 0);
 CHECK(bounded);
 CHECK(pending.bounded);
 writer.finish(request);
 const auto compiled = CompiledDataset::open(request.output_path);
 CHECK(std::memcmp(compiled.image_pixels(0), compiled.image_pixels(1), 8 * 8 * 3 * sizeof(float)) == 0);
}
TEST_CASE("image file failure releases retained records after physical settlement and permits another attempt", "[benchmark][pipeline][images][preload][resources]") {
 mmltk::testsupport::ScopedTempDir root("image-file-failure-records");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 mmltk::testsupport::TestGate physical("failed file call has returned but still owns its callback");
 std::atomic<bool> fail{true};
 PipelineImageReceipts failed(1), repaired(1);
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json& fields) {
  if (event == "file.physical_completed" && fields.at("service") == "image" && fields.at("failed").get<bool>()) physical.receipt().ArriveAndWait();
 };
 BenchmarkCompilePipeline pipeline(1, {}, {.transient_bytes = 4U << 20, .task_records = 1}, {}, trace);
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8, {}, &pipeline);
 request.extent_preparing = [&](const auto&, auto) {
  if (fail.exchange(false)) throw std::runtime_error("image extent file failure");
 };
 write_text(request.output_path, "prior published pixels");
 const auto previous = mmltk::common::io::sha256_file(request.output_path);
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 bind_benchmark_slots(pipeline, writer, split);
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  physical.Release();
  pipeline.retire_attempt();
 });
 const auto publication = pipeline.source_publication(images, {}, {}, true);
 publication({1});
 failed.subscribe(pipeline.image_completion(images, 1), 0);
 REQUIRE(physical.WaitEntered(5s));
 CHECK(pipeline.execution().snapshot().active == 0);
 CHECK(pipeline.execution().snapshot().records == 1);
 CHECK(pipeline.execution().snapshot().leased > 0);
 CHECK(failed.settled == 0);
 physical.Release();
 failed.wait();
 REQUIRE(failed.images.front().error);
 CHECK_THROWS_WITH(std::rethrow_exception(failed.images.front().error), "image extent file failure");
 CHECK_THROWS_WITH(pipeline.drain(), "image extent file failure");
 CHECK_FALSE(writer.image_complete(0));
 CHECK(pipeline.execution().snapshot().leased == 0);
 pipeline.retire_attempt();
 CHECK(pipeline.execution().snapshot().records == 0);
 bind_benchmark_slots(pipeline, writer, split);
 publication({1});
 CHECK_FALSE(pipeline.image_completion(images, 1));
 pipeline.source_publication(images, {}, {}, true)({1});
 repaired.subscribe(pipeline.image_completion(images, 1), 0);
 repaired.wait();
 pipeline.drain();
 pipeline.execution().join();
 CHECK_FALSE(repaired.images.front().error);
 CHECK(writer.image_complete(0));
 CHECK(pipeline.execution().snapshot().records == 0);
 CHECK(pipeline.execution().snapshot().leased == 0);
 CHECK(mmltk::common::io::sha256_file(request.output_path) == previous);
}
TEST_CASE("pending image stage probes and source withdrawal stay bounded behind a small record bank", "[benchmark][pipeline][images][preload][resources]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 2) SKIP("requires two assigned CPUs");
 const auto count = GENERATE(std::size_t{32}, std::size_t{256});
 mmltk::testsupport::ScopedTempDir root("pending-image-stage-heads");
 const auto images = root.path() / "images";
 const auto pending_images = root.path() / "pending-images";
 auto split = cached_pixel_membership(images);
 split.images = {{1, 16, 8, 0, 0}};
 add_cached_pixel_source(split, pending_images);
 const auto encoded = make_jpeg(70, 80, 90);
 for (std::uint64_t id = 1; id <= count; ++id) {
  BenchmarkEncodedImage::publish(cached_image_path(pending_images, id), encoded, {});
  split.images.push_back({id, 16, 8, 0, 1});
 }
 mmltk::testsupport::TestGate input("admitted peer retains its physical image read"), output("cache callbacks retain every output record"), pixels("peer resumes its retained record");
 PendingImageEvidence evidence;
 PipelineImageReceipts receipts(count + 1);
 std::promise<void> pending, prepared;
 std::atomic<unsigned> resumed{0}, output_returns{0};
 std::atomic<unsigned> headers{0};
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json& fields) {
  if (event == "benchmark.pipeline.ready") {
   evidence.observe(fields);
   if (fields.at("header") == count && fields.at("image_admitted") == 0 && fields.at("image_head_probes") == 1) mmltk::testsupport::release_test_promise(pending);
  }
  if (event == "benchmark.image.stage_started") {
   if (fields.at("stage") == "header") ++headers;
   if (fields.at("stage") == "input" && fields.at("image_id") == 1) input.receipt().ArriveAndWait();
   if (fields.at("stage") == "pixels" && fields.at("image_id") == 1) pixels.receipt().ArriveAndWait();
  }
  if (event == "benchmark.image.stage_completed" && fields.at("stage") == "output" && fields.at("image_id") == 1 && !fields.at("failed").get<bool>())
   mmltk::testsupport::release_test_promise(prepared);
 };
 BenchmarkCompilePipeline pipeline(2, {}, {.transient_bytes = 4U << 20, .task_records = 2}, {}, trace);
 CompileExecution::Dependency dependency(pipeline.execution());
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8, {}, &pipeline);
 request.perceptual_downscale = true;
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 bind_benchmark_slots(pipeline, writer, split);
 std::future<void> retiring;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  input.Release();
  output.Release();
  pixels.Release();
  dependency.notify();
  if (retiring.valid()) {
   pipeline.execution().cancel();
   retiring.wait();
  }
  pipeline.retire_attempt();
  pipeline.files().join();
 });
 const auto publication = pipeline.source_publication(images, {}, {}, true);
 publication({1});
 REQUIRE(input.WaitEntered(5s));
 // The physical owner keeps one CPU record without holding a CPU grant after
 // the read returns. Fill only the remaining record with unrelated work.
 std::array<CompileExecution::Completion, 1> occupied;
 for (auto& completion : occupied) {
  auto record = pipeline.execution().try_subscribe(dependency, dependency.generation(), {.kind = CompileTaskKind::Normalize, .work = [&](std::size_t, const CompileAllowance&) { ++resumed; }});
  REQUIRE(record);
  completion = std::move(*record);
 }
 std::array<CompileExecution::Completion, 4> outputs;
 for (auto& completion : outputs)
  completion = pipeline.files().submit({.completed = [&](std::exception_ptr) {
   output.receipt().ArriveAndWait();
   ++output_returns;
  }, .work = [] {}});
 REQUIRE(output.WaitEntered(5s, 2));
 CHECK_FALSE(pipeline.files().has_capacity());
 // Published images can accumulate independently of the admitted record bank.
 // A denied Header head leaves the rest of that source's pending work alone.
 const auto queued = pipeline.source_publication(pending_images, {}, {}, true);
 receipts.subscribe(pipeline.image_completion(images, 1), 0);
 for (std::size_t id = 1; id <= count; ++id) {
  queued({id});
  receipts.subscribe(pipeline.image_completion(pending_images, id), id);
 }
 mmltk::testsupport::await_test_promise(pending, "large Header queue receives one denied head probe", 5s);
 CHECK(pipeline.execution().snapshot().records == occupied.size() + 1);
 input.Release();
 mmltk::testsupport::await_test_promise(prepared, "ready image extent bypasses the denied Header queue", 5s);
 REQUIRE(pixels.WaitEntered(5s));
 CHECK_FALSE(pipeline.image_completion(images, 1).ready());
 CHECK_FALSE(occupied.front().ready());
 CHECK(output_returns == 0);
 retiring = std::async(std::launch::async, [&] { pipeline.retire_source(pending_images); });
 mmltk::testsupport::await_test_future(retiring, "source withdrawal unlinks only its pending slots beside a live peer", 5s);
 CHECK_FALSE(pipeline.image_completion(images, 1).ready());
 CHECK(headers == 1);
 pixels.Release();
 dependency.notify();
 for (const auto& completion : occupied) completion.wait();
 receipts.wait();
 pipeline.drain();
 CHECK(resumed == occupied.size());
 CHECK(writer.completed() == 1);
 CHECK_FALSE(receipts.images.front().error);
 for (std::size_t index = 1; index < receipts.images.size(); ++index) {
  REQUIRE(receipts.images[index].error);
  CHECK_THROWS_WITH(std::rethrow_exception(receipts.images[index].error), "benchmark work retired before completion");
 }
 CHECK_FALSE(outputs.front().ready());
 CHECK(evidence.bounded);
 CHECK(evidence.observations > 0);
 output.Release();
 pipeline.files().join();
 CHECK(output_returns == outputs.size());
}
TEST_CASE("large pending image sets retire behind full physical records and leave a reusable attempt", "[benchmark][pipeline][images][preload][resources]") {
 if (mmltk::common::system::allowed_cpu_set().size() < 4) SKIP("requires four assigned CPUs");
 const bool fail = GENERATE(false, true);
 constexpr std::size_t admitted = 4, count = 64;
 mmltk::testsupport::ScopedTempDir root("pending-image-physical-retirement");
 const auto images = root.path() / "images";
 auto split = cached_pixel_membership(images);
 const auto encoded = make_jpeg(70, 80, 90);
 for (std::uint64_t id = 1; id <= admitted + count; ++id) {
  BenchmarkEncodedImage::publish(cached_image_path(images, id), encoded, {});
  split.images.push_back({id, 16, 8, 0, 0});
 }
 mmltk::testsupport::TestGate physical("full image service retains its physical borrowers");
 PendingImageEvidence evidence;
 PipelineImageReceipts physical_receipts(admitted), pending_receipts(count), replacement_receipt(1);
 std::array<std::atomic<bool>, admitted> physically_started{};
 std::promise<void> full, pending;
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json& fields) {
  if (event == "file.admitted" && fields.at("service") == "image" && fields.at("records") == admitted) mmltk::testsupport::release_test_promise(full);
  if (event == "benchmark.image.stage_started" && fields.at("stage") == "input") {
   physically_started.at(fields.at("image_id").get<std::size_t>() - 1) = true;
   physical.receipt().ArriveAndWait();
  }
  if (event == "benchmark.pipeline.ready") {
   evidence.observe(fields);
   if (fields.at("header") == count && fields.at("image_admitted") == 0 && fields.at("image_head_probes") == 1) mmltk::testsupport::release_test_promise(pending);
  }
 };
 BenchmarkCompilePipeline pipeline(4, {}, {.transient_bytes = 4U << 20, .task_records = admitted}, {}, trace);
 auto request = benchmark_write_request(split, root.path() / "result.bin", 8, {}, &pipeline);
 write_text(request.output_path, "previously published pixels");
 const auto previous = mmltk::common::io::sha256_file(request.output_path);
 BenchmarkSplitWriter writer(request);
 BenchmarkCompilePipeline::Attempt attempt(pipeline);
 bind_benchmark_slots(pipeline, writer, split);
 std::future<void> draining;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  physical.Release();
  if (draining.valid()) {
   pipeline.execution().cancel();
   draining.wait();
  }
  pipeline.retire_attempt();
 });
 const auto publication = pipeline.source_publication(images, {}, {}, true);
 for (std::size_t id = 1; id <= admitted; ++id) {
  publication({id});
  physical_receipts.subscribe(pipeline.image_completion(images, id), id - 1);
 }
 REQUIRE(physical.WaitEntered(5s, 2));
 mmltk::testsupport::await_test_promise(full, "all bounded image file records hold admitted work", 5s);
 const auto locked = pipeline.execution().snapshot().normal_locked;
 CHECK(pipeline.execution().snapshot().records == admitted);
 for (std::size_t index = 0; index < count; ++index) {
  const auto id = admitted + index + 1;
  publication({id});
  pending_receipts.subscribe(pipeline.image_completion(images, id), index);
 }
 mmltk::testsupport::await_test_promise(pending, "pending population exceeds both physical and CPU record banks", 5s);
 const auto original = std::make_exception_ptr(std::runtime_error("first failure behind full image records"));
 if (fail) {
  pipeline.execution().fail(original);
  pipeline.execution().fail(std::make_exception_ptr(std::runtime_error("later failure must not replace the cause")));
 } else
  pipeline.execution().cancel();
 draining = std::async(std::launch::async, [&] { pipeline.drain(); });
 pending_receipts.wait();
 for (const auto& image : pending_receipts.images) {
  CHECK(image.error);
  if (fail) CHECK(image.error == original);
 }
 CHECK(physical_receipts.settled <= admitted - 2);
 CHECK(draining.wait_for(0ms) == std::future_status::timeout);
 for (std::size_t index = 0; index < admitted; ++index)
  if (physically_started[index]) {
   CHECK(physical_receipts.images[index].settled == 0);
   CHECK_FALSE(pipeline.image_completion(images, index + 1).ready());
  }
 CHECK(pipeline.execution().snapshot().leased > 0);
 CHECK(mmltk::common::io::sha256_file(request.output_path) == previous);
 physical.Release();
 physical_receipts.wait();
 if (fail)
  CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(draining, "failed attempt joins all physical image borrowers", 5s), "first failure behind full image records");
 else
  CHECK_THROWS(mmltk::testsupport::await_test_future(draining, "cancelled attempt joins all physical image borrowers", 5s));
 for (const auto& image : physical_receipts.images) {
  CHECK(image.error);
  if (fail) CHECK(image.error == original);
 }
 CHECK(pipeline.execution().snapshot().leased == 0);
 pipeline.retire_attempt();
 bind_benchmark_slots(pipeline, writer, split);
 publication({1});
 CHECK_FALSE(pipeline.image_completion(images, 1));
 const auto replacement = pipeline.source_publication(images, {}, {}, true);
 replacement({1});
 replacement_receipt.subscribe(pipeline.image_completion(images, 1), 0);
 replacement_receipt.wait();
 pipeline.drain();
 CHECK_FALSE(replacement_receipt.images.front().error);
 CHECK(writer.completed() == 1);
 CHECK(pipeline.execution().snapshot().leased == 0);
 CHECK(pipeline.execution().snapshot().normal_locked == locked);
 CHECK(evidence.bounded);
 CHECK(evidence.observations > 0);
 CHECK(mmltk::common::io::sha256_file(request.output_path) == previous);
}
TEST_CASE("mixed annotation extents retain every row and independent pixels through settlement", "[benchmark][progress]") {
 std::vector<BenchmarkCompileProgress> snapshots;
 ProgressReporter reporter([&](const auto& snapshot) { snapshots.push_back(snapshot); }, {});
 const std::array<std::uint64_t, 3> extents{241602, 0, 0};
 auto& rows = *reporter.indexing(extents);
 reporter.label_plans(3);
 for (std::size_t i = 0; i < 3; ++i) reporter.label_plan_started(i);
 reporter.pixels(16177, 0, false);
 rows.update(0, 241596, reporter);
 reporter.flush();
 REQUIRE_FALSE(snapshots.empty());
 CHECK(snapshots.back().tracks.labels.completed == 241596);
 CHECK_FALSE(snapshots.back().tracks.labels.total_known);
 CHECK(snapshots.back().tracks.labels.total == 0);
 for (std::size_t release = 1; release < 3; ++release) {
  rows.update(release, 64, reporter);
  reporter.pixel_completed();
  reporter.phase(DatasetCompilePhase::Indexing, release, 2);
  reporter.flush();
  CHECK(snapshots.back().tracks.labels.completed == 241596 + 64 * release);
  CHECK(snapshots.back().tracks.pixels.completed == 16177 + release);
  CHECK(snapshots.back().total == 0);
 }
 rows.settle(1, 64, reporter);
 rows.settle(2, 64, reporter);
 reporter.flush();
 CHECK(snapshots.back().tracks.labels.total_known);
 CHECK(snapshots.back().tracks.labels.total == 241730);
 rows.settle(0, 241602, reporter);
 reporter.flush();
 CHECK(snapshots.back().tracks.labels.completed == 241730);
 CHECK_FALSE(snapshots.back().tracks.labels.complete);
 for (std::size_t i = 0; i < 3; ++i) reporter.label_plan_completed(i);
 reporter.flush();
 CHECK(snapshots.back().tracks.labels.completed == 241730);
 CHECK(snapshots.back().tracks.labels.complete);
 rows.invalidate(1, reporter);
 reporter.flush();
 CHECK_FALSE(snapshots.back().tracks.labels.total_known);
 CHECK_FALSE(snapshots.back().tracks.labels.complete);
 CHECK(snapshots.back().tracks.labels.invalidated == 64);
 rows.settle(1, 64, reporter);
 reporter.flush();
 for (const auto& snapshot : snapshots) {
  CHECK(snapshot.tracks.valid());
  CHECK((snapshot.total == 0 || snapshot.completed <= snapshot.total));
 }
}
TEST_CASE("an empty annotation extent is known only while its settlement remains admitted", "[benchmark][progress]") {
 BenchmarkCompileProgress latest;
 ProgressReporter reporter([&](const auto& snapshot) { latest = snapshot; }, {});
 const std::array<std::uint64_t, 1> extent{0};
 auto& rows = *reporter.indexing(extent);
 reporter.label_plans(1);
 reporter.label_plan_started(0);
 rows.update(0, 0, reporter);
 reporter.flush();
 CHECK_FALSE(latest.tracks.labels.total_known);
 rows.settle(0, 0, reporter);
 reporter.label_plan_completed(0);
 reporter.flush();
 CHECK(latest.tracks.labels.total_known);
 CHECK(latest.tracks.labels.complete);
 rows.invalidate(0, reporter, 0);
 reporter.flush();
 CHECK_FALSE(latest.tracks.labels.total_known);
 CHECK_FALSE(latest.tracks.labels.complete);
 CHECK(latest.tracks.labels.invalidated == 0);
 rows.update(0, 7, reporter);
 reporter.flush();
 CHECK(latest.tracks.labels.completed == 7);
 CHECK(latest.total == 0);
 rows.settle(0, 7, reporter);
 reporter.flush();
 CHECK(latest.tracks.labels.complete);
 CHECK(latest.tracks.labels.total == 7);
}
TEST_CASE("normalized cache publication transfers one gathered inode and preserves independent destinations", "[benchmark][annotations][cache][resources]") {
 mmltk::testsupport::ScopedTempDir root("normalized-cache-layout");
 const unsigned shape = GENERATE(0U, 1U, 2U);  // Background, empty mask, nonempty mask.
 auto input = label_projection_fixture(7);
 if (shape < 2) {
  NormalizedAnnotationBuilder builder;
  builder.images.push_back(input.image(0));
  builder.images.front().box_count = shape;
  if (shape) {
   builder.boxes.push_back(input.storage().boxes.front());
   builder.boxes.front().mask_rle_pairs = 0;
  }
  input = NormalizedAnnotationReadView(seal_normalized_annotations(std::move(builder)));
 }
 auto metadata = static_cast<const NormalizedAnnotationMetadata&>(input);
 metadata.split = "same-backing";
 metadata.annotation_sha256 = std::string(64, 'a');
 detail::StorageReservationPool storage(root.path());
 NormalizedAnnotationAssembler assembly(metadata, {}, root.path(), &storage);
 const std::array slices{NormalizedAnnotationAssembler::Slice::admitted(input.storage(), 0)};
 std::uint64_t validated_runs = UINT64_MAX;
 assembly.gather(slices, nullptr, nullptr, {}, [&](std::string_view event, const nlohmann::json& fields) {
  if (event == "benchmark.annotations.gathered") validated_runs = fields.at("validated_runs");
 });
 auto index = assembly.finish();
 CHECK(validated_runs == 0);
 std::filesystem::path staged;
 for (const auto& file : fs::directory_iterator(root.path())) staged = file.path();
 REQUIRE_FALSE(staged.empty());
 struct stat before{}, first{}, second{};
 REQUIRE(::stat(staged.c_str(), &before) == 0);
 const auto destination = root.path() / "first.bin";
 std::vector<std::uint64_t> copied;
 const BenchmarkTraceSink trace = [&](std::string_view event, const nlohmann::json& fields) {
  if (event == "benchmark.annotations.cache_prepare") copied.push_back(fields.at("payload_copy_bytes"));
 };
 const auto completion = store_normalized_annotation_index(destination, index, {}, trace, &storage);
 REQUIRE(::stat(destination.c_str(), &first) == 0);
 CHECK(first.st_ino == before.st_ino);
 CHECK_FALSE(fs::exists(staged));
 REQUIRE(copied.size() == 1);
 CHECK(copied.front() == 0);
 const auto other = root.path() / "second.bin";
 const auto another = store_normalized_annotation_index(other, index, {}, trace, &storage);
 REQUIRE(::stat(other.c_str(), &second) == 0);
 REQUIRE(::stat(destination.c_str(), &first) == 0);
 CHECK(first.st_ino == before.st_ino);
 CHECK(second.st_ino != first.st_ino);
 CHECK(another->identity == completion->identity);
 CHECK(mmltk::common::io::sha256_file(destination) == mmltk::common::io::sha256_file(other));
 REQUIRE(copied.size() == 2);
 CHECK(copied.back() == index.images.size_bytes() + index.boxes.size_bytes() + index.mask_rle_pairs.size_bytes());
 const auto loaded = load_normalized_annotation_index(destination, index.source, index.split, index.annotation_sha256, {});
 REQUIRE(loaded);
 CHECK(loaded->images.size() == 1);
 CHECK(loaded->boxes.size() == (shape ? 1 : 0));
 CHECK(loaded->mask_rle_pairs.size() == (shape == 2 ? 3 : 0));
 if (shape == 2) CHECK(std::memcmp(loaded->mask_rle_pairs.data(), input.storage().mask_rle_pairs.data(), input.storage().mask_rle_pairs.size_bytes()) == 0);
 std::array<std::uint64_t, 4> layout{};
 FileHandle::open_readonly(destination.string()).pread_all(layout.data(), sizeof(layout), 36);
 const std::uint64_t run_offset = shape ? 352 : 288;
 CHECK((layout == std::array<std::uint64_t, 4>{256, 288, run_offset, run_offset + (shape == 2 ? 24 : 0)}));
 std::atomic<bool> cancelled{true};
 CHECK_THROWS(store_normalized_annotation_index(destination, index, mmltk::common::concurrency::CancellationObservation::Atomic(cancelled), trace, &storage));
 REQUIRE(::stat(destination.c_str(), &first) == 0);
 CHECK(first.st_ino == before.st_ino);
 if (shape == 2) CHECK(index.mask_rle_pairs.back().start == 4);
}
TEST_CASE("immutable row admission survives identity rebinding and rejects changed mask geometry", "[benchmark][annotations][resources]") {
 const auto input = label_projection_fixture(7);
 auto row = seal_normalized_annotation_image(input.source, input.image(0), input.storage().boxes, input.storage().mask_rle_pairs, input.storage().backing);
 auto image = row.images.front();
 image.source_shard = 4;
 image.source_image_id = 8;
 auto rebound = rebind_normalized_annotation_image(row, row.source, image);
 CHECK(rebound.mask_rle_pairs.data() == row.mask_rle_pairs.data());
 CHECK(rebound.images.front().source_shard == 4);
 image.width = image.height = 1;
 CHECK_THROWS(rebind_normalized_annotation_image(row, row.source, image));
 auto slice = NormalizedAnnotationAssembler::Slice::admitted(rebound, 0);
 slice.image.width = slice.image.height = 1;
 NormalizedAnnotationAssembler assembly({}, {}, {});
 CHECK_THROWS(assembly.gather(std::span(&slice, 1)));
 CHECK_THROWS(assembly.finish());
}
