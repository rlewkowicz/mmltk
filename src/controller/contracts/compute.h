#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"

#include "mmltk/frameworks/reflection/materializer.h"
#include "src/controller/contracts/terminal_presentation.h"

#include "src/controller/contracts/workflows.h"
#include "src/frameworks/serialization/serialization.h"
namespace mmltk::controller::contracts {

inline constexpr std::size_t kComputeStatusCapacity = std::size_t{4U} * 1024U;
inline constexpr std::size_t kComputeErrorCapacity = std::size_t{4U} * 1024U;
inline constexpr std::size_t kComputePathCapacity = std::size_t{4U} * 1024U;
[[nodiscard]] inline bool valid_compute_text(const std::string_view value, const std::size_t capacity) noexcept {
    return value.size() <= capacity;
}

[[nodiscard]] inline std::string bounded_compute_error(const std::string_view value) {
    return std::string{value.substr(0U, kComputeErrorCapacity)};
}

[[nodiscard]] inline std::string bounded_compute_output(const std::string_view value) {
    return std::string{value.substr(0U, kComputePathCapacity)};
}

enum class ComputeOperationOutcome : std::uint8_t { Idle, Running, Succeeded, Failed, Cancelled, CancellationRequested, Refused };
inline constexpr std::array kComputeTerminalPresentations{
    terminal_presentation::Policy{ComputeOperationOutcome::Idle, terminal_presentation::Classification::Refused, "compute.idle",
                                  "No compute operation has completed."},
    terminal_presentation::Policy{ComputeOperationOutcome::Running, terminal_presentation::Classification::Refused, "compute.running",
                                  "The compute operation is still running."},
    terminal_presentation::Policy{ComputeOperationOutcome::Succeeded, terminal_presentation::Classification::Success, "compute.succeeded",
                                  ""},
    terminal_presentation::Policy{ComputeOperationOutcome::Failed, terminal_presentation::Classification::Failed, "compute.failed",
                                  "The compute operation failed."},
    terminal_presentation::Policy{ComputeOperationOutcome::Cancelled, terminal_presentation::Classification::Cancelled, "compute.cancelled",
                                  "The compute operation was cancelled."},
    terminal_presentation::Policy{ComputeOperationOutcome::CancellationRequested, terminal_presentation::Classification::Cancelled,
                                  "compute.cancellation_requested", "Compute cancellation was requested."},
    terminal_presentation::Policy{ComputeOperationOutcome::Refused, terminal_presentation::Classification::Refused, "compute.refused",
                                  "The compute operation was refused."},
};
static_assert(terminal_presentation::complete(kComputeTerminalPresentations));
[[nodiscard]] consteval const auto& materialized_terminal_presentation_policy(std::type_identity<ComputeOperationOutcome>) {
    return kComputeTerminalPresentations;
}

[[nodiscard]] inline constexpr std::optional<std::uint64_t> next_compute_generation(const std::uint64_t frontier) noexcept {
    if (frontier == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
    return frontier + 1U;
}

struct ComputeTerminal final {
    ComputeOperationOutcome outcome = ComputeOperationOutcome::Idle;
    std::uint64_t generation = 0U;
    std::uint64_t completed = 0U;
    [[= mmltk::frameworks::reflection::MaxBytes{kComputePathCapacity}]] std::string output;
    [[= mmltk::frameworks::reflection::MaxBytes{kComputeErrorCapacity}]] std::string detail;
    [[nodiscard]] bool valid_worker_terminal() const noexcept {
        if (!valid_compute_text(output, kComputePathCapacity) || !valid_compute_text(detail, kComputeErrorCapacity)) return false;
        switch (outcome) {
            case ComputeOperationOutcome::Succeeded:
                return detail.empty();
            case ComputeOperationOutcome::Failed:
            case ComputeOperationOutcome::Refused:
                return output.empty();
            case ComputeOperationOutcome::Cancelled:
                return output.empty() && detail.empty();
            case ComputeOperationOutcome::Idle:
            case ComputeOperationOutcome::Running:
            case ComputeOperationOutcome::CancellationRequested:
                return false;
        }
        return false;
    }
    bool operator==(const ComputeTerminal&) const = default;
};

// The only construction boundary for public compute operation state.  Keeping
// the complete record here prevents individual systems from publishing partial
// aggregates with inconsistent field initialization or byte bounds.
[[nodiscard]] inline ComputeTerminal make_compute_terminal(const ComputeOperationOutcome outcome, const std::uint64_t generation = 0U,
                                                           const std::uint64_t completed = 0U, const std::string_view output = {},
                                                           const std::string_view detail = {}) {
    return {.outcome = outcome,
            .generation = generation,
            .completed = completed,
            .output = bounded_compute_output(output),
            .detail = bounded_compute_error(detail)};
}

[[nodiscard]] inline ComputeTerminal compute_failure_terminal(const std::exception_ptr failure, const std::string_view fallback) {
    try {
        if (failure) std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        return make_compute_terminal(ComputeOperationOutcome::Failed, 0U, 0U, {}, error.what());
    } catch (...) {}
    return make_compute_terminal(ComputeOperationOutcome::Failed, 0U, 0U, {}, fallback);
}

struct ComputeProgress final {
    std::uint64_t sequence = 0U;
    [[= reflection::ProgressField{reflection::ProgressFieldSemantic::Completed}]] std::uint64_t completed = 0U;
    [[= reflection::ProgressField{reflection::ProgressFieldSemantic::Total}]] std::uint64_t total = 0U;
    [[= reflection::ProgressField{reflection::ProgressFieldSemantic::StageOrStatus}]]
        [[= mmltk::frameworks::reflection::MaxBytes{kComputeStatusCapacity}]] std::string status;
    [[nodiscard]] bool valid() const noexcept {
        return sequence != 0U && valid_compute_text(status, kComputeStatusCapacity) &&
               (total == 0U || completed <= total);
    }
    bool operator==(const ComputeProgress&) const = default;
};

[[nodiscard]] inline bool compute_progress_follows(const ComputeProgress& value, const std::uint64_t sequence_frontier) noexcept {
    return value.valid() && value.sequence > sequence_frontier;
}

struct ComputeUiState final {
    // The frontier advances when a system begins an operation, so a worker infrastructure failure can leave the
    // last domain terminal at an earlier generation without permitting that operation identity to be reused.
    std::uint64_t generation_frontier = 0U;

    [[= reflection::OperationStateField{reflection::OperationStateSemantic::Active}]] bool active = false;

    [[= reflection::OperationStateField{reflection::OperationStateSemantic::Progress}]] ComputeProgress progress{};

    [[= reflection::OperationStateField{reflection::OperationStateSemantic::Terminal}]] ComputeTerminal terminal{};
    bool operator==(const ComputeUiState&) const = default;
};

// Value transitions only. The caller owns admission, locking and publication.
inline void begin_compute(ComputeUiState& state, std::uint64_t generation, std::string_view detail = {}) {
    auto terminal = make_compute_terminal(ComputeOperationOutcome::Running, generation, 0U, {}, detail);
    state.generation_frontier = generation;
    state.active = true;
    state.progress = {};
    state.terminal = std::move(terminal);
}
inline void cancel_compute(ComputeUiState& state) {
    if (state.active)
        state.terminal = make_compute_terminal(ComputeOperationOutcome::CancellationRequested, state.generation_frontier);
}
[[nodiscard]] inline bool advance_compute(ComputeUiState& state, const ComputeProgress& progress) {
    if (!state.active || !compute_progress_follows(progress, state.progress.sequence)) return false;
    state.progress = progress;
    if (state.terminal.outcome == ComputeOperationOutcome::Running) state.terminal.detail.clear();
    return true;
}
inline void complete_compute(ComputeUiState& state, ComputeTerminal terminal) {
    state.active = false;
    state.terminal = std::move(terminal);
}

MMLTK_REFLECT_FIELDS(ComputeTerminal)
MMLTK_REFLECT_FIELDS(ComputeProgress)
MMLTK_REFLECT_FIELDS(ComputeUiState)
MMLTK_REFLECT_ENUM(ComputeOperationOutcome)

}  // namespace mmltk::controller::contracts
