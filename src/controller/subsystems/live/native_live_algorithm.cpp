#include "src/backend/media/capture/capture_session.h"
#include "src/backend/media/live/live_session_controller.h"
#include "src/backend/media/live/live_types.h"
#include "src/controller/subsystems/live/live_system.h"
#include "src/controller/subsystems/live/live_analysis_configuration.h"
#include "src/frameworks/gpu/runtime/system_image_runtime.h"
#include <cuda_runtime_api.h>
#include <optional>
#include <cstring>
#include <vector>
#include "src/backend/imaging/raster/skeleton_projection.h"
#include "src/frameworks/gpu/cuda/cuda_high_water_allocation.h"
#include "src/frameworks/gpu/memory/pinned_host_buffer.h"
#include <atomic>
#include <stdexcept>
#include <thread>
#include <utility>
#include "src/controller/subsystems/live/live_receiver_copy.h"
#include "src/frameworks/gpu/cuda/resource_owner_command_authority.h"
import mmltk.backend.imaging.raster;
import mmltk.backend.models.rfdetr.inference.analysis_provider;
import mmltk.backend.models.rfdetr.inference.runtime_backend;
namespace mmltk::controller {
namespace {
namespace capture = mmltk::backend::media::capture;
namespace media = mmltk::backend::media::live;
namespace gpu = mmltk::frameworks::gpu;
class NativeLiveAlgorithm final : public LiveAlgorithm {
public:
 NativeLiveAlgorithm(const VisualDeviceSettings settings, LiveNativeConfiguration configuration, gpu::DeviceExecution execution)
     : settings_(settings), execution_(std::move(execution)), configuration_(std::move(configuration)), owner_thread_(std::this_thread::get_id()) {}
 ~NativeLiveAlgorithm() override { static_cast<void>(Stop()); }
 void Start(const LiveStart& request, std::shared_ptr<const LiveAnalysisConfiguration> analysis, std::stop_token stop) override {
  if (plane_) throw std::logic_error("Live data plane is already active");
  media::LiveDataPlaneConfig config{};
  config.capture.device_path = configuration_.capture_device;
  config.capture.device_api = configuration_.capture_api;
  config.capture.cuda_device_index = settings_.device;
  config.capture.execution = execution_;
  config.capture.width = request.extent.width;
  config.capture.height = request.extent.height;
  config.capture.fps = request.frames_per_second;
  config.capture.v4l2_buffer_count = configuration_.capture_buffers;
  config.resource_worker = gpu::ResourceOwnerWorkerCapability{this, 1U, &NativeLiveAlgorithm::IsCurrent, &NativeLiveAlgorithm::FailCurrent};
  config.ingress_slots = configuration_.ingress_slots;
  config.fanout_slots = configuration_.fanout_slots;
  config.analysis_slots = configuration_.analysis_slots;
  config.composite_slots = configuration_.composite_slots;
  plane_ = std::make_unique<media::LiveMediaDataPlane>(config);
  failed_.store(false, std::memory_order_release);
  provider_admission_refused_ = false;
  last_revision_ = 0U;
  plane_->set_revision_listener({
   .context = this,
   .notify = &NativeLiveAlgorithm::RevisionReady,
  });
  plane_->set_admitted_run_terminal_listener({
   .context = this,
   .notify = &NativeLiveAlgorithm::TerminalReady,
  });
  analysis_ = std::move(analysis);
  stop_ = stop;
  media::LiveAnalysisProviderFactory factory;
  if (analysis_) factory = {this, [](const void* context, mmltk::backend::ml::runtime::BorrowedCommandStream stream) {
   auto& owner = *const_cast<NativeLiveAlgorithm*>(static_cast<const NativeLiveAlgorithm*>(context));
   const auto& request = owner.analysis_->request;
   try { return mmltk::backend::models::rfdetr::MakeRfdetrAnalysisProvider({
    .artifacts = request, .backend = request.onnx_path.empty() ? "tensorrt" : "onnx", .device = owner.settings_.device,
    .command_stream = stream, .static_resolution = static_cast<std::uint32_t>(request.resolution),
    .maximum_detections = request.max_dets_per_image, .allow_fp16 = request.allow_fp16, .stop = owner.stop_}, request.threshold); }
   catch (const mmltk::backend::models::rfdetr::RfdetrArtifactAdmissionCancelled&) {
    owner.provider_admission_refused_ = true;
    throw;
   }
   catch (const mmltk::backend::models::rfdetr::RfdetrArtifactAdmissionFailure& error) {
    owner.provider_admission_refused_ = true;
    throw std::runtime_error("Live RF-DETR analysis provider for " + (request.onnx_path.empty() ? request.tensorrt_path.string() : request.onnx_path.string()) + ": " + error.what());
   }
  }};
  try {
   const auto started = plane_->start(factory);
   if (!started.running()) {
    const auto failure = "Live capture session failed to start: " + started.status.message;
    if (started.valid() && !started.has_custody() && provider_admission_refused_) {
     // NoCustody is published after PhysicalResources destruction. Joining also
     // settles callbacks. The media owner independently records every CUDA
     // cleanup failure; the first operation cause is never its safety proof.
     static_cast<void>(plane_->stop());
     const auto settlement = plane_->no_custody_start_settlement();
     if (settlement.ok() && !failed_.load(std::memory_order_acquire)) {
      plane_.reset(); analysis_.reset();
      throw LiveProviderAdmissionFailure(failure);
     }
     if (!settlement.ok()) throw std::runtime_error(failure + "; " + settlement.message);
    }
    throw std::runtime_error(failure);
   }
  } catch (...) {
   static_cast<void>(Stop());
   throw;
  }
 }
 void SetOutputAvailableSink(std::function<void()> sink) override { ready_ = std::move(sink); }
 bool AcquireOutput() override {
  if (failed_.load(std::memory_order_acquire) || !plane_) throw std::runtime_error("Live media data plane failed");
  if (lease_) return true;
  const auto observed = plane_->newest_revision();
  if (!observed || observed->revision <= last_revision_) return false;
  if (!media::try_acquire_live_output(plane_.get(), *observed, &NativeLiveAlgorithm::AcquireMediaOutput, &lease_)) {
   if (failed_.load(std::memory_order_acquire)) throw std::runtime_error("Live media data plane failed");
   return false;
  }
  last_revision_ = observed->revision;
  return true;
 }
 bool Capture(const mmltk::frameworks::gpu::ImagePlaneView target, mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream_value, media::LiveAnalysisProduct& analysis, const std::stop_token stop) override {
  if (stop.stop_requested()) {
   lease_ = {};
   return false;
  }
  if (!lease_) throw std::logic_error("Live capture has no acquired output");
  const auto source = lease_.view();
  if (source.width != target.descriptor.width || source.height != target.descriptor.height) throw std::runtime_error("Live composite extent changed unexpectedly");
  const cudaError_t status = detail::copy_live_receiver_frame(target, semantic, lease_, analysis, stream_value, detail::native_live_receiver_copy_operations());
  if (status != cudaSuccess) throw std::runtime_error("Live composite receiver copy failed");
  return true;
 }
 void RenderAnalysis(const gpu::ImagePlaneView semantic, const std::uintptr_t stream_value, const media::LiveAnalysisProduct& product) override {
  if (semantic.data == 0U) return;
  namespace raster = mmltk::backend::imaging::raster;
  const auto stream = reinterpret_cast<cudaStream_t>(stream_value);
  words_.clear();
  skeletons_.clear();
  if (analysis_ && (analysis_->keypoints || analysis_->uncertainty)) for (const auto& region : product.regions()) {
   if (!region.keypoints_available || !region.class_catalog || region.class_domain != mmltk::backend::data::catalog::ClassReferenceDomain::Foreground) continue;
   for (std::size_t instance = 0; instance < region.count; ++instance) {
    const auto category = region.class_references[instance];
    if (category < 0 || static_cast<std::size_t>(category) >= region.class_catalog->size()) throw std::runtime_error("Live skeleton class exceeds catalog");
    const auto& schema = region.class_catalog->keypoints(category);
    if (schema.names.empty()) continue;
    if (region.keypoint_counts[instance] != static_cast<std::int32_t>(schema.names.size())) throw std::runtime_error("Live skeleton count disagrees with schema");
    if (region.confidences[instance] < analysis_->request.threshold) continue;
    auto skeleton = raster::append_skeleton_projection(words_, schema, [&](std::size_t joint) {
     const auto& point = region.keypoints[instance * region.keypoint_capacity + joint];
     return raster::SkeletonSample{point.x, point.y, point.confidence, analysis_->uncertainty ? std::array<float, 3>{point.covariance_xx, point.covariance_xy, point.covariance_yy} : std::array<float, 3>{}};
    });
    skeleton.color = region.colors_rgb[instance];
    skeletons_.push_back(skeleton);
   }
  }
  if (words_.empty()) return;
  const auto bytes = words_.size() * sizeof(std::uint32_t);
  if (bytes > geometry_capacity_) {
   if (!geometry_host_) geometry_host_ = gpu::PinnedHostBuffer::ForCurrentDevice();
   geometry_host_->ensure_bytes(bytes);
   if (!geometry_device_.AllocateCandidate([bytes](void*& value) { return cudaMalloc(&value, bytes); }).released() ||
       !geometry_device_.PromoteCandidate([](void* value) { return cudaFree(value); }).released()) throw std::runtime_error("Live skeleton allocation failed");
   geometry_capacity_ = bytes;
  }
  std::memcpy(geometry_host_->data(), words_.data(), bytes);
  if (cudaMemcpyAsync(geometry_device_.active(), geometry_host_->data(), bytes, cudaMemcpyHostToDevice, stream) != cudaSuccess) throw std::runtime_error("Live skeleton upload failed");
  raster::MutableBytes target{reinterpret_cast<std::uint8_t*>(semantic.data), semantic.descriptor.pitch_bytes, static_cast<int>(semantic.descriptor.width), static_cast<int>(semantic.descriptor.height)};
  for (const auto& skeleton : skeletons_)
   if (raster::raster_projected_skeleton(target, static_cast<const std::uint32_t*>(geometry_device_.active()), skeleton, {stream}, analysis_->keypoints, analysis_->uncertainty) != cudaSuccess)
    throw std::runtime_error("Live skeleton rendering failed");
 }
 Release ReleaseResources() noexcept override {
  const auto device = geometry_device_.ReleaseAll([](void* value) { return cudaFree(value); });
  const auto host = geometry_host_ ? geometry_host_->ReleaseSettled() : CUDA_SUCCESS;
  if (!device.released() || host != CUDA_SUCCESS) return {.all_released = false, .failure = release_failure_};
  return {};
 }
 capture::Status Stop() noexcept override {
  if (!plane_) return capture::Status::Ok();
  lease_ = {};
  const auto status = plane_->stop();
  plane_.reset();
  analysis_.reset();
  failed_.store(true, std::memory_order_release);
  return status;
 }

private:
 static bool IsCurrent(const void* context, std::uintptr_t) noexcept { return static_cast<const NativeLiveAlgorithm*>(context)->owner_thread_ == std::this_thread::get_id(); }
 static bool FailCurrent(const void* context, std::uintptr_t) noexcept {
  auto& owner = *const_cast<NativeLiveAlgorithm*>(static_cast<const NativeLiveAlgorithm*>(context));
  owner.Fail();
  return true;
 }
 void Notify() noexcept {
  if (ready_) {
   try {
    ready_();
   } catch (...) {}
  }
 }
 void Fail() noexcept {
  failed_.store(true, std::memory_order_release);
  Notify();
 }
 static void RevisionReady(void* context) noexcept { static_cast<NativeLiveAlgorithm*>(context)->Notify(); }
 static void TerminalReady(void* context, const media::LivePhysicalTerminal&) noexcept { static_cast<NativeLiveAlgorithm*>(context)->Fail(); }
 static bool AcquireMediaOutput(void* context, const media::PhysicalFrameRevision revision, media::LiveCompositeOutputLease* output) {
  return context != nullptr && static_cast<media::LiveMediaDataPlane*>(context)->try_acquire_output(revision, output);
 }
 VisualDeviceSettings settings_;
 gpu::DeviceExecution execution_;
 LiveNativeConfiguration configuration_;
 std::thread::id owner_thread_;
 std::unique_ptr<media::LiveMediaDataPlane> plane_;
 std::shared_ptr<const LiveAnalysisConfiguration> analysis_;
 std::stop_token stop_;
 media::LiveCompositeOutputLease lease_;
 std::uint64_t last_revision_ = 0U;
 std::atomic_bool failed_{false};
 bool provider_admission_refused_ = false;
 std::function<void()> ready_;
 std::vector<std::uint32_t> words_;
 std::vector<mmltk::backend::imaging::raster::SkeletonProjection> skeletons_;
 gpu::CudaHighWaterAllocation<void*> geometry_device_;
 std::unique_ptr<gpu::PinnedHostBuffer> geometry_host_;
 std::size_t geometry_capacity_ = 0;
 const std::exception_ptr release_failure_ = std::make_exception_ptr(std::runtime_error("Live skeleton release failed"));
};
}  // namespace
VisualRuntimeFactory make_native_live_runtime_factory(const VisualDeviceSettings settings, LiveNativeConfiguration configuration) {
 if (!settings.valid()) throw contracts::InvalidIntentError("Live device settings are invalid");
 return [settings, execution = resolve_visual_device_execution(settings), configuration = std::move(configuration)](auto revisions) {
  mmltk::frameworks::gpu::SystemImageRuntimeConfig config{
   .device = settings.device,
   .model = std::make_unique<NativeLiveAlgorithm>(settings, configuration, execution),
   .output_layout = gpu::ImageProductLayout::CleanAndSemantic,
   .output_buffer_count = 2U,
   .numa_node = settings.numa_node,
   .execution = execution,
   .product_revisions = std::move(revisions),
  };
  return make_visual_runtime(std::move(config));
 };
}
}  // namespace mmltk::controller
