#pragma once

#include <functional>
#include <cstdint>
#include <memory>

#include "src/controller/contracts/annotation.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/frameworks/gpu/image_buffer.h"

namespace mmltk::controller {

struct VisualDocumentFacts final {
    contracts::WorkspaceResource resource{};
    std::uint64_t meaning_identity = 0U;
    [[nodiscard]] bool valid() const noexcept { return resource.valid() && meaning_identity != 0U; }
    bool operator==(const VisualDocumentFacts&) const = default;
};
MMLTK_REFLECT_FIELDS(VisualDocumentFacts)

// Immutable annotation meaning accompanies a product borrow. Mask support is
// queried in normalized full-image coordinates; editable runs are made only
// by the importing document, never by gallery publication.
struct VisualDocument final {
    VisualDocument() = default;
    VisualDocument(const VisualDocument& other) : scene(other.scene), mask_contains(other.mask_contains) {}
    contracts::AnnotationSceneContent scene{};
    std::function<bool(std::size_t, float, float)> mask_contains{};
    [[nodiscard]] VisualDocumentFacts facts() const { return {scene.document, meaning_identity_}; }
   private:
    [[nodiscard]] static std::uint64_t NextIdentity();
    const std::uint64_t meaning_identity_ = NextIdentity();
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
