#pragma once
#include "src/backend/data/benchmark/coconut/detail/coconut_mask_recovery.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_catalog.h"
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/backend/data/benchmark/detail/benchmark_catalog.h"
#include "src/backend/data/compiled/compiled_format.h"
#include "src/backend/data/detail/mask_rle_utils.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
[[nodiscard]] const CategoryLookup& coconut_categories();
[[nodiscard]] std::string coconut_image_input_identity(std::string_view annotations, const CoconutPhysicalImage&, std::string_view originals);
struct CoconutComponentMetadata {
 CoconutEdition edition = CoconutEdition::Base;
 CoconutImageNamespace source = CoconutImageNamespace::CocoTrain;
 std::string input_identity;
 std::uint32_t recovery_policy = 0;
 std::string original_annotation_identity;
 // Compile-local annotation lineage; persisted input_identity seals this plus physical dependencies.
 std::string annotation_input_identity{};
 std::uint64_t original_generation = 0;
};
struct CoconutSegment {
 std::uint32_t id = 0;
 std::uint64_t category_id = 0;
 bool isthing = false;
 bool crowd = false;
 bool ignore = false;
 std::optional<double> area = std::nullopt;
 // External COCO bbox convention: x, y, width, height.
 std::optional<std::array<double, 4>> bbox = std::nullopt;
};
struct CoconutRecord {
 std::uint64_t image_id = 0;
 std::string file_name;
 std::string physical_stem;
 std::optional<CoconutImageNamespace> namespace_hint;
 std::uint32_t width = 0, height = 0;
 std::uint64_t source_ordinal = 0;
 std::uint64_t first_segment_ordinal = 0;
 std::vector<CoconutSegment> segments;
};
struct CoconutImportLimits {
 std::uint64_t max_png_bytes = 64U * 1024U * 1024U;
 std::uint64_t max_pixels = 64U * 1024U * 1024U;
 std::uint32_t max_segments = 65535U;
 std::uint32_t max_dimension = MAX_IMAGE_EXTENT;
};
// Shared once per release/source/original generation; each image owns payload only.
struct CoconutNativeLineage {
 CoconutComponentMetadata component;
 NormalizedAnnotationMetadata index;
};
class CoconutNativeWorkspace;
class CoconutNativeImage final {
public:
 struct Read {
  std::shared_ptr<const CoconutNativeImage> owner;
  NormalizedAnnotationReadView view;
 };
 [[nodiscard]] static Read read(std::shared_ptr<const CoconutNativeImage>);
 [[nodiscard]] static BenchmarkLabelInput labels(std::shared_ptr<const CoconutNativeImage>);
 [[nodiscard]] const std::string& input_identity() const noexcept { return input_identity_; }
 [[nodiscard]] const CoconutNativeLineage& lineage() const noexcept { return *lineage_; }
 [[nodiscard]] const NormalizedImage& image() const noexcept { return image_; }
 [[nodiscard]] std::span<const NormalizedBox> boxes() const noexcept { return boxes_; }
 [[nodiscard]] std::span<const RLEPair> runs() const noexcept { return runs_; }
 [[nodiscard]] const AnnotationRejectCounts& rejected() const noexcept { return rejected_; }
 [[nodiscard]] const CoconutInventoryImage& inventory() const noexcept { return inventory_; }
 [[nodiscard]] const CoconutRecoveryImage& recovery() const noexcept { return recovery_; }
 [[nodiscard]] std::uint64_t segment_begin() const noexcept { return segment_begin_; }

private:
 friend class CoconutNativeWorkspace;
 explicit CoconutNativeImage(std::shared_ptr<const CoconutNativeLineage> lineage) : lineage_(std::move(lineage)) {}
 std::shared_ptr<const CoconutNativeLineage> lineage_;
 std::string input_identity_;
 NormalizedImage image_;
 std::vector<NormalizedBox> boxes_;
 std::vector<RLEPair> runs_;
 AnnotationRejectCounts rejected_;
 CoconutInventoryImage inventory_;
 CoconutRecoveryImage recovery_;
 std::uint64_t segment_begin_ = 0;
};
using CoconutRejectedObject = std::function<void(const CoconutPhysicalImage&, const CoconutRecord&, const CoconutSegment&, std::string_view, std::uint32_t)>;
// No controller or publication state. One CPU lane owns each mutable workspace.
class CoconutNativeWorkspace final {
public:
 explicit CoconutNativeWorkspace(const CoconutImportLimits&, mmltk::common::concurrency::CancellationObservation = {});
 CoconutNativeWorkspace(const CoconutNativeWorkspace&) = delete;
 CoconutNativeWorkspace& operator=(const CoconutNativeWorkspace&) = delete;
 CoconutNativeWorkspace(CoconutNativeWorkspace&&) = delete;
 CoconutNativeWorkspace& operator=(CoconutNativeWorkspace&&) = delete;
 void originals(std::shared_ptr<const CoconutRecoveryOriginals>);
 void recovery(const CoconutMaskRecovery*);
 [[nodiscard]] CoconutMaskRecovery* recovery() const noexcept { return recovery_.get(); }
 [[nodiscard]] std::uint64_t retained_bytes() const noexcept;
 [[nodiscard]] std::uint64_t workspace_bytes(const CoconutRecord&, std::uint64_t pixels, std::uint64_t segments, std::uint64_t encoded, bool recover = true) const;
 [[nodiscard]] dataset::MaskDimensions admit_png(const CoconutRecord&, std::span<const std::uint8_t>) const;
 void decode(const CoconutRecord&, std::span<const std::uint8_t>, dataset::MaskDimensions);
 [[nodiscard]] std::vector<CoconutSegmentSupport> take_support(std::size_t);
 void borrow_support(const CoconutRecord&, std::span<const CoconutSegmentSupport>, dataset::MaskDimensions, std::shared_ptr<const void>);
 [[nodiscard]] std::shared_ptr<const CoconutNativeImage> finish(
  const CoconutRecord&, const CoconutPhysicalImage&, dataset::MaskDimensions, std::shared_ptr<const CoconutNativeLineage>, const CoconutRejectedObject&);
 [[nodiscard]] std::shared_ptr<const CoconutNativeImage> reuse(const CoconutRecord&, const CoconutPhysicalImage&, const NormalizedAnnotationReadView&, std::size_t, const CoconutInventoryImage&,
  const CoconutRecoveryImage*, std::shared_ptr<const CoconutNativeLineage>, std::string_view dependency);
 void retire() noexcept;

private:
 struct SegmentEntry {
  std::uint32_t id = 0;
  std::size_t index = 0;
 };
 SegmentEntry& segment_entry(std::uint32_t);
 CoconutImportLimits limits_;
 mmltk::common::concurrency::CancellationObservation cancellation_;
 std::shared_ptr<const CoconutRecoveryOriginals> originals_;
 std::unique_ptr<CoconutMaskRecovery> recovery_;
 const CoconutRecord* active_record_ = nullptr;
 dataset::MaskDimensions active_dimensions_{};
 std::uint64_t run_bytes_ = 0;
 std::vector<SegmentEntry> segment_by_id_;
 std::vector<CoconutSegmentSupport> support_;
};
}  // namespace mmltk::backend::data::benchmark_internal
