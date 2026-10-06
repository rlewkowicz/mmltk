#pragma once
#include "src/backend/data/detail/compile_arena_observation.h"
#include "src/backend/data/detail/compile_resources.h"
#include "src/backend/data/detail/compile_pixel_workspace.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>
#include <utility>
namespace mmltk::backend::data::detail {
struct CompileWorkspaceProfile {
 std::size_t normal_bytes = 0;
 std::size_t completion_bytes = 0;
};
struct CompileWorkspacePlan {
 static constexpr std::uint64_t ceiling = 6ULL << 30;
 std::uint64_t budget = 0, protected_bytes = 0;
 std::size_t context_bytes = 0, contexts = 0, shared_bytes = 0, compaction_bytes = 0;
 std::vector<std::size_t> context_capacities;
 std::vector<std::size_t> completion;
 [[nodiscard]] static CompileWorkspacePlan prepare(
  std::size_t workers, std::span<const CompileWorkspaceProfile>, std::size_t shared_windows, std::size_t compaction, std::uint64_t explicit_budget = 0);
};
[[nodiscard]] CompileWorkspacePlan compile_workspace_plan(
 std::size_t workers, std::uint32_t resolution, bool perceptual, std::size_t reader_window = 0, std::uint64_t explicit_budget = 0, std::size_t encoded_window = 0, imaging::resample::RgbResizeMethod method = imaging::resample::RgbResizeMethod::Native);
[[nodiscard]] CompileWorkspacePlan compile_workspace_plan(std::size_t workers, CompilePixelPolicy, std::span<const CompileWorkClass> enabled, CompileWorkspaceProfile annotations,
 std::size_t encoded_window, std::size_t pixel_input_bytes = 0, std::size_t source_window = 0);
// Shared custody supports a moved output lease or immutable batch descendants.
// Its backing is physically released only after its last actual borrower.
class CompileArenaLease final {
 struct State;
 friend class CompileArenaBank;
 explicit CompileArenaLease(std::shared_ptr<State> state, std::size_t offset, std::size_t bytes) : state_(std::move(state)), offset_(offset), bytes_(bytes) {}
 std::shared_ptr<State> state_;
 std::size_t offset_ = 0, bytes_ = 0;

public:
 CompileArenaLease() = default;
 [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
 [[nodiscard]] std::span<std::byte> bytes() const;
 [[nodiscard]] std::size_t capacity() const noexcept;
 [[nodiscard]] bool exceptional() const noexcept;
 [[nodiscard]] CompileArenaLease slice(std::size_t offset, std::size_t bytes) const;
};
class CompileArenaBank final {
 friend class CompileArenaLease;
 struct State;
 std::shared_ptr<State> state_;

public:
 static constexpr std::size_t alignment = 64;
 struct Snapshot {
  std::uint64_t locked = 0, resident = 0, leased = 0, exceptional = 0, exceptional_peak = 0, exceptional_reserved = 0, reused = 0, physical_retirements = 0;
  CompileArenaObservation observation{};
 };
 CompileArenaBank(const CompileWorkspacePlan&, std::span<const int> numa_nodes, std::function<void()> released, bool observe = false);
 [[nodiscard]] const CompileWorkspacePlan& plan() const noexcept;
 // Ordinary slices serve every window; named and class-completion reserves require
 // the matching request. Ordinary free extents are used before protected storage.
 [[nodiscard]] bool fits(std::size_t bytes, bool completion = false, CompileWorkClass = CompileWorkClass::Count, CompileWorkspaceWindow = CompileWorkspaceWindow::Task) const;
 // Uses the same compatibility rule without occupancy or exceptional activity.
 [[nodiscard]] bool normal_fit(std::size_t bytes, bool completion = false, CompileWorkClass = CompileWorkClass::Count, CompileWorkspaceWindow = CompileWorkspaceWindow::Task) const;
 [[nodiscard]] std::size_t normal_capacity(bool completion = false, CompileWorkClass = CompileWorkClass::Count, CompileWorkspaceWindow = CompileWorkspaceWindow::Task) const noexcept;
 // Actual slice charge, including alignment; a shape beyond normal capacity uses
 // page-rounded exceptional backing. Occupancy never changes this calculation.
 [[nodiscard]] std::size_t reservation_bytes(std::size_t bytes, bool completion = false, CompileWorkClass = CompileWorkClass::Count, CompileWorkspaceWindow = CompileWorkspaceWindow::Task) const;
 [[nodiscard]] bool exceptional_active() const;
 // Called only after CompileAdmission has drained unrelated physical users.
 // Exceptional materialization occurs at bytes(), outside scheduler locks.
 [[nodiscard]] CompileArenaLease acquire(
  std::size_t bytes, bool exceptional, bool completion = false, CompileWorkClass = CompileWorkClass::Count, CompileWorkspaceWindow = CompileWorkspaceWindow::Task);
 [[nodiscard]] Snapshot snapshot() const;
 [[nodiscard]] std::uint64_t largest_available(bool completion, CompileWorkClass, CompileWorkspaceWindow) const;
};
}  // namespace mmltk::backend::data::detail
