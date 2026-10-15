#include "src/frameworks/gpu/runtime/system_image_runtime.h"
#include "src/controller/presentation/tests/support/visual_runtime_fixture.h"
#include "src/controller/subsystems/live/tests/support/live_system_fixture.h"
#include "src/controller/presentation/tests/support/visual_system_fixture.h"
#include "src/controller/subsystems/live/live_system.h"
#include "src/controller/subsystems/live/live_receiver_copy.h"
#include "src/backend/media/live/tests/support/live_analysis_fixture.h"
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/frameworks/gpu/image/image_buffer.h"
#include <atomic>
#include <condition_variable>
#include "src/controller/subsystems/live/live_analysis_configuration.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/controller/services/settings/settings_system.h"
#include "src/controller/services/settings/settings_store.h"
#include "src/controller/contracts/default_state.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <mutex>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cuda_runtime_api.h>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <new>
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <variant>
// CLEANUP-IGNORE: Namespace and using declarations provide test-local names; the fixture algorithms already have shared owners.
namespace mmltk::controller {
namespace {
using namespace visual_test_support;
namespace media = mmltk::backend::media::live;
using media::test_support::LiveAnalysisFixture;
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
struct LiveReceiverCopyProbe final {
 static inline cudaError_t wait_status = cudaSuccess;
 static inline cudaError_t copy_status = cudaSuccess;
 static inline cudaError_t synchronize_status = cudaSuccess;
 static inline std::size_t waits = 0U;
 static inline std::size_t copies = 0U, fail_copy = 1U;
 static inline std::size_t synchronizations = 0U;
 static inline std::size_t completions = 0U, abandonments = 0U;
 static inline media::LiveAnalysisProduct* source_product = nullptr;
 static void Complete(void*,media::PhysicalFrameRevision) noexcept {
  ++completions;
  if (source_product) source_product->Reset();
 }
 static void Abandon(void*,media::PhysicalFrameRevision) noexcept { ++abandonments; }
 static media::LiveCompositeOutputLease Lease(media::LiveAnalysisProduct* product = nullptr) {
  static int owner = 0;
  return media::LiveCompositeOutputLease::Create(&owner,&Complete,&Abandon,
   {.clean=2,.clean_pitch_bytes=16,.semantic=5,.semantic_pitch_bytes=16,.width=4,.height=4,.ready_event=3,.analysis=product},
   {.revision=1,.frame={1,1},.slot=0,.ready_event=3});
 }
 static void Reset() noexcept {
  wait_status = cudaSuccess;
  copy_status = cudaSuccess;
  synchronize_status = cudaSuccess;
  waits = 0U;
  copies = 0U; fail_copy = 1U;
  synchronizations = 0U;
  completions = abandonments = 0U;
  source_product = nullptr;
 }
 static cudaError_t Wait(std::uintptr_t, std::uintptr_t) noexcept {
  ++waits;
  return wait_status;
 }
 static cudaError_t Copy(mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t, std::size_t, std::uintptr_t) noexcept {
  ++copies;
  return copies == fail_copy ? copy_status : cudaSuccess;
 }
 static cudaError_t Synchronize(std::uintptr_t) noexcept {
  ++synchronizations;
  return synchronize_status;
 }
};
constexpr detail::LiveReceiverCopyOperations kLiveReceiverCopyProbe{
 .wait = &LiveReceiverCopyProbe::Wait,
 .copy = &LiveReceiverCopyProbe::Copy,
 .synchronize = &LiveReceiverCopyProbe::Synchronize,
};
[[nodiscard]] constexpr mmltk::frameworks::gpu::ImagePlaneView live_receiver_copy_target(bool semantic = false) noexcept {
 return {
  .data = semantic ? 6U : 1U,
  .descriptor = {
   .kind = semantic ? mmltk::frameworks::gpu::ImagePlaneKind::Semantic : mmltk::frameworks::gpu::ImagePlaneKind::Clean,
   .format = mmltk::frameworks::gpu::ImageFormat::Rgba8,
   .width = 4U,
   .height = 4U,
   .pitch_bytes = 16U,
  },
 };
}
TEST_CASE("Live receiver completes image and semantic copies before releasing the media lease") {
 LiveReceiverCopyProbe::Reset();
 LiveAnalysisFixture fixture;
 media::LiveAnalysisProduct source,destination;
 fixture.Deliver(source,{1,1});
 LiveReceiverCopyProbe::source_product = &source;
 auto lease = LiveReceiverCopyProbe::Lease(&source);
 CHECK(detail::copy_live_receiver_frame(live_receiver_copy_target(),live_receiver_copy_target(true),lease,destination,4,kLiveReceiverCopyProbe) == cudaSuccess);
 CHECK(LiveReceiverCopyProbe::waits == 1);
 CHECK(LiveReceiverCopyProbe::copies == 2);
 CHECK(LiveReceiverCopyProbe::synchronizations == 1);
 CHECK(LiveReceiverCopyProbe::completions == 1);
 CHECK_FALSE(lease);
 CHECK_FALSE(source.ready());
 REQUIRE(destination.ready());
 CHECK(destination.frame() == media::LiveFrameId{1,1});
 REQUIRE(destination.regions().size() == 1);
 CHECK(destination.regions()[0].keypoints[0].covariance_xy == -.5F);
 CHECK(destination.regions()[0].class_catalog == fixture.catalog);
 LiveReceiverCopyProbe::source_product = nullptr;
}
TEST_CASE("Live receiver failures settle prior reads or retain terminal media custody") {
 const int failure = GENERATE(0,1,2,3);
 LiveReceiverCopyProbe::Reset();
 if (failure == 0) LiveReceiverCopyProbe::wait_status = cudaErrorLaunchFailure;
 if (failure == 1) LiveReceiverCopyProbe::copy_status = cudaErrorLaunchFailure;
 if (failure == 2) { LiveReceiverCopyProbe::copy_status = cudaErrorLaunchFailure; LiveReceiverCopyProbe::fail_copy = 2; }
 if (failure == 3) LiveReceiverCopyProbe::synchronize_status = cudaErrorLaunchFailure;
 media::LiveAnalysisProduct destination;
 destination.Reset({8,9});
 {
  auto lease = LiveReceiverCopyProbe::Lease();
  CHECK(detail::copy_live_receiver_frame(live_receiver_copy_target(),live_receiver_copy_target(true),lease,destination,4,kLiveReceiverCopyProbe) == cudaErrorLaunchFailure);
  CHECK(LiveReceiverCopyProbe::copies == (failure == 0 ? 0 : failure == 1 ? 1 : 2));
  CHECK(LiveReceiverCopyProbe::synchronizations == 1);
  CHECK(LiveReceiverCopyProbe::completions == (failure == 3 ? 0 : 1));
  CHECK(static_cast<bool>(lease) == (failure == 3));
  CHECK(destination.frame() == media::LiveFrameId{8,9});
 }
 CHECK(LiveReceiverCopyProbe::abandonments == (failure == 3 ? 1 : 0));
}
TEST_CASE("Live receiver semantic allocation failure releases only after settled pixels") {
 LiveReceiverCopyProbe::Reset();
 LiveAnalysisFixture fixture;
 media::LiveAnalysisProduct source;
 fixture.Deliver(source,{1,1});
 media::LiveAnalysisProduct destination{media::LiveAnalysisProduct::Storage::Host,
  [](std::size_t,media::LiveAnalysisProduct::Storage)->std::shared_ptr<void> { throw std::bad_alloc{}; }};
 destination.Reset({8,9});
 auto lease = LiveReceiverCopyProbe::Lease(&source);
 CHECK(detail::copy_live_receiver_frame(live_receiver_copy_target(),live_receiver_copy_target(true),lease,destination,4,kLiveReceiverCopyProbe) == cudaErrorMemoryAllocation);
 CHECK(LiveReceiverCopyProbe::synchronizations == 1);
 CHECK(LiveReceiverCopyProbe::completions == 1);
 CHECK(destination.frame() == media::LiveFrameId{8,9});
 CHECK_FALSE(lease);
}
struct ControlledLiveCapture final {
 EventGate changed;
 std::mutex mutex;
 std::function<void()> wake;
 std::atomic_bool available{false};
 std::atomic_size_t acquisitions{0U}, captures{0U};
 bool result = true;
 std::function<void()> after_capture;
 std::function<void(std::shared_ptr<const LiveAnalysisConfiguration>, std::stop_token)> on_start;
 std::atomic_size_t stops{0};
 mmltk::backend::media::capture::Status stop_status = mmltk::backend::media::capture::Status::Ok();
 std::function<void(media::LiveAnalysisProduct&,std::uint64_t)> fill_analysis;
 void Offer() {
  std::function<void()> notify;
  {
   std::scoped_lock lock(mutex);
   available.store(true, std::memory_order_release);
   notify = wake;
  }
  if (notify) notify();
 }
};
class ControlledLiveAlgorithm final : public LiveAlgorithm {
public:
 explicit ControlledLiveAlgorithm(std::shared_ptr<ControlledLiveCapture> state) : state_(std::move(state)) {}
 void Start(const LiveStart&, std::shared_ptr<const LiveAnalysisConfiguration> configuration, std::stop_token stop) override { if (state_->on_start) state_->on_start(std::move(configuration), stop); }
 void SetOutputAvailableSink(std::function<void()> wake) override {
  std::scoped_lock lock(state_->mutex);
  state_->wake = std::move(wake);
 }
 bool AcquireOutput() override {
  ++state_->acquisitions;
  const bool ready = state_->available.exchange(false, std::memory_order_acq_rel);
  state_->changed.Advance();
  return ready;
 }
 bool Capture(mmltk::frameworks::gpu::ImagePlaneView target, mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t, mmltk::backend::media::live::LiveAnalysisProduct& analysis, std::stop_token) override {
  const auto value = ++state_->captures;
  Fill(semantic, static_cast<std::uint8_t>(value + 100U));
  analysis.Reset({1U,value});
  for (std::uint32_t y = 0U; y != target.descriptor.height; ++y)
   for (std::uint32_t x = 0U; x != target.descriptor.row_bytes(); ++x)
    *(reinterpret_cast<std::uint8_t*>(target.data) + y * target.descriptor.pitch_bytes + x) = static_cast<std::uint8_t>(value * 31U + y * 7U + x);
  std::function<void()> after;
  std::function<void(media::LiveAnalysisProduct&,std::uint64_t)> fill;
  bool result = false;
  {
   std::scoped_lock lock(state_->mutex);
   after = state_->after_capture;
   fill = state_->fill_analysis;
   result = state_->result;
  }
  if (fill) fill(analysis,value);
  state_->changed.Advance();
  if (after) after();
  return result;
 }
 mmltk::backend::media::capture::Status Stop() noexcept override { ++state_->stops; return state_->stop_status; }

private:
 std::shared_ptr<ControlledLiveCapture> state_;
};
TEST_CASE("Live captures immutable prepared Predict settings and cancels partial starts without replacing retained products") {
 class PreparedModel final : public ModelRuntime {
 public:
  ModelArtifactAdmission Acquire(const contracts::ModelSelectionKey&, const std::filesystem::path& artifact, int, std::stop_token, const std::function<void(const contracts::ModelProgress&)>&) override { return {.artifact = artifact.string()}; }
 };
 mmltk::testsupport::ScopedTempDir root{"live-admitted-settings"};
 auto draft = contracts::default_gui_settings_state();
 draft.workflows.predict.model_source = contracts::ModelSelectionSource::Custom;
 draft.workflows.predict.model_input = contracts::ModelArtifactInputKind::Onnx;
 draft.workflows.predict.request.onnx_path = "/tmp/admitted-live.onnx";
 draft.workflows.predict.request.resolution = 64;
 draft.workflows.predict.request.threshold = .25F;
 const services::SettingsLocation location{(root.path() / "settings.json").string()};
 REQUIRE(services::SettingsStore::save(location.value(), draft, 1).succeeded());
 SettingsSystem settings;
 REQUIRE(settings.Load(location).applied());
 EventGate events;
 ModelSystem model{settings, [] { return std::make_unique<PreparedModel>(); }, [&](ModelSystem::event_type) { events.Advance(); }};
 const auto prepare = [&] { static_cast<void>(model.Select({.workflow = contracts::FeatureId::Predict})); REQUIRE(events.Wait([&] { return !model.snapshot().active; })); REQUIRE(model.snapshot().terminal.outcome == contracts::ModelSelectionOutcome::Accepted); };
 prepare();
 auto backend = std::make_shared<FakeImageBackend>();
 auto state = std::make_shared<ControlledLiveCapture>();
 std::mutex admitted_mutex;
 std::shared_ptr<const LiveAnalysisConfiguration> admitted;
 std::stop_token admitted_stop;
 std::atomic_bool entered{false}, fail_start{false}, failed{false};
 state->on_start = [&](std::shared_ptr<const LiveAnalysisConfiguration> configuration, std::stop_token stop) {
  { std::scoped_lock lock(admitted_mutex); admitted = std::move(configuration); admitted_stop = stop; }
  entered = true; events.Advance();
  if (fail_start) throw LiveProviderAdmissionFailure("requested settled provider admission failure");
 };
 LiveSystem live{kDevice, RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic, [state] { return std::make_unique<ControlledLiveAlgorithm>(state); }, 2),
  [&](LiveSystem::event_type event) { if (std::holds_alternative<LiveFailed>(event)) failed = true; events.Advance(); }, {}, &settings, &model};
 const auto start = [&] { entered = false; state->Offer(); static_cast<void>(live.Start({.extent={7,3}, .frames_per_second=240})); REQUIRE(events.Wait([&] { return entered.load(); })); };
 const auto stop = [&] { static_cast<void>(live.Stop()); REQUIRE(events.Wait([&] { return !live.snapshot().running && !live.snapshot().cancellation_requested; })); };
 const auto edit = [&](std::string path, auto value) { contracts::SettingsUpdateRequest request; request.updates.push_back({.path=std::move(path), .value=mmltk::frameworks::serialization::wire::FlatValue{value}}); static_cast<void>(settings.Update(std::move(request))); };
 start(); REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == 1; }));
 { std::scoped_lock lock(admitted_mutex); CHECK_FALSE(admitted); }
 stop();
 edit("workflows.predict.live_analysis_enabled", true);
 start(); REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == 2; }));
 const auto retained = live.snapshot();
 auto read = live.BorrowDocument(retained.frame);
 REQUIRE(read.valid());
 REQUIRE(read.image_metadata);
 CHECK(std::holds_alternative<mmltk::frameworks::serialization::wire::Value::Array>(read.image_metadata->storage));
 CHECK(read.document->facts() == retained.document);
 edit("workflows.predict.request.threshold", 1.0);
 { std::scoped_lock lock(admitted_mutex); REQUIRE(admitted); CHECK(admitted->request.threshold == .25F); CHECK_FALSE(admitted_stop.stop_requested()); }
 stop();
 { std::scoped_lock lock(admitted_mutex); CHECK(admitted_stop.stop_requested()); }
 fail_start = true;
 const auto stops = state->stops.load();
 start(); REQUIRE(events.Wait([&] { return failed.load(); }));
 CHECK(state->stops > stops);
 CHECK(live.snapshot().frame == retained.frame);
 CHECK(live.snapshot().document == retained.document);
 { std::scoped_lock lock(admitted_mutex); CHECK(admitted->request.threshold == 1.F); }
 CHECK(read.valid());
 read = {};
 fail_start = false;
 state->on_start = [&](std::shared_ptr<const LiveAnalysisConfiguration>, std::stop_token stop) {
  entered = true; events.Advance();
  std::mutex mutex;
  std::condition_variable_any ready;
  std::unique_lock lock(mutex);
  ready.wait(lock, stop, [] { return false; });
 };
 const auto completed = live.snapshot().completed_frames;
 start(); stop();
 CHECK(live.snapshot().completed_frames == completed);
 CHECK(live.snapshot().frame == retained.frame);
 CHECK(live.snapshot().document == retained.document);
 const auto before_refusal = live.snapshot();
 entered = false;
 edit("workflows.predict.request.onnx_path", std::string{"/tmp/unprepared.onnx"});
 CHECK_THROWS_AS(live.Start({.extent={7,3}, .frames_per_second=240}), contracts::InvalidIntentError);
 CHECK_FALSE(entered.load());
 CHECK(live.snapshot().revision == before_refusal.revision);
 CHECK(live.snapshot().frame == before_refusal.frame);
 live.Shutdown();
}
TEST_CASE("Live complete frames alternate native output slots without preparation copies") {
 auto backend = std::make_shared<FakeImageBackend>();
 backend->pitch_padding_bytes = 17U;
 auto state = std::make_shared<ControlledLiveCapture>();
 EventGate events;
 LiveSystem live{
  kDevice, RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic, [state] { return std::make_unique<ControlledLiveAlgorithm>(state); }, 2U),
  [&](LiveSystem::event_type) { events.Advance(); }
 };
 const auto stop_and_wait = [&] {
  static_cast<void>(live.Stop());
  REQUIRE(events.Wait([&] { return !live.snapshot().running; }));
 };
 const auto check = [](const auto& read, const std::size_t value) {
  REQUIRE(read.valid());
  const auto plane = read.plane(0U).plane();
  for (std::uint32_t y = 0U; y != plane.descriptor.height; ++y)
   for (std::uint32_t x = 0U; x != plane.descriptor.row_bytes(); ++x)
    CHECK(*(reinterpret_cast<const std::uint8_t*>(plane.data) + y * plane.descriptor.pitch_bytes + x) == static_cast<std::uint8_t>(value * 31U + y * 7U + x));
 };
 static_cast<void>(live.Start({.extent = {7U, 3U}, .frames_per_second = 240U}));
 REQUIRE(state->changed.Wait([&] { return state->acquisitions.load() != 0U; }));
 CHECK(state->captures == 0U);
 CHECK(live.snapshot().completed_frames == 0U);
 state->Offer();
 REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == 1U; }));
 auto first = live.BorrowFrame();
 check(first, 1U);
 const auto first_pointer = first.plane(0U).plane().data;
 state->Offer();
 REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == 2U; }));
 auto second = live.BorrowFrame();
 check(first, 1U);
 check(second, 2U);
 CHECK(second.plane(0U).plane().data != first_pointer);
 state->Offer();
 CHECK(live.snapshot().completed_frames == 2U);
 first = {};
 REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == 3U; }));
 check(second, 2U);
 auto third = live.BorrowFrame();
 check(third, 3U);
 CHECK(third.plane(0U).plane().data == first_pointer);
 second = {};
 third = {};
 stop_and_wait();
 state->Offer();
 static_cast<void>(live.Start({.extent = {3U, 5U}, .frames_per_second = 240U}));
 REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == 4U; }));
 auto resized = live.BorrowFrame();
 check(resized, 4U);
 CHECK(resized.plane(0U).plane().descriptor.width == 3U);
 CHECK(resized.plane(0U).plane().descriptor.height == 5U);
 resized = {};
 stop_and_wait();
 CHECK(backend->same_copies == 0U);
 CHECK(backend->plane_clears == 0U);
}
TEST_CASE("Live rejected or stopped complete captures never report frame progress") {
 const bool stop_after_capture = GENERATE(false, true);
 auto backend = std::make_shared<FakeImageBackend>();
 auto state = std::make_shared<ControlledLiveCapture>();
 state->result = stop_after_capture;
 EventGate events;
 LiveSystem live{
  kDevice, RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic, [state] { return std::make_unique<ControlledLiveAlgorithm>(state); }, 2U),
  [&](LiveSystem::event_type) { events.Advance(); }
 };
 if (stop_after_capture) state->after_capture = [&] { static_cast<void>(live.Stop()); };
 state->Offer();
 static_cast<void>(live.Start({.extent = {7U, 3U}, .frames_per_second = 240U}));
 if (stop_after_capture)
  REQUIRE(events.Wait([&] { return !live.snapshot().running; }));
 else {
  REQUIRE(state->changed.Wait([&] { return state->acquisitions.load() >= 2U; }));
  static_cast<void>(live.Stop());
  REQUIRE(events.Wait([&] { return !live.snapshot().running; }));
 }
 CHECK(state->captures == 1U);
 CHECK(live.snapshot().completed_frames == 0U);
 CHECK_FALSE(live.snapshot().frame.valid());
 CHECK_FALSE(live.BorrowFrame().valid());
}
TEST_CASE("Live receiver publication failure does not commit a captured candidate") {
 const auto point = GENERATE(FakeImageBackend::FailurePoint::RecordEvent, FakeImageBackend::FailurePoint::SynchronizeStream);
 auto backend = std::make_shared<FakeImageBackend>();
 auto state = std::make_shared<ControlledLiveCapture>();
 state->after_capture = [backend, point] { backend->FailAfter(point); };
 EventGate events;
 std::atomic_bool failed{false};
 LiveSystem live{
  kDevice, RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic, [state] { return std::make_unique<ControlledLiveAlgorithm>(state); }, 2U), [&](LiveSystem::event_type event) {
  if (std::holds_alternative<LiveFailed>(event)) failed = true;
  events.Advance();
 }
 };
 state->Offer();
 static_cast<void>(live.Start({.extent = {7U, 3U}, .frames_per_second = 240U}));
 REQUIRE(events.Wait([&] { return failed.load(); }));
 CHECK(state->captures == 1U);
 CHECK(live.snapshot().completed_frames == 0U);
 CHECK_FALSE(live.BorrowFrame().valid());
}
TEST_CASE("Live exact-frame analysis reads retain image and schema across reuse Stop and extent changes") {
 auto backend = std::make_shared<FakeImageBackend>();
 auto state = std::make_shared<ControlledLiveCapture>();
 auto fixture = std::make_shared<LiveAnalysisFixture>();
 const auto catalog = fixture->catalog;
 state->fill_analysis = [fixture](media::LiveAnalysisProduct& product,std::uint64_t sequence) {
  if (sequence == 4) return; // Capture's Reset supplies an explicit no-analysis frame.
  *fixture->count = sequence == 2 ? 0 : 2;
  fixture->joints[0].x = static_cast<float>(sequence);
  fixture->Deliver(product,{1,sequence});
 };
 EventGate events;
 LiveSystem live{kDevice,RuntimeFactory(0,backend,mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
  [state] { return std::make_unique<ControlledLiveAlgorithm>(state); },2),[&](LiveSystem::event_type) { events.Advance(); }};
 const auto await_frame = [&](std::uint64_t count) { REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == count; })); };
 const auto stop = [&] { static_cast<void>(live.Stop()); REQUIRE(events.Wait([&] { return !live.snapshot().running; })); };
 state->Offer();
 static_cast<void>(live.Start({.extent={7,3},.frames_per_second=240}));
 await_frame(1);
 const auto first_frame = live.snapshot().frame;
 auto first = live.ReadAnalysis(first_frame);
 REQUIRE(first.valid());
 CHECK(first.frame() == first_frame);
 CHECK(first.analysis().frame() == media::LiveFrameId{1,1});
 CHECK(first.analysis().regions()[0].class_catalog == catalog);
 CHECK(first.analysis().regions()[0].keypoints[0].covariance_xy == -.5F);
 state->Offer();
 await_frame(2);
 auto second = live.ReadAnalysis(live.snapshot().frame);
 REQUIRE(second.valid());
 CHECK(second.analysis().analyzed());
 CHECK(second.analysis().regions()[0].count == 0);
 CHECK(first.analysis().regions()[0].count == 2);
 CHECK(first.analysis().regions()[0].keypoints[0].x == 1.F);
 CHECK_FALSE(live.ReadAnalysis(first_frame).valid());
 const auto image = first.image().plane(0).plane();
 CHECK(*reinterpret_cast<const std::uint8_t*>(image.data) == 31);
 state->Offer();
 CHECK(live.snapshot().completed_frames == 2);
 first = {};
 await_frame(3);
 CHECK(second.analysis().frame() == media::LiveFrameId{1,2});
 CHECK(second.analysis().regions()[0].count == 0);
 second = {};
 auto third = live.ReadAnalysis(live.snapshot().frame);
 stop();
 CHECK(third.valid());
 CHECK(third.analysis().regions()[0].keypoints[0].x == 3.F);
 CHECK(live.ReadAnalysis(third.frame()).valid());
 state->Offer();
 static_cast<void>(live.Start({.extent={3,5},.frames_per_second=240}));
 await_frame(4);
 auto resized = live.ReadAnalysis(live.snapshot().frame);
 REQUIRE(resized.valid());
 CHECK(resized.frame().extent == VisualExtent{3,5});
 CHECK_FALSE(resized.analysis().analyzed());
 CHECK(resized.analysis().regions().empty());
 CHECK(third.analysis().regions()[0].class_catalog == catalog);
 CHECK(third.analysis().regions()[0].keypoints[0].x == 3.F);
 stop();
 resized = {};
 third = {};
 live.Shutdown();
}
TEST_CASE("Live failed and cancelled replacements preserve a retained completed semantic read") {
 const int outcome = GENERATE(0,1,2);
 auto backend = std::make_shared<FakeImageBackend>();
 auto state = std::make_shared<ControlledLiveCapture>();
 auto fixture = std::make_shared<LiveAnalysisFixture>();
 state->fill_analysis = [fixture](media::LiveAnalysisProduct& product,std::uint64_t sequence) { fixture->Deliver(product,{1,sequence}); };
 EventGate events;
 std::atomic_bool failed{false};
 LiveSystem live{kDevice,RuntimeFactory(0,backend,mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
  [state] { return std::make_unique<ControlledLiveAlgorithm>(state); },2),[&](LiveSystem::event_type event) {
   if (std::holds_alternative<LiveFailed>(event)) failed = true;
   events.Advance();
  }};
 state->Offer();
 static_cast<void>(live.Start({.extent={7,3},.frames_per_second=240}));
 REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == 1; }));
 auto retained = live.ReadAnalysis(live.snapshot().frame);
 REQUIRE(retained.valid());
 {
  std::scoped_lock lock(state->mutex);
  state->result = outcome != 0;
  state->after_capture = [&] {
   if (outcome == 1) static_cast<void>(live.Stop());
   if (outcome == 2) throw std::runtime_error("failed Live replacement");
  };
 }
 state->Offer();
 if (outcome == 0) {
  REQUIRE(state->changed.Wait([&] { return state->captures.load() == 2 && state->acquisitions.load() >= 3; }));
  static_cast<void>(live.Stop());
 }
 REQUIRE(events.Wait([&] { return !live.snapshot().running; }));
 // Failure has published its terminal state while this exact image read still
 // holds deferred retirement; its independently owned semantics remain usable.
 CHECK(retained.analysis().frame() == media::LiveFrameId{1,1});
 CHECK(retained.analysis().regions()[0].keypoint_counts[0] == 2);
 CHECK(retained.analysis().regions()[0].keypoints[0].covariance_xy == -.5F);
 CHECK(retained.image().valid());
 retained = {};
 live.Shutdown();
 CHECK(live.snapshot().completed_frames == 1);
 CHECK(failed.load() == (outcome == 2));
}
TEST_CASE("Live metadata capacity refuses only the replacement and stops with usable paired reads") {
 const bool cleanup_failure = GENERATE(false, true);
 auto backend = std::make_shared<FakeImageBackend>();
 auto state = std::make_shared<ControlledLiveCapture>();
 auto fixture = std::make_shared<LiveAnalysisFixture>();
 state->fill_analysis = [fixture](media::LiveAnalysisProduct& product, std::uint64_t sequence) {
  if (sequence == 2) LiveAnalysisFixture::DeliverLargeSchema(product,{1,sequence},128);
  else fixture->Deliver(product,{1,sequence});
 };
 EventGate events;
 std::atomic_bool failed{false};
 LiveSystem live{kDevice,RuntimeFactory(0,backend,mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
  [state] { return std::make_unique<ControlledLiveAlgorithm>(state); },2),[&](LiveSystem::event_type event) {
   if (const auto* failure = std::get_if<LiveFailed>(&event)) {
    CHECK(failure->detail.find(cleanup_failure ? "cleanup failed" : "aggregate publication capacity") != std::string::npos);
    failed = true;
   }
   events.Advance();
  }};
 state->Offer();
 static_cast<void>(live.Start({.extent={4,4},.frames_per_second=240}));
 REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == 1; }));
 const auto first = live.snapshot();
 auto held = live.BorrowDocument(first.frame);
 REQUIRE(held.valid());
 const auto check = [](const auto& read) {
  REQUIRE(read.valid());
  CHECK(*reinterpret_cast<const std::uint8_t*>(read.pixels.plane(0).plane().data) == 31);
  CHECK(*reinterpret_cast<const std::uint8_t*>(read.pixels.plane(1).plane().data) == 101);
 };
 check(held);
 if (cleanup_failure) state->stop_status = {mmltk::backend::media::capture::StatusCode::kCudaError,"injected physical cleanup failure"};
 state->Offer();
 REQUIRE(events.Wait([&] { return failed.load(); }));
 CHECK_FALSE(live.snapshot().running);
 CHECK(live.snapshot().completed_frames == 1);
 check(held);
 if (!cleanup_failure) {
  CHECK(live.snapshot().frame == first.frame);
  CHECK(live.snapshot().document == first.document);
  auto fresh = live.BorrowDocument(first.frame);
  check(fresh);
  CHECK(fresh.document->facts() == held.document->facts());
  CHECK(live.ImageSnapshot(first.frame).has_value());
  fresh = {};
  state->Offer();
  static_cast<void>(live.Start({.extent={4,4},.frames_per_second=240}));
  REQUIRE(events.Wait([&] { return live.snapshot().completed_frames == 2; }));
  CHECK(live.snapshot().frame != first.frame);
  check(held);
  static_cast<void>(live.Stop());
  REQUIRE(events.Wait([&] { return !live.snapshot().running; }));
 } else {
  CHECK_FALSE(live.BorrowDocument(first.frame).valid());
  CHECK_FALSE(live.ImageSnapshot(first.frame));
 }
 held = {};
 live.Shutdown();
}
class FailingLiveAlgorithm final : public LiveAlgorithm {
public:
 void Start(const LiveStart&, std::shared_ptr<const LiveAnalysisConfiguration>, std::stop_token) override {}
 void SetOutputAvailableSink(std::function<void()>) override {}
 bool AcquireOutput() override { return true; }
 bool Capture(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t, mmltk::backend::media::live::LiveAnalysisProduct&, std::stop_token) override { throw std::runtime_error("deterministic Live capture failure"); }
 mmltk::backend::media::capture::Status Stop() noexcept override { return mmltk::backend::media::capture::Status::Ok(); }
};
TEST_CASE("Live queued discrete cancellation settles without running obsolete work") {
 // CLEANUP-IGNORE: Independent visual scenarios use the same backend, counter, and promise primitives; ordering and cancellation stay with their different runtime owners.
 auto backend = std::make_shared<FakeImageBackend>();
 // CLEANUP-IGNORE: These independent ordering and cancellation tests use separate counters and promise receipts; only fixture construction is repeated.
 auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
 std::promise<void> latest_entered;
 std::promise<void> release_latest;
 auto release = release_latest.get_future().share();
 std::promise<void> queued_cancelled;
 std::promise<void> active_cancelled;
 std::atomic_bool queued_ran = false;
 std::atomic_bool owner_failed = false;
 detail::VisualRuntimeOwner owner{test_live_runtime_factory(backend, captures), [&owner_failed](std::exception_ptr) { owner_failed.store(true, std::memory_order_release); }};
 auto settle_owner = settle_visual_on_exit(owner, release_latest);
 owner.SubmitLatest([&latest_entered, &active_cancelled, release](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token stop) mutable {
  std::stop_callback observe_stop{stop, [&active_cancelled] { active_cancelled.set_value(); }};
  latest_entered.set_value();
  release.wait();
  return detail::VisualRuntimeOwner::Notification{};
 });
 mmltk::testsupport::await_test_promise(latest_entered, "latest_entered");
 REQUIRE(owner.SubmitDiscrete([&queued_ran](mmltk::frameworks::gpu::SystemImageRuntime&, std::stop_token) {
  queued_ran.store(true, std::memory_order_release);
  return detail::VisualRuntimeOwner::Notification{};
 }, [&queued_cancelled] { queued_cancelled.set_value(); }));
 owner.RequestActiveStop();
 REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(active_cancelled, "active_cancelled", 2s));
 REQUIRE_NOTHROW(mmltk::testsupport::await_test_promise(queued_cancelled, "queued_cancelled", 2s));
 CHECK_FALSE(queued_ran.load(std::memory_order_acquire));
 release_latest.set_value();
 owner.StopAndWait();
 CHECK_FALSE(owner_failed.load(std::memory_order_acquire));
}
TEST_CASE("Live failure publishes its newer settled snapshot") {
 auto backend = std::make_shared<FakeImageBackend>();
 std::promise<LiveFailed> failure;
 LiveSystem live{
  kDevice, RuntimeFactory(0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic, [] { return std::make_unique<FailingLiveAlgorithm>(); }), [&failure](LiveSystem::event_type event) {
  if (auto* failed = std::get_if<LiveFailed>(&event)) failure.set_value(std::move(*failed));
 }
 };
 const auto admitted = live.Start({.extent = {80U, 45U}, .frames_per_second = 120U});
 const auto failed = failure.get_future().get();
 CHECK_FALSE(failed.snapshot.running);
 CHECK_FALSE(failed.snapshot.cancellation_requested);
 CHECK(failed.snapshot.revision > admitted.revision);
 CHECK(failed.snapshot.revision == live.snapshot().revision);
 CHECK(failed.snapshot.completed_frames == live.snapshot().completed_frames);
 CHECK_FALSE(failed.snapshot.frame.valid());
 CHECK_FALSE(live.BorrowFrame().valid());
}
}  // namespace
}  // namespace mmltk::controller
