#pragma once
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <stdexcept>
namespace mmltk::backend::data::detail {
enum class CompileWorkClass : std::uint8_t { Acquisition, Extraction, Annotations, Pixels, Gather, Output, Count };
// Product readiness facts. A subscription selects only predicates it owns.
enum class CompileChange : std::uint8_t { None = 0, Resources = 1, Cpu = 2, Records = 4, Local = 8, Pressure = 16, Interruption = 32, Fairness = 64 };
[[nodiscard]] constexpr CompileChange operator|(CompileChange left, CompileChange right) noexcept {
 return static_cast<CompileChange>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
}
[[nodiscard]] constexpr bool includes(CompileChange interests, CompileChange cause) noexcept {
 return (static_cast<unsigned>(interests) & static_cast<unsigned>(cause)) != 0;
}
enum class CompileWorkspaceWindow : std::uint8_t { Task, SharedSource, Compaction };
enum class CompileResourceDenial : std::uint8_t {
 None, Infeasible, ExceptionalDrain, ExceptionalLineage, ArenaBusy, RetainedLineage, Cpu, Descriptors, Bytes, LoanCapacity, AgedDemand, Records
};
class CompileExecution;
struct CompileWorkspacePlan;
struct CompileAdmission;
class InsufficientCompileResources : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
struct CompileResources {
 std::uint64_t bytes = 0;
 std::size_t descriptors = 0;
 // Producer allowances include their consumers' scratch/output, atomically.
 bool producer = false;
 // Fixed nonpreemptible library grant, including its calling CPU.
 std::size_t cpu_workers = 0;
 // Small retained file/lease control records are charged separately from the
 // workspace their consumer needs to finish. Never use this for data buffers.
 bool retained_handles = false;
 // Live descriptor commitment for explicitly bound dependent reservations.
 std::size_t continuation_descriptors = 0;
 // Requested fixed-bank payload, distinct from separately owned source/control
 // charges. Admission additionally accounts the bank's bounded slice alignment.
 bool supplied_workspace = false;
 bool completion_workspace = false;
 CompileWorkClass workspace_class = CompileWorkClass::Count;
 // Checked upper bound for library-owned bookkeeping outside pinned payload.
 std::uint64_t opaque_bytes = 0;
 CompileWorkspaceWindow workspace_window = CompileWorkspaceWindow::Task;
 [[nodiscard]] static CompileResources handles(std::size_t count, bool producer = false, std::size_t continuation = 0);
};
MMLTK_REFLECT_FIELDS(CompileResources)
struct CompileExecutionLimits {
 std::uint64_t transient_bytes = 0;
 std::size_t descriptors = 0;
 std::size_t task_records = 0;
 std::shared_ptr<const CompileWorkspacePlan> workspace_plan{};
};
// Shared custody charges backing once and returns it after its last reader.
class CompileAllowance {
 struct Credits;
 friend struct CompileAdmission;
 friend class CompileResourceWait;

public:
 CompileAllowance() = default;
 [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(credits_); }
 [[nodiscard]] std::uint64_t bytes() const noexcept;
 [[nodiscard]] std::size_t descriptors() const noexcept;
 [[nodiscard]] std::span<std::byte> workspace() const;
 [[nodiscard]] bool exceptional_workspace() const noexcept;
 // Stable for the admitted external grant until its physical retirement.
 [[nodiscard]] std::span<const int> cpus() const noexcept;
 [[nodiscard]] bool aliases(const CompileAllowance& other) const noexcept { return credits_ && credits_ == other.credits_; }
 // The backing owner calls this only after joining its workers and releasing
 // its own storage. Aliases keep descriptor commitments; split storage children
 // keep their separately charged bytes. External CPU capacity retires here too.
 // Accounting waits for any remaining CPU frames and stable offer scope to
 // settle; the request never releases physical storage on the owner's behalf.
 void retire_workspace() const noexcept;
 // Return unclaimed promises now and as live children finish. Their physical
 // charges and upstream return paths remain intact; future work reserves anew.
 void release_unused_continuation() const noexcept;
 // After physically closing this owner's files and completing its descriptor
 // production, return its draw and unused promises. Published consumers must
 // already own their continuation. Live child returns pass through this retired
 // owner; aliases and split storage cannot return the same promise twice.
 void retire_descriptors() const noexcept;
 // Partition an already admitted storage envelope, without another ledger
 // charge. Use before publishing the allowance to readers and outside any
 // unused-workspace offer. Positive storage keeps its own workspace loans;
 // descriptor-only aliases retain none after the backing owner retires bytes.
 [[nodiscard]] CompileAllowance split_storage(std::uint64_t);

private:
 friend class CompileExecution;
 explicit CompileAllowance(std::shared_ptr<Credits> value) : credits_(std::move(value)) {}
 std::shared_ptr<Credits> credits_;
};
}  // namespace mmltk::backend::data::detail
