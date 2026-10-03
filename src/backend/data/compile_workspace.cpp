#include "src/backend/data/detail/compile_workspace.h"
#include "src/backend/data/detail/compile_pixel_workspace.h"
#include "src/backend/data/detail/compiled_label_workspace.h"
#include "src/backend/data/compiled/compiled_file_utils.h"
#include <stb_image.h>
#include <array>
#include "src/backend/data/detail/compile_resources.h"
#include "src/common/system/numa_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <system_error>
#include <string>
namespace mmltk::backend::data::detail {
namespace {
using common::math::checked_add;
using common::math::checked_multiply;
using common::system::NumaMemory;
using common::system::page_rounded_bytes;
}  // namespace
CompileWorkspacePlan compile_workspace_plan(std::size_t workers, std::uint32_t resolution, bool perceptual, std::size_t reader_window, std::uint64_t explicit_budget, std::size_t encoded_window) {
 if (explicit_budget) return CompileWorkspacePlan::prepare(workers, {}, 0, 0, explicit_budget);
 constexpr std::array enabled{CompileWorkClass::Acquisition, CompileWorkClass::Extraction, CompileWorkClass::Annotations, CompileWorkClass::Pixels, CompileWorkClass::Gather, CompileWorkClass::Output};
 return compile_workspace_plan(
  workers, {std::max(1U, resolution), std::max(1U, resolution), imaging::resample::ImageResizeMode::Stretch, perceptual}, enabled, {reader_window, 0}, encoded_window, 0, reader_window);
}
CompileWorkspacePlan compile_workspace_plan(std::size_t workers, CompilePixelPolicy policy, std::span<const CompileWorkClass> enabled, CompileWorkspaceProfile annotations, std::size_t encoded_window,
 std::size_t pixel_input_bytes, std::size_t source_window) {
 // These normal shapes choose reusable capacity, never legal-input admission.
 constexpr std::uint32_t normal_extent = 2048;
 const CompileImageHeader normal{
  normal_extent, normal_extent, 0, false, CompileImageEncoding::Png, stbi_workspace_png_profile(normal_extent, normal_extent, std::max(encoded_window, pixel_input_bytes))
 };
 const auto pixels = checked_add(compile_pixel_workspace_plan(normal, policy).bytes(), std::max(encoded_window, pixel_input_bytes), "pixel input workspace overflow");
 const auto labels = CompiledLabelWorkspace::required_bytes(normal_extent);
 const std::array<CompileWorkspaceProfile, static_cast<std::size_t>(CompileWorkClass::Count)> available{
  {{64U << 10, 64U << 10}, {std::max(encoded_window, annotations.normal_bytes), 64U << 10}, {std::max(labels, annotations.normal_bytes), std::max(labels, annotations.completion_bytes)},
   {pixels, 64U << 10}, {labels, 64U << 10}, {labels, labels}}
 };
 std::array<CompileWorkspaceProfile, static_cast<std::size_t>(CompileWorkClass::Count)> profiles{};
 for (const auto kind : enabled) {
  const auto index = static_cast<std::size_t>(kind);
  if (index >= profiles.size()) throw std::invalid_argument("invalid enabled compile workspace class");
  profiles[index] = available[index];
 }
 return CompileWorkspacePlan::prepare(workers, profiles, std::max(encoded_window, source_window), COMPILED_COMPACTION_SCRATCH_BYTES);
}
CompileWorkspacePlan CompileWorkspacePlan::prepare(std::size_t workers, std::span<const CompileWorkspaceProfile> profiles, std::size_t shared, std::size_t compaction, std::uint64_t explicit_budget) {
 if (!workers || profiles.size() > static_cast<std::size_t>(CompileWorkClass::Count)) throw std::invalid_argument("compile workspace plan requires CPUs and canonical class profiles");
 CompileWorkspacePlan result;
 result.shared_bytes = page_rounded_bytes(shared);
 result.compaction_bytes = page_rounded_bytes(compaction);
 auto reserved = checked_add(result.shared_bytes, result.compaction_bytes, "compile protected workspace overflow");
 for (const auto& profile : profiles) {
  const auto completion = page_rounded_bytes(profile.completion_bytes);
  result.completion.push_back(completion);
  reserved = checked_add(reserved, completion, "compile completion workspace overflow");
 }
 result.protected_bytes = reserved;
 if (reserved > ceiling) throw InsufficientCompileResources("compile service and completion windows exceed normal pinned ceiling");
 for (const auto& profile : profiles) {
  const auto bytes = page_rounded_bytes(profile.normal_bytes);
  if (bytes <= ceiling - reserved) result.context_bytes = std::max(result.context_bytes, bytes);
 }
 const auto requested = checked_add<std::uint64_t>(reserved, checked_multiply<std::uint64_t>(workers, result.context_bytes, "compile context workspace overflow"), "compile workspace overflow");
 result.budget = explicit_budget ? page_rounded_bytes(explicit_budget) : std::min(ceiling, std::max<std::uint64_t>(256ULL << 20, requested));
 if (result.budget < reserved || result.budget > ceiling) throw InsufficientCompileResources("compile pinned budget does not cover protected windows");
 auto available = result.budget - reserved;
 const auto quantum = common::system::host_page_size();
 if (!result.context_bytes) result.context_bytes = std::max<std::uint64_t>(quantum, available / workers / quantum * quantum);
 // Prepare a fitting context for each enabled class first. Any lane may borrow
 // these stable slices; CPU class floors never imply imaginary storage.
 if (workers >= static_cast<std::size_t>(std::ranges::count_if(profiles, [](const auto& profile) { return profile.normal_bytes || profile.completion_bytes; }))) {
  std::vector<std::size_t> capacities;
  for (const auto& profile : profiles)
   if (profile.normal_bytes) capacities.push_back(page_rounded_bytes(profile.normal_bytes));
  std::ranges::sort(capacities, std::greater<>{});
  for (auto extent : capacities) {
   if (extent > available || result.context_capacities.size() == workers) continue;
   result.context_capacities.push_back(extent);
   available -= extent;
  }
 }
 while (result.context_capacities.size() < workers && result.context_bytes <= available) {
  result.context_capacities.push_back(result.context_bytes);
  available -= result.context_bytes;
 }
 if (result.context_capacities.empty() && available >= quantum) result.context_capacities.push_back(available / quantum * quantum);
 result.contexts = result.context_capacities.size();
 result.context_bytes = 0;
 for (auto extent : result.context_capacities) result.context_bytes = std::max(result.context_bytes, extent);
 return result;
}
namespace {
// Allocated links live in the existing shared lease control. Free links live at
// the end of their own unused extent. No allocator node or payload allocation is
// needed to split or coalesce, and no metadata occupies an admitted byte.
struct ArenaBlock {
 std::byte* data = nullptr;
 std::size_t size = 0;
 ArenaBlock *previous = nullptr, *next = nullptr;
 ArenaBlock *free_previous = nullptr, *free_next = nullptr;
 bool free = false;
};
static_assert(sizeof(ArenaBlock) <= CompileArenaBank::alignment);
static_assert(CompileArenaBank::alignment % alignof(ArenaBlock) == 0);
}  // namespace
struct CompileArenaLease::State {
 ArenaBlock block;
 std::span<std::byte> memory;
 std::size_t requested = 0, region = 0;
 int node = -1;
 bool oversized = false, acquired = false;
 std::unique_ptr<NumaMemory> exceptional;
 std::shared_ptr<CompileArenaBank::State> bank;
 mutable std::mutex mutex;
 ~State();
};
struct CompileArenaBank::State {
 struct Region {
  std::span<std::byte> bytes;
  CompileWorkClass completion = CompileWorkClass::Count;
  CompileWorkspaceWindow window = CompileWorkspaceWindow::Task;
  std::array<ArenaBlock*, std::numeric_limits<std::size_t>::digits> bins{};
  std::uint64_t nonempty = 0;
  std::size_t free_bytes = 0, observed_end = 0;
  [[nodiscard]] bool ordinary() const noexcept { return window == CompileWorkspaceWindow::Task && completion == CompileWorkClass::Count; }
  [[nodiscard]] bool eligible(bool completing, CompileWorkClass kind, CompileWorkspaceWindow requested) const noexcept {
   return ordinary() || (window != CompileWorkspaceWindow::Task && window == requested) || (completing && kind != CompileWorkClass::Count && completion == kind);
  }
  [[nodiscard]] static std::size_t bin(std::size_t size) noexcept { return std::bit_width(size / alignment) - 1; }
  void unlink(ArenaBlock& block) noexcept {
   const auto index = bin(block.size);
   if (block.free_previous)
    block.free_previous->free_next = block.free_next;
   else
    bins[index] = block.free_next;
   if (block.free_next) block.free_next->free_previous = block.free_previous;
   if (!bins[index]) nonempty &= ~(std::uint64_t{1} << index);
  }
  ArenaBlock* insert(std::byte* data, std::size_t size, ArenaBlock* previous, ArenaBlock* next) noexcept {
   const auto index = bin(size);
   auto* block = std::construct_at(reinterpret_cast<ArenaBlock*>(data + size - alignment), ArenaBlock{data, size, previous, next, nullptr, bins[index], true});
   if (previous) previous->next = block;
   if (next) next->previous = block;
   if (bins[index]) bins[index]->free_previous = block;
   bins[index] = block;
   nonempty |= std::uint64_t{1} << index;
   return block;
  }
  [[nodiscard]] ArenaBlock* find(std::size_t size) const noexcept {
   if (size > free_bytes) return nullptr;
   const auto index = bin(size);
   // Only the size's own bin can contain undersized fragments. Every block in
   // the next occupied bin fits; bounded bin selection skips all smaller ones.
   ArenaBlock* best = nullptr;
   for (auto* block = bins[index]; block; block = block->free_next)
    if (block->size >= size && (!best || block->size < best->size)) {
     best = block;
     if (block->size == size) return best;
    }
   if (best) return best;
   const auto larger = nonempty & (~std::uint64_t{1} << index);
   return larger ? bins[std::countr_zero(larger)] : nullptr;
  }
  [[nodiscard]] std::size_t largest() const noexcept {
   if (!nonempty) return 0;
   std::size_t result = 0;
   for (auto* block = bins[std::bit_width(nonempty) - 1]; block; block = block->free_next) result = std::max(result, block->size);
   return result;
  }
  void acquire(ArenaBlock& free, ArenaBlock& owned, std::size_t size) noexcept {
   unlink(free);
   owned = free;
   owned.free = false;
   owned.free_previous = owned.free_next = nullptr;
   if (owned.previous) owned.previous->next = &owned;
   if (owned.next) owned.next->previous = &owned;
   const auto remaining = owned.size - size;
   owned.size = size;
   if (remaining) owned.next = insert(owned.data + size, remaining, &owned, owned.next);
   free_bytes -= size;
  }
  void release(ArenaBlock& owned) noexcept {
   auto* data = owned.data;
   auto size = owned.size;
   auto* previous = owned.previous;
   auto* next = owned.next;
   free_bytes += size;
   if (previous && previous->free) {
    unlink(*previous);
    data = previous->data;
    size += previous->size;
    previous = previous->previous;
   }
   if (next && next->free) {
    unlink(*next);
    size += next->size;
    next = next->next;
   }
   insert(data, size, previous, next);
  }
 };
 CompileWorkspacePlan plan;
 std::vector<NumaMemory> memory;
 std::vector<Region> regions;
 std::function<void()> released;
 mutable std::mutex mutex;
 std::uint64_t leased = 0, exceptional = 0, exceptional_locked = 0, peak = 0, reused = 0, retired = 0;
 int exceptional_node = -1;
 bool observing = false;
 std::uint64_t exceptional_requested = 0;
 std::uint64_t observed_live = 0, observed_occupied = 0, lease_acquisitions = 0, allocation_high_water = 0, occupied_high_water = 0;
 void observe_acquire(std::uint64_t bytes) noexcept {
  if (!observing) return;
  ++lease_acquisitions;
  observed_occupied += bytes;
  allocation_high_water = std::max(allocation_high_water, ++observed_live);
  occupied_high_water = std::max(occupied_high_water, observed_occupied);
 }
 void observe_release(std::uint64_t bytes) noexcept {
  if (!observing || !bytes) return;
  --observed_live;
  observed_occupied -= bytes;
 }
 struct Selection {
  std::size_t region = 0;
  ArenaBlock* block = nullptr;
 };
 // The caller holds mutex. Protected windows are separate regions even when
 // they share one physical allocation; coalescing never crosses their edges.
 [[nodiscard]] Selection select(std::size_t bytes, bool completion, CompileWorkClass kind, CompileWorkspaceWindow window) const {
  Selection selected;
  bool selected_ordinary = false;
  for (std::size_t i = 0; i < regions.size(); ++i) {
   const auto& value = regions[i];
   if (!value.eligible(completion, kind, window)) continue;
   const bool ordinary = value.ordinary();
   if (selected.block && selected_ordinary && !ordinary) continue;
   auto* block = value.find(bytes);
   if (block && (!selected.block || (ordinary && !selected_ordinary) || (ordinary == selected_ordinary && block->size < selected.block->size))) {
    selected = {i, block};
    selected_ordinary = ordinary;
   }
  }
  return selected;
 }
 void release(CompileArenaLease::State& lease) {
  {
   const std::lock_guard lock(mutex);
   if (lease.oversized) {
    observe_release(exceptional);
    exceptional = 0;
    if (observing) exceptional_requested = 0;
    if (observing && exceptional_locked) ++retired;
    exceptional_locked = 0;
   } else {
    observe_release(lease.block.size);
    regions[lease.region].release(lease.block);
    leased -= lease.requested;
   }
  }
  if (released) released();
 }
 void materialized(std::size_t extent) {
  {
   const std::lock_guard lock(mutex);
   exceptional_locked = extent;
   if (observing) peak = std::max(peak, plan.budget + extent);
  }
  if (released) released();
 }
};
CompileArenaLease::State::~State() {
 exceptional.reset();  // Physical retirement precedes eligibility publication.
 if (acquired) bank->release(*this);
}
CompileArenaBank::CompileArenaBank(const CompileWorkspacePlan& plan, std::span<const int> nodes, std::function<void()> released, bool observe) : state_(std::make_shared<State>()) {
 if (nodes.empty()) throw std::invalid_argument("compile arena has no resolved NUMA placement");
 const auto page = common::system::host_page_size();
 auto protected_bytes = checked_add(plan.shared_bytes, plan.compaction_bytes, "compile protected workspace overflow");
 for (auto bytes : plan.completion) {
  if (bytes % page) throw std::invalid_argument("compile completion window is not page aligned");
  protected_bytes = checked_add(protected_bytes, bytes, "compile protected workspace overflow");
 }
 auto committed = protected_bytes;
 std::size_t largest = 0;
 for (auto bytes : plan.context_capacities) {
  if (!bytes || bytes % page) throw std::invalid_argument("compile context is not page aligned");
  committed = checked_add(committed, bytes, "compile arena extent overflow");
  largest = std::max(largest, bytes);
 }
 if (!plan.budget || plan.budget > CompileWorkspacePlan::ceiling || plan.budget % page || plan.shared_bytes % page || plan.compaction_bytes % page || protected_bytes != plan.protected_bytes ||
     committed > plan.budget || plan.contexts != plan.context_capacities.size() || plan.contexts > nodes.size() || largest != plan.context_bytes ||
     plan.completion.size() > static_cast<std::size_t>(CompileWorkClass::Count))
  throw std::invalid_argument("compile arena requires a checked fitting workspace plan");
 auto& state = *state_;
 state.plan = plan;
 state.released = std::move(released);
 state.exceptional_node = nodes.front();
 state.observing = observe;
 // Preserve the prepared physical allocations and their resolved CPU placement.
 // Every ordinary byte, including the tail after protected windows, is usable.
 state.memory.reserve(plan.contexts + 1);
 state.regions.reserve(plan.contexts + plan.completion.size() + 3);
 const auto allocate = [&](std::size_t bytes, int node) {
  state.memory.emplace_back(node, NumaMemory::Lock::Pages);
  try {
   state.memory.back().ensure_bytes(bytes);
  } catch (const std::bad_alloc&) { throw std::system_error(std::make_error_code(std::errc::not_enough_memory), "prepare fixed pinned compile arena on NUMA node " + std::to_string(node)); }
  return std::span(static_cast<std::byte*>(state.memory.back().data()), state.memory.back().capacity_bytes());
 };
 const auto region = [&](std::span<std::byte> bytes, CompileWorkClass completion = CompileWorkClass::Count, CompileWorkspaceWindow window = CompileWorkspaceWindow::Task) {
  if (bytes.empty()) return;
  auto& value = state.regions.emplace_back();
  value.bytes = bytes;
  value.completion = completion;
  value.window = window;
  value.free_bytes = bytes.size();
  value.insert(bytes.data(), bytes.size(), nullptr, nullptr);
 };
 std::size_t used = 0;
 for (std::size_t i = 0; i < plan.context_capacities.size(); ++i) {
  const auto bytes = plan.context_capacities[i];
  used += bytes;
  region(allocate(bytes, nodes[i % nodes.size()]));
 }
 const auto remaining = static_cast<std::size_t>(plan.budget) - used;
 auto tail = remaining ? allocate(remaining, nodes.front()) : std::span<std::byte>{};
 for (std::size_t i = 0; i < plan.completion.size(); ++i)
  if (const auto bytes = plan.completion[i]) {
   region(tail.first(bytes), static_cast<CompileWorkClass>(i));
   tail = tail.subspan(bytes);
  }
 for (const auto window : {CompileWorkspaceWindow::SharedSource, CompileWorkspaceWindow::Compaction}) {
  const auto bytes = window == CompileWorkspaceWindow::SharedSource ? plan.shared_bytes : plan.compaction_bytes;
  if (bytes) {
   region(tail.first(bytes), CompileWorkClass::Count, window);
   tail = tail.subspan(bytes);
  }
 }
 region(tail);
}
const CompileWorkspacePlan& CompileArenaBank::plan() const noexcept { return state_->plan; }
bool CompileArenaBank::fits(std::size_t bytes, bool completion, CompileWorkClass kind, CompileWorkspaceWindow window) const {
 const std::lock_guard lock(state_->mutex);
 if (state_->exceptional) return false;
 if (!bytes) return true;
 if (!normal_fit(bytes, completion, kind, window)) return false;
 return state_->select(reservation_bytes(bytes, completion, kind, window), completion, kind, window).block != nullptr;
}
bool CompileArenaBank::normal_fit(std::size_t bytes, bool completion, CompileWorkClass kind, CompileWorkspaceWindow window) const { return bytes <= normal_capacity(completion, kind, window); }
std::size_t CompileArenaBank::normal_capacity(bool completion, CompileWorkClass kind, CompileWorkspaceWindow window) const noexcept {
 // Shape and protection are immutable after bank construction.
 std::size_t largest = 0;
 for (const auto& region : state_->regions)
  if (region.eligible(completion, kind, window)) largest = std::max(largest, region.bytes.size());
 return largest;
}
std::size_t CompileArenaBank::reservation_bytes(std::size_t bytes, bool completion, CompileWorkClass kind, CompileWorkspaceWindow window) const {
 if (!bytes) return 0;
 if (!normal_fit(bytes, completion, kind, window)) return page_rounded_bytes(bytes);
 return checked_add(bytes, alignment - 1, "compile arena alignment overflow") / alignment * alignment;
}
bool CompileArenaBank::exceptional_active() const {
 const std::lock_guard lock(state_->mutex);
 return state_->exceptional != 0;
}
CompileArenaLease CompileArenaBank::acquire(std::size_t bytes, bool exceptional, bool completion, CompileWorkClass kind, CompileWorkspaceWindow window) {
 if (!bytes) return {};
 const auto extent = reservation_bytes(bytes, completion, kind, window);
 auto lease = std::make_shared<CompileArenaLease::State>();
 lease->bank = state_;
 lease->requested = bytes;
 const std::lock_guard lock(state_->mutex);
 if (state_->exceptional) throw InsufficientCompileResources("compile exceptional lineage has not physically retired");
 const auto selected = state_->select(extent, completion, kind, window);
 if (selected.block) {
  const auto i = selected.region;
  auto& value = state_->regions[i];
  lease->memory = {selected.block->data, bytes};
  lease->region = i;
  value.acquire(*selected.block, lease->block, extent);
  lease->acquired = true;
  state_->observe_acquire(extent);
  if (state_->observing) {
   const auto offset = static_cast<std::size_t>(lease->block.data - value.bytes.data());
   if (offset < value.observed_end) ++state_->reused;
   value.observed_end = std::max(value.observed_end, offset + extent);
  }
  state_->leased += bytes;
  return CompileArenaLease(std::move(lease), 0, bytes);
 }
 if (!exceptional || state_->leased || normal_fit(bytes, completion, kind, window)) throw InsufficientCompileResources("compile arena has no fitting available extent");
 lease->oversized = true;
 lease->node = state_->exceptional_node;
 state_->exceptional = extent;
 lease->acquired = true;
 state_->observe_acquire(extent);
 if (state_->observing) state_->exceptional_requested = bytes;
 return CompileArenaLease(std::move(lease), 0, bytes);
}
CompileArenaBank::Snapshot CompileArenaBank::snapshot() const {
 const std::lock_guard lock(state_->mutex);
 Snapshot result{state_->plan.budget, state_->plan.budget + state_->exceptional_locked, state_->leased, state_->exceptional_locked, state_->peak, state_->exceptional, state_->reused, state_->retired};
 if (state_->observing) {
  auto& value = result.observation;
  value.lease_acquisitions = state_->lease_acquisitions;
  value.allocation_high_water = state_->allocation_high_water;
  value.occupied_high_water = state_->occupied_high_water;
  value.requested_bytes = state_->leased + state_->exceptional_requested;
  value.live_allocations = state_->observed_live;
  value.occupied_bytes = state_->observed_occupied;
  value.alignment_loss = value.occupied_bytes - value.requested_bytes;
  value.backing_regions = state_->memory.size() + (state_->exceptional_locked ? 1 : 0);
  std::uint64_t registered = 0;
  for (const auto& region : state_->regions) {
   const auto bytes = region.bytes.size();
   registered += bytes;
   const auto occupied = bytes - region.free_bytes;
   if (region.window == CompileWorkspaceWindow::SharedSource)
    value.shared_occupied += occupied;
   else if (region.window == CompileWorkspaceWindow::Compaction)
    value.compaction_occupied += occupied;
   else if (region.completion != CompileWorkClass::Count)
    value.completion_occupied += occupied;
   else {
    value.ordinary_capacity += bytes;
    value.largest_ordinary_capacity = std::max<std::uint64_t>(value.largest_ordinary_capacity, bytes);
    value.ordinary_free_bytes += region.free_bytes;
    value.largest_ordinary_extent = std::max<std::uint64_t>(value.largest_ordinary_extent, region.largest());
   }
  }
  value.unregistered_bytes = state_->plan.budget - registered;
 }
 return result;
}
std::uint64_t CompileArenaBank::largest_available(bool completion, CompileWorkClass kind, CompileWorkspaceWindow window) const {
 const std::lock_guard lock(state_->mutex);
 if (state_->exceptional) return 0;
 std::uint64_t largest = 0;
 for (const auto& region : state_->regions)
  if (region.eligible(completion, kind, window)) largest = std::max<std::uint64_t>(largest, region.largest());
 return largest;
}
std::span<std::byte> CompileArenaLease::bytes() const {
 if (!state_) return {};
 const std::lock_guard lock(state_->mutex);
 if (state_->oversized && !state_->exceptional) {
  auto memory = std::make_unique<NumaMemory>(state_->node, NumaMemory::Lock::Pages);
  memory->ensure_bytes(state_->requested);
  state_->memory = {static_cast<std::byte*>(memory->data()), memory->capacity_bytes()};
  state_->exceptional = std::move(memory);
  state_->bank->materialized(state_->memory.size());
 }
 return state_->memory.subspan(offset_, bytes_);
}
std::size_t CompileArenaLease::capacity() const noexcept { return state_ ? bytes_ : 0; }
bool CompileArenaLease::exceptional() const noexcept { return state_ && state_->oversized; }
CompileArenaLease CompileArenaLease::slice(std::size_t offset, std::size_t bytes) const {
 if (!state_ || offset > bytes_ || bytes > bytes_ - offset) throw std::out_of_range("compile arena slice exceeds its custody");
 return CompileArenaLease(state_, offset_ + offset, bytes);
}
}  // namespace mmltk::backend::data::detail
