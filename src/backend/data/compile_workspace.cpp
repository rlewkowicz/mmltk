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
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <system_error>
#include <string>
namespace mmltk::backend::data::detail {
namespace {
using common::system::NumaMemory;
using common::system::page_rounded_bytes;
using common::math::checked_add;
using common::math::checked_multiply;
}
CompileWorkspacePlan compile_workspace_plan(std::size_t workers,std::uint32_t resolution,bool perceptual,std::size_t reader_window,std::uint64_t explicit_budget,std::size_t encoded_window) {
 if (explicit_budget) return CompileWorkspacePlan::prepare(workers,{},0,0,explicit_budget);
 constexpr std::array enabled{CompileWorkClass::Acquisition,CompileWorkClass::Extraction,CompileWorkClass::Annotations,
  CompileWorkClass::Pixels,CompileWorkClass::Gather,CompileWorkClass::Output};
 return compile_workspace_plan(workers,{std::max(1U,resolution),std::max(1U,resolution),imaging::resample::ImageResizeMode::Stretch,perceptual},enabled,{reader_window,0},encoded_window,0,reader_window);
}
CompileWorkspacePlan compile_workspace_plan(std::size_t workers, CompilePixelPolicy policy, std::span<const CompileWorkClass> enabled,
 CompileWorkspaceProfile annotations, std::size_t encoded_window, std::size_t pixel_input_bytes, std::size_t source_window) {
 // These normal shapes choose reusable capacity, never legal-input admission.
 constexpr std::uint32_t normal_extent=2048;
 const CompileImageHeader normal{normal_extent,normal_extent,0,false,CompileImageEncoding::Png,stbi_workspace_png_profile(normal_extent,normal_extent,std::max(encoded_window,pixel_input_bytes))};
 const auto pixels=checked_add(compile_pixel_workspace_plan(normal,policy).bytes(),pixel_input_bytes,"pixel input workspace overflow");
 const auto labels=CompiledLabelWorkspace::required_bytes(normal_extent);
 const std::array<CompileWorkspaceProfile,static_cast<std::size_t>(CompileWorkClass::Count)> available{{{64U<<10,64U<<10},{std::max(encoded_window,annotations.normal_bytes),64U<<10},
  {std::max(labels,annotations.normal_bytes),std::max(labels,annotations.completion_bytes)},{pixels,64U<<10},{labels,64U<<10},{labels,labels}}};
 std::array<CompileWorkspaceProfile,static_cast<std::size_t>(CompileWorkClass::Count)> profiles{};
 for (const auto kind:enabled) {
  const auto index=static_cast<std::size_t>(kind);
  if (index>=profiles.size()) throw std::invalid_argument("invalid enabled compile workspace class");
  profiles[index]=available[index];
 }
 return CompileWorkspacePlan::prepare(workers,profiles,std::max(encoded_window,source_window),COMPILED_COMPACTION_SCRATCH_BYTES);
}

CompileWorkspacePlan CompileWorkspacePlan::prepare(std::size_t workers, std::span<const CompileWorkspaceProfile> profiles,
 std::size_t shared, std::size_t compaction, std::uint64_t explicit_budget) {
 if (!workers || profiles.size()>static_cast<std::size_t>(CompileWorkClass::Count)) throw std::invalid_argument("compile workspace plan requires CPUs and canonical class profiles");
 CompileWorkspacePlan result;
 result.shared_bytes=page_rounded_bytes(shared); result.compaction_bytes=page_rounded_bytes(compaction);
 auto reserved = checked_add(result.shared_bytes,result.compaction_bytes,"compile protected workspace overflow");
 for (const auto& profile : profiles) {
  const auto completion = page_rounded_bytes(profile.completion_bytes);
  result.completion.push_back(completion);
  reserved = checked_add(reserved,completion,"compile completion workspace overflow");
 }
 result.protected_bytes = reserved;
 if (reserved > ceiling) throw InsufficientCompileResources("compile service and completion windows exceed normal pinned ceiling");
 for (const auto& profile : profiles) {
  const auto bytes=page_rounded_bytes(profile.normal_bytes);
  if (bytes <= ceiling-reserved) result.context_bytes=std::max(result.context_bytes,bytes);
 }
 const auto requested = checked_add<std::uint64_t>(reserved,checked_multiply<std::uint64_t>(workers,result.context_bytes,"compile context workspace overflow"),"compile workspace overflow");
 result.budget = explicit_budget ? page_rounded_bytes(explicit_budget) : std::min(ceiling,std::max<std::uint64_t>(256ULL<<20,requested));
 if (result.budget < reserved || result.budget > ceiling) throw InsufficientCompileResources("compile pinned budget does not cover protected windows");
 auto available=result.budget-reserved;
 const auto quantum=common::system::host_page_size();
 if (!result.context_bytes) result.context_bytes=std::max<std::uint64_t>(quantum,available/workers/quantum*quantum);
 // Prepare a fitting context for each enabled class first. Any lane may borrow
 // these stable slices; CPU class floors never imply imaginary storage.
 if (workers>=static_cast<std::size_t>(std::ranges::count_if(profiles,[](const auto& profile) { return profile.normal_bytes || profile.completion_bytes; }))) {
  std::vector<std::size_t> capacities;
  for (const auto& profile : profiles) if (profile.normal_bytes) capacities.push_back(page_rounded_bytes(profile.normal_bytes));
  std::ranges::sort(capacities,std::greater<>{});
  for (auto extent : capacities) {
   if (extent>available || result.context_capacities.size()==workers) continue;
   result.context_capacities.push_back(extent); available-=extent;
  }
 }
 while (result.context_capacities.size()<workers && result.context_bytes<=available) {
  result.context_capacities.push_back(result.context_bytes); available-=result.context_bytes;
 }
 if (result.context_capacities.empty() && available>=quantum)
  result.context_capacities.push_back(available/quantum*quantum);
 result.contexts=result.context_capacities.size();
 result.context_bytes=0;
 for (auto extent : result.context_capacities) result.context_bytes=std::max(result.context_bytes,extent);
 return result;
}
struct CompileArenaLease::State {
 std::span<std::byte> memory;
 std::size_t requested = 0;
 int node = -1;
 bool oversized = false;
 std::unique_ptr<NumaMemory> exceptional;
 std::shared_ptr<void> bank;
 std::function<void()> release, materialized;
 mutable std::mutex mutex;
 ~State() {
  exceptional.reset(); // Physical retirement precedes eligibility publication.
  if (release) release();
 }
};
struct CompileArenaBank::State {
 struct Context { std::span<std::byte> bytes; bool used = false; CompileWorkClass completion = CompileWorkClass::Count; CompileWorkspaceWindow window = CompileWorkspaceWindow::Task; bool borrowed = false; };
 CompileWorkspacePlan plan;
 std::vector<NumaMemory> memory;
 std::vector<Context> contexts;
 std::function<void()> released;
 mutable std::mutex mutex;
 std::uint64_t leased = 0, exceptional = 0, exceptional_locked = 0, peak = 0, reused = 0, retired = 0;
 int exceptional_node = -1;
 bool observing = false;
 // The caller holds mutex. Occupancy affects availability, never shape fit.
 [[nodiscard]] std::size_t select(std::size_t bytes, bool completion, CompileWorkClass kind, CompileWorkspaceWindow window, bool available_only) const {
  std::size_t selected=contexts.size();
  bool selected_ordinary=false;
  for (std::size_t i=0; i<contexts.size(); ++i) {
   const auto& value=contexts[i];
   if ((available_only && value.used) || bytes>value.bytes.size()) continue;
   const bool ordinary=value.window==CompileWorkspaceWindow::Task && value.completion==CompileWorkClass::Count;
   const bool named=value.window!=CompileWorkspaceWindow::Task && value.window==window;
   const bool completing=completion && kind!=CompileWorkClass::Count && value.completion==kind;
   if (!ordinary && !named && !completing) continue;
   if (selected==contexts.size() || (ordinary && !selected_ordinary) ||
       (ordinary==selected_ordinary && value.bytes.size()<contexts[selected].bytes.size())) {
    selected=i; selected_ordinary=ordinary;
   }
  }
  return selected;
 }
};
CompileArenaBank::CompileArenaBank(const CompileWorkspacePlan& plan, std::span<const int> nodes, std::function<void()> released, bool observe) : state_(std::make_shared<State>()) {
 if (nodes.empty()) throw std::invalid_argument("compile arena has no resolved NUMA placement");
 const auto page=common::system::host_page_size();
 auto protected_bytes=checked_add(plan.shared_bytes,plan.compaction_bytes,"compile protected workspace overflow");
 for (auto bytes : plan.completion) {
  if (bytes%page) throw std::invalid_argument("compile completion window is not page aligned");
  protected_bytes=checked_add(protected_bytes,bytes,"compile protected workspace overflow");
 }
 auto committed=protected_bytes;
 std::size_t largest=0;
 for (auto bytes : plan.context_capacities) {
  if (!bytes || bytes%page) throw std::invalid_argument("compile context is not page aligned");
  committed=checked_add(committed,bytes,"compile arena extent overflow"); largest=std::max(largest,bytes);
 }
 if (!plan.budget || plan.budget>CompileWorkspacePlan::ceiling || plan.budget%page || plan.shared_bytes%page || plan.compaction_bytes%page ||
     protected_bytes!=plan.protected_bytes || committed>plan.budget || plan.contexts!=plan.context_capacities.size() || plan.contexts>nodes.size() ||
     largest!=plan.context_bytes || plan.completion.size()>static_cast<std::size_t>(CompileWorkClass::Count))
  throw std::invalid_argument("compile arena requires a checked fitting workspace plan");
 auto& state = *state_; state.plan = plan; state.released = std::move(released); state.exceptional_node = nodes.front(); state.observing=observe;
 // Each context is independently placed in resolved CPU order. The protected
 // windows and the remaining budget are pinned too, even while no task uses them.
 state.memory.reserve(plan.contexts+1); state.contexts.reserve(plan.contexts+plan.completion.size());
 const auto allocate = [&](std::size_t bytes,int node) {
  state.memory.emplace_back(node,NumaMemory::Lock::Pages); try { state.memory.back().ensure_bytes(bytes); }
  catch (const std::bad_alloc&) { throw std::system_error(std::make_error_code(std::errc::not_enough_memory),"prepare fixed pinned compile arena on NUMA node "+std::to_string(node)); }
  return std::span(static_cast<std::byte*>(state.memory.back().data()),state.memory.back().capacity_bytes());
 };
 std::size_t used=0;
 for (std::size_t i=0; i<plan.context_capacities.size(); ++i) {
  const auto bytes=plan.context_capacities[i]; used+=bytes;
  state.contexts.push_back({allocate(bytes,nodes[i%nodes.size()])});
 }
 auto tail = allocate(static_cast<std::size_t>(plan.budget)-used,nodes.front());
 for (std::size_t i=0; i<plan.completion.size(); ++i) if (const auto bytes=plan.completion[i]) {
  state.contexts.push_back({tail.first(bytes),false,static_cast<CompileWorkClass>(i)}); tail=tail.subspan(bytes);
 }
 for (const auto window : {CompileWorkspaceWindow::SharedSource,CompileWorkspaceWindow::Compaction}) {
  const auto bytes=window==CompileWorkspaceWindow::SharedSource ? plan.shared_bytes : plan.compaction_bytes;
  if (bytes) { state.contexts.push_back({tail.first(bytes),false,CompileWorkClass::Count,window}); tail=tail.subspan(bytes); }
 }
}
const CompileWorkspacePlan& CompileArenaBank::plan() const noexcept { return state_->plan; }
bool CompileArenaBank::fits(std::size_t bytes, bool completion, CompileWorkClass kind, CompileWorkspaceWindow window) const {
 const std::lock_guard lock(state_->mutex);
 if (state_->exceptional) return false;
 return state_->select(bytes,completion,kind,window,true)!=state_->contexts.size();
}
bool CompileArenaBank::normal_fit(std::size_t bytes, bool completion, CompileWorkClass kind, CompileWorkspaceWindow window) const {
 const std::lock_guard lock(state_->mutex);
 return state_->select(bytes,completion,kind,window,false)!=state_->contexts.size();
}
bool CompileArenaBank::exceptional_active() const { const std::lock_guard lock(state_->mutex); return state_->exceptional != 0; }
CompileArenaLease CompileArenaBank::acquire(std::size_t bytes, bool exceptional, bool completion, CompileWorkClass kind, CompileWorkspaceWindow window) {
 auto lease = std::make_shared<CompileArenaLease::State>();
 lease->bank = state_; lease->requested = bytes;
 std::weak_ptr<State> weak = state_;
 const std::lock_guard lock(state_->mutex);
 if (state_->exceptional) throw InsufficientCompileResources("compile exceptional lineage has not physically retired");
 const auto selected=state_->select(bytes,completion,kind,window,true);
 if (selected!=state_->contexts.size()) {
  const auto i=selected;
  auto& value=state_->contexts[i];
  lease->memory = value.bytes;
  lease->release = [weak,i,bytes] {
   if (auto state = weak.lock()) {
    { const std::lock_guard guard(state->mutex); state->contexts[i].used = false; state->leased -= bytes; }
    if (state->released) state->released();
   }
  };
  value.used = true; if (state_->observing) { if (value.borrowed) ++state_->reused; value.borrowed=true; } state_->leased += bytes; return CompileArenaLease(std::move(lease),0,bytes);
 }
 if (!exceptional || state_->leased || state_->select(bytes,completion,kind,window,false)!=state_->contexts.size())
  throw InsufficientCompileResources("compile arena has no fitting available context");
 lease->oversized = true; lease->node = state_->exceptional_node;
 const auto extent = page_rounded_bytes(bytes);
 lease->release = [weak] {
  if (auto state = weak.lock()) {
   { const std::lock_guard guard(state->mutex); state->exceptional = 0; if (state->observing && state->exceptional_locked) ++state->retired; state->exceptional_locked=0; }
   if (state->released) state->released();
  }
 };
 lease->materialized=[weak,extent] {
  if (auto state=weak.lock()) {
   { const std::lock_guard guard(state->mutex); state->exceptional_locked=extent; if (state->observing) state->peak=std::max(state->peak,state->plan.budget+extent); }
   if (state->released) state->released();
  }
 };
 state_->exceptional = extent;
 return CompileArenaLease(std::move(lease),0,bytes);
}
CompileArenaBank::Snapshot CompileArenaBank::snapshot() const {
 const std::lock_guard lock(state_->mutex);
 return {state_->plan.budget,state_->plan.budget+state_->exceptional_locked,state_->leased,state_->exceptional_locked,state_->peak,state_->exceptional,state_->reused,state_->retired,state_->plan.contexts};
}
std::span<std::byte> CompileArenaLease::bytes() const {
 if (!state_) return {};
 const std::lock_guard lock(state_->mutex);
 if (state_->oversized && !state_->exceptional) {
  auto memory = std::make_unique<NumaMemory>(state_->node,NumaMemory::Lock::Pages);
  memory->ensure_bytes(state_->requested);
  state_->memory = {static_cast<std::byte*>(memory->data()),memory->capacity_bytes()};
  state_->exceptional = std::move(memory);
  if (state_->materialized) state_->materialized();
 }
 return state_->memory.subspan(offset_,bytes_);
}
std::size_t CompileArenaLease::capacity() const noexcept { return state_ ? bytes_ : 0; }
bool CompileArenaLease::exceptional() const noexcept { return state_ && state_->oversized; }
CompileArenaLease CompileArenaLease::slice(std::size_t offset,std::size_t bytes) const {
 if (!state_ || offset>bytes_ || bytes>bytes_-offset) throw std::out_of_range("compile arena slice exceeds its custody");
 return CompileArenaLease(state_,offset_+offset,bytes);
}
}
