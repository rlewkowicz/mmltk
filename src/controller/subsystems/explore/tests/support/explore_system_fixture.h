#pragma once
#include <new>
#include <variant>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "src/test_support/async_test_utils.hpp"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/subsystems/explore/detail/gallery_stream.h"
#include "src/controller/presentation/tests/support/visual_system_fixture.h"
namespace mmltk::controller::visual_test_support {
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
struct ExploreRenderGate final {
 std::atomic<std::size_t> calls{0U};
 std::promise<void> entered;
 std::promise<void> release;
 std::shared_future<void> released = release.get_future().share();
};
struct ExploreFinalizationGate final {
 void Arm() noexcept { armed.store(true, std::memory_order_release); }
 std::atomic_bool armed{false};
 std::promise<void> entered;
 std::promise<void> release;
 std::shared_future<void> released = release.get_future().share();
};
struct ExplorePostRenderGate final {
 explicit ExplorePostRenderGate(const std::size_t target) : target_call(target) {}
 std::size_t target_call;
 std::atomic<std::size_t> calls{0U};
 std::promise<void> entered;
 std::promise<void> release;
 std::shared_future<void> released = release.get_future().share();
};
struct ExploreWorkProbe final {
 std::atomic<std::size_t> opens{0U};
 std::atomic<std::size_t> prepares{0U};
 std::atomic<std::size_t> renders{0U};
 std::atomic<std::size_t> rendered_count{0U};
 std::atomic<float> rendered_copy_paste_probability{0.0F};
 std::array<std::atomic<std::uint32_t>, 2U> rendered_slots{};
};
struct ExploreDetailExtentProbe final {
 VisualExtent padded{64U, 64U};
 VisualExtent original{48U, 32U};
};
[[nodiscard]] inline ExploreAtlasLayout test_atlas_layout(const ExploreRenderPlan& plan, const mmltk::frameworks::gpu::ImagePlaneView plane) {
 const auto side = explore_atlas_card_extent(plan.viewport);
 return {plan.viewport.first_row, plan.viewport.row_count, plane.descriptor.height / side, 0U, plan.viewport.columns, side};
}
class SynchronousExploreAlgorithm : public ExploreAlgorithm {
public:
 void SetCurrentDemand(ExploreDemandCheck demand) override {
  if (demand_bound_) throw std::logic_error("test Explore demand rebound");
  demand_ = std::move(demand);
  demand_bound_ = true;
 }
 ExploreOutputChange OutputChange(const ExploreRenderPlan&, const ExploreOrderCandidate*) const override { return ExploreOutputChange::Initialize; }
 void AbortRenderGeneration() override { generation_ = 0U; }
 void DiscardCandidate() override {}
 void SetGalleryReadySink(GalleryReadySink) final {}
 void PrepareDetailOutput(mmltk::frameworks::gpu::ImageAllocation) noexcept override {}
 void PrepareOutputPublication(ExploreOutputChange, ExploreMode) final {}
 void CommitOutputPublication() noexcept final {}
 bool RollbackOutputPublication() noexcept final { return true; }
 ExploreGalleryPublication BeginGallery(const ExploreRenderPlan& plan, const ExploreOrderCandidate* candidate, const std::size_t nproc, const mmltk::frameworks::gpu::ImagePlaneView clean,
  const mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream) final {
  RenderProduct(plan, candidate, nproc, clean, semantic, stream);
  generation_ = plan.generation;
  return {.generation = generation_, .layout = test_atlas_layout(plan, clean)};
 }
 ExploreGalleryPublication AdvanceGallery() final { return {.generation = generation_}; }
 bool HasGalleryTiles() const final { return false; }
 ExploreGalleryPublication PublishGalleryTiles(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) final { return {.generation = generation_}; }
 void RenderDetail(const ExploreRenderPlan& plan, const std::size_t nproc, const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
  const std::uintptr_t stream) final {
  RenderProduct(plan, nullptr, nproc, clean, semantic, stream);
  generation_ = 0U;
 }

protected:
 virtual void RenderProduct(const ExploreRenderPlan&, const ExploreOrderCandidate*, std::size_t, mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) = 0;

private:
 ExploreDemandCheck demand_;
 bool demand_bound_ = false;
 std::uint64_t generation_ = 0U;
};
struct ExplorePublicationDemandGate final {
 ExploreDemandCheck demand;
 std::uint64_t generation = 0U;
 std::promise<void> entered;
 std::promise<void> release;
 std::shared_future<void> released = release.get_future().share();
 std::atomic_bool release_timed_out{false};
};
struct StreamingExploreAssignment final {
 std::uint64_t generation = 0U;
 std::uint32_t compiled_index = 0U;
 std::uint32_t slot = 0U;
 bool read_started = false;
 bool ready = false;
 bool gpu_pending = false;
};
struct StreamingExplorePublicationState {
 std::vector<StreamingExploreAssignment> assignments;
 std::vector<std::uint32_t> visible;
 std::vector<bool> completed_slots;
 std::vector<std::uint32_t> priority_slots;
 std::uint64_t generation = 0U;
 std::size_t nproc = 1U;
 std::size_t next_slot = 0U;
 std::size_t cumulative = 0U;
 ExploreAtlasLayout layout{};
};
struct StreamingExploreProbe final : StreamingExplorePublicationState {
 void Release(const std::uint32_t compiled_index) {
  ExploreAlgorithm::GalleryReadySink wake;
  {
   std::scoped_lock lock(mutex);
   const auto found = std::ranges::find(assignments, compiled_index, &StreamingExploreAssignment::compiled_index);
   REQUIRE(found != assignments.end());
   found->ready = true;
   wake = ready_sink;
   changed.notify_all();
  }
  if (wake) wake();
 }
 void ReleaseAll() {
  ExploreAlgorithm::GalleryReadySink wake;
  {
   std::scoped_lock lock(mutex);
   for (auto& assignment : assignments) assignment.ready = true;
   wake = ready_sink;
   changed.notify_all();
  }
  if (wake) wake();
 }
 void StartRead(const std::uint32_t compiled_index) {
  std::scoped_lock lock(mutex);
  const auto found = std::ranges::find(assignments, compiled_index, &StreamingExploreAssignment::compiled_index);
  REQUIRE(found != assignments.end());
  REQUIRE_FALSE(found->read_started);
  found->read_started = true;
  ++mapped_reads;
  changed.notify_all();
 }
 void AllowAllocation() {
  ExploreAlgorithm::GalleryReadySink wake;
  {
   std::scoped_lock lock(mutex);
   allocation_allowed = true;
   wake = ready_sink;
  }
  if (wake) wake();
 }
 void AllowPreRead() { SignalGate(pre_read_allowed); }
 void AllowPostRead() { SignalGate(post_read_allowed); }
 void CompleteCallbacks() {
  ExploreAlgorithm::GalleryReadySink wake;
  {
   std::scoped_lock lock(mutex);
   callback_completion_allowed = true;
   ++callback_completions;
   std::erase_if(assignments, [this](const auto& assignment) {
    if (!assignment.gpu_pending) return false;
    if (assignment.generation == generation) {
     completed_slots[assignment.slot] = 1U;
     ++cumulative;
    }
    return true;
   });
   wake = ready_sink;
   changed.notify_all();
  }
  if (wake) wake();
 }
 [[nodiscard]] bool Wait(std::function<bool()> predicate) {
  std::unique_lock lock(mutex);
  return changed.wait_for(lock, 2s, std::move(predicate));
 }
 mutable std::mutex mutex;
 std::condition_variable changed;
 ExploreAlgorithm::GalleryReadySink ready_sink;
 std::size_t publications = 0U;
 std::size_t stale = 0U;
 std::size_t maximum_active = 0U;
 bool allocation_allowed = false;
 bool pre_read_allowed = false;
 bool post_read_allowed = false;
 bool callback_completion_allowed = true;
 bool fail_callback_admission = false;
 std::size_t lane_preparations = 0U;
 std::size_t mapped_reads = 0U;
 std::size_t queued_closures = 0U;
 std::size_t callback_completions = 0U;
 std::size_t quiescences = 0U;
 std::size_t aborted_assignments = 0U;
 ExploreScrollDirection scroll_direction = ExploreScrollDirection::Forward;
 bool fail_next_render = false;
 bool fail_next_labels = false;
 bool fail_rollback = false;
 std::size_t rollbacks = 0U;
 std::string opened_source;
 std::shared_ptr<ExplorePostRenderGate> render_gate;
 std::shared_ptr<ExplorePublicationDemandGate> publication_gate;
 std::vector<ExploreDemandCheck> bound_demands;
 std::size_t bound_opens = 0U;

private:
 void SignalGate(bool& gate) {
  ExploreAlgorithm::GalleryReadySink wake;
  {
   std::scoped_lock lock(mutex);
   gate = true;
   wake = ready_sink;
  }
  if (wake) wake();
 }
};
[[nodiscard]] inline std::vector<std::uint32_t> visible_test_order(const std::vector<std::uint32_t>& order, const ExploreViewport viewport) {
 if (!viewport.valid()) return order;
 const auto first = std::min<std::size_t>(static_cast<std::size_t>(viewport.first_row) * viewport.columns, order.size());
 const auto count = std::min<std::size_t>(static_cast<std::size_t>(viewport.row_count) * viewport.columns, order.size() - first);
 return {order.begin() + static_cast<std::ptrdiff_t>(first), order.begin() + static_cast<std::ptrdiff_t>(first + count)};
}
class ControlledStreamingExploreAlgorithm final : public ExploreAlgorithm {
 void SetCurrentDemand(ExploreDemandCheck demand) override {
  if (demand_bound_) throw std::logic_error("test streaming Explore demand rebound");
  demand_ = std::move(demand);
  demand_bound_ = true;
  std::scoped_lock lock(probe_->mutex);
  probe_->bound_demands.push_back(demand_);
 }
 ExploreOutputChange OutputChange(const ExploreRenderPlan&, const ExploreOrderCandidate*) const override { return ExploreOutputChange::Initialize; }

public:
 explicit ControlledStreamingExploreAlgorithm(std::shared_ptr<StreamingExploreProbe> probe) : probe_(std::move(probe)) {}
 ~ControlledStreamingExploreAlgorithm() override {
  std::scoped_lock lock(probe_->mutex);
  ++probe_->quiescences;
  probe_->assignments.clear();
  probe_->ready_sink = {};
 }
 ExploreOpened Open(const std::string_view source, std::stop_token) override {
  if (!demand_bound_ || !demand_.generation()) throw std::logic_error("Explore ingress preceded demand binding");
  std::scoped_lock lock(probe_->mutex);
  ++probe_->bound_opens;
  probe_->opened_source = source;
  order_ = {0U, 1U, 2U, 3U, 4U, 5U};
  return {
   .dataset = {.image_count = 6U, .image_width = 4U, .image_height = 4U},
   .order = Visible({}),
  };
 }
 ExploreOrderCandidate PrepareFilter(const ExploreFilter& filter, const std::uint64_t seed, std::size_t, std::stop_token) override {
  return {.filter = filter, .order = {.matching_count = static_cast<std::uint32_t>(order_.size()), .shuffle_seed = seed, .visible_indices = order_}, .generation = ++candidate_generation_};
 }
 void Commit(ExploreOrderCandidate) noexcept override {}
 void AbortRenderGeneration() override { ClearStreamingState(); }
 void DiscardCandidate() override {}
 void Reset() noexcept override {
  ClearStreamingState();
  order_.clear();
 }
 ExploreOrderFacts Visible(const ExploreViewport viewport, const ExploreOrderCandidate* candidate = nullptr) const override {
  const auto& order = candidate == nullptr ? order_ : candidate->order.visible_indices;
  return {
   .matching_count = static_cast<std::uint32_t>(order.size()),
   .visible_indices = visible_test_order(order, viewport),
  };
 }
 bool Contains(const std::uint32_t value) const override { return std::ranges::find(order_, value) != order_.end(); }
 std::optional<std::uint32_t> Adjacent(const std::uint32_t value, const std::int64_t offset) const override {
  if (!Contains(value)) return {};
  const auto count = static_cast<std::int64_t>(order_.size());
  const auto adjacent = (static_cast<std::int64_t>(value) + offset % count + count) % count;
  return static_cast<std::uint32_t>(adjacent);
 }
 void SetGalleryReadySink(GalleryReadySink sink) override {
  std::scoped_lock lock(probe_->mutex);
  probe_->ready_sink = std::move(sink);
 }
 void PrepareDetailOutput(mmltk::frameworks::gpu::ImageAllocation) noexcept override {}
 void PrepareOutputPublication(ExploreOutputChange, ExploreMode) override {
  std::scoped_lock lock(probe_->mutex);
  if (checkpoint_) return;
  checkpoint_.emplace(PublicationCheckpoint{
   .assignments = probe_->assignments,
   .visible = probe_->visible,
   .completed_slots = probe_->completed_slots,
   .priority_slots = probe_->priority_slots,
   .generation = probe_->generation,
   .nproc = probe_->nproc,
   .next_slot = probe_->next_slot,
   .cumulative = probe_->cumulative,
   .layout = probe_->layout,
  });
 }
 void CommitOutputPublication() noexcept override {
  checkpoint_.reset();
  if (auto gate = std::exchange(probe_->publication_gate, {})) {
   gate->demand = demand_;
   gate->generation = probe_->generation;
   gate->entered.set_value();
   gate->release_timed_out.store(gate->released.wait_for(5s) != std::future_status::ready);
  }
 }
 bool RollbackOutputPublication() noexcept override {
  GalleryReadySink wake;
  {
   std::scoped_lock lock(probe_->mutex);
   if (!checkpoint_) return true;
   ++probe_->rollbacks;
   if (probe_->fail_rollback) return false;
   ++probe_->quiescences;
   probe_->aborted_assignments += probe_->assignments.size();
   probe_->assignments = std::move(checkpoint_->assignments);
   probe_->visible = std::move(checkpoint_->visible);
   probe_->completed_slots = std::move(checkpoint_->completed_slots);
   probe_->priority_slots = std::move(checkpoint_->priority_slots);
   probe_->generation = checkpoint_->generation;
   probe_->nproc = checkpoint_->nproc;
   probe_->next_slot = checkpoint_->next_slot;
   probe_->cumulative = checkpoint_->cumulative;
   probe_->layout = checkpoint_->layout;
   checkpoint_.reset();
   wake = probe_->ready_sink;
   probe_->changed.notify_all();
  }
  if (wake) wake();
  return true;
 }
 std::vector<ExploreLabel> Labels() const override {
  std::scoped_lock lock(probe_->mutex);
  if (std::exchange(probe_->fail_next_labels, false)) throw std::runtime_error("deterministic Explore prepared-label failure");
  return {};
 }
 ExploreGalleryPublication BeginGallery(const ExploreRenderPlan& plan, const ExploreOrderCandidate* candidate, const std::size_t nproc, const mmltk::frameworks::gpu::ImagePlaneView clean,
  const mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t) override {
  std::shared_ptr<ExplorePostRenderGate> gate;
  {
   std::scoped_lock lock(probe_->mutex);
   if (std::exchange(probe_->fail_next_render, false)) throw std::runtime_error("deterministic streaming Explore render failure");
   gate = probe_->render_gate;
  }
  if (gate && gate->calls.fetch_add(1U, std::memory_order_acq_rel) == gate->target_call) {
   gate->entered.set_value();
   gate->released.wait();
  }
  {
   std::scoped_lock lock(probe_->mutex);
   if (probe_->generation == plan.generation) {
    RestorePixels(clean, semantic);
    PrioritizeLocked(plan);
    probe_->next_slot = 0U;
    return FactsLocked(0U);
   }
  }
  Fill(clean, 0x11U);
  Fill(semantic, 0U);
  std::scoped_lock lock(probe_->mutex);
  probe_->generation = plan.generation;
  probe_->layout = test_atlas_layout(plan, clean);
  probe_->visible = Visible(plan.viewport, candidate).visible_indices;
  probe_->completed_slots.assign(probe_->visible.size(), 0U);
  probe_->nproc = nproc;
  probe_->next_slot = 0U;
  probe_->cumulative = 0U;
  PrioritizeLocked(plan);
  return FactsLocked(0U);
 }
 ExploreGalleryPublication AdvanceGallery() override {
  std::scoped_lock lock(probe_->mutex);
  const auto before = probe_->assignments.size();
  std::erase_if(probe_->assignments, [this](const auto& assignment) { return assignment.ready && assignment.generation != probe_->generation; });
  const auto discarded = before - probe_->assignments.size();
  probe_->stale += discarded;
  if (probe_->allocation_allowed) ScheduleLocked();
  for (auto& assignment : probe_->assignments) {
   if (assignment.generation != probe_->generation) continue;
   if (!assignment.read_started && probe_->pre_read_allowed) {
    assignment.read_started = true;
    ++probe_->mapped_reads;
   }
   if (assignment.read_started && probe_->post_read_allowed) assignment.ready = true;
  }
  probe_->changed.notify_all();
  return FactsLocked(discarded);
 }
 bool HasGalleryTiles() const override {
  std::scoped_lock lock(probe_->mutex);
  return std::ranges::any_of(probe_->assignments, [this](const auto& assignment) { return assignment.ready && !assignment.gpu_pending && assignment.generation == probe_->generation; });
 }
 ExploreGalleryPublication PublishGalleryTiles(const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t) override {
  GalleryReadySink wake;
  ExploreGalleryPublication facts;
  {
   std::scoped_lock lock(probe_->mutex);
   if (probe_->fail_callback_admission) throw std::runtime_error("deterministic Explore callback admission failure");
   RestorePixels(clean, semantic);
   for (auto& assignment : probe_->assignments) {
    if (!assignment.ready || assignment.gpu_pending || assignment.generation != probe_->generation) continue;
    FillSlot(clean, assignment.slot, probe_->visible.size(), static_cast<std::uint8_t>(0x40U + assignment.compiled_index));
    FillSlot(semantic, assignment.slot, probe_->visible.size(), static_cast<std::uint8_t>(0x80U + assignment.compiled_index));
    assignment.gpu_pending = true;
   }
   if (probe_->callback_completion_allowed) {
    ++probe_->callback_completions;
    std::erase_if(probe_->assignments, [this](const auto& assignment) {
     if (!assignment.gpu_pending || assignment.generation != probe_->generation) return false;
     probe_->completed_slots[assignment.slot] = 1U;
     ++probe_->cumulative;
     return true;
    });
   }
   ++probe_->publications;
   facts = FactsLocked(0U);
   probe_->changed.notify_all();
   if (probe_->callback_completion_allowed) wake = probe_->ready_sink;
  }
  if (wake) wake();
  return facts;
 }
 void RenderDetail(const ExploreRenderPlan& plan, std::size_t, const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t) override {
  Fill(clean, static_cast<std::uint8_t>(0x60U + plan.selected_image.value_or(0U)));
  Fill(semantic, 0U);
  std::scoped_lock lock(probe_->mutex);
  probe_->generation = 0U;
  probe_->assignments.clear();
 }

private:
 using PublicationCheckpoint = StreamingExplorePublicationState;
 void RestorePixels(const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic) const {
  Fill(clean, 0x11U);
  Fill(semantic, 0U);
  const auto place = [&](const std::size_t slot, const std::uint32_t image) {
   FillSlot(clean, slot, probe_->visible.size(), static_cast<std::uint8_t>(0x40U + image));
   FillSlot(semantic, slot, probe_->visible.size(), static_cast<std::uint8_t>(0x80U + image));
  };
  for (std::size_t slot = 0U; slot < probe_->completed_slots.size(); ++slot)
   if (probe_->completed_slots[slot]) place(slot, probe_->visible[slot]);
  for (const auto& assignment : probe_->assignments)
   if (assignment.gpu_pending && assignment.generation == probe_->generation) place(assignment.slot, assignment.compiled_index);
 }
 void PrioritizeLocked(const ExploreRenderPlan& plan) {
  probe_->scroll_direction = plan.scroll_direction;
  probe_->priority_slots.clear();
  for (std::size_t slot = 0U; slot != probe_->visible.size(); ++slot) probe_->priority_slots.push_back(static_cast<std::uint32_t>(slot));
 }
 void ClearStreamingState() noexcept {
  std::scoped_lock lock(probe_->mutex);
  ++probe_->quiescences;
  probe_->aborted_assignments += probe_->assignments.size();
  probe_->assignments.clear();
  probe_->visible.clear();
  probe_->priority_slots.clear();
  probe_->generation = 0U;
  probe_->next_slot = 0U;
  probe_->cumulative = 0U;
 }
 static void FillSlot(const mmltk::frameworks::gpu::ImagePlaneView plane, const std::size_t slot, const std::size_t count, const std::uint8_t value) {
  const auto width = plane.descriptor.width / static_cast<std::uint32_t>(std::max<std::size_t>(count, 1U));
  for (std::uint32_t y = 0U; y != plane.descriptor.height; ++y) {
   auto* row = reinterpret_cast<std::uint8_t*>(plane.data) + y * plane.descriptor.pitch_bytes;
   std::memset(row + slot * width * 4U, value, width * 4U);
  }
 }
 void ScheduleLocked() {
  while (probe_->assignments.size() < probe_->nproc && probe_->next_slot != probe_->visible.size()) {
   const auto slot = probe_->priority_slots[probe_->next_slot++];
   if (probe_->completed_slots[slot] != 0U || std::ranges::any_of(probe_->assignments, [&](const auto& work) { return work.generation == probe_->generation && work.slot == slot; })) continue;
   probe_->assignments.push_back({
    .generation = probe_->generation,
    .compiled_index = probe_->visible[slot],
    .slot = slot,
   });
   ++probe_->lane_preparations;
   ++probe_->queued_closures;
  }
  probe_->maximum_active = std::max(probe_->maximum_active, probe_->assignments.size());
  probe_->changed.notify_all();
 }
 [[nodiscard]] ExploreGalleryPublication FactsLocked(const std::size_t stale) const {
  return {
   .generation = probe_->generation,
   .layout = probe_->layout,
   .ready_slots = probe_->completed_slots,
   .cumulative_tiles = probe_->cumulative,
   .remaining_tiles = probe_->visible.size() - probe_->cumulative,
   .active_pinned_bytes = probe_->assignments.size() * 64U,
   .stale_discarded = stale,
  };
 }
 std::shared_ptr<StreamingExploreProbe> probe_;
 ExploreDemandCheck demand_;
 bool demand_bound_ = false;
 std::vector<std::uint32_t> order_;
 std::uint64_t candidate_generation_ = 0U;
 std::optional<PublicationCheckpoint> checkpoint_;
};
[[nodiscard]] inline VisualRuntimeFactory streaming_explore_runtime_factory(std::shared_ptr<FakeImageBackend> backend, std::shared_ptr<StreamingExploreProbe> probe) {
 return RuntimeFactory(
  0, std::move(backend), mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic, [probe = std::move(probe)] { return std::make_unique<ControlledStreamingExploreAlgorithm>(probe); }, 3U);
}
class TestExploreAlgorithm : public SynchronousExploreAlgorithm {
public:
 explicit TestExploreAlgorithm(std::shared_ptr<std::atomic<std::size_t>> observed_nproc, std::shared_ptr<ExploreRenderGate> gate = {}, std::shared_ptr<std::atomic_uint64_t> commits = {},
  std::shared_ptr<ExploreFinalizationGate> finalization_gate = {}, std::shared_ptr<ExploreWorkProbe> work_probe = {}, std::shared_ptr<ExplorePostRenderGate> post_render_gate = {},
  std::shared_ptr<ExploreDetailExtentProbe> detail_extent = {})  // CLEANUP-IGNORE: This test algorithm has its own injected controls.
     : observed_nproc_(std::move(observed_nproc)),               // CLEANUP-IGNORE: Distinct test algorithms directly retain their own injected controls.
       gate_(std::move(gate)),
       commits_(std::move(commits)),
       finalization_gate_(std::move(finalization_gate)),
       work_probe_(std::move(work_probe)),
       post_render_gate_(std::move(post_render_gate)),
       detail_extent_(std::move(detail_extent)) {}
 ExploreOpened Open(const std::string_view source, std::stop_token) override {
  if (work_probe_) work_probe_->opens.fetch_add(1U, std::memory_order_release);
  if (source == "/failed") throw std::runtime_error("deterministic Explore open failure");
  candidate_order_ = {0U, 1U, 2U};
  candidate_render_failure_ = source == "/render-failed";
  if (source == "/allocation-failed") throw std::bad_alloc{};
  const std::vector<mmltk::backend::data::catalog::ClassName> class_names =
   source == "/different-catalog" ? std::vector<mmltk::backend::data::catalog::ClassName>{{"animal"}, {"building"}} : std::vector<mmltk::backend::data::catalog::ClassName>{{"person"}, {"vehicle"}};
  return {.dataset = {.image_count = 3U, .image_width = 64U, .image_height = 64U, .class_names = class_names},
   .order = {.matching_count = static_cast<std::uint32_t>(candidate_order_.size()), .visible_indices = candidate_order_}};
 }
 ExploreOrderCandidate PrepareFilter(const ExploreFilter& filter, std::uint64_t seed, std::size_t, std::stop_token) override {
  if (work_probe_) work_probe_->prepares.fetch_add(1U, std::memory_order_release);
  if (filter.minimum_instances == 7U) throw std::bad_alloc{};
  if (filter.minimum_instances == 8U) candidate_render_failure_ = true;
  if (candidate_order_.empty()) candidate_order_ = order_;
  return {.filter = filter,
   .order = {.matching_count = static_cast<std::uint32_t>(candidate_order_.size()), .shuffle_seed = seed, .visible_indices = candidate_order_},
   .generation = ++candidate_generation_};
 }
 void Commit(ExploreOrderCandidate) noexcept override {
  order_ = std::move(candidate_order_);
  candidate_render_failure_ = false;
  if (commits_) commits_->fetch_add(1U, std::memory_order_release);
 }
 void DiscardCandidate() override {
  candidate_order_.clear();
  candidate_render_failure_ = false;
 }
 void Reset() noexcept override {
  order_.clear();
  candidate_order_.clear();
  candidate_render_failure_ = false;
  candidate_generation_ = 0U;
 }
 ExploreOrderFacts Visible(const ExploreViewport viewport, const ExploreOrderCandidate* candidate = nullptr) const override {
  const auto& order = candidate == nullptr ? order_ : candidate->order.visible_indices;
  ExploreOrderFacts facts{
   .matching_count = static_cast<std::uint32_t>(order.size()),
   .shuffle_seed = candidate == nullptr ? 0U : candidate->order.shuffle_seed,
  };
  if (candidate != nullptr) {
   if (finalization_gate_ && finalization_gate_->armed.exchange(false, std::memory_order_acq_rel)) {
    finalization_gate_->entered.set_value();
    finalization_gate_->released.wait();
   }
  }
  facts.visible_indices = visible_test_order(order, viewport);
  return facts;
 }
 bool Contains(std::uint32_t value) const override { return std::ranges::find(order_, value) != order_.end(); }
 VisualExtent DetailExtent(const ExploreRenderPlan& plan) const override { return detail_extent_ ? detail_extent_->padded : ExploreAlgorithm::DetailExtent(plan); }
 VisualRegion DetailContent(const ExploreRenderPlan&) const override {
  if (!detail_extent_) return {};
  return {(detail_extent_->padded.width - detail_extent_->original.width) / 2U, (detail_extent_->padded.height - detail_extent_->original.height) / 2U, detail_extent_->original.width,
   detail_extent_->original.height};
 }
 std::optional<std::uint32_t> Adjacent(std::uint32_t selected, std::int64_t offset) const override { return static_cast<std::uint32_t>((static_cast<std::int64_t>(selected) + offset % 3 + 3) % 3); }
 void RenderProduct(const ExploreRenderPlan& plan, const ExploreOrderCandidate* candidate, const std::size_t nproc, const mmltk::frameworks::gpu::ImagePlaneView clean,
  const mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t) override {
  if (candidate != nullptr && candidate_render_failure_) throw std::runtime_error("deterministic Explore candidate render failure");
  if (work_probe_) work_probe_->renders.fetch_add(1U, std::memory_order_release);
  if (work_probe_) {
   work_probe_->rendered_copy_paste_probability.store(plan.augmentation_config.copy_paste_probability, std::memory_order_release);
   const auto visible = Visible(plan.viewport, candidate).visible_indices;
   work_probe_->rendered_count.store(visible.size(), std::memory_order_release);
   for (std::size_t slot = 0U; slot != work_probe_->rendered_slots.size(); ++slot)
    work_probe_->rendered_slots[slot].store(slot < visible.size() ? visible[slot] : std::numeric_limits<std::uint32_t>::max(), std::memory_order_release);
  }
  observed_nproc_->store(nproc, std::memory_order_release);
  if (gate_ && gate_->calls.fetch_add(1U, std::memory_order_acq_rel) == 1U) {
   gate_->entered.set_value();
   gate_->released.wait();
  }
  Fill(clean, static_cast<std::uint8_t>(plan.generation));
  Fill(semantic, 0x5aU);
  if (post_render_gate_ && post_render_gate_->calls.fetch_add(1U, std::memory_order_acq_rel) == post_render_gate_->target_call) {
   post_render_gate_->entered.set_value();
   post_render_gate_->released.wait();
  }
 }

private:
 std::shared_ptr<std::atomic<std::size_t>> observed_nproc_;
 std::shared_ptr<ExploreRenderGate> gate_;
 std::shared_ptr<std::atomic_uint64_t> commits_;
 std::shared_ptr<ExploreFinalizationGate> finalization_gate_;
 std::shared_ptr<ExploreWorkProbe> work_probe_;
 std::shared_ptr<ExplorePostRenderGate> post_render_gate_;
 std::shared_ptr<ExploreDetailExtentProbe> detail_extent_;
 std::vector<std::uint32_t> order_;
 std::vector<std::uint32_t> candidate_order_;
 std::uint64_t candidate_generation_ = 0U;
 bool candidate_render_failure_ = false;
};
class OpenedExplore final {
public:
 OpenedExplore(std::shared_ptr<FakeImageBackend> backend, const VisualExtent extent)
     : explore_(settings_.system(), kDevice, 2U,
        RuntimeFactory(
         0, std::move(backend), mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic, [] { return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U)); },
         3U),
        [this](ExploreSystem::event_type) { events_.Advance(); }) {
  static_cast<void>(explore_.Open({.viewport = {.extent = extent, .columns = extent.width / extent.height}, .compiled_source = "/test"}));
  REQUIRE(events_.Wait([this] { return explore_.snapshot().ready; }));
 }
 [[nodiscard]] ExploreSystem& system() noexcept { return explore_; }
 void Reopen(const VisualExtent extent) {
  const auto admitted = explore_.Open({.viewport = {.extent = extent, .columns = extent.width / extent.height}, .compiled_source = "/reopened"});
  REQUIRE(events_.Wait([this, admitted] { return explore_.snapshot().ready && explore_.snapshot().revision > admitted.revision; }));
 }

