#include "src/controller/presentation/visual_runtime.h"

#include "src/common/system/numa_topology.h"

namespace mmltk::controller {

mmltk::frameworks::gpu::DeviceExecution resolve_visual_device_execution(const VisualDeviceSettings& settings) {
    return mmltk::frameworks::gpu::resolve_device_execution(settings.device, mmltk::common::system::NumaTopology::Capture(),
                                                         settings.numa_node);
}

}  // namespace mmltk::controller
