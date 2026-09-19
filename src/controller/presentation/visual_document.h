#pragma once
#include <functional>
#include <array>
#include <vector>
#include <cstdint>
#include <memory>
#include "src/controller/contracts/annotation.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/frameworks/gpu/image_buffer.h"
namespace mmltk::frameworks::serialization::wire {
class Value;
}
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
    VisualDocument(const VisualDocument& other) : scene(other.scene), mask_contains(other.mask_contains), mask_bounds(other.mask_bounds) {}
    contracts::AnnotationSceneContent scene{};
    std::function<bool(std::size_t, float, float)> mask_contains{};
    // Object-indexed normalized support bounds, checked on import.
    // Every present mask has an entry; empty masks use {0,0,0,0}.
    std::vector<std::array<float, 4>> mask_bounds{};
    [[nodiscard]] VisualDocumentFacts facts() const { return {scene.document, meaning_identity_}; }

   private:
    [[nodiscard]] static std::uint64_t NextIdentity();
    const std::uint64_t meaning_identity_ = NextIdentity();
};
struct VisualDocumentRead final {
    mmltk::frameworks::gpu::BorrowedImageProductReadView pixels{};
    std::shared_ptr<const VisualDocument> document{};
    // Source-owned reflected image meaning retained with the exact borrowed
    // product. Receiver products carry it without consulting mutable UI state.
    std::shared_ptr<const mmltk::frameworks::serialization::wire::Value> image_metadata{};
    [[nodiscard]] bool valid() const noexcept { return pixels.valid() && document != nullptr; }
};
using ExactVisualDocumentBorrower = std::function<VisualDocumentRead(const VisualFrame&)>;
[[nodiscard]] VisualExtent visual_materialized_extent(const VisualFrame&, bool original);
[[nodiscard]] contracts::AnnotationSceneContent materialize_visual_document(const VisualDocument&, VisualExtent, VisualRegion, VisualExtent target = {});
[[nodiscard]] std::shared_ptr<const VisualDocument> scale_visual_document(const std::shared_ptr<const VisualDocument>&, std::uint32_t);
}  // namespace mmltk::controller