private:
 EventGate events_;
 LoadedSettings settings_;
 ExploreSystem explore_;
};
class ExploreScenario final {
public:
 using ModelFactory = std::function<std::unique_ptr<mmltk::frameworks::gpu::SystemImageModel>()>;
 using Observer = std::function<void(ExploreSystem::event_type)>;
 ExploreScenario(LoadedSettings& settings, std::shared_ptr<FakeImageBackend> backend, ModelFactory model = {}, Observer observer = {}, VisualDiagnosticSink diagnostics = {})
     : ExploreScenario(settings, 2U, RuntimeFactory(0, std::move(backend), mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic, model ? std::move(model) : DefaultModel(), 3U),
        std::move(observer), diagnostics) {}
 ExploreScenario(LoadedSettings& settings, const std::size_t nproc, VisualRuntimeFactory runtime, Observer observer = {}, VisualDiagnosticSink diagnostics = {})
     : observer_(std::move(observer)),
       explore_(
        settings.system(), kDevice, nproc, std::move(runtime),
        [this](ExploreSystem::event_type event) {
         if (observer_) observer_(std::move(event));
         events_.Advance();
        },
        diagnostics) {}
 [[nodiscard]] ExploreSystem& system() noexcept { return explore_; }
 void OpenAndWait(const ExploreViewport viewport, const std::string_view compiled_source = "/test") {
  const auto admitted = explore_.Open({.viewport = viewport, .compiled_source = std::string{compiled_source}});
  REQUIRE(Wait([this, admitted] {
   const auto current = explore_.snapshot();
   return current.ready && !current.busy && current.revision > admitted.revision;
  }));
 }
 [[nodiscard]] bool Wait(std::function<bool()> predicate) { return events_.Wait(std::move(predicate)); }
 [[nodiscard]] static ModelFactory TrackCommits(std::shared_ptr<std::atomic_uint64_t> commits) {
  return [commits = std::move(commits)] { return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr, commits); };
 }
 [[nodiscard]] static ModelFactory TrackWork(std::shared_ptr<ExploreWorkProbe> work) {
  return [work = std::move(work)] { return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr, nullptr, nullptr, work); };
 }
 [[nodiscard]] static ModelFactory GateAfterRender(std::shared_ptr<ExplorePostRenderGate> gate, std::shared_ptr<std::atomic_uint64_t> commits = {}, std::shared_ptr<ExploreWorkProbe> work = {}) {
  return [gate = std::move(gate), commits = std::move(commits), work = std::move(work)] {
   return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr, commits, nullptr, work, gate);
  };
 }

