#include "src/controller/presentation/visual_runtime.h"
#include "src/common/system/numa_topology.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/cuda_error.h"
import mmltk.backend.imaging.raster;
namespace mmltk::controller {
void configure_visual_workspace_finalization(mmltk::frameworks::gpu::SystemImageRuntimeConfig& config) {
 config.workspace_finalize = [](const auto clean, const auto semantic, const auto destination, const auto coverage, std::uintptr_t stream) {
  namespace raster = mmltk::backend::imaging::raster;
  const auto source = [](const auto plane) -> raster::ConstBytes {
   return {reinterpret_cast<const std::uint8_t*>(plane.data), plane.descriptor.pitch_bytes, static_cast<int>(plane.descriptor.width), static_cast<int>(plane.descriptor.height)};
  };
  raster::FinalizeRgbaWork work{.clean = source(clean),
   .semantic = source(semantic),
   .destination = {reinterpret_cast<std::uint8_t*>(destination.data), destination.descriptor.pitch_bytes, static_cast<int>(destination.descriptor.width),
    static_cast<int>(destination.descriptor.height)},
   .full_image = coverage.full_image,
   .stream = reinterpret_cast<void*>(stream)};
  const auto submit = [&] { mmltk::frameworks::gpu::ensure_cuda_ok(static_cast<cudaError_t>(raster::finalize_rgba(work)), "workspace raster finalization"); };
  if (coverage.full_image) {
   submit();
   return;
  }
  for (const auto region : coverage.regions) {
   const raster::IntRect rectangle{region.x1, region.y1, region.x2, region.y2};
   work.regions = {&rectangle, 1U};
   submit();
  }
 };
}
mmltk::frameworks::gpu::DeviceExecution resolve_visual_device_execution(const VisualDeviceSettings& settings) {
 return mmltk::frameworks::gpu::resolve_device_execution(settings.device, mmltk::common::system::NumaTopology::Capture(), settings.numa_node);
}
}  // namespace mmltk::controller
