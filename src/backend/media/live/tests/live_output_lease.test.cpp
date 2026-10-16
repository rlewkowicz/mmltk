#include "src/backend/media/live/live_types.h"
#include "src/backend/media/live/tests/support/live_capture_fixture.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/cuda_test_utils.hpp"
#include <condition_variable>
#include <array>
#include <string>
#include <vector>
#include <mutex>
#include "src/backend/media/live/tests/support/live_analysis_fixture.h"
#include <cstring>
#include <cerrno>
#include <linux/videodev2.h>
#include <new>
#include "src/backend/media/live/live_session_controller.h"
#include "src/backend/media/capture/capture_session.h"
#include "src/backend/media/capture/status.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include "src/test_support/async_test_utils.hpp"
#include "src/backend/media/live/detail/live_output_callback_lifetime.h"
#include "src/backend/media/live/detail/live_physical_retirement.h"
#include "src/backend/media/live/detail/live_slot_state.h"
#include "src/frameworks/gpu/cuda/device_execution.h"
#include "src/frameworks/gpu/cuda/resource_owner_command_authority.h"
import mmltk.backend.imaging.raster;
namespace mmltk::backend::media::live {
namespace {
using test_support::LiveAnalysisFixture;
struct AnalysisAllocationProbe final {
 static inline std::size_t allocations = 0;
 static inline bool fail = false;
 static std::shared_ptr<void> Allocate(std::size_t bytes, LiveAnalysisProduct::Storage) {
  ++allocations;
  if (fail) throw std::bad_alloc{};
  return {::operator new(bytes), [](void* address) { ::operator delete(address); }};
 }
};
TEST_CASE("Live completed semantic copies retain named mixed joints independently of provider storage") {
 LiveAnalysisFixture source;
 auto catalog = source.catalog;
 const std::weak_ptr<const mmltk::backend::data::catalog::ClassCatalog> catalog_custody = catalog;
 const std::weak_ptr<std::int64_t> count_custody = source.count;
 LiveAnalysisProduct compositor{LiveAnalysisProduct::Storage::Pinned, &AnalysisAllocationProbe::Allocate};
 source.Deliver(compositor, {4, 5});
 REQUIRE(compositor.ready());
 REQUIRE(compositor.analyzed());
 REQUIRE(compositor.regions().size() == 1);
 const auto& region = compositor.regions()[0];
 CHECK(region.source_region == mmltk::backend::ml::runtime::AnalysisRegion{1, 1, 2, 2});
 CHECK(region.class_domain == mmltk::backend::data::catalog::ClassReferenceDomain::Foreground);
 CHECK(region.class_catalog == catalog);
 CHECK(region.count == 2);
 REQUIRE(region.keypoint_counts.size() == 2);
 CHECK(region.keypoint_counts[0] == 2);
 CHECK(region.keypoint_counts[1] == 0);
 REQUIRE(region.keypoints.size() == 4);
 CHECK(region.keypoints[0].x == 1.25F);
 CHECK(region.keypoints[0].covariance_xy == -.5F);
 CHECK(region.class_catalog->keypoints(0).names[1] == "right");
 CHECK(region.masks.size() == 8);
 CHECK(region.masks[3] == 1);
 source.joints[0] = {};
 source.count.reset();
 CHECK(count_custody.expired());
 LiveAnalysisProduct receiver;
 receiver.CopyFrom(compositor);
 compositor.Reset();
 source.catalog.reset();
 CHECK(receiver.frame() == LiveFrameId{4, 5});
 CHECK(receiver.regions()[0].keypoints[0].covariance_xy == -.5F);
 CHECK(receiver.regions()[0].class_catalog == catalog);
 CHECK_FALSE(compositor.ready());
 catalog.reset();
 CHECK_FALSE(catalog_custody.expired());
 receiver.Reset();
 CHECK(catalog_custody.expired());
}
TEST_CASE("Live distinguishes no analysis from successful zero detections and clears stale values") {
 const bool pending = GENERATE(false, true);
 LiveAnalysisFixture source;
 LiveAnalysisProduct product;
 source.Deliver(product, {2, 1}, pending);
 const auto capacity = product.capacity_bytes();
 *source.count = 0;
 source.Deliver(product, {2, 2}, pending);
 REQUIRE(product.ready());
 REQUIRE(product.analyzed());
 REQUIRE(product.regions().size() == 1);
 CHECK(product.regions()[0].count == 0);
 CHECK(product.regions()[0].keypoints.empty());
 CHECK(product.regions()[0].boxes_xyxy.empty());
 CHECK(product.regions()[0].class_catalog == source.catalog);
 CHECK(product.capacity_bytes() == capacity);
 product.Reset({2, 3});
 CHECK(product.ready());
 CHECK_FALSE(product.analyzed());
 CHECK(product.regions().empty());
 CHECK(product.capacity_bytes() == capacity);
 product.Reset();
 CHECK_FALSE(product.ready());
}
TEST_CASE("Live semantic capacity is reusable and failed growth preserves receiver publication") {
 AnalysisAllocationProbe::allocations = 0;
 AnalysisAllocationProbe::fail = false;
 LiveAnalysisFixture source;
 LiveAnalysisProduct compositor, receiver{LiveAnalysisProduct::Storage::Host, &AnalysisAllocationProbe::Allocate};
 *source.count = 0;
 source.Deliver(compositor, {3, 1}, false);
 receiver.CopyFrom(compositor);
 const auto capacity = receiver.capacity_bytes();
 *source.count = 2;
 source.Deliver(compositor, {3, 2}, false);
 AnalysisAllocationProbe::fail = true;
 CHECK_THROWS_AS(receiver.CopyFrom(compositor), std::bad_alloc);
 AnalysisAllocationProbe::fail = false;
 CHECK(receiver.frame() == LiveFrameId{3, 1});
 CHECK(receiver.ready());
 CHECK(receiver.regions()[0].count == 0);
 CHECK(receiver.capacity_bytes() == capacity);
 receiver.CopyFrom(compositor);
 const auto allocations = AnalysisAllocationProbe::allocations;
 receiver.CopyFrom(compositor);
 CHECK(AnalysisAllocationProbe::allocations == allocations);
 CHECK(receiver.regions()[0].keypoint_counts[1] == 0);
}
TEST_CASE("Live semantic admission rejects invalid extents and never publishes invalid completed counts") {
 LiveAnalysisFixture source;
 LiveAnalysisProduct product;
 auto annotation = source.Annotation();
 SECTION("region outside source") { annotation.source_region.x = 4; }
 SECTION("rank") { annotation.keypoints.shape.rank = 2; }
 SECTION("full plane bytes even with a known zero count") {
  annotation.count.SetKnown(0, 2);
  annotation.keypoints.capacity_bytes -= sizeof(float);
 }
 SECTION("task joint capacity") { annotation.keypoint_capacity = 257; }
 SECTION("named domain") { annotation.class_domain = mmltk::backend::data::catalog::ClassReferenceDomain::RawOutputSlot; }
 CHECK_THROWS_AS(product.PrepareDevice({1, 1}, 4, 4, std::span{&annotation, 1}), std::invalid_argument);
 CHECK_FALSE(product.ready());
 CHECK(product.regions().empty());
}
TEST_CASE("Live semantic completion validates survivor and named joint counts before publication") {
 const int invalid = GENERATE(0, 1, 2);
 LiveAnalysisFixture source;
 if (invalid == 0) *source.count = 3;
 if (invalid == 1) source.joint_counts[0] = 1;
 if (invalid == 2) source.classes[1] = 2;
 LiveAnalysisProduct product;
 CHECK_THROWS_AS(source.Deliver(product, {1, 1}), std::invalid_argument);
 CHECK_FALSE(product.ready());
 CHECK(product.regions().empty());
}
TEST_CASE("Live retains raw detection products without inventing joint or catalog meaning") {
 LiveAnalysisFixture source;
 auto annotation = source.Annotation(false);
 annotation.class_domain = mmltk::backend::data::catalog::ClassReferenceDomain::RawOutputSlot;
 annotation.class_catalog.reset();
 annotation.keypoints_available = false;
 annotation.keypoint_capacity = 0;
 annotation.keypoints = annotation.keypoint_counts = {};
 annotation.masks_available = false;
 source.classes = {91, 365};
 LiveAnalysisProduct product;
 product.PrepareDevice({1, 1}, 4, 4, std::span{&annotation, 1});
 REQUIRE(product.EnqueueDeviceCopy(1, &LiveAnalysisFixture::Copy) == cudaSuccess);
 product.CompleteDeviceCopy();
 const auto& region = product.regions()[0];
 CHECK(region.class_references[1] == 365);
 CHECK_FALSE(region.class_catalog);
 CHECK_FALSE(region.keypoints_available);
 CHECK(region.keypoints.empty());
 CHECK_FALSE(region.masks_available);
 CHECK(region.masks.empty());
 CHECK(region.boxes_xyxy[1][2] == 3.F);
}
struct AnalysisCopyFailure final {
 static inline std::size_t copies = 0;
 static cudaError_t Copy(void* target, const void* source, std::size_t bytes, std::uintptr_t stream) noexcept {
  if (++copies == 3) return cudaErrorLaunchFailure;
  return LiveAnalysisFixture::Copy(target, source, bytes, stream);
 }
};
TEST_CASE("Live partial semantic submission remains unpublished until its owner settles and resets") {
 LiveAnalysisFixture source;
 auto annotation = source.Annotation();
 LiveAnalysisProduct product;
 product.PrepareDevice({1, 1}, 4, 4, std::span{&annotation, 1});
 CHECK_THROWS_AS(product.CompleteDeviceCopy(), std::logic_error);
 AnalysisCopyFailure::copies = 0;
 CHECK(product.EnqueueDeviceCopy(1, &AnalysisCopyFailure::Copy) == cudaErrorLaunchFailure);
 CHECK_FALSE(product.ready());
 CHECK(product.regions().empty());
 CHECK_THROWS_AS(product.CompleteDeviceCopy(), std::logic_error);
 // The injected copies have already settled. Production uses its slot stream.
 product.SettleDeviceCopy();
 product.Reset();
 source.Deliver(product, {1, 2});
 CHECK(product.ready());
 CHECK(product.regions()[0].keypoint_counts[0] == 2);
}
struct StartupOwner final {
 std::atomic_bool failed{false};
 [[nodiscard]] LiveDataPlaneConfig Configuration() {
  LiveDataPlaneConfig config;
  config.resource_worker = mmltk::frameworks::gpu::ResourceOwnerWorkerCapability{this, 1, [](const void*, std::uintptr_t) noexcept { return true; }, [](const void* context, std::uintptr_t) noexcept {
   static_cast<StartupOwner*>(const_cast<void*>(context))->failed = true;
   return true;
  }};
  config.capture.cuda_device_index = 0;
  config.capture.width = config.capture.height = 32;
  config.capture.device_path = "/must-not-open-before-provider";
  return config;
 }
};
TEST_CASE("Live provider admission failure requires joined clean media resources", "[live][cuda][startup]") {
 const bool fail_cleanup = GENERATE(false, true);
 const bool cancelled = GENERATE(false, true);
 StartupOwner owner;
 struct Provider {
  mmltk::testsupport::TestGate construction{"Live provider admission"};
  bool fail_cleanup = false;
  bool cancelled = false;
  std::atomic_bool fail_restore{false};
  std::atomic<unsigned> restore_failures{0};
 } provider;
 provider.fail_cleanup = fail_cleanup;
 provider.cancelled = cancelled;
 auto config = owner.Configuration();
 config.device_api = {
  &provider,
  [](void*, int* device) noexcept { return cudaGetDevice(device); },
  [](void* context, int device) noexcept {
   auto& state = *static_cast<Provider*>(context);
   if (state.fail_restore.exchange(false)) {
    ++state.restore_failures;
    return cudaErrorInvalidValue;
   }
   return cudaSetDevice(device);
  }
 };
 LiveMediaDataPlane plane(config);
 CHECK_FALSE(plane.no_custody_start_settlement().ok());
 auto startup = std::async(std::launch::async, [&] {
  return plane.start({&provider, [](const void* context, mmltk::backend::ml::runtime::BorrowedCommandStream stream) -> std::shared_ptr<mmltk::backend::ml::runtime::AnalysisProvider> {
   auto& state = *static_cast<Provider*>(const_cast<void*>(context));
   if (!stream.valid) throw std::runtime_error("provider stream unavailable");
   state.construction.receipt().ArriveAndWait();
   // Fail one cleanup scope's device restoration through the existing device
   // API boundary. The owner retains and releases every real CUDA handle.
   state.fail_restore = state.fail_cleanup;
   throw std::runtime_error(state.cancelled ? "provider admission cancelled before capture" : "provider artifact IO admission refused before capture");
  }});
 });
 const mmltk::testsupport::ScopedTestCleanup cleanup([&] { provider.construction.Release(); });
 REQUIRE(provider.construction.WaitEntered(std::chrono::seconds{20}));
 CHECK_FALSE(plane.no_custody_start_settlement().ok());
 provider.construction.Release();
 const auto start = mmltk::testsupport::await_test_future(startup, "Live provider startup", std::chrono::seconds{20});
 REQUIRE(provider.restore_failures.load() == (fail_cleanup ? 1U : 0U));
 REQUIRE(start.valid());
 CHECK_FALSE(start.has_custody());
 CHECK(start.status.message == (cancelled ? "provider admission cancelled before capture" : "provider artifact IO admission refused before capture"));
 CHECK_FALSE(plane.admitted_run_terminal());
 CHECK_FALSE(plane.no_custody_start_settlement().ok());
 CHECK(plane.stop().message == start.status.message);
 const auto settled = plane.no_custody_start_settlement();
 CHECK(settled.ok() == !fail_cleanup);
 if (fail_cleanup) CHECK(settled.code == mmltk::backend::media::capture::StatusCode::kCudaError);
 CHECK_FALSE(owner.failed.load());
 CHECK(plane.first_failure().message == start.status.message);
 // The device-restoration failure is ordinary: it must not poison the
 // CUDA context or prevent subsequent owner allocations and release.
 void* allocation = nullptr;
 REQUIRE(cudaMalloc(&allocation, 32) == cudaSuccess);
 CHECK(cudaFree(allocation) == cudaSuccess);
 static_cast<void>(cudaGetLastError());
}
class PlaneTestProvider final : public mmltk::backend::ml::runtime::AnalysisProvider {
 using Runtime = mmltk::backend::ml::runtime::AnalysisRequest;
 cudaStream_t stream_;
 cudaEvent_t event_ = nullptr;
 bool empty_;
 const std::array<float, 4> box_{4, 4, 26, 26};
 const std::array<std::uint8_t, 3> color_{255, 0, 0};
 const int category_ = 0;
 const float confidence_ = .9F;
 std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> catalog_ = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"object"});

public:
 PlaneTestProvider(std::uintptr_t stream, bool empty) : stream_(reinterpret_cast<cudaStream_t>(stream)), empty_(empty) {
  if (cudaEventCreateWithFlags(&event_, cudaEventDisableTiming) != cudaSuccess) throw std::runtime_error("fixture event");
 }
 ~PlaneTestProvider() override {
  if (event_) static_cast<void>(cudaEventDestroy(event_));
 }
 ProviderWorkResult DoAnalyze(const Runtime& request) noexcept override {
  using namespace mmltk::backend::ml::runtime;
  auto status = cudaStreamWaitEvent(stream_, reinterpret_cast<cudaEvent_t>(request.source_ready.event), 0);
  for (auto& output : request.annotations) {
   const auto copy = [&](std::uintptr_t destination, const void* source, std::size_t bytes) {
    if (status == cudaSuccess) status = cudaMemcpyAsync(reinterpret_cast<void*>(destination), source, bytes, cudaMemcpyHostToDevice, stream_);
   };
   copy(output.boxes_xyxy.address, box_.data(), sizeof(box_));
   copy(output.colors_rgb.address, color_.data(), sizeof(color_));
   copy(output.class_references.address, &category_, sizeof(category_));
   copy(output.confidences.address, &confidence_, sizeof(confidence_));
   if (status == cudaSuccess) status = cudaMemsetAsync(reinterpret_cast<void*>(output.masks.address), 1, output.source_region.width * output.source_region.height, stream_);
   output.class_catalog = catalog_;
   output.class_domain = mmltk::backend::data::catalog::ClassReferenceDomain::Foreground;
   output.masks_available = true;
   output.count.SetKnown(empty_ ? 0 : 1, output.value_capacity);
  }
  if (status == cudaSuccess) status = cudaEventRecord(event_, stream_);
  if (status != cudaSuccess) return {.identity = request.identity};
  return {
   .identity = request.identity,
   .terminal = AnalysisTerminal::Completed,
   .completed_ns = 1,
   .output_count = request.annotations.size(),
   .completion = {request.source.device, reinterpret_cast<std::uintptr_t>(event_), reinterpret_cast<std::uintptr_t>(stream_)}
  };
 }
 bool ObserveCompletion(const mmltk::backend::ml::runtime::AnalysisCompletion&) noexcept override { return cudaEventSynchronize(event_) == cudaSuccess; }
 void RetireIssuedWork(const ProviderWorkResult&) noexcept override { static_cast<void>(cudaStreamSynchronize(stream_)); }
 void DoShutdown() noexcept override {}
};
TEST_CASE("Live media leases keep capture clean beside masks boxes and manual alpha", "[live][cuda][planes]") {
 const int mode = GENERATE(0, 1, 2);  // Capture only, completed zero, completed mask/box.
 mmltk::testsupport::ScopedTempDir directory("live-media-planes");
 test_support::LiveCaptureFixture capture(directory.path() / "capture.fifo", 16);
 StartupOwner owner;
 auto config = owner.Configuration();
 config.capture.device_path = capture.path();
 config.capture.device_api = capture.api();
 config.fanout_slots = 16;
 LiveMediaDataPlane plane(config);
 std::mutex mutex;
 std::condition_variable changed;
 std::uint64_t generation = 0;
 struct Notification {
  std::mutex* mutex;
  std::condition_variable* changed;
  std::uint64_t* generation;
 } notification{&mutex, &changed, &generation};
 plane.set_revision_listener({&notification, [](void* context) noexcept {
  auto& state = *static_cast<Notification*>(context);
  {
   std::scoped_lock lock(*state.mutex);
   ++*state.generation;
  }
  state.changed->notify_one();
 }});
 ManualOverlayDocumentSnapshot manual{};
 manual.capture_width = manual.capture_height = 32;
 ManualOverlayInstance object{};
 object.instance_id = "manual";
 object.mask_region = {8, 8, 8, 8};
 object.mask.assign(64, 1);
 object.style = ManualOverlayStyle{.r = 0, .g = 100, .b = 0, .alpha = 128};
 manual.instances.push_back(std::move(object));
 plane.publish_manual_overlay(std::move(manual));
 LiveAnalysisProviderFactory factory;
 if (mode)
  factory = {&mode, [](const void* context, mmltk::backend::ml::runtime::BorrowedCommandStream stream) -> std::shared_ptr<mmltk::backend::ml::runtime::AnalysisProvider> {
   return std::make_shared<PlaneTestProvider>(stream.native_handle, *static_cast<const int*>(context) == 1);
  }};
 REQUIRE(plane.start(factory).running());
 const mmltk::testsupport::ScopedTestCleanup cleanup([&] { static_cast<void>(plane.stop()); });
 LiveCompositeOutputLease held;
 std::array<std::uint8_t, 32 * 32 * 4> clean{}, semantic{};
 std::uint64_t observed = 0;
 const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
 for (;;) {
  {
   std::unique_lock lock(mutex);
   REQUIRE(changed.wait_until(lock, deadline, [&] { return generation != observed; }));
   observed = generation;
  }
  const auto revision = plane.newest_revision();
  if (!revision || !plane.try_acquire_output(*revision, &held)) continue;
  const auto view = held.view();
  REQUIRE(cudaEventSynchronize(reinterpret_cast<cudaEvent_t>(view.ready_event)) == cudaSuccess);
  REQUIRE(cudaMemcpy2D(clean.data(), 128, reinterpret_cast<void*>(view.clean), view.clean_pitch_bytes, 128, 32, cudaMemcpyDeviceToHost) == cudaSuccess);
  REQUIRE(cudaMemcpy2D(semantic.data(), 128, reinterpret_cast<void*>(view.semantic), view.semantic_pitch_bytes, 128, 32, cudaMemcpyDeviceToHost) == cudaSuccess);
  const auto center = (10 * 32 + 10) * 4;
  if (semantic[center + 1] && (!mode || view.analysis->analyzed())) break;
  std::move(held).Complete();
 }
 for (std::size_t y = 0; y < 32; ++y)
  for (std::size_t x = 0; x < 32; ++x) {
   const auto offset = (y * 32 + x) * 4;
   CHECK(clean[offset] == 16);
   CHECK(clean[offset + 1] == 24);
   CHECK(clean[offset + 2] == (((x / 16 + y / 16) % 2) ? 48 : 96));
   CHECK(clean[offset + 3] == 255);
  }
 CHECK(held.view().analysis->analyzed() == (mode != 0));
 if (mode) CHECK(held.view().analysis->regions().front().count == (mode == 2 ? 1 : 0));
 const auto center = (10 * 32 + 10) * 4;
 CHECK(semantic[center + 3] == (mode == 2 ? 185 : 128));
 CHECK(semantic[center] == (mode == 2 ? 78 : 0));
 CHECK(semantic[center + 1] == (mode == 2 ? 69 : 100));
 if (mode == 2) {
  // Box strokes extend outside the bounds; the inside corner remains mask.
  CHECK(semantic[(4 * 32 + 3) * 4] == 255);
  CHECK(semantic[(4 * 32 + 3) * 4 + 3] == 255);
  CHECK(semantic[(4 * 32 + 4) * 4 + 3] == 115);
 }
 namespace raster = mmltk::backend::imaging::raster;
 mmltk::testsupport::ScopedTestStream composite_stream;
 auto output = mmltk::testsupport::allocate_cuda_test_object<decltype(clean)>();
 REQUIRE(raster::finalize_rgba(
          {.clean = {reinterpret_cast<const std::uint8_t*>(held.view().clean), held.view().clean_pitch_bytes, 32, 32},
           .semantic = {reinterpret_cast<const std::uint8_t*>(held.view().semantic), held.view().semantic_pitch_bytes, 32, 32},
           .destination = {reinterpret_cast<std::uint8_t*>(output.get()), 128, 32, 32},
           .stream = {composite_stream.get()}}) == cudaSuccess);
 decltype(clean) composite;
 REQUIRE(cudaMemcpyAsync(composite.data(), output.get(), composite.size(), cudaMemcpyDeviceToHost, composite_stream.get()) == cudaSuccess);
 REQUIRE(cudaStreamSynchronize(composite_stream.get()) == cudaSuccess);
 CHECK(composite[center] == (mode == 2 ? 60 : 7));
 CHECK(composite[center + 1] == (mode == 2 ? 56 : 62));
 CHECK(composite[center + 2] == (mode == 2 ? 26 : 47));
 CHECK(composite[center + 3] == 255);
 std::array<std::uint8_t, 32 * 32 * 3> raw{};
 std::promise<bool> readback;
 REQUIRE(
  plane.request_raw_readback({.frame = held.frame_revision().frame, .destination = raw, .context = &readback, .complete = [](void* context, LiveFrameId, std::size_t bytes, bool completed) noexcept {
  try {
   static_cast<std::promise<bool>*>(context)->set_value(completed && bytes == 32 * 32 * 3);
  } catch (...) {}
 }}));
 auto result = readback.get_future();
 REQUIRE(mmltk::testsupport::await_test_future(result, "Live clean raw readback"));
 CHECK(raw[0] == 96);
 CHECK(raw[1] == 24);
 CHECK(raw[2] == 16);
 auto stop = std::async(std::launch::async, [&] { return plane.stop(); });
 mmltk::testsupport::ScopedTestCleanup release_held([&] {
  if (held) std::move(held).Complete();
 });
 // The exact acquired slot keeps both physical planes alive during Stop.
 REQUIRE(cudaMemcpy2D(clean.data(), 128, reinterpret_cast<void*>(held.view().clean), held.view().clean_pitch_bytes, 128, 32, cudaMemcpyDeviceToHost) == cudaSuccess);
 CHECK(clean[0] == 16);
 std::move(held).Complete();
 CHECK(mmltk::testsupport::await_test_future(stop, "Live plane retirement").ok());
}
TEST_CASE("Live capture custody and pending physical release cannot prove no-custody recovery", "[live][cuda][startup]") {
 StartupOwner owner;
 struct Capture {
  mmltk::testsupport::TestGate release{"Live capture host release"};
  std::atomic_uint released{0};
 } capture;
 auto config = owner.Configuration();
 config.capture.device_path = "/dev/null";
 config.capture.device_api = {&capture, [](void*, int, unsigned long request, void*) noexcept {
  if (request == VIDIOC_STREAMON) {
   errno = EIO;
   return -1;
  }
  return 0;
 }, [](void* context, std::uint32_t slot) noexcept {
  auto& state = *static_cast<Capture*>(context);
  if (slot == 0) state.release.receipt().ArriveAndWait();
  ++state.released;
 }};
 LiveMediaDataPlane plane(config);
 auto startup = std::async(std::launch::async, [&] { return plane.start(); });
 const mmltk::testsupport::ScopedTestCleanup release([&] { capture.release.Release(); });
 REQUIRE(capture.release.WaitEntered(std::chrono::seconds{20}));
 CHECK_FALSE(plane.no_custody_start_settlement().ok());
 CHECK_FALSE(plane.admitted_run_terminal());
 CHECK(capture.released == 0);
 capture.release.Release();
 const auto start = mmltk::testsupport::await_test_future(startup, "Live capture startup", std::chrono::seconds{20});
 REQUIRE(start.valid());
 REQUIRE(start.has_custody());
 CHECK_FALSE(plane.no_custody_start_settlement().ok());
 CHECK_FALSE(plane.stop().ok());
 const auto terminal = plane.admitted_run_terminal();
 REQUIRE(terminal);
 CHECK(terminal->valid());
 CHECK(terminal->capture_terminal().kind == mmltk::backend::media::capture::CaptureStopKind::kStartupFailed);
 CHECK(capture.released == config.capture.v4l2_buffer_count);
 CHECK_FALSE(plane.no_custody_start_settlement().ok());
}
TEST_CASE("Live rejects placement belonging to a different capture receiver before allocating resources", "[live][numa]") {
 int owner = 0;
 LiveDataPlaneConfig config;
 config.resource_worker =
  mmltk::frameworks::gpu::ResourceOwnerWorkerCapability{&owner, 1, [](const void*, std::uintptr_t) noexcept { return true; }, [](const void*, std::uintptr_t) noexcept { return true; }};
 config.capture.cuda_device_index = 0;
 config.capture.execution = mmltk::frameworks::gpu::DeviceExecution{.device = 1, .placement = {.numa_node = 3, .cpus = {17}}};
 REQUIRE_THROWS_AS(LiveMediaDataPlane(config), std::invalid_argument);
}
struct LeaseProbe final {
 std::size_t completions = 0U;
 std::size_t abandonments = 0U;
 PhysicalFrameRevision observed{};
 static void Complete(void* context, const PhysicalFrameRevision revision) noexcept {
  auto& probe = *static_cast<LeaseProbe*>(context);
  ++probe.completions;
  probe.observed = revision;
 }
 static void Abandon(void* context, const PhysicalFrameRevision revision) noexcept {
  auto& probe = *static_cast<LeaseProbe*>(context);
  ++probe.abandonments;
  probe.observed = revision;
 }
};
constexpr PhysicalFrameRevision kRevision{
 .revision = 3U,
 .frame = {.session = 4U, .sequence = 5U},
 .slot = 1U,
 .ready_event = 6U,
};
constexpr LiveOutputFrame kView{
 .clean = 7U,
 .clean_pitch_bytes = 32U,
 .semantic = 8U,
 .semantic_pitch_bytes = 32U,
 .width = 8U,
 .height = 2U,
 .ready_event = 6U,
};
TEST_CASE("Live output leases admit only completed semantics for their exact physical frame") {
 LiveAnalysisFixture source;
 LiveAnalysisProduct product;
 source.Deliver(product, kRevision.frame);
 auto view = kView;
 view.analysis = &product;
 LeaseProbe probe;
 auto lease = LiveCompositeOutputLease::Create(&probe, &LeaseProbe::Complete, &LeaseProbe::Abandon, view, kRevision);
 REQUIRE(lease);
 CHECK(lease.view().analysis->frame() == lease.frame_revision().frame);
 CHECK(lease.view().analysis->regions()[0].keypoint_counts[0] == 2);
 std::move(lease).Complete();
 CHECK(probe.completions == 1);
 product.Reset({4, 6});
 CHECK_FALSE(LiveCompositeOutputLease::Create(&probe, &LeaseProbe::Complete, &LeaseProbe::Abandon, view, kRevision));
 CHECK(probe.abandonments == 0);
}
TEST_CASE("completed Live output lease releases its exact source revision once") {
 LeaseProbe probe;
 auto lease = LiveCompositeOutputLease::Create(&probe, &LeaseProbe::Complete, &LeaseProbe::Abandon, kView, kRevision);
 std::move(lease).Complete();
 CHECK(probe.completions == 1U);
 CHECK(probe.abandonments == 0U);
 CHECK(probe.observed.revision == kRevision.revision);
 CHECK_FALSE(lease);
}
TEST_CASE("abandoned Live output lease terminalizes its exact source revision once") {
 LeaseProbe probe;
 {
  auto lease = LiveCompositeOutputLease::Create(&probe, &LeaseProbe::Complete, &LeaseProbe::Abandon, kView, kRevision);
  REQUIRE(lease);
 }
 CHECK(probe.completions == 0U);
 CHECK(probe.abandonments == 1U);
 CHECK(probe.observed.slot == kRevision.slot);
}
TEST_CASE("moving over an active Live output lease abandons only the displaced slot") {
 LeaseProbe first;
 LeaseProbe second;
 auto displaced = LiveCompositeOutputLease::Create(&first, &LeaseProbe::Complete, &LeaseProbe::Abandon, kView, kRevision);
 auto replacement_view = kView;
 replacement_view.ready_event = 8U;
 auto replacement = LiveCompositeOutputLease::Create(&second, &LeaseProbe::Complete, &LeaseProbe::Abandon, replacement_view,
  PhysicalFrameRevision{
   .revision = 4U,
   .frame = {.session = 4U, .sequence = 6U},
   .slot = 2U,
   .ready_event = 8U,
  });
 REQUIRE(displaced);
 REQUIRE(replacement);
 displaced = std::move(replacement);
 std::move(displaced).Complete();
 CHECK(first.abandonments == 1U);
 CHECK(second.completions == 1U);
 CHECK(second.abandonments == 0U);
}
TEST_CASE("Live retirement claims every occupied state and preserves inert states", "[live][retirement]") {
 const auto initial = GENERATE(SlotState::Free, SlotState::Uploading, SlotState::Published, SlotState::Acquired, SlotState::Completing, SlotState::Terminal);
 std::atomic<std::uint32_t> state{slot_state_value(initial)};
 const bool occupied = initial != SlotState::Free && initial != SlotState::Terminal;
 CHECK(claim_live_slot_retirement(state) == occupied);
 CHECK(slot_state_is(state, occupied ? SlotState::Completing : initial));
}
TEST_CASE("Live single-attempt claim preserves a concurrently replaced state", "[live][retirement]") {
 const auto observed = GENERATE(SlotState::Uploading, SlotState::Published, SlotState::Acquired);
 const auto replacement = GENERATE(SlotState::Free, SlotState::Completing, SlotState::Terminal);
 std::atomic<std::uint32_t> state{slot_state_value(observed)};
 // Model the state changing between the retirement acquire-load and its
 // single compare-exchange; a losing claim must not retry on the new state.
 publish_live_slot_state(state, replacement);
 CHECK_FALSE(claim_live_slot(state, observed));
 CHECK(slot_state_is(state, replacement));
}
void check_live_product_scrub(std::atomic<std::uint32_t>& state, PhysicalFrameRevision& product) {
 bool exclusive = false;
 publish_live_owner_slot(state, SlotState::Free, [&] noexcept {
  exclusive = slot_state_is(state, SlotState::Completing);
  product = {};
 });
 CHECK(exclusive);
 CHECK_FALSE(product.valid());
 CHECK(slot_state_is(state, SlotState::Free));
}
TEST_CASE("Live source slot publishes Free only after product scrub") {
 // CLEANUP-IGNORE: Free-slot and terminal-slot publication independently start from the same acquired state and then verify distinct required transitions.
 std::atomic<std::uint32_t> state{slot_state_value(SlotState::Acquired)};
 PhysicalFrameRevision product = kRevision;
 REQUIRE(claim_live_slot(state, SlotState::Acquired));
 check_live_product_scrub(state, product);
}
TEST_CASE("Live fanout slot publishes Free only after completion scrub") {
 std::atomic<std::uint32_t> state{slot_state_value(SlotState::Published)};
 REQUIRE(claim_live_slot(state, SlotState::Published));
 CHECK_FALSE(transition_slot_state(state, SlotState::Published, SlotState::Acquired));
 PhysicalFrameRevision product = kRevision;
 check_live_product_scrub(state, product);
}
struct CallbackWakeProbe final {
 std::size_t wakes = 0U;
 static void Wake(void* context) noexcept { ++static_cast<CallbackWakeProbe*>(context)->wakes; }
};
TEST_CASE("Live output callback guard protects source work after slot release") {
 const auto released = GENERATE(SlotState::Free, SlotState::Terminal);
 CallbackWakeProbe wake;
 LiveOutputCallbackLifetime lifetime{1U, &wake, &CallbackWakeProbe::Wake};
 REQUIRE(lifetime.acquire());
 std::atomic<std::uint32_t> state{slot_state_value(SlotState::Acquired)};
 {
  LiveOutputCallbackGuard callback{lifetime};
  REQUIRE(claim_live_slot(state, SlotState::Acquired));
  publish_live_owner_slot(state, released, [] noexcept {});
  CHECK(slot_state_is(state, released));
  CHECK_FALSE(lifetime.idle());
 }
 CHECK(lifetime.idle());
 CHECK(wake.wakes == 1U);
}
TEST_CASE("Live incomplete fanout release publishes Terminal after scrub") {
 // CLEANUP-IGNORE: Free-slot and terminal-slot publication independently start from the same acquired state and then verify distinct required transitions.
 std::atomic<std::uint32_t> state{slot_state_value(SlotState::Acquired)};
 PhysicalFrameRevision product = kRevision;
 REQUIRE(claim_live_slot(state, SlotState::Acquired));
 publish_live_owner_slot(state, SlotState::Terminal, [&] noexcept { product = {}; });
 CHECK_FALSE(product.valid());
 CHECK(slot_state_is(state, SlotState::Terminal));
}
struct RevisionSnapshotProbe final {
 std::atomic<std::size_t> calls{0U};
 PhysicalFrameRevision revision{};
 static std::optional<PhysicalFrameRevision> Snapshot(void* context) noexcept {
  auto& probe = *static_cast<RevisionSnapshotProbe*>(context);
  probe.calls.fetch_add(1U, std::memory_order_acq_rel);
  return probe.revision.valid() ? std::optional<PhysicalFrameRevision>{probe.revision} : std::nullopt;
 }
};
struct GatedRevisionSnapshotProbe final {
 std::atomic<std::size_t> calls{0U};
 mmltk::testsupport::TestGate checked{"Live checked revision snapshot"};
 PhysicalFrameRevision revision{};
 static std::optional<PhysicalFrameRevision> Snapshot(void* context) noexcept {
  auto& probe = *static_cast<GatedRevisionSnapshotProbe*>(context);
  const std::size_t call = probe.calls.fetch_add(1U, std::memory_order_acq_rel);
  const PhysicalFrameRevision checked = probe.revision;
  if (call == 0U) { probe.checked.receipt().ArriveAndWait(); }
  return checked.valid() ? std::optional<PhysicalFrameRevision>{checked} : std::nullopt;
 }
};
class GatedRevisionWait final {
public:
 GatedRevisionWait() { revisions.Reset(); }
 ~GatedRevisionWait() {
  stop.request_stop();
  probe.checked.Release();
  if (result.valid()) result.wait();
 }
 void Start() {
  result = std::async(std::launch::async, [this] { return revisions.Wait(stop.get_token(), &probe, &GatedRevisionSnapshotProbe::Snapshot); });
  REQUIRE(probe.checked.WaitEntered(std::chrono::seconds{2}));
 }
 void SettleNotifier(std::promise<void>& started, std::future<void>& notifier) {
  mmltk::testsupport::await_test_promise(started, "Live notifier started");
  probe.checked.Release();
  mmltk::testsupport::await_test_future(notifier, "Live notification settlement");
 }
 LiveRevisionWait revisions;
 GatedRevisionSnapshotProbe probe;
 std::stop_source stop;
 std::future<std::optional<PhysicalFrameRevision>> result;
};
TEST_CASE("Live revision notification is retained after checked snapshot") {
 GatedRevisionWait wait;
 wait.Start();
 wait.probe.revision = kRevision;
 std::promise<void> notifier_started;
 auto notifier = std::async(std::launch::async, [&] {
  notifier_started.set_value();
  wait.revisions.RevisionReady();
 });
 // CLEANUP-IGNORE: The release guard must live beside its notifier future; the shared settlement method precedes opposite revision-success and terminal-failure assertions.
 const mmltk::testsupport::ScopedTestCleanup release_snapshot([&] { wait.probe.checked.Release(); });
 wait.SettleNotifier(notifier_started, notifier);
 const auto observed = mmltk::testsupport::await_test_future(wait.result, "Live revision settlement");
 REQUIRE(observed.has_value());
 CHECK(observed->revision == kRevision.revision);
 CHECK(wait.probe.calls.load(std::memory_order_acquire) == 2U);
}
TEST_CASE("Live admitted-run terminal notification is retained after checked snapshot") {
 GatedRevisionWait wait;
 wait.Start();
 struct TypedTerminalProbe final {
  LiveRevisionWait* revisions = nullptr;
  std::size_t notifications = 0U;
  static void Notify(void* context, const LivePhysicalTerminal&) noexcept {
   auto& probe = *static_cast<TypedTerminalProbe*>(context);
   ++probe.notifications;
   probe.revisions->Fail();
  }
 } terminal_probe{&wait.revisions};
 auto capture_terminal = std::make_shared<mmltk::backend::media::capture::CaptureStopTerminal>();
 capture_terminal->identity = {.session = 1U, .generation = 1U};
 LivePhysicalTerminal terminal{std::move(capture_terminal), {mmltk::backend::media::capture::StatusCode::kInternalError, "post-start failure"}};
 LiveAdmittedRunTerminalListener listener{&terminal_probe, &TypedTerminalProbe::Notify};
 std::promise<void> notifier_started;
 auto notifier = std::async(std::launch::async, [&] {
  notifier_started.set_value();
  listener(terminal);
 });
 // CLEANUP-IGNORE: The release guard must live beside its notifier future; the shared settlement method precedes opposite revision-success and terminal-failure assertions.
 const mmltk::testsupport::ScopedTestCleanup release_snapshot([&] { wait.probe.checked.Release(); });
 wait.SettleNotifier(notifier_started, notifier);
 CHECK_FALSE(mmltk::testsupport::await_test_future(wait.result, "Live revision settlement").has_value());
 CHECK(wait.revisions.failed());
 CHECK(terminal_probe.notifications == 1U);
 CHECK(wait.probe.calls.load(std::memory_order_acquire) == 1U);
}
TEST_CASE("Live no-custody start failure is synchronous and has no admitted terminal") {
 const mmltk::backend::media::capture::CaptureSessionStartResult rejected{
  mmltk::backend::media::capture::CaptureSessionStartPhase::NoCustody, {mmltk::backend::media::capture::StatusCode::kNoDevice, "capture unavailable"}, {}
 };
 CHECK_FALSE(rejected.running());
 CHECK_FALSE(rejected.has_custody());
}
TEST_CASE("Live revision wait returns one checked snapshot without refetch") {
 LiveRevisionWait revisions;
 revisions.Reset();
 RevisionSnapshotProbe probe;
 probe.revision = kRevision;
 const auto observed = revisions.Wait(std::stop_token{}, &probe, &RevisionSnapshotProbe::Snapshot);
 REQUIRE(observed.has_value());
 CHECK(observed->revision == kRevision.revision);
 CHECK(probe.calls.load(std::memory_order_acquire) == 1U);
}
TEST_CASE("Live terminal race refuses one observed output without borrowing") {
 struct AcquireProbe final {
  std::size_t calls = 0U;
  static bool Acquire(void* context, PhysicalFrameRevision, LiveCompositeOutputLease*) {
   ++static_cast<AcquireProbe*>(context)->calls;
   return false;
  }
 } probe;
 LiveCompositeOutputLease lease;
 CHECK_FALSE(try_acquire_live_output(&probe, kRevision, &AcquireProbe::Acquire, &lease));
 CHECK(probe.calls == 1U);
 CHECK_FALSE(lease);
}
TEST_CASE("rejected Live release leaves its local handle inert") {
 std::uintptr_t handle = 41U;
 std::size_t attempts = 0U;
 const auto reject = [&attempts](std::uintptr_t) noexcept {
  ++attempts;
  return cudaErrorNotPermitted;
 };
 CHECK(retire_live_local_handle(handle, std::uintptr_t{0U}, reject) == cudaErrorNotPermitted);
 CHECK(handle == 0U);
 CHECK(attempts == 1U);
 CHECK(retire_live_local_handle(handle, std::uintptr_t{0U}, reject) == cudaSuccess);
 CHECK(attempts == 1U);
}
}  // namespace
}  // namespace mmltk::backend::media::live
