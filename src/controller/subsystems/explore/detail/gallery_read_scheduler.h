#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include "src/backend/data/compiled_image_stream.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/subsystems/explore/detail/gallery_thumbnail_cache.h"
#include "src/controller/subsystems/explore/detail/gallery_payload.h"
namespace mmltk::controller::explore_detail {
struct GalleryProductState;
class GalleryStream;
// The stream transaction alone may inspect lanes under this owner's mutex.
// No callback references the transaction or its mutable product.
class GalleryReadScheduler final {
 friend class GalleryStream;
 friend class GalleryStreamProbe;

public:
 GalleryReadScheduler(std::size_t, const mmltk::frameworks::gpu::DeviceExecution&, const ExploreNativeConfiguration&);
 GalleryReadScheduler(const GalleryReadScheduler&) = delete;
 GalleryReadScheduler& operator=(const GalleryReadScheduler&) = delete;

private:
 enum class LaneState : std::uint8_t {
  Idle,
  Preparing,
  Queued,
  Reading,
  AwaitingTransfer,
  InputReady,
  StaleReady,
  GpuPending,
  GpuComplete,
  Failed,
 };
 struct PayloadLayout final {
  // A lane payload's card offset begins a storage layout distinct from its batch descriptor.
  std::size_t card = 0U;
  StorageSpan annotations{};
  StorageSpan rle{};
  std::size_t tile = 0U;
  std::size_t donor_pixels = 0U;
  std::size_t donor_instance = 0U;
  // Donor payload offsets are lane-local storage facts, not assembled descriptor capacities.
  std::size_t donor_box = 0U;
  StorageSpan donor_rle{};
  std::size_t donor_mask = 0U;
  std::size_t bytes = 0U;
  std::size_t pixel_bytes = 0U;
  std::size_t donor_mask_words = 0U;
 };
 using TileMeaning = GalleryTileMeaning;
 struct Lane final {
  Lane(const std::size_t lane_index, const mmltk::backend::data::CompiledImageStream::Buffer& storage) : pinned(storage), index(lane_index) {}
  const mmltk::backend::data::CompiledImageStream::Buffer& pinned;
  std::shared_ptr<const mmltk::backend::data::CompiledDataset> store;
  GalleryThumbnailCache::Identity identity{};
  PayloadLayout layout{};
  std::size_t index = 0U;
  LaneState state = LaneState::Idle;
  // Physical descriptor generation gates admission and atlas publication.
  // Fully submitted cache pixels may complete under newer demand.
  // Failure/cancellation decisions use DemandGeneration.
  std::uint64_t generation = 0U;
  // A new scheduler may claim an in-flight read without changing any
  // payload/generation fields still observed by its completion callback.
  std::uint64_t reserved_generation = 0U;
  [[nodiscard]] std::uint64_t DemandGeneration() const noexcept { return reserved_generation != 0U ? reserved_generation : generation; }
  std::uint64_t tile_generation = 0U;
  // CLEANUP-IGNORE: Lane correlation, compiled identity and raster demand have distinct custody from diagnostic and graphics ABI
  // records.
  contracts::DiagnosticLink diagnostic_link{};
  // CLEANUP-IGNORE: Physical lane scheduling fields are private execution state, not a shared scalar schema.
  std::uint32_t compiled_index = 0U;
  std::uint32_t destination_slot = 0U;
  std::size_t position = 0U;
  std::size_t reserved_position = 0U;
  std::size_t cache_slot = 0U;
  std::uint64_t semantic_identity = 0U;
  std::uint8_t cache_bank = 0U;
  std::uint8_t semantic_bank = 0U;
  std::uint32_t card_extent = 0U;
  std::uint32_t columns = 1U;
  std::uint32_t first_row = 0U;
  std::uint64_t preview_key = 0U;
  bool prefetch = false;
  bool transfer_ready = false;
  bool read_valid = false;
  std::uint32_t donor_index = 0U;
  std::optional<mmltk::backend::data::PackedInstance> donor_instance;
  std::exception_ptr failure{};
  std::shared_ptr<const TileMeaning> pending_meaning;
 };
 bool BeginReadLane(const std::size_t lane_index);
 void FinishReadLane(const std::size_t lane_index, std::exception_ptr failure, const bool read) noexcept;
 void PublishInput(Lane& lane);
 void FinishTransfer(std::size_t index, std::exception_ptr failure) noexcept;
 void AcceptanceDiagnostic(const VisualDiagnosticOperation operation, const Lane& lane, const std::uint64_t detail) const noexcept;
 void ReadLanePayload(Lane& lane);
 void CompleteLane(const std::size_t lane_index, std::exception_ptr failure) noexcept;
 mmltk::backend::data::CompiledImageStream::CompletionObserver LaneCompletion() noexcept;
 void SubmitRead(Lane& lane, const bool observed);
 PayloadLayout LayoutFor(const GalleryProductState& product, const std::uint32_t compiled_index, const bool has_donor, const std::size_t donor_rle_count) const;
 void PrepareLaneStorage(const GalleryProductState& product, Lane& lane, const std::uint32_t compiled_index, const std::uint32_t slot, const std::uint64_t generation);
 void Prioritize(GalleryProductState& product);
 bool ReserveInput(const GalleryProductState& product, Lane& lane);
 void DiscardSettledInput(const GalleryProductState& product, Lane& lane);
 void ReconcileInitializationLane(const GalleryProductState& product, const GalleryThumbnailCache* incumbent, Lane& lane);
 void RebindInput(const GalleryProductState& product, const GalleryThumbnailCache* incumbent, Lane& lane);
 void StartIdleLanes(const GalleryProductState& product, const GalleryThumbnailCache* incumbent);
 bool HasReadyTiles() const;
 void SetReadySink(ExploreAlgorithm::GalleryReadySink sink);
 void StopIngress() noexcept;
 void SettleConsumers();
 void RestoreSettledLanes();
 void QuiesceReads(std::size_t);
 void ResetLogical();
 void ReleaseLane(Lane&, cudaStream_t);
 void SetCurrentDemand(ExploreDemandCheck check) {
  if (demand_bound_) throw std::logic_error("Explore gallery demand is already bound");
  current_demand_ = std::move(check);
  demand_bound_ = true;
 }
 [[nodiscard]] VisualDiagnosticFact Diagnostic(const VisualDiagnosticOperation operation, const std::uint64_t generation) const noexcept {
  return {.system = contracts::DiagnosticOwner::Explore, .operation = operation, .device = device_, .generation = generation};
 }
 [[nodiscard]] VisualDiagnosticFact LaneDiagnostic(const VisualDiagnosticOperation operation, const Lane& lane) const noexcept {
  auto fact = Diagnostic(operation, lane.generation);
  fact.value = lane.destination_slot;
  fact.detail = lane.compiled_index;
  fact.context.demand.demand_generation = lane.DemandGeneration();
  fact.context.link = lane.diagnostic_link;
  return fact;
 }
 std::shared_ptr<ExploreAcceptanceGate> acceptance_;
 VisualDiagnosticSink diagnostics_{};
 int device_ = 0;
 mmltk::backend::data::CompiledImageStream image_stream_;
 mutable std::mutex lanes_mutex_;
 std::vector<std::unique_ptr<Lane>> lanes_;
 // One lazily allocated physical input slot lets a detail candidate render
 // without overwriting any useful incumbent gallery input.
 std::unique_ptr<Lane> detail_lane_;
 [[nodiscard]] Lane& LaneAt(std::size_t index) noexcept { return index == lanes_.size() ? *detail_lane_ : *lanes_[index]; }
 std::shared_ptr<const ExploreAlgorithm::GalleryReadySink> ready_sink_;
 std::atomic<std::uint64_t> desired_generation_{0U};
 ExploreDemandCheck current_demand_{};
 bool demand_bound_ = false;
 [[nodiscard]] bool Current(std::uint64_t generation) const noexcept { return generation != 0U && generation == desired_generation_.load(std::memory_order_acquire) && current_demand_(generation); }
 std::atomic<std::size_t> stale_discarded_{0U};
 std::uint64_t next_tile_generation_ = 0U;
 std::vector<std::uint64_t> scheduled_slots_;
 std::vector<std::size_t> priority_rank_;
 std::size_t next_priority_ = 0U;
};
}  // namespace mmltk::controller::explore_detail