private:
 [[nodiscard]] static ModelFactory DefaultModel() {
  return [] { return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U)); };
 }
 Observer observer_;
 EventGate events_;
 ExploreSystem explore_;
};
[[nodiscard]] inline ExploreScenario::Observer count_explore_failures(std::shared_ptr<std::atomic_uint64_t> failures) {
 return [failures = std::move(failures)](ExploreSystem::event_type event) {
  if (std::holds_alternative<ExploreFailed>(event)) failures->fetch_add(1U, std::memory_order_acq_rel);
 };
}
class StreamingExploreFixture final {
public:
 explicit StreamingExploreFixture(const std::size_t nproc)
     : backend_(std::make_shared<FakeImageBackend>()),
       probe_(std::make_shared<StreamingExploreProbe>()),
       failures_(std::make_shared<std::atomic_uint64_t>(0U)),
       scenario_(settings_, nproc, streaming_explore_runtime_factory(backend_, probe_), count_explore_failures(failures_)) {}
 [[nodiscard]] LoadedSettings& settings() noexcept { return settings_; }
 [[nodiscard]] FakeImageBackend& backend() noexcept { return *backend_; }
 [[nodiscard]] StreamingExploreProbe& probe() noexcept { return *probe_; }
 [[nodiscard]] std::uint64_t failure_count() const noexcept { return failures_->load(std::memory_order_acquire); }
 [[nodiscard]] ExploreSystem& system() noexcept { return scenario_.system(); }
 void OpenAndWait(const ExploreViewport viewport) { scenario_.OpenAndWait(viewport, "/stream"); }
 [[nodiscard]] bool Wait(std::function<bool()> predicate) { return scenario_.Wait(std::move(predicate)); }
 void ReleaseAndWaitForFrameAfter(const std::uint64_t revision) {
  probe_->ReleaseAll();
  REQUIRE(Wait([&] { return system().snapshot().frame.revision > revision; }));
 }

private:
 LoadedSettings settings_;
 std::shared_ptr<FakeImageBackend> backend_;
 std::shared_ptr<StreamingExploreProbe> probe_;
 std::shared_ptr<std::atomic_uint64_t> failures_;
 ExploreScenario scenario_;
};
[[nodiscard]] inline ExactVisualDocumentBorrower borrow_exactly_from(ExploreSystem& explore) {
 return [&explore](const VisualFrame& frame) {
  if (frame.source != explore.snapshot().frame.source) return VisualDocumentRead{};
  return test_document(borrow_matching_visual_product(frame, explore.BorrowFrame()));
 };
}
}  // namespace mmltk::controller::visual_test_support
