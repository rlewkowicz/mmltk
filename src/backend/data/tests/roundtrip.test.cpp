#include <cuda_runtime.h>
#include <cuda.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <signal.h>
#include <pthread.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <random>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <optional>
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <span>
#include <vector>
#include "src/backend/data/compiled/compiled_format.h"
#include "src/backend/data/compiled/compiled_file_utils.h"
#include "src/common/io/file_memory.h"
#include <limits>
#include "src/backend/data/compiled/compiled_dataset.h"
#include "src/backend/data/compiled/compiled_image_stream.h"
#include "src/backend/data/detail/file_work_queue.h"
#include "src/frameworks/gpu/image/image_buffer.h"
#include "src/frameworks/gpu/cuda/terminal_cuda_retirement_owner.h"
#include "src/backend/data/compiler/dataset_compiler.h"
#include "src/backend/data/loading/dataset_loader.h"
#include "src/common/concurrency/concurrency.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/backend/imaging/resample/tests/perceptual_downscale_reference.h"
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
#include "src/common/system/cpu_affinity.h"
#include "src/common/system/execution_policy.h"
#include "src/frameworks/gpu/cuda/cuda_error.h"
#include "src/frameworks/gpu/memory/gdr_mapped_buffer.h"
namespace fs = std::filesystem;
using namespace mmltk::backend::data;
using namespace mmltk::common::concurrency;
using namespace mmltk::common::logging;
using namespace mmltk::common::system;
using mmltk::frameworks::gpu::ensure_cuda_ok;
using namespace mmltk::backend::data::testsupport;
// CLEANUP-IGNORE: Independent stream and bounded-gather cases open their own dataset and read its stride; no shared execution algorithm.
void exercise_compiled_stream(const std::string& path, bool h2d) {
 const auto source = CompiledDataset::open(path);
 const auto stride = static_cast<std::size_t>(source.header().image_stride);
 REQUIRE(source.slot_count() == source.image_entries().size());
 REQUIRE(source.pixel_offset(0) == HUGE_PAGE_SIZE);
 DatasetLoader::Config training_config;
 training_config.compiled_path = path;
 training_config.batch_size = 3U;
 training_config.prefetch_factor = 2;
 training_config.loading.h2d_dataloader = h2d;
 DatasetLoader training(training_config);
 training.begin_epoch();
 // Model construction precedes the isolated runtime context. The stream
 // must capture the later context and bind it on its completion/I/O workers.
 std::optional<mmltk::frameworks::gpu::DeviceContext> context;
 // Callback facts and receiver memory outlive stream teardown on assertions.
 std::vector<std::byte> received(2U * stride);
 struct Completion {
  std::atomic<bool> done = false;
  std::exception_ptr failure;
 } completion;
 const auto execution = mmltk::frameworks::gpu::resolve_device_execution(0, NumaTopology::Capture());
 auto readers = std::make_shared<WorkerPool>(1U, execution.placement.cpus, "shared-read", 2U, &execution.placement, true);
 std::size_t storage_growths = 0;
 CompiledImageStream stream(
  {.slots = 1U,
   .workers = 1U,
   .device = 0,
   .prefault = true,
   .loading = data_loading_options(h2d),
   .execution = execution,
   .reader_pool = readers,
   .storage_growth = {&storage_growths, [](void* observer_context, CompiledImageStream::StorageKind, std::size_t, std::size_t, std::size_t) noexcept {
  ++*static_cast<std::size_t*>(observer_context);
 }}});
 CHECK(&stream.workers() == readers.get());
 context.emplace(0, mmltk::frameworks::gpu::cuda_image_copy_backend());
 context->Bind();
 stream.bind_current_context();
 constexpr std::size_t maximum_batch = 30;
 stream.prepare_images(0, maximum_batch * stride);
 stream.prepare_reads(0, maximum_batch, source.header().max_label_bytes);
 const auto prepared_growths = storage_growths;
 CHECK(stream.host_storage(0).owns_allocation());
 CHECK(stream.host_storage(0).capacity_bytes() >= maximum_batch * stride);
 CHECK(stream.device_storage(0).capacity_bytes() >= maximum_batch * stride);
 const auto* host = stream.host_storage(0).data();
 const auto* const prepared_host = host;
 auto* const labels = static_cast<std::byte*>(stream.label_storage(0).data());
 const auto label_capacity = stream.label_storage(0).capacity_bytes();
 REQUIRE(label_capacity >= maximum_batch * source.header().max_label_bytes);
 CHECK(label_capacity < maximum_batch * COMPILED_LABEL_CAPACITY);
 std::memset(labels, 0x5a, 2U * source.header().max_label_bytes);
 const std::array reads{CompiledImageRead{13U, 0U}, CompiledImageRead{1U, stride}};
 const auto caller_policy = capture_execution_policy_snapshot();
 const auto first_read_started = std::chrono::steady_clock::now();
 stream.submit(0U, source, reads, {});
 REQUIRE(stream.wait_read(0U));
 host = stream.host_images(0U).data();
 const auto first_host_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - first_read_started).count();
 const auto restored_policy = capture_execution_policy_snapshot();
 CHECK(restored_policy.affinity == caller_policy.affinity);
 CHECK(restored_policy.memory_policy.mode == caller_policy.memory_policy.mode);
 CHECK(restored_policy.memory_policy.mask == caller_policy.memory_policy.mask);
 CHECK(restored_policy.nice_value == caller_policy.nice_value);
 CHECK(restored_policy.scheduler_policy == caller_policy.scheduler_policy);
 CHECK(restored_policy.scheduler_priority == caller_policy.scheduler_priority);
 CHECK(restored_policy.io_class == caller_policy.io_class);
 CHECK(restored_policy.io_priority_data == caller_policy.io_priority_data);
 CHECK(restored_policy.thread_name == caller_policy.thread_name);
 const auto* device = stream.device_storage(0U).data();
 REQUIRE(host == prepared_host);
 REQUIRE(std::memcmp(host, source.image_pixels(13U), stride) == 0);
 REQUIRE(stream.annotations(0).size() == 2U);
 CHECK(stream.annotations(0)[0].labels.data() == reinterpret_cast<const PackedInstance*>(labels));
 CHECK(stream.annotations(0)[0].labels.size() == source.image_labels(13U).size());
 CHECK(stream.annotations(0)[1].labels.empty());
 const auto used = source.label_payload(13U);
 CHECK(std::memcmp(labels, used.data(), used.size()) == 0);
 CHECK(std::ranges::all_of(std::span{labels + used.size(), 2U * source.header().max_label_bytes - used.size()}, [](std::byte value) { return value == std::byte{0x5a}; }));
 REQUIRE(std::memcmp(static_cast<const std::byte*>(host) + stride, source.image_pixels(1U), stride) == 0);
 stream.handoff(0U, nullptr);
 ensure_cuda_ok(cudaMemcpyAsync(received.data(), device, received.size(), cudaMemcpyDeviceToHost, nullptr), "compiled stream consumer read");
 const CompiledImageStream::CompletionObserver completed{.context = &completion, .complete = [](void* raw, std::size_t, std::exception_ptr error) noexcept {
  auto& state = *static_cast<Completion*>(raw);
  state.failure = error;
  state.done.store(true, std::memory_order_release);
 }};
 stream.release(0U, nullptr, completed);
 stream.synchronize();
 REQUIRE(completion.done.load(std::memory_order_acquire));
 REQUIRE_FALSE(completion.failure);
 REQUIRE(std::memcmp(received.data(), host, received.size()) == 0);
 Batch training_batch{};
 std::size_t training_images = 0U;
 while (training.next_batch(training_batch)) {
  training.wait_batch(training_batch);
  training_images += training_batch.num_images;
  training.release_batch(training_batch);
 }
 CHECK(training_images == source.header().num_images);
 training.synchronize();
 stream.prepare_host(0U, stride);
 stream.prepare_device(0U, stride);
 stream.prepare_reads(0U, 1U);
 REQUIRE(stream.host_storage(0U).data() == host);
 REQUIRE(stream.device_storage(0U).data() == device);
 for (std::size_t iteration = 0U; iteration < 8U; ++iteration) {
  stream.submit(0U, source, reads, {});
  REQUIRE(stream.wait_read(0U));
  stream.handoff(0U, nullptr);
  stream.release(0U, nullptr, {});
  stream.synchronize();
  CHECK(stream.host_storage(0U).data() == host);
  CHECK(stream.device_storage(0U).data() == device);
  host = stream.host_images(0U).data();
  CHECK(host == prepared_host);
  CHECK(stream.label_storage(0).data() == labels);
  CHECK(stream.label_storage(0).capacity_bytes() == label_capacity);
  CHECK(std::memcmp(host, source.image_pixels(13U), stride) == 0);
 }
 CHECK(storage_growths == prepared_growths);
 const auto preparation_growths = prepared_growths;
 std::printf(
  "compiled_stream_fixture transport=%s first_host_pixels_labels_ns=%lld active_pixel_bytes=%zu active_label_bytes=%zu logical_label_capacity=%zu retained_label_bytes=%zu retained_host_bytes=%zu "
  "retained_device_bytes=%zu preparation_growths=%zu steady_growths=0 scope=owned_buffers_and_read_containers_not_total_malloc cache=warm_fixture\n",
  h2d ? "h2d" : "gdrcopy", static_cast<long long>(first_host_ns), 2U * stride, used.size(), maximum_batch * source.header().max_label_bytes, label_capacity, stream.host_storage(0).capacity_bytes(),
  stream.device_storage(0).capacity_bytes(), preparation_growths);
 const std::array invalid{CompiledImageRead{source.header().num_images, 0U}};
 stream.submit(0U, source, invalid, {});
 REQUIRE_THROWS_AS(stream.wait_read(0U), std::out_of_range);
 stream.synchronize();
 {
  mmltk::testsupport::TestGate gate("compiled stream physical read");
  auto receipt = gate.receipt();
  std::future<bool> read_result;
  const mmltk::testsupport::ScopedTestCleanup release_read([&] {
   stream.cancel_read(0U);
   gate.Release();
   stream.wait_reads();
  });
  stream.submit(0U, source, reads, {.context = &receipt, .before = [](void* raw, std::size_t) {
   static_cast<mmltk::testsupport::TestGate::Receipt*>(raw)->ArriveAndWait();
   return true;
  }});
  read_result = std::async(std::launch::async, [&] { return stream.wait_read(0U); });
  REQUIRE(gate.WaitEntered(std::chrono::seconds{2}));
  // A checked-out physical read job cannot be replaced while its worker owns it.
  CHECK_THROWS_AS(stream.submit(0U, source, reads, {}), std::logic_error);
  stream.cancel_read(0U);
  gate.Release();
  REQUIRE_FALSE(mmltk::testsupport::await_test_future(read_result, "cancelled compiled read settlement"));
  stream.synchronize();
 }
 // Retained-cache work still needs the lane's completion event after a
 // cancelled read, including failures before a new mapped lease is published.
 completion.done.store(false, std::memory_order_release);
 stream.fence(0U, nullptr, completed);
 stream.synchronize();
 REQUIRE(completion.done.load(std::memory_order_acquire));
 REQUIRE_FALSE(completion.failure);
 stream.submit(0U, source, reads, {});
 REQUIRE(stream.wait_read(0U));
 stream.handoff(0U, nullptr);
 ensure_cuda_ok(cudaMemcpyAsync(received.data(), device, received.size(), cudaMemcpyDeviceToHost, nullptr), "compiled stream reused consumer read");
 completion.done.store(false, std::memory_order_release);
 stream.release(0U, nullptr, completed);
 stream.synchronize();
 REQUIRE(completion.done.load(std::memory_order_acquire));
 REQUIRE_FALSE(completion.failure);
 CHECK(std::memcmp(received.data(), source.image_pixels(13U), stride) == 0);
 CHECK(std::memcmp(received.data() + stride, source.image_pixels(1U), stride) == 0);
 REQUIRE(stream.reset_storage() == cudaSuccess);
 REQUIRE_FALSE(stream.owns_allocation());
 {
  mmltk::testsupport::TestGate gate("unrelated shared reader work");
  auto held = readers->enqueue([receipt = gate.receipt()] { receipt.ArriveAndWait(); });
  std::future<void> stopped;
  const mmltk::testsupport::ScopedTestCleanup release([&] { gate.Release(); });
  REQUIRE(gate.WaitEntered(std::chrono::seconds(2)));
  stopped = std::async(std::launch::async, [&] { stream.stop_workers(); });
  REQUIRE_NOTHROW(mmltk::testsupport::await_test_future(stopped, "stream retirement independent of shared readers"));
  gate.Release();
  held.get();
  CHECK(readers->enqueue([] { return 42; }).get() == 42);
 }
 stream.close();
 REQUIRE_FALSE(stream.owns_resources());
}
void exercise_schedule_capacity(const std::string& path, const bool h2d) {
 const auto source = CompiledDataset::open(path);
 struct Schedule {
  std::size_t batch;
  int prefetch;
  std::uint32_t shards, rank;
  bool drop;
 };
 const std::array cases{
  Schedule{32U, 8, 1U, 0U, false}, Schedule{7U, 8, 3U, 2U, false}, Schedule{7U, 8, 4U, 3U, false}, Schedule{32U, 8, 1U, 0U, true}, Schedule{4U, 2, 1U, 0U, false}, Schedule{10U, 2, 1U, 0U, false},
  Schedule{7U, 8, 1U, 0U, true}
 };
 for (const auto schedule : cases)
  for (const bool shuffle : {false, true}) {
   INFO("batch " << schedule.batch << " prefetch " << schedule.prefetch << " shards " << schedule.shards << " rank " << schedule.rank << " drop " << schedule.drop << " shuffle " << shuffle << " H2D "
                 << h2d);
   DatasetLoader::Config config;
   config.compiled_path = path;
   config.batch_size = schedule.batch;
   config.prefetch_factor = schedule.prefetch;
   config.gather_workers = schedule.prefetch;
   config.batch_shard_count = schedule.shards;
   config.batch_shard_rank = schedule.rank;
   config.drop_last = schedule.drop;
   config.shuffle = shuffle;
   config.loading.h2d_dataloader = h2d;
   DatasetLoader loader(config);
   const auto count = source.header().num_images;
   std::vector<std::size_t> starts;
   for (std::size_t start = 0U, ordinal = 0U; start < count; start += schedule.batch, ++ordinal) {
    if (schedule.drop && count - start < schedule.batch) break;
    if (ordinal % schedule.shards == schedule.rank) starts.push_back(start);
   }
   CHECK(loader.num_batches() == starts.size());
   for (unsigned epoch = 0U; epoch < 2U; ++epoch) {
    loader.begin_epoch();
    Batch batch{};
    std::size_t ordinal = 0U;
    std::vector<std::uint32_t> seen;
    while (loader.next_batch(batch)) {
     REQUIRE(ordinal < starts.size());
     CHECK(batch.num_images == std::min(schedule.batch, count - starts[ordinal]));
     CHECK(batch.image_capacity_bytes == batch.num_images * source.header().image_stride);
     CHECK(batch.slot_index < std::min<std::size_t>(schedule.prefetch, starts.size()));
     loader.wait_batch(batch);
     const auto host = loader.host_images(batch);
     std::vector<float> received(host.size());
     ensure_cuda_ok(cudaMemcpy(received.data(), batch.device_images, batch.image_capacity_bytes, cudaMemcpyDeviceToHost), "scheduled batch pixels");
     CHECK(std::memcmp(received.data(), host.data(), batch.image_capacity_bytes) == 0);
     for (std::size_t image = 0U; image < batch.num_images; ++image) {
      const auto index = batch.image_indices[image];
      CHECK(index < count);
      if (!shuffle) CHECK(index == starts[ordinal] + image);
      REQUIRE(std::memcmp(host.data() + image * source.header().image_stride / sizeof(float), source.image_pixels(index), source.header().image_stride) == 0);
      seen.push_back(index);
     }
     loader.release_batch(batch);
     ++ordinal;
    }
    CHECK(ordinal == starts.size());
    std::ranges::sort(seen);
    CHECK(std::ranges::adjacent_find(seen) == seen.end());
    loader.synchronize();
   }
   loader.stop_workers();
   Batch stopped{};
   CHECK_FALSE(loader.next_batch(stopped));
  }
 DatasetLoader::Config overflow;
 overflow.compiled_path = path;
 overflow.loading.h2d_dataloader = h2d;
 overflow.batch_size = std::numeric_limits<std::size_t>::max();
 overflow.drop_last = true;
 CHECK_THROWS_AS(DatasetLoader(overflow), std::overflow_error);
}
void exercise_sparse_capacity(const std::string& path, const bool h2d) {
 const mmltk::testsupport::ScopedTempDir root("sparse-loader-capacity");
 const auto sparse_path = root.path() / "sparse.bin";
 const auto original = CompiledDataset::open(path);
 auto header = original.header();
 header.num_images = 2;
 auto entries = std::vector<ImageEntry>(original.image_entries().begin(), original.image_entries().end());
 for (std::size_t slot = 0; slot < entries.size(); ++slot)
  if (slot != 0 && slot != 13) entries[slot] = {};
 const auto layout = compute_file_layout(header.num_slots, header.image_stride, header.max_label_bytes);
 {
  const auto file = create_compiled_fixture_file(sparse_path, header);
  file.pwrite_all(entries.data(), layout.index_size, layout.index_offset);
  for (const auto slot : {0U, 13U}) {
   file.pwrite_all(original.image_pixels(slot), header.image_stride, layout.pixel_offset(slot));
   const auto payload = original.label_payload(slot);
   file.pwrite_all(payload.data(), payload.size(), layout.label_offset(slot));
  }
 }
 DatasetLoader::Config config;
 config.compiled_path = sparse_path.string();
 config.batch_size = 32;
 config.prefetch_factor = 1;
 config.shuffle = false;
 config.loading.h2d_dataloader = h2d;
 DatasetLoader loader(config);
 REQUIRE(loader.num_images() == 2);
 CHECK(std::ranges::equal(loader.eligible_slots(), std::array{0U, 13U}));
 Batch batch;
 REQUIRE(loader.next_batch(batch));
 REQUIRE(batch.num_images == 2);
 CHECK(batch.image_indices[0] == 0);
 CHECK(batch.image_indices[1] == 13);
 loader.wait_batch(batch);
 const auto* device = batch.device_images;
 const auto* host = loader.host_images(batch).data();
 const auto* views = batch.annotation_views;
 const auto* labels = batch.annotations(1).labels.data();
 REQUIRE(labels != nullptr);
 loader.release_batch(batch);
 CHECK_FALSE(loader.next_batch(batch));
 loader.synchronize();
 auto invalid = std::make_shared<DatasetIndexSchedule>();
 invalid->image_indices = {1};
 CHECK_THROWS(loader.begin_epoch(invalid));
 auto repeated = std::make_shared<DatasetIndexSchedule>();
 repeated->image_indices.resize(config.batch_size * 3);
 for (std::size_t i = 0; i < repeated->image_indices.size(); ++i) repeated->image_indices[i] = i % 2 ? 13U : 0U;
 for (unsigned epoch = 0; epoch < 2; ++epoch) {
  loader.begin_epoch(repeated, epoch * config.batch_size);
  CHECK(loader.num_batches() == 3 - epoch);
  std::size_t batches = 0;
  while (loader.next_batch(batch)) {
   REQUIRE(batch.num_images == config.batch_size);
   loader.wait_batch(batch);
   CHECK(batch.device_images == device);
   CHECK(batch.annotation_views == views);
   CHECK(batch.annotations(1).labels.data() == labels);
   const auto pixels = loader.host_images(batch);
   CHECK(pixels.data() == host);
   for (std::size_t i = 0; i < batch.num_images; ++i) {
    const auto slot = repeated->image_indices[i];
    CHECK(batch.image_indices[i] == slot);
    CHECK(batch.annotations(i).labels.size() == original.image_labels(slot).size());
    CHECK(std::memcmp(pixels.data() + i * header.image_stride / sizeof(float), original.image_pixels(slot), header.image_stride) == 0);
    if (slot == 13)
     CHECK(reinterpret_cast<const std::byte*>(batch.annotations(i).labels.data()) - reinterpret_cast<const std::byte*>(labels) == static_cast<std::ptrdiff_t>((i - 1) * header.max_label_bytes));
   }
   ++batches;
   loader.release_batch(batch);
  }
  CHECK(batches == 3 - epoch);
  loader.synchronize();
 }
}
void exercise_demanded_host_shutdown(const std::string& path, const bool h2d) {
 DatasetLoader::Config config;
 config.compiled_path = path;
 config.batch_size = 2;
 config.prefetch_factor = 2;
 config.gather_workers = 1;
 config.shuffle = false;
 config.loading.h2d_dataloader = h2d;
 auto schedule = std::make_shared<DatasetIndexSchedule>();
 schedule->image_indices = {3, 1, 13, 1};
 for (const bool materialize_before_stop : {false, true}) {
  DatasetLoader loader(config);
  loader.begin_epoch(schedule);
  Batch batch;
  REQUIRE(loader.next_batch(batch));
  const mmltk::testsupport::ScopedTestCleanup release([&] {
   loader.stop_workers();
   loader.release_batch(batch);
  });
  loader.wait_batch(batch);
  const float* prepared = materialize_before_stop ? loader.host_images(batch).data() : nullptr;
  loader.stop_workers();
  CHECK_FALSE(batch.image_custody.expired());
  const auto pixels = loader.host_images(batch);
  if (prepared) CHECK(pixels.data() == prepared);
  CHECK(loader.host_images(batch).data() == pixels.data());
  const auto stride = loader.image_stride();
  for (std::size_t image = 0; image < batch.num_images; ++image) CHECK(std::memcmp(pixels.data() + image * stride / sizeof(float), loader.image_pixels(batch.image_indices[image]), stride) == 0);
 }
}
void exercise_independent_completions(const std::string& path, const bool h2d) {
 namespace gpu = mmltk::frameworks::gpu;
 // The ordinary roundtrip fixture uses identical RGB stubs. Give each image
 // in this private copy a distinct marker so destination-order checks can fail.
 const mmltk::testsupport::ScopedTempDir locality("compiled-locality");
 const auto locality_path = locality.path() / "images.bin";
 REQUIRE(fs::copy_file(path, locality_path));
 {
  const auto original = CompiledDataset::open(path);
  std::fstream pixels(locality_path, std::ios::in | std::ios::out | std::ios::binary);
  for (std::uint32_t index = 0U; index < original.header().num_images; ++index) {
   const float marker = static_cast<float>(index + 1U) / static_cast<float>(original.header().num_images + 1U);
   pixels.seekp(static_cast<std::streamoff>(original.pixel_offset(index)));
   pixels.write(reinterpret_cast<const char*>(&marker), sizeof(marker));
  }
  pixels.flush();
  REQUIRE(pixels.good());
 }
 const auto source = CompiledDataset::open(locality_path);
 const auto stride = static_cast<std::size_t>(source.header().image_stride);
 const std::array reads{CompiledImageRead{3U, 0U}, CompiledImageRead{1U, stride}, CompiledImageRead{3U, 2U * stride}};
 const gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::PrimaryInterop);
 gpu::ImageStream consumer(context), releaser(context);
 const auto consumer_stream = reinterpret_cast<cudaStream_t>(consumer.native_handle());
 const auto release_stream = reinterpret_cast<CUstream>(releaser.native_handle());
 CUdeviceptr gate = 0U;
 gpu::ensure_cuda_driver_ok(cuMemAlloc(&gate, sizeof(std::uint32_t)), "compiled consumer gate allocation");
 const mmltk::testsupport::ScopedTestCleanup free_gate([&] { (void)cuMemFree(gate); });
 gpu::ensure_cuda_driver_ok(cuStreamWriteValue32(release_stream, gate, 0U, CU_STREAM_WRITE_VALUE_DEFAULT), "compiled consumer gate initialization");
 releaser.Synchronize();
 std::array<std::promise<std::exception_ptr>, 5> transfers;
 const auto transfer_complete = +[](void* raw, std::size_t, std::exception_ptr error) noexcept { static_cast<std::promise<std::exception_ptr>*>(raw)->set_value(error); };
 std::atomic<bool> consumed{false};
 CompiledImageStream stream({.slots = 2U, .workers = 1U, .device = 0, .loading = data_loading_options(h2d)});
 stream.bind_current_context();
 for (std::size_t slot = 0U; slot < 2U; ++slot) {
  stream.prepare_images(slot, reads.size() * stride);
  stream.prepare_reads(slot, reads.size());
 }
 stream.prepare_host(1U, reads.size() * stride);
 stream.prepare_metadata(0U, reads.size() * stride);
 const auto* second_device = stream.device_storage(1U).data();
 const auto* second_host = stream.host_storage(1U).data();
 stream.submit(0U, source, reads, {});
 REQUIRE(stream.wait_read(0U));
 stream.synchronize(0U);
 {
  // Model a second prefetch completing through an owner's lease mutex while
  // the checked-out first slot demands CPU pixels. One reader must suffice.
  struct CompletionBoundary {
   std::mutex mutex;
   std::promise<void> entered;
  } boundary;
  std::unique_lock owner_lock(boundary.mutex);
  std::future<std::span<const std::byte>> demanded;
  const mmltk::testsupport::ScopedTestCleanup settle([&] {
   if (owner_lock.owns_lock()) owner_lock.unlock();
   stream.synchronize(1U);
  });
  const auto* prepared = stream.host_storage(0U).data();
  const auto capacity = stream.host_storage(0U).capacity_bytes();
  stream.submit(1U, source, reads, {.context = &boundary, .complete = [](void* raw, std::size_t, std::exception_ptr, bool) noexcept {
   auto& completion_boundary = *static_cast<CompletionBoundary*>(raw);
   completion_boundary.entered.set_value();
   const std::lock_guard lock(completion_boundary.mutex);
  }});
  mmltk::testsupport::await_test_promise(boundary.entered, "second prefetch completion entered");
  demanded = std::async(std::launch::async, [&] { return stream.host_images(0U); });
  const auto pixels = mmltk::testsupport::await_test_future(demanded, "demanded host view independent of prefetch completion");
  CHECK(pixels.data() == prepared);
  CHECK(stream.host_storage(0U).capacity_bytes() == capacity);
  CHECK(stream.host_images(0U).data() == prepared);
  for (const auto& read : reads) CHECK(std::memcmp(pixels.data() + read.destination_offset, source.image_pixels(read.index), stride) == 0);
 }
 // A device-side gate holds a real consumer without tying up CUDA callbacks.
 // Cleanup always opens the gate before any GPU owner can join or release it.
 const mmltk::testsupport::ScopedTestCleanup release_gate([&] {
  (void)cuStreamWriteValue32(release_stream, gate, 1U, CU_STREAM_WRITE_VALUE_DEFAULT);
  (void)consumer.Settle();
 });
 gpu::ensure_cuda_driver_ok(cuStreamWaitValue32(reinterpret_cast<CUstream>(consumer_stream), gate, 1U, CU_STREAM_WAIT_VALUE_EQ), "compiled delayed consumer");
 stream.handoff(0U, consumer_stream);
 ensure_cuda_ok(
  cudaMemcpyAsync(stream.metadata_storage(0U).data(), stream.device_storage(0U).data(), reads.size() * stride, cudaMemcpyDeviceToHost, consumer_stream), "compiled delayed consumer pixels");
 stream.release(0U, consumer_stream, {.context = &consumed, .complete = [](void* raw, std::size_t, std::exception_ptr error) noexcept {
  if (!error) static_cast<std::atomic<bool>*>(raw)->store(true, std::memory_order_release);
 }});
 CHECK_THROWS_AS(stream.prepare_host(0U, reads.size() * stride), std::logic_error);
 CHECK_THROWS_AS(stream.submit(0U, source, reads, {}), std::logic_error);
 CHECK_THROWS_AS(stream.prepare_images(2U, stride), std::out_of_range);
 for (auto& transfer : std::span{transfers}.first(3U)) {
  stream.submit(1U, source, reads, {}, {.context = &transfer, .complete = transfer_complete});
  CHECK_FALSE(mmltk::testsupport::await_test_promise(transfer, "independent compiled transfer"));
  stream.synchronize(1U);
  CHECK_FALSE(consumed.load(std::memory_order_acquire));
  CHECK(stream.reset_storage() == cudaErrorNotReady);
  if (h2d) {
   CHECK(stream.device_storage(1U).data() == second_device);
   CHECK(stream.host_storage(1U).data() == second_host);
  }
  const auto pixels = stream.host_images(1U);
  for (const auto& read : reads) CHECK(std::memcmp(pixels.data() + read.destination_offset, source.image_pixels(read.index), stride) == 0);
 }
 const std::array invalid{CompiledImageRead{source.header().num_images, 0U}};
 stream.submit(1U, source, invalid, {}, {.context = &transfers[3], .complete = transfer_complete});
 CHECK(mmltk::testsupport::await_test_promise(transfers[3], "independent compiled read failure"));
 REQUIRE_THROWS_AS(stream.wait_read(1U), std::out_of_range);
 stream.synchronize(1U);
 stream.submit(1U, source, reads, {.before = [](void*, std::size_t) { return false; }}, {.context = &transfers[4], .complete = transfer_complete});
 CHECK_FALSE(mmltk::testsupport::await_test_promise(transfers[4], "independent cancelled compiled read"));
 REQUIRE_FALSE(stream.wait_read(1U));
 stream.synchronize(1U);
 CHECK_FALSE(consumed.load(std::memory_order_acquire));
 gpu::ensure_cuda_driver_ok(cuStreamWriteValue32(release_stream, gate, 1U, CU_STREAM_WRITE_VALUE_DEFAULT), "compiled delayed consumer release");
 stream.synchronize();
 CHECK(consumed.load(std::memory_order_acquire));
 const auto* received = static_cast<const std::byte*>(stream.metadata_storage(0U).data());
 for (const auto& read : reads) CHECK(std::memcmp(received + read.destination_offset, source.image_pixels(read.index), stride) == 0);
}
void exercise_used_ranges(const std::string& path) {
 const auto source = CompiledDataset::open(path);
 const auto stride = static_cast<std::size_t>(source.header().image_stride);
 const auto pitch = source.header().max_label_bytes;
 const auto layout = compute_file_layout(source.header().num_slots, stride, pitch);
 const std::array reads{CompiledImageRead{1, stride}, CompiledImageRead{13, 0}, CompiledImageRead{13, 2 * stride}};
 std::vector<std::byte> pixels(3 * stride, std::byte{0x5a}), labels(3 * pitch, std::byte{0x5a});
 auto advice = source.used_ranges(reads, pitch, true);
 std::size_t advised_pixels = 0, advised_labels = 0;
 bool saw_pixels = false;
 while (const auto range = advice.next()) {
  CHECK(range->bytes <= CompiledDataset::read_chunk_bytes);
  if (range->section == CompiledDataset::ReadSection::Pixels) {
   saw_pixels = true;
   advised_pixels += range->bytes;
   CHECK((range->source_offset == source.pixel_offset(1) || range->source_offset == source.pixel_offset(13)));
  } else {
   CHECK_FALSE(saw_pixels);
   advised_labels += range->bytes;
   CHECK(range->source_offset == layout.label_offset(13));
  }
 }
 CHECK(advised_pixels == 2 * stride);
 CHECK(advised_labels == source.image_entry(13).label_bytes);
 auto cursor = source.used_ranges(reads, pitch);
 std::size_t copied_pixels = 0, copied_labels = 0;
 while (const auto range = cursor.next(stride / 3 + 1)) {
  auto& destination = range->section == CompiledDataset::ReadSection::Pixels ? pixels : labels;
  (range->section == CompiledDataset::ReadSection::Pixels ? copied_pixels : copied_labels) += range->bytes;
  source.gather_range(*range, {destination.data(), destination.size(),
                               [](void* context, std::size_t offset, std::span<const std::byte> bytes) { std::memcpy(static_cast<std::byte*>(context) + offset, bytes.data(), bytes.size()); }});
 }
 CHECK(cursor.empty());
 CHECK(copied_pixels == pixels.size());
 CHECK(copied_labels == 2 * source.image_entry(13).label_bytes);
 for (const auto& read : reads) {
  CHECK(std::memcmp(pixels.data() + read.destination_offset, source.image_pixels(read.index), stride) == 0);
  const auto payload = source.label_payload(read.index);
  const auto destination = std::span{labels}.subspan(read.destination_offset / stride * pitch, pitch);
  CHECK(std::ranges::equal(destination.first(payload.size()), payload));
  CHECK(std::ranges::all_of(destination.subspan(payload.size()), [](std::byte value) { return value == std::byte{0x5a}; }));
 }
 CHECK(CompiledDataset::gather_chunks(3, CompiledDataset::read_chunk_bytes, pitch) == 4);
 CHECK_THROWS_AS(cursor.next(0), std::invalid_argument);
}
void exercise_bounded_gathers(const std::string& path, bool h2d) {
 const auto source = CompiledDataset::open(path);
 const auto stride = static_cast<std::size_t>(source.header().image_stride);
 const auto count = 2 * CompiledDataset::read_chunk_bytes / stride + 3;
 std::vector<CompiledImageRead> reads(count);
 for (std::size_t image = 0; image < count; ++image) reads[image] = {image % 3 == 1 ? 1U : 13U, image * stride};
 struct Observation {
  mmltk::testsupport::TestGate first{"first bounded gather retains its slot"};
  std::promise<void> disjoint;
  std::mutex mutex;
  std::array<unsigned, 2> labels{}, pixels{}, gathers{}, completions{};
  bool ordering_error = false;
 } observation;
 const CompiledImageStream::ReadObserver observer{.context = &observation, .complete = [](void* context, std::size_t slot, std::exception_ptr, bool) noexcept {
  auto& state = *static_cast<Observation*>(context);
  const std::lock_guard lock(state.mutex);
  ++state.completions[slot];
 }, .range = [](void* context, std::size_t slot, CompiledImageStream::ReadOperation operation, const CompiledDataset::UsedRange& range) {
  auto& state = *static_cast<Observation*>(context);
  bool hold = false;
  {
   const std::lock_guard lock(state.mutex);
   if (operation == CompiledImageStream::ReadOperation::Advice) {
    state.ordering_error |= state.gathers[slot] != 0;
    ++(range.section == CompiledDataset::ReadSection::Labels ? state.labels[slot] : state.pixels[slot]);
   } else if (operation == CompiledImageStream::ReadOperation::Gather) {
    state.ordering_error |= !state.labels[slot] || !state.pixels[slot];
    ++state.gathers[slot];
    hold = slot == 0 && state.gathers[slot] == 1;
    if (slot == 0 && state.gathers[slot] == 2) state.disjoint.set_value();
   }
  }
  if (hold) state.first.receipt().ArriveAndWait();
 }};
 CompiledImageStream stream({.slots = 2, .workers = 2, .device = 0, .loading = data_loading_options(h2d)});
 stream.bind_current_context();
 for (std::size_t slot = 0; slot < 2; ++slot) {
  stream.prepare_images(slot, count * stride);
  stream.prepare_reads(slot, count, source.header().max_label_bytes);
 }
 std::future<bool> peer, cancelled;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  stream.cancel_reads();
  observation.first.Release();
  stream.wait_reads();
 });
 stream.submit(0, source, reads, observer);
 REQUIRE(observation.first.WaitEntered(std::chrono::seconds{2}));
 if (h2d) mmltk::testsupport::await_test_promise(observation.disjoint, "disjoint chunk enters while its peer is held");
 stream.submit(1, source, reads, observer);
 peer = std::async(std::launch::async, [&] { return stream.wait_read(1); });
 REQUIRE(mmltk::testsupport::await_test_future(peer, "independent slot gathers while a peer chunk is held"));
 stream.synchronize(1);
 const auto pixels = stream.host_images(1);
 for (const auto& read : reads) {
  CHECK(std::memcmp(pixels.data() + read.destination_offset, source.image_pixels(read.index), stride) == 0);
  CHECK(stream.annotations(1)[read.destination_offset / stride].labels.size() == source.image_labels(read.index).size());
 }
 CHECK_THROWS_AS(stream.prepare_images(0, stride), std::logic_error);
 cancelled = std::async(std::launch::async, [&] { return stream.wait_read(0); });
 stream.cancel_read(0);
 CHECK(cancelled.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
 observation.first.Release();
 CHECK_FALSE(mmltk::testsupport::await_test_future(cancelled, "all cancelled chunk custody settles"));
 stream.synchronize();
 CHECK_FALSE(observation.ordering_error);
 CHECK(observation.completions == std::array<unsigned, 2>{1, 1});
 // The same maximum-sized storage and task records remain usable after drain.
 stream.submit(0, source, reads, {});
 REQUIRE(stream.wait_read(0));
 stream.stop_workers();
 CHECK(std::memcmp(stream.host_images(0).data(), source.image_pixels(13), stride) == 0);
}
void exercise_prefault_custody(const std::string& path) {
 const auto source = CompiledDataset::open(path);
 const auto stride = static_cast<std::size_t>(source.header().image_stride);
 const std::array reads{CompiledImageRead{13, 0}, CompiledImageRead{1, stride}};
 const auto execution = mmltk::frameworks::gpu::resolve_device_execution(0, NumaTopology::Capture());
 auto readers = std::make_shared<WorkerPool>(2, execution.placement.cpus, "prefault-read", 1, &execution.placement);
 struct Observation {
  mmltk::testsupport::TestGate physical{"admitted prefault retains source and slot"};
  mmltk::testsupport::TestGate rejection{"reader prepares before service rejection"};
  std::promise<void> second_advice;
  std::atomic<bool> observed_second{false}, held{false};
  std::array<std::atomic<unsigned>, 2> completed{};
 } observation;
 CompiledImageStream stream({.slots = 2, .workers = 1, .device = 0, .prefault = true, .execution = execution, .reader_pool = readers});
 stream.bind_current_context();
 for (std::size_t slot = 0; slot < 2; ++slot) {
  stream.prepare_images(slot, 2 * stride);
  stream.prepare_reads(slot, 2, source.header().max_label_bytes);
 }
 std::future<bool> first, second;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  observation.physical.Release();
  observation.rejection.Release();
  stream.cancel_reads();
  stream.wait_reads();
 });
 const CompiledImageStream::ReadObserver observer{.context = &observation, .complete = [](void* context, std::size_t slot, std::exception_ptr, bool) noexcept {
  ++static_cast<Observation*>(context)->completed[slot];
 }, .range = [](void* context, std::size_t slot, CompiledImageStream::ReadOperation operation, const CompiledDataset::UsedRange&) {
  auto& state = *static_cast<Observation*>(context);
  if (slot == 0 && operation == CompiledImageStream::ReadOperation::Prefault && !state.held.exchange(true)) state.physical.receipt().ArriveAndWait();
  if (slot == 1 && operation == CompiledImageStream::ReadOperation::Advice && !state.observed_second.exchange(true)) state.second_advice.set_value();
 }};
 stream.submit(0, source, reads, observer);
 REQUIRE(observation.physical.WaitEntered(std::chrono::seconds{2}));
 stream.submit(1, source, reads, observer);
 mmltk::testsupport::await_test_promise(observation.second_advice, "second slot prepares while file record is occupied");
 // With the first worker physically held, this FIFO barrier proves the other
 // worker returned from the second slot's full-admission attempt.
 auto prepared = readers->enqueue([] {});
 mmltk::testsupport::await_test_future(prepared, "full prefault request remains parked");
 first = std::async(std::launch::async, [&] { return stream.wait_read(0); });
 second = std::async(std::launch::async, [&] { return stream.wait_read(1); });
 for (const bool all : {false, true}) {
  if (all)
   stream.cancel_reads();
  else
   stream.cancel_read(1);
  CHECK_FALSE(mmltk::testsupport::await_test_future(second, "unadmitted cancellation settles before the held peer"));
  CHECK(first.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
  CHECK(observation.completed[0] == 0);
  CHECK(observation.completed[1] == (all ? 2U : 1U));
  stream.submit(1, source, reads, observer);
  prepared = readers->enqueue([] {});
  mmltk::testsupport::await_test_future(prepared, "reused slot parks behind the same physical prefault");
  second = std::async(std::launch::async, [&] { return stream.wait_read(1); });
 }
 stream.cancel_read(0);
 CHECK(first.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
 CHECK(second.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
 CHECK_THROWS(stream.prepare_images(0, stride));
 observation.physical.Release();
 CHECK_FALSE(mmltk::testsupport::await_test_future(first, "cancelled prefault physically returned"));
 REQUIRE(mmltk::testsupport::await_test_future(second, "full prefault resumes after capacity return"));
 stream.synchronize();
 CHECK(observation.completed[0] == 1);
 CHECK(observation.completed[1] == 3);
 CHECK(std::memcmp(stream.host_images(1).data(), source.image_pixels(13), stride) == 0);
 stream.submit(0, source, reads, {.context = &observation, .before = [](void* context, std::size_t) {
  static_cast<Observation*>(context)->rejection.receipt().ArriveAndWait();
  return true;
 }, .complete = observer.complete});
 REQUIRE(observation.rejection.WaitEntered(std::chrono::seconds{2}));
 readers->enqueue_detached([] { throw std::runtime_error("runtime reader admission failed"); });
 CHECK_THROWS_WITH(readers->wait_idle(), "runtime reader admission failed");
 observation.rejection.Release();
 first = std::async(std::launch::async, [&] { return stream.wait_read(0); });
 CHECK_THROWS_WITH(mmltk::testsupport::await_test_future(first, "rejected prefault settles its slot directly"), "runtime reader admission failed");
 stream.wait_reads();
 CHECK(observation.completed[0] == 2);
 stream.stop_workers();
}
void exercise_roundtrip_transport(const FixtureSpec& fixture, const bool h2d, cudaStream_t compute_stream) {
 const std::string bin_path = compiled_bin_path(fixture);
 const std::string dataset_dir_path = dataset_dir(fixture);
 const std::string& split = fixture.split;
 const int W = fixture.width;
 const int H = fixture.height;
 const int NUM_IMAGES = fixture.num_images;
 const size_t IMAGE_STRIDE = static_cast<size_t>(3) * H * W * sizeof(float);
 const size_t STRIDE_FLOATS = IMAGE_STRIDE / sizeof(float);
 INFO("h2d_dataloader=" << h2d);
 exercise_schedule_capacity(bin_path, h2d);
 exercise_sparse_capacity(bin_path, h2d);
 exercise_demanded_host_shutdown(bin_path, h2d);
 exercise_compiled_stream(bin_path, h2d);
 ensure_cuda_ok(cudaSetDevice(0), "restore primary training context after isolated stream coverage");
 exercise_independent_completions(bin_path, h2d);
 ensure_cuda_ok(cudaSetDevice(0), "restore primary training context after independent stream coverage");
 exercise_bounded_gathers(bin_path, h2d);
 {
  DatasetLoader::Config direct_cfg;
  direct_cfg.loading.h2d_dataloader = h2d;
  direct_cfg.compiled_path = bin_path;
  direct_cfg.batch_size = 1;
  direct_cfg.shuffle = false;
  DatasetLoader direct_loader(direct_cfg);
  REQUIRE(direct_loader.num_images() == static_cast<size_t>(NUM_IMAGES));
  REQUIRE(direct_loader.num_classes() == 6u);
 }
 DatasetLoader::Config direct_cfg;
 direct_cfg.loading.h2d_dataloader = h2d;
 direct_cfg.compiled_path = bin_path;
 direct_cfg.batch_size = 8;
 direct_cfg.shuffle = false;
 direct_cfg.prefetch_factor = 1;
 DatasetLoader direct_loader(direct_cfg);
 REQUIRE(direct_loader.num_images() == static_cast<size_t>(NUM_IMAGES));
 REQUIRE(direct_loader.image_width() == static_cast<uint32_t>(W));
 REQUIRE(direct_loader.image_height() == static_cast<uint32_t>(H));
 REQUIRE(direct_loader.num_classes() == 6u);
 REQUIRE(direct_loader.image_stride() == IMAGE_STRIDE);
 const fs::path first_image_path = fs::path(dataset_dir_path) / split / "000001.png";
 const std::vector<float> expected_first_image = expected_nchw_stub(first_image_path.string(), W, H);
 assert_image_matches(direct_loader.image_pixels(0), expected_first_image);
 for (size_t i = 0; i < 100; ++i) {
  const float pixel = direct_loader.image_pixels(0)[i];
  REQUIRE((pixel >= 0.0f && pixel <= 1.0f));
 }
 printf("Pixel values in [0,1] — OK\n");
 direct_loader.begin_epoch();
 Batch batch;
 size_t total = 0;
 size_t total_instances = 0;
 size_t next_expected_idx = 0;
 while (direct_loader.next_batch(batch)) {
  REQUIRE(batch.image_indices[0] == next_expected_idx);
  const bool contiguous = batch.num_images == 1 || (IMAGE_STRIDE % PAGE_SIZE == 0 && batch.image_indices[0] / COMPILED_BLOCK_SLOTS == batch.image_indices[batch.num_images - 1] / COMPILED_BLOCK_SLOTS);
  REQUIRE((direct_loader.host_images(batch).data() == direct_loader.image_pixels(batch.image_indices[0])) == contiguous);
  total += batch.num_images;
  for (size_t i = 0; i < batch.num_images; ++i) {
   const uint32_t idx = batch.image_indices[i];
   const auto entry = batch.annotations(i);
   total_instances += entry.labels.size();
   REQUIRE(idx == batch.image_indices[0] + i);
   for (int p = 0; p < 10; ++p) {
    const float pixel = direct_loader.host_images(batch).data()[i * STRIDE_FLOATS + static_cast<size_t>(p)];
    REQUIRE((pixel >= 0.0f && pixel <= 1.0f));
   }
   REQUIRE(entry.labels.size() == (idx >= 10 ? 1 : 0));
   if (entry.labels.size() > 0) {
    const PackedInstance& inst = entry.labels[0];
    REQUIRE(inst.class_id == static_cast<uint8_t>((idx - 10) % direct_loader.num_classes()));
    REQUIRE(inst.mask_rle_pairs == static_cast<uint16_t>(std::min(H - 1, 30) - 10));
    const RLEPair& first_pair = entry.instance_rle(inst).front();
    REQUIRE(first_pair.start == static_cast<uint32_t>(10 * W + 10));
    REQUIRE(first_pair.length == static_cast<uint32_t>(std::min(W - 1, 30) - 10));
   }
  }
  direct_loader.handoff_batch(batch, compute_stream);
  ensure_cuda_ok(cudaMemsetAsync(const_cast<float*>(batch.device_images), 0, batch.num_images * IMAGE_STRIDE, compute_stream), "cudaMemsetAsync");
  direct_loader.release_batch(batch, compute_stream);
  next_expected_idx += batch.num_images;
 }
 ensure_cuda_ok(cudaStreamSynchronize(compute_stream), "cudaStreamSynchronize");
 direct_loader.synchronize();
 REQUIRE(total == static_cast<size_t>(NUM_IMAGES));
 REQUIRE(total_instances == static_cast<size_t>(std::max(NUM_IMAGES - 10, 0)));
 printf("Sequential iteration: %zu images, %zu instances\n", total, total_instances);
 direct_loader.begin_epoch();
 total = 0;
 while (direct_loader.next_batch(batch)) {
  direct_loader.wait_batch(batch);
  direct_loader.release_batch(batch);
  total += batch.num_images;
 }
 direct_loader.synchronize();
 REQUIRE(total == static_cast<size_t>(NUM_IMAGES));
 direct_loader.begin_epoch();
 if (direct_loader.next_batch(batch)) {
  direct_loader.wait_batch(batch);
  direct_loader.handoff_batch(batch, nullptr);
  ensure_cuda_ok(cudaMemsetAsync(const_cast<float*>(batch.device_images), 0, batch.num_images * IMAGE_STRIDE, nullptr), "cudaMemsetAsync");
  direct_loader.release_batch(batch, nullptr);
 }
 while (direct_loader.next_batch(batch)) { direct_loader.release_batch(batch); }
 ensure_cuda_ok(cudaStreamSynchronize(nullptr), "cudaStreamSynchronize");
 direct_loader.synchronize();
 DatasetLoader::Config shuffled_cfg = direct_cfg;
 shuffled_cfg.shuffle = true;
 shuffled_cfg.seed = 7;
 shuffled_cfg.prefetch_factor = 5;
 DatasetLoader shuffled_loader(shuffled_cfg);
 shuffled_loader.begin_epoch();
 std::vector<uint32_t> seen_indices;
 total = 0;
 while (shuffled_loader.next_batch(batch)) {
  CHECK_THROWS_AS(direct_loader.host_images(batch), std::runtime_error);
  total += batch.num_images;
  seen_indices.insert(seen_indices.end(), batch.image_indices, batch.image_indices + batch.num_images);
  const bool direct_shuffle_span = shuffled_loader.host_images(batch).data() == shuffled_loader.image_pixels(batch.image_indices[0]);
  if (direct_shuffle_span) {
   for (size_t i = 0; i < batch.num_images; ++i) { REQUIRE(batch.image_indices[i] == batch.image_indices[0] + i); }
  }
  // CLEANUP-IGNORE: Shuffled-batch transfer has its own host-view invalidation oracle; retain the explicit handoff and release boundary.
  shuffled_loader.handoff_batch(batch, compute_stream);
  ensure_cuda_ok(cudaMemsetAsync(const_cast<float*>(batch.device_images), 0, batch.num_images * IMAGE_STRIDE, compute_stream), "cudaMemsetAsync");
  shuffled_loader.release_batch(batch, compute_stream);
  CHECK_THROWS_AS(shuffled_loader.host_images(batch), std::runtime_error);
 }
 ensure_cuda_ok(cudaStreamSynchronize(compute_stream), "cudaStreamSynchronize");
 shuffled_loader.synchronize();
 REQUIRE(total == static_cast<size_t>(NUM_IMAGES));
 std::sort(seen_indices.begin(), seen_indices.end());
 for (uint32_t i = 0; i < static_cast<uint32_t>(seen_indices.size()); ++i) { REQUIRE(seen_indices[i] == i); }
 printf("Shuffled iteration: %zu images with prefetch_factor=%d\n", total, shuffled_cfg.prefetch_factor);
 shuffled_loader.begin_epoch();
 seen_indices.clear();
 while (shuffled_loader.next_batch(batch)) {
  seen_indices.insert(seen_indices.end(), batch.image_indices, batch.image_indices + batch.num_images);
  shuffled_loader.release_batch(batch);
 }
 shuffled_loader.synchronize();
 std::sort(seen_indices.begin(), seen_indices.end());
 for (uint32_t i = 0; i < static_cast<uint32_t>(seen_indices.size()); ++i) { REQUIRE(seen_indices[i] == i); }
 const std::vector<int> cpu_set = allowed_cpu_set();
 DatasetLoader::Config budgeted_cfg = shuffled_cfg;
 budgeted_cfg.prefetch_factor = 2;
 budgeted_cfg.gather_workers = 1;
 budgeted_cfg.cpu_affinity = std::to_string(cpu_set.front());
 DatasetLoader budgeted_loader(budgeted_cfg);
 std::vector<const float*> persistent_device_slots(static_cast<std::size_t>(budgeted_cfg.prefetch_factor));
 std::vector<std::uint64_t> previous_leases(persistent_device_slots.size());
 for (int epoch = 0; epoch < 3; ++epoch) {
  budgeted_loader.begin_epoch();
  total = 0;
  while (budgeted_loader.next_batch(batch)) {
   auto& device = persistent_device_slots[batch.slot_index];
   if (device == nullptr) device = batch.device_images;
   CHECK(batch.device_images == device);
   CHECK(batch.lease_id > previous_leases[batch.slot_index]);
   previous_leases[batch.slot_index] = batch.lease_id;
   total += batch.num_images;
   if ((batch.lease_id & 1U) != 0U) {
    budgeted_loader.handoff_batch(batch, compute_stream);
    budgeted_loader.release_batch(batch, compute_stream);
   } else {
    budgeted_loader.release_batch(batch);
   }
  }
  budgeted_loader.synchronize();
  REQUIRE(total == static_cast<size_t>(NUM_IMAGES));
 }
 // Useful chunks can outnumber slots; the reader CPU ceiling is independent
 // of prefetch depth. Small batches still clamp to their useful work.
 auto chunk_budget_cfg = shuffled_cfg;
 chunk_budget_cfg.prefetch_factor = 1;
 chunk_budget_cfg.gather_workers = 3;
 DatasetLoader chunk_budget_loader(chunk_budget_cfg);
 while (chunk_budget_loader.next_batch(batch)) chunk_budget_loader.release_batch(batch);
 chunk_budget_loader.synchronize();
 DatasetLoader::Config invalid_affinity_cfg = direct_cfg;
 invalid_affinity_cfg.cpu_affinity = "999999";
 bool invalid_affinity_threw = false;
 try {
  DatasetLoader invalid_affinity_loader(invalid_affinity_cfg);
 } catch (const std::runtime_error&) { invalid_affinity_threw = true; }
 REQUIRE(invalid_affinity_threw);
 DatasetLoader::Config shard0_cfg = direct_cfg;
 shard0_cfg.batch_size = 4;
 shard0_cfg.batch_shard_rank = 0;
 shard0_cfg.batch_shard_count = 2;
 DatasetLoader::Config shard1_cfg = shard0_cfg;
 shard1_cfg.batch_shard_rank = 1;
 DatasetLoader shard0_loader(shard0_cfg);
 DatasetLoader shard1_loader(shard1_cfg);
 shard0_loader.begin_epoch();
 shard1_loader.begin_epoch();
 std::vector<uint32_t> sharded_seen;
 for (auto* loader : std::array{&shard0_loader, &shard1_loader}) {
  while (loader->next_batch(batch)) {
   sharded_seen.insert(sharded_seen.end(), batch.image_indices, batch.image_indices + batch.num_images);
   loader->release_batch(batch);
  }
 }
 shard0_loader.synchronize();
 shard1_loader.synchronize();
 std::sort(sharded_seen.begin(), sharded_seen.end());
 REQUIRE(sharded_seen.size() == static_cast<size_t>(NUM_IMAGES));
 for (uint32_t i = 0; i < static_cast<uint32_t>(sharded_seen.size()); ++i) { REQUIRE(sharded_seen[i] == i); }
 printf("Batch sharding: %zu images across %u shards\n", sharded_seen.size(), shard0_cfg.batch_shard_count);
 {
  auto failed_config = direct_cfg;
  failed_config.batch_size = static_cast<std::size_t>(NUM_IMAGES) + 1U;
  failed_config.prefetch_factor = 8;
  DatasetLoader failed(failed_config, {}, +[](cudaEvent_t, cudaStream_t) { return cudaErrorUnknown; });
  Batch retained{};
  REQUIRE(failed.next_batch(retained));
  const mmltk::testsupport::ScopedTestCleanup release_retained([&] {
   failed.stop_workers();
   failed.release_batch(retained, compute_stream);
  });
  failed.wait_batch(retained);
  failed.handoff_batch(retained, compute_stream);
  REQUIRE_THROWS(failed.release_batch(retained, compute_stream));
  CHECK(retained.image_capacity_bytes == static_cast<std::size_t>(NUM_IMAGES) * IMAGE_STRIDE);
  CHECK_FALSE(retained.image_custody.expired());
  CHECK(failed.host_images(retained).size() == static_cast<std::size_t>(NUM_IMAGES) * STRIDE_FLOATS);
  // A refused event record retains the lease; stopped release settles the
  // exact consumer stream before returning that same slot's custody.
 }
 DatasetLoader::Config partial_cfg = direct_cfg;
 partial_cfg.batch_size = 7U;
 DatasetLoader partial_loader(partial_cfg);
 std::vector<std::size_t> batch_sizes;
 while (partial_loader.next_batch(batch)) {
  batch_sizes.push_back(batch.num_images);
  REQUIRE_THROWS(partial_loader.begin_epoch());
  partial_loader.release_batch(batch);
  REQUIRE_THROWS(partial_loader.release_batch(batch));
 }
 partial_loader.synchronize();
 REQUIRE(batch_sizes == std::vector<std::size_t>{7U, 7U, 6U});
 partial_cfg.drop_last = true;
 DatasetLoader dropped_loader(partial_cfg);
 REQUIRE(dropped_loader.num_batches() == 2U);
 total = 0U;
 while (dropped_loader.next_batch(batch)) {
  total += batch.num_images;
  dropped_loader.release_batch(batch);
 }
 REQUIRE(total == 14U);
 dropped_loader.synchronize();
 // Explicit schedules own repeats, order and logical keys even when legacy
 // loader configuration requests a shuffle/shard. Borrowed views retain them.
 auto planned_cfg = shuffled_cfg;
 planned_cfg.batch_size = 4;
 planned_cfg.batch_shard_count = 2;
 planned_cfg.batch_shard_rank = 1;
 DatasetLoader planned_loader(planned_cfg);
 auto schedule = std::make_shared<DatasetIndexSchedule>();
 schedule->image_indices = {3, 3, 1, 0, 2, 4, 2, 6, 3, 3, 3, 3};
 schedule->draw_keys = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
 schedule->microbatch_keys = {101, 102, 103};
 std::weak_ptr<const DatasetIndexSchedule> lifetime = schedule;
 planned_loader.begin_epoch(schedule, 4);
 CHECK(planned_loader.num_batches() == 2);
 schedule.reset();
 REQUIRE(planned_loader.next_batch(batch));
 CHECK_FALSE(lifetime.expired());
 CHECK(batch.num_images == 4);
 CHECK(std::vector<std::uint32_t>(batch.image_indices, batch.image_indices + batch.num_images) == std::vector<std::uint32_t>{2, 4, 2, 6});
 CHECK(std::vector<std::uint64_t>(batch.draw_keys.begin(), batch.draw_keys.end()) == std::vector<std::uint64_t>{15, 16, 17, 18});
 CHECK(batch.microbatch_key == 102);
 planned_loader.wait_batch(batch);
 const auto planned_pixels = planned_loader.host_images(batch);
 std::vector<float> delivered_pixels(planned_pixels.size());
 ensure_cuda_ok(cudaMemcpy(delivered_pixels.data(), batch.device_images, batch.image_capacity_bytes, cudaMemcpyDeviceToHost), "explicit schedule destination order");
 for (std::size_t image = 0U; image < batch.num_images; ++image) {
  const auto* expected = planned_loader.compiled_source()->image_pixels(batch.image_indices[image]);
  CHECK(std::memcmp(planned_pixels.data() + image * STRIDE_FLOATS, expected, IMAGE_STRIDE) == 0);
  CHECK(std::memcmp(delivered_pixels.data() + image * STRIDE_FLOATS, expected, IMAGE_STRIDE) == 0);
 }
 REQUIRE_THROWS(planned_loader.begin_epoch());
 planned_loader.release_batch(batch);
 REQUIRE(planned_loader.next_batch(batch));
 CHECK(batch.microbatch_key == 103);
 planned_loader.release_batch(batch);
 CHECK_FALSE(planned_loader.next_batch(batch));
 planned_loader.synchronize();
 // A global training microbatch may have no images on this rank. Its loader
 // receives an empty physical schedule; logical keys remain with the session.
 planned_loader.begin_epoch(std::make_shared<DatasetIndexSchedule>());
 CHECK(planned_loader.num_batches() == 0);
 CHECK_FALSE(planned_loader.next_batch(batch));
 planned_loader.synchronize();
 auto invalid = std::make_shared<DatasetIndexSchedule>();
 invalid->image_indices = {static_cast<std::uint32_t>(NUM_IMAGES)};
 REQUIRE_THROWS(planned_loader.begin_epoch(invalid));
 invalid->image_indices = {0, 1, 2, 3};
 invalid->draw_keys = {1};
 REQUIRE_THROWS(planned_loader.begin_epoch(invalid));
 invalid->draw_keys.clear();
 REQUIRE_THROWS(planned_loader.begin_epoch(invalid, 1));
 REQUIRE_THROWS(planned_loader.begin_epoch(invalid, 8));
 planned_loader.begin_epoch();
 CHECK(lifetime.expired());
 while (planned_loader.next_batch(batch)) planned_loader.release_batch(batch);
 planned_loader.synchronize();
 // Independent cursors and cancellation share one immutable mapping/index.
 auto shared_config = planned_cfg;
 shared_config.source = planned_loader.compiled_source();
 const auto shared_execution = mmltk::frameworks::gpu::resolve_device_execution(shared_config.device_id, NumaTopology::Capture());
 shared_config.execution = shared_execution;
 shared_config.reader_pool = std::make_shared<WorkerPool>(1U, shared_execution.placement.cpus, "shared-data", 4U, &shared_execution.placement, true);
 auto shared_owner = std::make_unique<DatasetLoader>(shared_config);
 DatasetLoader shared_peer(shared_config);
 CHECK(shared_owner->compiled_source() == shared_peer.compiled_source());
 CHECK(shared_owner->compiled_source().get() == shared_peer.compiled_source().get());
 auto first_schedule = std::make_shared<DatasetIndexSchedule>();
 first_schedule->image_indices = {0, 1, 2, 3};
 auto other_schedule = std::make_shared<DatasetIndexSchedule>();
 other_schedule->image_indices = {4, 5, 6, 7};
 shared_owner->begin_epoch(first_schedule);
 shared_peer.begin_epoch(other_schedule);
 shared_owner->stop_workers();
 shared_owner.reset();
 CHECK(shared_config.reader_pool->enqueue([] { return 42; }).get() == 42);
 shared_config.source.reset();
 REQUIRE(shared_peer.next_batch(batch));
 CHECK(batch.image_indices[0] == 4);
 CHECK(batch.image_indices[3] == 7);
 shared_peer.release_batch(batch);
 CHECK_FALSE(shared_peer.next_batch(batch));
 shared_peer.synchronize();
 auto mismatched = planned_cfg;
 mismatched.source = shared_peer.compiled_source();
 mismatched.compiled_path += ".missing";
 REQUIRE_THROWS(DatasetLoader(mismatched));
 DatasetLoader same_seed_a(shuffled_cfg);
 DatasetLoader same_seed_b(shuffled_cfg);
 for (int epoch = 0; epoch < 2; ++epoch) {
  same_seed_a.begin_epoch();
  same_seed_b.begin_epoch();
  Batch other{};
  while (same_seed_a.next_batch(batch)) {
   REQUIRE(same_seed_b.next_batch(other));
   REQUIRE(batch.num_images == other.num_images);
   REQUIRE(std::equal(batch.image_indices, batch.image_indices + batch.num_images, other.image_indices));
   same_seed_a.release_batch(batch);
   same_seed_b.release_batch(other);
  }
  REQUIRE_FALSE(same_seed_b.next_batch(other));
 }
 // Replacing an unconsumed epoch cancels bounded reads and starts from the
 // next deterministic schedule after all physical transfers settle.
 same_seed_a.begin_epoch();
 same_seed_a.begin_epoch();
 while (same_seed_a.next_batch(batch)) same_seed_a.release_batch(batch);
 same_seed_a.synchronize();
 DatasetLoader stopped(direct_cfg);
 stopped.begin_epoch();
 Batch retained{};
 REQUIRE(stopped.next_batch(retained));
 stopped.wait_batch(retained);
 stopped.stop_workers();
 stopped.stop_workers();
 REQUIRE(retained.device_images != nullptr);
 float first_channel = -1.0F;
 REQUIRE(cudaMemcpy(&first_channel, retained.device_images, sizeof(float), cudaMemcpyDeviceToHost) == cudaSuccess);
 REQUIRE(std::isfinite(first_channel));
 REQUIRE_FALSE(stopped.next_batch(batch));
 REQUIRE_THROWS(stopped.begin_epoch());
 stopped.release_batch(retained, compute_stream);
 DatasetLoader cancellable(direct_cfg);
 cancellable.begin_epoch();
 Batch occupied{};
 REQUIRE(cancellable.next_batch(occupied));
 cancellable.wait_batch(occupied);
 // One prefetch slot remains checked out: readiness cannot complete this wait.
 std::stop_source cancellation;
 std::promise<void> entered;
 auto entered_future = entered.get_future();
 auto waiting = std::async(std::launch::async, [&] {
  entered.set_value();
  Batch unused{};
  return cancellable.next_batch(unused, cancellation.get_token());
 });
 mmltk::testsupport::await_test_future(entered_future, "compiled acquisition entered");
 CHECK(cancellation.request_stop());
 CHECK_FALSE(cancellation.request_stop());
 CHECK_FALSE(mmltk::testsupport::await_test_future(waiting, "cancelled batch acquisition"));
 // Stop neither releases nor joins on the requester; the owner still owns this lease.
 cancellable.release_batch(occupied, compute_stream);
 Batch after_stop{};
 CHECK_FALSE(cancellable.next_batch(after_stop, cancellation.get_token()));
 // Cancellation is scoped to one acquisition, not the ordinary training loader.
 REQUIRE(cancellable.next_batch(after_stop));
 cancellable.wait_batch(after_stop);
 cancellable.release_batch(after_stop, compute_stream);
 cancellable.begin_epoch();
 std::stop_source checkout_stop;
 REQUIRE(cancellable.next_batch(after_stop, checkout_stop.get_token()));
 CHECK(checkout_stop.request_stop());
 cancellable.wait_batch(after_stop);
 cancellable.release_batch(after_stop, compute_stream);
 CHECK_FALSE(cancellable.next_batch(after_stop, checkout_stop.get_token()));
 cancellable.begin_epoch();
 std::stop_source readiness_stop;
 auto stopper = std::async(std::launch::async, [&] { return readiness_stop.request_stop(); });
 const bool checked_out = cancellable.next_batch(after_stop, readiness_stop.get_token());
 CHECK(mmltk::testsupport::await_test_future(stopper, "stop racing batch readiness"));
 if (checked_out) {
  cancellable.wait_batch(after_stop);
  cancellable.release_batch(after_stop, compute_stream);
 }
 CHECK_FALSE(cancellable.next_batch(after_stop, readiness_stop.get_token()));
 DatasetLoader never_started(direct_cfg);
 never_started.stop_workers();
 REQUIRE_FALSE(never_started.next_batch(batch));
 REQUIRE_THROWS(never_started.begin_epoch());
}
void exercise_used_read_measurements(const std::string& path) {
 using Clock = std::chrono::steady_clock;
 const auto source = CompiledDataset::open(path);
 const auto count = source.header().num_images;
 REQUIRE(count > 0);
 REQUIRE(count <= 256);
 REQUIRE(source.slot_count() == count);
 const auto stride = static_cast<std::size_t>(source.header().image_stride);
 const auto packed_path = fs::path(path).parent_path() / "used-packed-fixture.bin";
 std::vector<std::size_t> label_offsets(count);
 std::size_t packed_bytes = static_cast<std::size_t>(count) * stride;
 {
  std::ofstream packed(packed_path, std::ios::binary | std::ios::trunc);
  REQUIRE(packed.is_open());
  for (std::uint32_t i = 0; i < count; ++i) packed.write(reinterpret_cast<const char*>(source.image_pixels(i)), static_cast<std::streamsize>(stride));
  for (std::uint32_t i = 0; i < count; ++i) {
   label_offsets[i] = packed_bytes;
   const auto used = source.label_payload(i);
   packed.write(reinterpret_cast<const char*>(used.data()), static_cast<std::streamsize>(used.size()));
   packed_bytes += used.size();
  }
  packed.close();
  REQUIRE(packed.good());
 }
 auto packed = mmltk::common::io::MappedFile::try_open_readonly(packed_path.string());
 REQUIRE(packed.has_value());
 REQUIRE(packed->size() == packed_bytes);
 std::vector<std::byte> pixels(static_cast<std::size_t>(count) * stride);
 std::vector<std::byte> labels(static_cast<std::size_t>(count) * source.header().max_label_bytes);
 std::vector<CompiledImageRead> reads(count);
 struct Gather {
  std::span<std::byte> pixels;
  std::uint64_t pixel_bytes = 0, label_bytes = 0, pixel_copies = 0;
  void copy(std::size_t offset, std::span<const std::byte> input) {
   std::memcpy(pixels.data() + offset, input.data(), input.size());
   pixel_bytes += input.size();
   ++pixel_copies;
  }
 };
 struct Measurement {
  std::uint64_t ns = 0, pixels = 0, labels = 0, copies = 0;
 };
 const std::atomic<bool> cancelled{false};
 constexpr std::size_t extent_limit = 16U * 1024U * 1024U;
 const auto packed_pixels = [&](Gather& output) {
  // Independent retained pixel algorithm from 3b1373b9: coalesce consecutive
  // source advice and source+destination copies, bounded to 16 MiB. Only the
  // fixture stores packed bytes; no historical dataset format is interpreted.
  for (const auto& read : reads) {
   if (read.index >= count || read.destination_offset > output.pixels.size() || stride > output.pixels.size() - read.destination_offset)
    throw std::out_of_range("packed fixture read exceeds prepared extents");
  }
  for (std::size_t first = 0; first < reads.size();) {
   auto end = first + 1;
   auto last = reads[first].index;
   while (end < reads.size() && reads[end].index >= last && std::uint64_t{reads[end].index} <= std::uint64_t{last} + 1U &&
          static_cast<std::size_t>(reads[end].index - reads[first].index) < std::max<std::size_t>(1, extent_limit / stride))
    last = reads[end++].index;
   const auto offset = static_cast<std::size_t>(reads[first].index) * stride;
   const auto bytes = (static_cast<std::size_t>(last - reads[first].index) + 1U) * stride;
   for (std::size_t done = 0; done < bytes;) {
    if (cancelled.load(std::memory_order_acquire)) throw std::runtime_error("packed fixture cancelled");
    const auto extent = std::min(extent_limit, bytes - done);
    packed->advise_aligned_range(offset + done, extent, MADV_WILLNEED);
    done += extent;
   }
   first = end;
  }
  for (std::size_t first = 0; first < reads.size();) {
   std::size_t consecutive = 1;
   while (first + consecutive < reads.size() && reads[first + consecutive].index == reads[first].index + consecutive &&
          reads[first + consecutive].destination_offset == reads[first].destination_offset + consecutive * stride && consecutive < std::max<std::size_t>(1, extent_limit / stride))
    ++consecutive;
   const auto offset = static_cast<std::size_t>(reads[first].index) * stride;
   const auto bytes = consecutive * stride;
   for (std::size_t done = 0; done < bytes;) {
    if (cancelled.load(std::memory_order_acquire)) throw std::runtime_error("packed fixture cancelled");
    const auto extent = std::min(extent_limit, bytes - done);
    output.copy(reads[first].destination_offset + done, {reinterpret_cast<const std::byte*>(packed->data() + offset + done), extent});
    done += extent;
   }
   first += consecutive;
  }
 };
 const auto gather = [&](bool fixed) {
  Gather output{pixels};
  for (std::size_t i = 0; i < reads.size(); ++i) {
   const auto index = reads[i].index;
   const auto bytes = source.image_entry(index).label_bytes;
   const auto* input = fixed ? source.label_payload(index).data() : reinterpret_cast<const std::byte*>(packed->data() + label_offsets[index]);
   std::memcpy(labels.data() + i * source.header().max_label_bytes, input, bytes);
   output.label_bytes += bytes;
  }
  if (fixed) {
   const bool complete = source.read_images_to(
    reads, {&output, pixels.size(), [](void* context, std::size_t offset, std::span<const std::byte> bytes) { static_cast<Gather*>(context)->copy(offset, bytes); }}, cancelled, false);
   if (!complete) throw std::runtime_error("fixed fixture cancelled");
  } else
   packed_pixels(output);
  return output;
 };
 constexpr unsigned repetitions = 16;
 for (const bool shuffled : {false, true}) {
  for (std::uint32_t i = 0; i < count; ++i) reads[i] = {i, static_cast<std::size_t>(i) * stride};
  if (shuffled) {
   std::mt19937 random(1729);
   std::shuffle(reads.begin(), reads.end(), random);
   for (std::size_t i = 0; i < reads.size(); ++i) reads[i].destination_offset = i * stride;
  }
  const auto verify = [&] {
   for (std::size_t i = 0; i < reads.size(); ++i) {
    CHECK(std::memcmp(pixels.data() + reads[i].destination_offset, source.image_pixels(reads[i].index), stride) == 0);
    const auto used = source.label_payload(reads[i].index);
    CHECK(std::memcmp(labels.data() + i * source.header().max_label_bytes, used.data(), used.size()) == 0);
   }
  };
  for (const bool fixed : {false, true}) {
   (void)gather(fixed);
   verify();
  }
  std::array<Measurement, 2> measured{};
  for (unsigned repetition = 0; repetition < repetitions; ++repetition) {
   for (unsigned pass = 0; pass < 2; ++pass) {
    const auto variant = (repetition + pass) % 2U;
    const auto started = Clock::now();
    const auto result = gather(variant == 0);
    auto& total = measured[variant];
    total.ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    total.pixels += result.pixel_bytes;
    total.labels += result.label_bytes;
    total.copies += result.pixel_copies;
   }
  }
  CHECK(measured[0].pixels == measured[1].pixels);
  CHECK(measured[0].labels == measured[1].labels);
  CHECK(measured[0].pixels == static_cast<std::uint64_t>(repetitions) * count * stride);
  verify();
  for (std::size_t variant = 0; variant < measured.size(); ++variant) {
   const auto& value = measured[variant];
   const auto throughput = value.ns ? static_cast<double>(value.pixels + value.labels) * 1.0e9 / static_cast<double>(value.ns) : 0.0;
   std::printf(
    "compiled_used_read_fixture layout=%s order=%s repetitions=%u pixel_bytes=%llu label_bytes=%llu elapsed_ns=%llu used_bytes_per_second=%.0f pixel_copy_calls=%llu prepared_arena_bytes=%zu "
    "resident_metadata_bytes=%zu mapped_apparent_bytes=%zu cache=warm fixture_only=1 production_baseline=unavailable\n",
    variant == 0 ? "fixed10" : "packed_algorithm_3b1373b9", shuffled ? "shuffled_seed_1729" : "sequential", repetitions, static_cast<unsigned long long>(value.pixels),
    static_cast<unsigned long long>(value.labels), static_cast<unsigned long long>(value.ns), throughput, static_cast<unsigned long long>(value.copies),
    pixels.capacity() + labels.capacity() + reads.capacity() * sizeof(CompiledImageRead), source.image_entries().size_bytes(),
    variant == 0 ? static_cast<std::size_t>(source.header().total_file_size) : packed->size());
  }
 }
}
void test_roundtrip_end_to_end() {
 profile_set_run_label("test_roundtrip");
 const mmltk::testsupport::ScopedTempDir root("mmltk_roundtrip");
 const FixtureSpec fixture{root.path().string(), "train", 65, 65, 20};
 const int W = fixture.width;
 const int H = fixture.height;
 const int NUM_IMAGES = fixture.num_images;
 ensure_cuda_ok(cudaSetDevice(0), "cudaSetDevice");
 cudaDeviceProp device{};
 ensure_cuda_ok(cudaGetDeviceProperties(&device, 0), "selected data test device");
 INFO("CUDA device 0: " << device.name << " CC " << device.major << "." << device.minor);
 const mmltk::frameworks::gpu::DeviceContext context(0, mmltk::frameworks::gpu::cuda_image_copy_backend(), mmltk::frameworks::gpu::DeviceContextMode::PrimaryInterop);
 mmltk::frameworks::gpu::ImageStream owned_stream(context);
 const auto compute_stream = reinterpret_cast<cudaStream_t>(owned_stream.native_handle());
 create_synthetic_dataset(fixture);
 printf("=== Test dataset: %d images at %dx%d ===\n", NUM_IMAGES, W, H);
 auto ccfg = compiler_config(fixture);
 ccfg.num_workers = 2;
 const DatasetCompilePlan compile_plan = DatasetCompiler::prepare(ccfg, {ccfg.split});
 DatasetCompiler::compile(compile_plan, 0U);
 printf("=== Compiled ===\n");
 // Resized compilation, canonical progress, letterboxing, and tiny masks
 // are covered by compile_progress; all transport runs share this artifact.
 const std::string bin_path = compiled_bin_path(fixture);
 const bool bin_exists = fs::exists(bin_path);
 REQUIRE(bin_exists);
 printf("Compiled file: %zu bytes\n", static_cast<size_t>(fs::file_size(bin_path)));
 exercise_used_ranges(bin_path);
 exercise_prefault_custody(bin_path);
 exercise_used_read_measurements(bin_path);
 // Run every H2D assertion before probing the optional GDR transport.
 for (const bool h2d : {true, false}) {
  try {
   exercise_roundtrip_transport(fixture, h2d, compute_stream);
  } catch (const mmltk::frameworks::gpu::GdrTransportUnavailable& error) {
   if (h2d) throw;
   SKIP("GDR hardware unavailable after complete H2D coverage: " << error.what());
  }
 }
 printf("=== ALL TESTS PASSED ===\n");
}
TEST_CASE("compiled dataset round trip", "[backend][data][roundtrip][cuda]") { test_roundtrip_end_to_end(); }
TEST_CASE("background-only compiled batches prepare no label allocation", "[backend][data][roundtrip][cuda]") {
 const mmltk::testsupport::ScopedTempDir root("compiled-background-buffers");
 const FixtureSpec fixture{.root_dir = root.path().string(), .width = 8, .height = 8, .num_images = 2, .background_images = 2};
 create_synthetic_dataset(fixture);
 compile_existing_fixture(fixture);
 const auto source = CompiledDataset::open(compiled_bin_path(fixture));
 REQUIRE(source.header().max_label_bytes == 0);
 REQUIRE(source.header().max_instances_per_image == 0);
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 REQUIRE(cudaFree(nullptr) == cudaSuccess);
 CompiledImageStream stream({.slots = 1, .workers = 1, .device = 0, .loading = data_loading_options(true)});
 stream.bind_current_context();
 const auto stride = source.header().image_stride;
 stream.prepare_images(0, 30 * stride);
 stream.prepare_reads(0, 30, source.header().max_label_bytes);
 CHECK_FALSE(stream.label_storage(0).owns_allocation());
 CHECK(stream.label_storage(0).capacity_bytes() == 0);
 const std::array reads{CompiledImageRead{1, 0}, CompiledImageRead{0, stride}};
 for (unsigned batch = 0; batch < 2; ++batch) {
  stream.submit(0, source, reads, {});
  REQUIRE(stream.wait_read(0));
  REQUIRE(stream.annotations(0).size() == 2);
  for (const auto annotations : stream.annotations(0)) {
   CHECK(annotations.labels.empty());
   CHECK(annotations.runs.empty());
  }
  CHECK(std::memcmp(stream.host_images(0).data(), source.image_pixels(1), stride) == 0);
  stream.handoff(0, nullptr);
  stream.release(0, nullptr, {});
  stream.synchronize();
  CHECK(stream.label_storage(0).capacity_bytes() == 0);
 }
}
TEST_CASE("compiled source teardown retains one durable authority across replacement", "[data][gpu][custody]") {
 namespace gpu = mmltk::frameworks::gpu;
 REQUIRE(cudaSetDevice(0) == cudaSuccess);
 REQUIRE(cudaFree(nullptr) == cudaSuccess);
 auto authority = std::make_shared<gpu::TerminalCudaRetirementOwner>(1U);
 CompiledImageStream::Config config{.slots = 1U, .workers = 1U, .device = 0, .loading = data_loading_options(true)};
 config.settle = +[](cudaStream_t stream) -> cudaError_t {
  const auto status = cudaStreamSynchronize(stream);
  return status == cudaSuccess ? cudaErrorUnknown : status;
 };
 {
  CompiledImageStream source(config, authority);
  source.bind_current_context();
  source.prepare_device(0U, 64U);
  source.stop_workers();
  source.stop_workers();
 }
 CHECK_FALSE(authority->admission_open());
 CHECK(authority->fact().occupancy == 1U);
 CHECK(authority->fact().reservations == 0U);
 for (unsigned attempt = 0U; attempt < 4U; ++attempt) CHECK_THROWS(CompiledImageStream(config, authority));
 CHECK(authority->fact().occupancy == 1U);
}
TEST_CASE("perceptual compiler changes shrinking RGB while preserving format catalog and categorical geometry", "[backend][data][compiler][perceptual]") {
 namespace oracle = mmltk::backend::imaging::resample::test_perceptual;
 mmltk::testsupport::ScopedTempDir root("perceptual-compiled");
 const FixtureSpec fixture{.root_dir = root.path().string(), .width = 65, .height = 49, .num_images = 2, .background_images = 0};
 create_synthetic_dataset(fixture);
 auto config = compiler_config(fixture);
 config.resize_mode = GENERATE(mmltk::backend::imaging::resample::ImageResizeMode::Stretch, mmltk::backend::imaging::resample::ImageResizeMode::Letterbox);
 config.output_dir = (root.path() / "ordinary").string();
 config.target_width = 31;
 config.target_height = 31;
 config.num_workers = 1;
 const auto ordinary_plan = DatasetCompiler::prepare(config, {"train"});
 DatasetCompiler::compile(ordinary_plan, 0U);
 const auto ordinary = CompiledDataset::open(fs::path(config.output_dir) / "train.bin");
 config.output_dir = (root.path() / "perceptual").string();
 config.perceptual_downscale = true;
 const auto selected_plan = DatasetCompiler::prepare(config, {"train"});
 DatasetCompiler::compile(selected_plan, 0U);
 const auto selected = CompiledDataset::open(fs::path(config.output_dir) / "train.bin");
 REQUIRE(ordinary.header().version == selected.header().version);
 REQUIRE(std::ranges::equal(ordinary.class_names(), selected.class_names()));
 for (const auto slot : ordinary.eligible_slots()) {
  const auto before = ordinary.label_payload(slot), after = selected.label_payload(slot);
  REQUIRE(before.size() == after.size());
  CHECK(std::memcmp(before.data(), after.data(), before.size()) == 0);
 }
 const auto geometry = mmltk::backend::imaging::resample::compute_image_resize_geometry(65, 49, 31, 31, config.resize_mode);
 for (std::uint32_t image = 0; image != 2; ++image) {
  const auto name = image == 0 ? "000001.png" : "000002.png";
  const auto source = expected_nchw_stub((fs::path(config.source_dir) / "train" / name).string(), 65, 49);
  oracle::Image pixels(65, 49, mmltk::backend::imaging::resample::RgbPixelFormat::RGB8);
  for (unsigned y = 0; y != 49; ++y)
   for (unsigned x = 0; x != 65; ++x)
    for (unsigned channel = 0; channel != 3; ++channel) pixels.set(x, y, channel, source[channel * 65U * 49U + y * 65U + x]);
  const auto expected = oracle::reference(pixels, geometry.resized_width, geometry.resized_height);
  const auto* actual = selected.image_pixels(image);
  std::vector<std::uint8_t> packed(65U * 49U * 3U);
  for (unsigned y = 0U; y < 49U; ++y)
   for (unsigned x = 0U; x < 65U; ++x)
    for (unsigned k = 0U; k < 3U; ++k) packed[(y * 65U + x) * 3U + k] = static_cast<std::uint8_t>(std::lround(pixels.at(x, y, k) * 255.0));
  const auto exact = expected_resized_rgb(packed, 65, 49, 31, 31, config.resize_mode, true);
  CHECK(std::memcmp(actual, exact.data(), exact.size() * sizeof(float)) == 0);
  for (unsigned channel = 0; channel != 3; ++channel)
   for (unsigned y = 0; y != 31; ++y)
    for (unsigned x = 0; x != 31; ++x) {
     double answer = 0;
     if (x >= geometry.offset_x && x < geometry.offset_x + geometry.resized_width && y >= geometry.offset_y && y < geometry.offset_y + geometry.resized_height)
      answer = expected.at(x - geometry.offset_x, y - geometry.offset_y, channel);
     CHECK(std::abs(actual[channel * 31U * 31U + y * 31U + x] - answer) <= 1.0 / 255 + 1e-6);
    }
 }
}
TEST_CASE("empty compiled datasets retain their format admission failure", "[backend][data][roundtrip]") {
 mmltk::testsupport::ScopedTempDir root("empty-compiled");
 const auto path = root.path() / "empty.bin";
 auto layout = compute_file_layout(1U, 12U);
 const std::vector<std::string> classes{"person"};
 auto header = make_file_header({1U, 1U, 1U, 3U, 0U, 12U}, classes, layout);
 header.num_images = 0;
 const auto file = mmltk::common::io::FileHandle::create_output(path.string(), layout.total_size);
 file.pwrite_all(&header, sizeof(header), 0U);
 DatasetLoader::Config config;
 config.compiled_path = path.string();
 CHECK_THROWS_AS(DatasetLoader(config), std::runtime_error);
}
TEST_CASE("compiled owner admits all record sections in its metadata construction", "[backend][data][roundtrip][admission]") {
 mmltk::testsupport::ScopedTempDir root("compiled-fused-admission");
 const auto path = root.path() / "dataset.bin";
 const auto defect = GENERATE("none", "incomplete ordinal", "foreign incomplete ordinal", "extent", "dimensions", "source identity", "source category", "flags", "label span", "class", "zero run",
  "overlap", "canvas", "coverage", "oversized maximum", "undersized maximum", "unaligned maximum");
 CAPTURE(defect);
 std::array<ImageEntry, 2> images{};
 std::array<PackedInstance, 2> labels{};
 std::vector<RLEPair> runs{{0, 1}, {4, 2}};
 const std::array<std::string, 1> names{"person"};
 for (std::size_t i = 0; i < images.size(); ++i) {
  images[i].original_width = images[i].original_height = 4;
  images[i].has_source_image_id = 1;
  images[i].source_image_id = i + 1;
  images[i].source = AnnotationSource::Coco;
  images[i].num_instances = 1;
  images[i].status = ImageStatus::Eligible;
  set_annotation_extent(images[i], 1, i == 0 ? runs.size() : 0, std::to_string(i));
  labels[i].bbox_x2 = labels[i].bbox_y2 = 4;
  labels[i].flags = kAnnotationMask | kAnnotationCategory;
  labels[i].source_category_id = 1;
 }
 labels[0].mask_rle_pairs = 2;
 labels[1].mask_rle_offset = 0;  // A present-empty mask.
 const std::string_view bad = defect;
 if (bad == "incomplete ordinal" || bad == "foreign incomplete ordinal") {
  labels[0].flags |= kAnnotationOrdinalIncomplete;
  labels[0].source_ordinal = 17;
  if (bad == "incomplete ordinal") images[0].source = AnnotationSource::CoconutCoco;
 }
 if (bad == "dimensions") images[1].original_height = 0;
 if (bad == "source identity") images[0].has_source_image_id = 0;
 if (bad == "source category") {
  labels[0].flags &= ~kAnnotationCategory;
  labels[0].source_category_id = 0;
 }
 if (bad == "flags") labels[1].flags |= 128;
 if (bad == "label span") ++images[1].label_bytes;
 if (bad == "class") labels[1].class_id = 1;
 if (bad == "zero run") runs[0].length = 0;
 if (bad == "overlap") runs[1].start = 0;
 if (bad == "canvas") runs[1] = {15, 2};
 if (bad == "coverage") runs.push_back({8, 1});
 if (bad == "coverage") set_annotation_extent(images[0], 1, runs.size(), "0");
 const auto layout = compute_file_layout(images.size(), 4U * 4U * 3U * sizeof(float), annotation_layout(1, runs.size()).used_bytes);
 auto header = make_file_header({2, 4, 4, 3, 1, 4U * 4U * 3U * sizeof(float)}, names, layout);
 if (bad == "extent") ++header.total_file_size;
 if (bad == "oversized maximum") header.max_label_bytes = COMPILED_LABEL_CAPACITY + 8;
 if (bad == "undersized maximum") header.max_label_bytes -= 8;
 if (bad == "unaligned maximum") ++header.max_label_bytes;
 const auto file = mmltk::common::io::FileHandle::create_output(path.string(), layout.total_size);
 file.pwrite_all(&header, sizeof(header), 0);
 file.pwrite_all(images.data(), sizeof(images), layout.index_offset);
 file.pwrite_all(&labels[0], sizeof(PackedInstance), layout.label_offset(0));
 file.pwrite_all(&labels[1], sizeof(PackedInstance), layout.label_offset(1));
 file.pwrite_all(runs.data(), runs.size() * sizeof(RLEPair), layout.label_offset(0) + annotation_layout(1, runs.size()).run_offset);
 if (bad != "none" && bad != "incomplete ordinal") {
  CHECK_THROWS(CompiledDataset::open(path));
  return;
 }
 const auto source = std::make_shared<const CompiledDataset>(CompiledDataset::open(path));
 CHECK(source->slot_count() == 2);
 CHECK(source->image_labels(0).front().has_complete_source_ordinal() == (bad != "incomplete ordinal"));
 if (bad == "incomplete ordinal") CHECK(source->image_labels(0).front().source_ordinal == 17);
 CHECK(source->masks_available());
 CHECK(source->image_labels(1).front().has_mask());
 CHECK(source->instance_rle(1, source->image_labels(1).front()).empty());
 const auto shared = source;
 CHECK(shared->annotations(0).labels.data() == source->annotations(0).labels.data());
 CHECK(shared->annotations(0).runs.data() == source->annotations(0).runs.data());
}
TEST_CASE("Directory metadata construction preserves empty masks offsets and publication across workers", "[backend][data][roundtrip][admission]") {
 mmltk::testsupport::ScopedTempDir root("directory-final-construction");
 const FixtureSpec fixture{.root_dir = root.path().string(), .width = 32, .height = 16, .num_images = 3, .background_images = 0};
 create_synthetic_dataset(fixture);
 auto config = compiler_config(fixture);
 config.target_width = 16;
 config.target_height = 8;
 const auto annotations = fs::path(config.source_dir) / "train";
 const auto write = [&](const char* name, std::string_view contents) {
  std::ofstream file(annotations / name, std::ios::trunc);
  file << contents;
  REQUIRE(file.good());
 };
 const std::string first =
  R"({"class":"person","image_id":0,"id":0,"category_id":0,"bbox_xyxy":[0,0,8,4],"iscrowd":true,"ignore":true,"area":32,"mask_rle_encoding":"row_major_start_length","mask_rle":"0:8 32:8 64:8 96:8","image_size_wh":[32,16]})";
 const std::string empty = R"({"class":"ret","image_id":0,"id":9,"bbox_xyxy":[2,2,6,6],"mask_rle_encoding":"row_major_start_length","mask_rle":"","image_size_wh":[32,16]})";
 write("000001.jsonl", first + "\n" + empty + "\n");
 write("000002.jsonl", "");
 write("000003.jsonl", empty + "\n");
 std::vector<std::uint8_t> baseline;
 for (const auto workers : {1, 2, 3}) {
  config.num_workers = workers;
  const auto plan = DatasetCompiler::prepare(config, {"train"});
  DatasetCompiler::compile(plan, 0);
  const auto result = CompiledDataset::open(compiled_bin_path(fixture));
  REQUIRE(result.image_entries().size() == 3);
  REQUIRE(result.label_count() == 3);
  CHECK(result.header().max_instances_per_image == 2);
  REQUIRE_FALSE((result.run_count() == 0));
  CHECK(result.image_labels(1).empty());
  CHECK(result.image_entry(0).has_source_image_id == 1);
  CHECK(result.image_entry(0).source_image_id == 0);
  const auto& label = result.image_labels(0)[0];
  CHECK(label.has_annotation_id());
  CHECK(label.annotation_id == 0);
  CHECK(label.source_category_id == 0);
  CHECK(label.is_crowd());
  CHECK(label.original_area == 32);
  CHECK(label.source_ordinal == 1);
  for (const auto image : {0U, 2U}) {
   const auto& present_empty = result.image_labels(image).back();
   CHECK(present_empty.has_mask());
   CHECK(result.instance_rle(image, present_empty).empty());
   CHECK(present_empty.mask_rle_offset == result.annotations(image).runs.size_bytes());
  }
  for (std::uint32_t image = 0; image < result.image_entries().size(); ++image)
   CHECK(result.pixel_offset(image) == compute_file_layout(result.header().num_slots, result.header().image_stride, result.header().max_label_bytes).pixel_offset(image));
  const auto bytes = mmltk::testsupport::read_binary_file(compiled_bin_path(fixture));
  if (baseline.empty())
   baseline = bytes;
  else
   CHECK(bytes == baseline);
 }
 // Pixel stores retain the rectangular Directory canvas through the shared
 // checked plan in both numerical policies and both geometry modes.
 for (const auto mode : {mmltk::backend::imaging::resample::ImageResizeMode::Stretch, mmltk::backend::imaging::resample::ImageResizeMode::Letterbox}) {
  config.resize_mode = mode;
  for (const bool perceptual : {false, true}) {
   config.perceptual_downscale = perceptual;
   const auto plan = DatasetCompiler::prepare(config, {"train"});
   DatasetCompiler::compile(plan, 0);
   const auto result = CompiledDataset::open(compiled_bin_path(fixture));
   CHECK(result.header().image_width == 16);
   CHECK(result.header().image_height == 8);
   CHECK(result.image_labels(0)[0].source_ordinal == 1);
  }
 }
 config.resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
 config.perceptual_downscale = false;
 DatasetCompiler::compile(DatasetCompiler::prepare(config, {"train"}), 0);
 // Consumed input errors use the same parser, merge and publication transaction.
 for (const auto malformed :
  {R"({"class":"person","bbox_xyxy":[0,0,4,4],"iscrowd":2})", R"({"class":"person","bbox_xyxy":[0,0,4,4],"mask_rle_encoding":"row_major_start_length","mask_rle":"0:4 2:2"})",
   R"({"class":"person","bbox_xyxy":[0,0,4,4],"category_id":-1})"}) {
  write("000001.jsonl", malformed);
  const auto plan = DatasetCompiler::prepare(config, {"train"});
  CHECK_THROWS(DatasetCompiler::compile(plan, 0));
  CHECK(mmltk::testsupport::read_binary_file(compiled_bin_path(fixture)) == baseline);
 }
}
TEST_CASE("compiled scatter output checks bounded vectors and partial physical writes", "[backend][data][storage]") {
 using mmltk::common::io::FileHandle;
 const mmltk::testsupport::ScopedTempDir root("compiled-scatter-write");
 auto file = FileHandle::create_output((root.path() / "scatter").string(), 0);
 SECTION("vector batches preserve empty spans and offsets") {
  std::array<std::uint8_t, 40> source{};
  std::array<FileHandle::WriteSpan, 80> spans{};
  for (std::size_t i = 0; i < source.size(); ++i) {
   source[i] = static_cast<std::uint8_t>(i);
   spans[i * 2] = {&source[i], 1};
  }
  file.pwritev_all(spans, 17);
  std::array<std::uint8_t, 40> stored{};
  file.pread_all(stored.data(), stored.size(), 17);
  CHECK(stored == source);
  CHECK_THROWS(file.pwritev_all(spans, SIZE_MAX - 1));
  CHECK(file.size() == 57);
 }
 SECTION("short label write reports the later physical limit error") {
  const auto layout = compute_file_layout(1, 12);
  PackedInstance label{};
  label.source_ordinal = 83;
  ImageEntry entry{};
  set_annotation_extent(entry, 1, 0, "short output");
  struct rlimit original{};
  REQUIRE(::getrlimit(RLIMIT_FSIZE, &original) == 0);
  sigset_t blocked{}, previous{};
  REQUIRE(::sigemptyset(&blocked) == 0);
  REQUIRE(::sigaddset(&blocked, SIGXFSZ) == 0);
  REQUIRE(::pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0);
  const mmltk::testsupport::ScopedTestCleanup restore([&] {
   (void)::setrlimit(RLIMIT_FSIZE, &original);
   timespec immediate{};
   while (::sigtimedwait(&blocked, nullptr, &immediate) >= 0) {}
   (void)::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  });
  auto limited = original;
  limited.rlim_cur = static_cast<rlim_t>(layout.label_offset(0) + 17);
  REQUIRE(limited.rlim_cur <= original.rlim_max);
  REQUIRE(::setrlimit(RLIMIT_FSIZE, &limited) == 0);
  CHECK_THROWS(write_compiled_annotations(file, layout, 0, entry, {{&label, 1}, {}}));
  CHECK(file.size() == layout.label_offset(0) + 17);
  std::array<std::byte, 17> prefix{};
  file.pread_all(prefix.data(), prefix.size(), layout.label_offset(0));
  CHECK(std::memcmp(prefix.data(), &label, prefix.size()) == 0);
 }
}
TEST_CASE("runtime file readiness follows actual capacity return and retains notification custody", "[backend][data][roundtrip]") {
 using mmltk::backend::data::detail::FileWorkQueue;
 const auto topology = NumaTopology::Capture();
 const auto first_cpu = std::ranges::find(topology.cpus, topology.permitted_cpus.front(), &CpuTopology::cpu);
 const auto placement = resolve_placement(topology, first_cpu->node);
 WorkerPool pool(2, placement.cpus, "runtime-file", 1, &placement);
 mmltk::testsupport::TestGate completing("file completion still owns its record"), notifying("file capacity notification retains its owner");
 struct Readiness {
  FileWorkQueue* files = nullptr;
  mmltk::testsupport::TestGate* notifying = nullptr;
  std::atomic<unsigned> notifications{0}, physical{0};
  std::optional<mmltk::backend::data::detail::CompileExecution::Completion> retried;
  std::exception_ptr failure;
 } readiness;
 readiness.notifying = &notifying;
 FileWorkQueue files(pool, 1, {&readiness, [](void* context, void*) noexcept {
  auto& state = *static_cast<Readiness*>(context);
  if (state.notifications.fetch_add(1) != 0) return;
  try {
   state.retried = state.files->try_submit({.context = &state.physical, .call = [](void* value) { ++*static_cast<std::atomic<unsigned>*>(value); }});
   state.notifying->receipt().ArriveAndWait();
  } catch (...) { state.failure = std::current_exception(); }
 }});
 readiness.files = &files;
 std::future<void> joined;
 const mmltk::testsupport::ScopedTestCleanup release([&] {
  completing.Release();
  notifying.Release();
  files.cancel();
  files.join();
 });
 auto first = files.try_submit(
  {.context = &readiness.physical, .call = [](void* value) { ++*static_cast<std::atomic<unsigned>*>(value); }, .completed = [&](std::exception_ptr) { completing.receipt().ArriveAndWait(); }});
 REQUIRE(first);
 REQUIRE(completing.WaitEntered(std::chrono::seconds{2}));
 CHECK_FALSE(files.has_capacity());
 CHECK_FALSE(files.try_submit({.work = [] {}}));
 completing.Release();
 REQUIRE(notifying.WaitEntered(std::chrono::seconds{2}));
 REQUIRE(readiness.retried);
 CHECK_FALSE(first->ready());
 readiness.retried->wait();
 joined = std::async(std::launch::async, [&] { files.join(); });
 CHECK(joined.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
 notifying.Release();
 mmltk::testsupport::await_test_future(joined, "file queue joins final readiness callback");
 first->wait();
 CHECK_FALSE(readiness.failure);
 CHECK(readiness.notifications == 2);
 CHECK(readiness.physical == 2);
 CHECK(files.has_capacity());
 files.cancel();
 CHECK_THROWS(files.try_submit({.work = [] {}}));
 CHECK_THROWS(files.has_capacity());
}
