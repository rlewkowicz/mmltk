#include "src/controller/presentation/visual_system_types.h"

#include <utility>

#include "src/frameworks/gpu/image_buffer.h"

namespace mmltk::controller {

bool visual_product_matches_frame(const VisualFrame& frame, const mmltk::frameworks::gpu::BorrowedImageProductReadView& product) noexcept {
    if (!frame.valid() || !product.valid() || (product.plane_count() != 1U && product.plane_count() != 2U)) return false;
    for (std::size_t index = 0U; index != product.plane_count(); ++index) {
        const auto& view = product.plane(index);
        const auto descriptor = view.plane().descriptor;
        if (view.revision() != frame.revision || descriptor.width != frame.extent.width || descriptor.height != frame.extent.height)
            return false;
    }
    return true;
}

mmltk::frameworks::gpu::BorrowedImageProductReadView borrow_matching_visual_product(
    const VisualFrame& frame, mmltk::frameworks::gpu::BorrowedImageProductReadView borrowed) {
    return visual_product_matches_frame(frame, borrowed) ? std::move(borrowed) : mmltk::frameworks::gpu::BorrowedImageProductReadView{};
}

}  // namespace mmltk::controller
