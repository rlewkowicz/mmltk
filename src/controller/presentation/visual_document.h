#pragma once

#include <functional>
#include <memory>

#include "src/controller/contracts/annotation.h"
#include "src/controller/presentation/visual_system_types.h"

namespace mmltk::controller {

// Immutable annotation meaning accompanies a product borrow. Mask support is
// queried in normalized full-image coordinates; editable runs are made only
// by the importing document, never by gallery publication.
struct VisualDocument final {
    contracts::AnnotationSceneContent scene{};
    std::function<bool(std::size_t, float, float)> mask_contains{};
};
struct VisualDocumentRead final {
    mmltk::frameworks::gpu::BorrowedImageProductReadView pixels{};
    std::shared_ptr<const VisualDocument> document{};
    [[nodiscard]] bool valid() const noexcept { return pixels.valid() && document != nullptr; }
};
using ExactVisualDocumentBorrower = std::function<VisualDocumentRead(const VisualFrame&)>;

[[nodiscard]] contracts::AnnotationSceneContent materialize_visual_document(const VisualDocument&, VisualExtent, VisualRegion);
[[nodiscard]] std::shared_ptr<const VisualDocument> scale_visual_document(const std::shared_ptr<const VisualDocument>&, std::uint32_t);

}  // namespace mmltk::controller
