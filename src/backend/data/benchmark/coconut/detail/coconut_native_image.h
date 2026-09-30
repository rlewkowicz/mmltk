#pragma once
#include "src/backend/data/benchmark/coconut/detail/coconut_input.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_mask_recovery.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_catalog.h"
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/backend/data/benchmark/detail/benchmark_catalog.h"
#include "src/backend/data/compiled/compiled_format.h"
#include "src/backend/data/detail/mask_rle_utils.h"
#include "src/common/concurrency/cancellation_observation.h"
#include "src/backend/data/detail/compile_image_decoder.h"
#include "src/common/system/numa_memory.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkMappedArtifact;
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
 [[nodiscard]] std::span<const NormalizedBox> boxes() const noexcept { return mapped_boxes_.empty() ? std::span<const NormalizedBox>(boxes_) : mapped_boxes_; }
 [[nodiscard]] std::span<const RLEPair> runs() const noexcept { return mapped_runs_.empty() ? std::span<const RLEPair>(runs_) : mapped_runs_; }
 [[nodiscard]] static std::shared_ptr<const CoconutNativeImage> stage(std::shared_ptr<const CoconutNativeImage>, const std::filesystem::path&, StorageReservationPool* = nullptr);
 [[nodiscard]] static std::shared_ptr<const CoconutNativeImage> bind(std::shared_ptr<const CoconutNativeImage>, const CoconutPhysicalImage&);
 [[nodiscard]] static std::shared_ptr<const CoconutNativeImage> rejected(const CoconutRecord&, const CoconutPhysicalImage&, std::shared_ptr<const CoconutNativeLineage>);
 [[nodiscard]] bool required_labels_valid() const noexcept { return required_labels_valid_; }
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
 std::shared_ptr<const BenchmarkMappedArtifact> backing_;
 std::span<const NormalizedBox> mapped_boxes_;
 std::span<const RLEPair> mapped_runs_;
 bool required_labels_valid_ = true;
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
 [[nodiscard]] std::uint64_t support_workspace_bytes(const CoconutRecord&, std::uint64_t pixels, std::uint64_t segments, bool recover = true) const;
 [[nodiscard]] dataset::MaskDimensions admit_png(const CoconutRecord&, std::span<const std::uint8_t>) const;
 [[nodiscard]] std::size_t decoder_workspace_bytes(std::span<const std::uint8_t>) const;
 void decode(const CoconutRecord&, std::span<const std::uint8_t>, dataset::MaskDimensions, std::span<std::byte> supplied = {});
 [[nodiscard]] std::vector<CoconutSegmentSupport> take_support(std::size_t);
 [[nodiscard]] std::uint32_t unlisted_segment() const noexcept { return unlisted_segment_; }
 void borrow_support(const CoconutRecord&, std::span<const CoconutSegmentSupport>, dataset::MaskDimensions, std::shared_ptr<const void>, std::uint32_t unlisted_segment = 0);
 [[nodiscard]] std::shared_ptr<const CoconutNativeImage> finish(
  const CoconutRecord&, const CoconutPhysicalImage&, dataset::MaskDimensions, std::shared_ptr<const CoconutNativeLineage>, const CoconutRejectedObject&);
 [[nodiscard]] std::shared_ptr<const CoconutNativeImage> reuse(const CoconutRecord&, const CoconutPhysicalImage&, const NormalizedAnnotationReadView&, std::size_t, const CoconutInventoryImage&,
  const CoconutRecoveryImage*, std::shared_ptr<const CoconutNativeLineage>, std::string_view dependency, bool required_labels_valid);
 void retire() noexcept;

private:
 struct SegmentEntry {
  std::uint32_t id = 0;
  std::size_t index = 0;
 };
 SegmentEntry& segment_entry(std::uint32_t);
 mutable detail::CompileImageDecoder decoder_;
 std::unique_ptr<common::system::NumaMemory> decode_memory_;
 CoconutImportLimits limits_;
 mmltk::common::concurrency::CancellationObservation cancellation_;
 std::shared_ptr<const CoconutRecoveryOriginals> originals_;
 std::unique_ptr<CoconutMaskRecovery> recovery_;
 const CoconutRecord* active_record_ = nullptr;
 dataset::MaskDimensions active_dimensions_{};
 std::uint64_t run_bytes_ = 0;
 std::uint32_t unlisted_segment_ = 0;
 std::vector<SegmentEntry> segment_by_id_;
 std::vector<CoconutSegmentSupport> support_;
};
}  // namespace mmltk::backend::data::benchmark_internal
