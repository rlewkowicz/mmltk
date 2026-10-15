#pragma once
#include <functional>
#include <cstddef>
#include <array>
#include <vector>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include "src/controller/contracts/annotation.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/frameworks/gpu/image/image_buffer.h"
#include "src/frameworks/serialization/reflected_cbor.h"
namespace mmltk::controller {
// A paired source occupies one existing 8 MiB output value. Derived products
// carry source and destination values in the existing two-value envelope.
inline constexpr std::size_t kVisualMetadataByteBudget = 8U * 1024U * 1024U;
template <class Metadata>
[[nodiscard]] bool visual_metadata_admitted(const Metadata& metadata, std::size_t budget = kVisualMetadataByteBudget) {
 namespace serial = mmltk::frameworks::serialization;
 return serial::measure_transport(metadata, {.max_bytes = budget, .max_items = kVisualMetadataByteBudget}).has_value();
}
struct VisualKeypoint final {
 contracts::AnnotationPoint point{};
 MMLTK_FINITE float confidence = 0;
 std::optional<mmltk::backend::data::catalog::KeypointVisibility> visibility{};
 std::optional<std::array<float, 3>> covariance{};
};
MMLTK_REFLECT_FIELDS(VisualKeypoint)
struct VisualSkeleton final {
 mmltk::backend::data::catalog::ClassKeypointSchema schema;
 MMLTK_MAX_ITEMS(contracts::kAnnotationJointCapacity) std::vector<VisualKeypoint> joints;
 [[nodiscard]] std::vector<mmltk::backend::data::catalog::KeypointAnnotation> Annotations() const;
};
MMLTK_REFLECT_FIELDS(VisualSkeleton)
struct VisualObjectLabel final {
 contracts::AnnotationBox box{};
 int class_reference = 0;
 mmltk::backend::data::catalog::ClassReferenceDomain class_domain = mmltk::backend::data::catalog::ClassReferenceDomain::Foreground;
 float confidence = 0.0F;
 contracts::AnnotationColor color{};
 MMLTK_MAX_BYTES(256U) std::string name {};
 VisualSkeleton skeleton{};
};
MMLTK_REFLECT_FIELDS(VisualObjectLabel)
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
 void BindResource(contracts::WorkspaceResource);
 void SetCatalog(const mmltk::backend::data::catalog::ClassCatalog&);
 void AppendSourceObject(contracts::AnnotationObject object, std::array<float, 4> bounds, std::span<const mmltk::backend::data::catalog::KeypointAnnotation> joints = {});
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
[[nodiscard]] contracts::AnnotationSceneContent materialize_visual_document(const VisualDocument&, VisualExtent, VisualRegion, VisualExtent target = {});
[[nodiscard]] std::shared_ptr<const VisualDocument> scale_visual_document(const std::shared_ptr<const VisualDocument>&, VisualExtent source_extent, VisualExtent target_extent);
}  // namespace mmltk::controller
