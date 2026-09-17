#include "visual_system_fixture.h"
#include <cstring>
namespace mmltk::controller::visual_test_support {
void Fill(const mmltk::frameworks::gpu::ImagePlaneView plane, const std::uint8_t value) {
    std::memset(reinterpret_cast<void*>(plane.data), value, plane.descriptor.pitch_bytes * plane.descriptor.height);
}
[[nodiscard]] bool has_cuda_device() noexcept {
    int device_count = 0;
    return ::cudaGetDeviceCount(&device_count) == cudaSuccess && device_count > 0;
}
[[nodiscard]] VisualDocumentRead test_document(mmltk::frameworks::gpu::BorrowedImageProductReadView pixels) {
    static const auto document = [] {
        auto value = std::make_shared<VisualDocument>();
        value->scene.document = contracts::WorkspaceResource::From("test://image", 1U);
        value->scene.categories.push_back({.value = "object"});
        return std::shared_ptr<const VisualDocument>{std::move(value)};
    }();
    return {std::move(pixels), document};
}
}  // namespace mmltk::controller::visual_test_support
