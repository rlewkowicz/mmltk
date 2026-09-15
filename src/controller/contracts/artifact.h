#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "src/backend/data/dataset_compile_phase.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"

#include "mmltk/frameworks/reflection/materializer.h"
#include "src/controller/contracts/terminal_presentation.h"

#include "src/backend/data/catalog/class_catalog.h"
#include "src/controller/contracts/workflows.h"
namespace mmltk::controller::contracts {

inline constexpr std::size_t kArtifactSplitCapacity = 3U;
inline constexpr std::size_t kArtifactPathCapacity = 4096U;
inline constexpr std::size_t kArtifactErrorCapacity = 4096U;
inline constexpr std::size_t kArtifactPresetCapacity = 256U;
inline constexpr std::size_t kArtifactUiStateByteBudget = 64U * 1024U;
inline constexpr std::size_t kArtifactProgressTextCapacity = 1024U;

[[nodiscard]] inline std::string bounded_artifact_detail(const std::string_view value) {
    return std::string{value.substr(0U, kArtifactErrorCapacity)};
}

struct ArtifactSplitFact final {
    [[= mmltk::frameworks::reflection::MaxBytes{kArtifactPathCapacity}]] std::string path;
    std::uint32_t image_count = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t channels = 0U;
    std::uint32_t max_instances_per_image = 0U;
    [[= mmltk::frameworks::reflection::MaxItems{mmltk::backend::data::catalog::kClassCatalogCapacity}]] std::vector<mmltk::backend::data::catalog::ClassName> class_names;
    [[nodiscard]] bool valid() const noexcept {
        if (path.empty() || path.size() > kArtifactPathCapacity || image_count == 0U || width == 0U || height == 0U || channels == 0U ||
            class_names.empty() || class_names.size() > mmltk::backend::data::catalog::kClassCatalogCapacity)
            return false;
        for (const auto& name : class_names)
            if (!name.valid()) return false;
        return true;
    }
    bool operator==(const ArtifactSplitFact&) const = default;
};

// This is the bounded owner-neutral inspection vocabulary.  ArtifactStore
// produces it, Artifact retains it as observed domain state, and Model
// materializes its selection from it.  Filesystem paths used to perform an
// inspection stay in the service request rather than becoming browser input.
struct ArtifactInspection final {
    bool compatible = false;
    [[= mmltk::frameworks::reflection::MaxItems{kArtifactSplitCapacity}]] std::vector<ArtifactSplitFact> splits;
    [[= mmltk::frameworks::reflection::MaxBytes{kArtifactErrorCapacity}]] std::string detail;

    [[nodiscard]] bool available() const noexcept { return compatible && !splits.empty() && detail.empty(); }
    [[nodiscard]] bool valid() const noexcept {
        if (detail.size() > kArtifactErrorCapacity || splits.size() > kArtifactSplitCapacity) return false;
        for (const auto& split : splits)
            if (!split.valid()) return false;
        return compatible ? !splits.empty() && detail.empty() : splits.empty();
    }
    bool operator==(const ArtifactInspection&) const = default;
};

using ArtifactCompilePhase = mmltk::backend::data::DatasetCompilePhase;

struct ArtifactProgress final {
    [[= reflection::ProgressField{reflection::ProgressFieldSemantic::StageOrStatus}]] ArtifactCompilePhase phase =
        ArtifactCompilePhase::Idle;
    [[= reflection::ProgressField{reflection::ProgressFieldSemantic::ActivityOrDetail}]]
        [[= mmltk::frameworks::reflection::MaxBytes{kArtifactProgressTextCapacity}]] std::string activity;
    // CLEANUP-IGNORE: Artifact progress fields carry compile-specific semantics and generated identities.
    [[= reflection::ProgressField{reflection::ProgressFieldSemantic::Completed}]] std::uint64_t completed = 0U;
    [[= reflection::ProgressField{reflection::ProgressFieldSemantic::Total}]] std::uint64_t total = 0U;
    [[= reflection::ProgressField{reflection::ProgressFieldSemantic::Elapsed}]] std::uint64_t elapsed_seconds = 0U;
    // CLEANUP-IGNORE: Remaining-time projection is specific to dataset compilation progress.
    [[= reflection::ProgressField{reflection::ProgressFieldSemantic::Remaining}]] std::uint64_t remaining_seconds = 0U;
    [[= reflection::ProgressField{reflection::ProgressFieldSemantic::Throughput}]] std::uint64_t throughput_per_second =
        // CLEANUP-IGNORE: Every progress semantic is an independently generated typed field, not an indexed metric
        // table.
        0U;
    [[= reflection::ProgressField{reflection::ProgressFieldSemantic::ProjectedOutput}]] std::uint64_t projected_output_bytes = 0U;
    [[= reflection::ProgressField{reflection::ProgressFieldSemantic::Dropped}]] std::uint64_t dropped_instances = 0U;
    [[= reflection::ProgressField{reflection::ProgressFieldSemantic::Quarantined}]] std::uint64_t quarantined_images = 0U;
    [[nodiscard]] bool valid() const noexcept {
        const auto phase_value = static_cast<std::uint8_t>(phase);
        return phase_value > static_cast<std::uint8_t>(ArtifactCompilePhase::Idle) &&
               phase_value <= static_cast<std::uint8_t>(ArtifactCompilePhase::Publishing) &&
               activity.size() <= kArtifactProgressTextCapacity &&
               ((total == 0U && completed == 0U) || (total != 0U && completed <= total));
    }
    bool operator==(const ArtifactProgress&) const = default;
};

enum class ArtifactTerminalOutcome : std::uint8_t { Idle, Succeeded, Failed, Cancelled, CancellationRequested, Refused };
inline constexpr std::array kArtifactTerminalPresentations{
    terminal_presentation::Policy{ArtifactTerminalOutcome::Idle, terminal_presentation::Classification::Refused, "artifact.idle",
                                  "No artifact operation has completed."},
    terminal_presentation::Policy{ArtifactTerminalOutcome::Succeeded, terminal_presentation::Classification::Success, "artifact.succeeded",
                                  ""},
    terminal_presentation::Policy{ArtifactTerminalOutcome::Failed, terminal_presentation::Classification::Failed, "artifact.failed",
                                  "The artifact operation failed."},
    terminal_presentation::Policy{ArtifactTerminalOutcome::Cancelled, terminal_presentation::Classification::Cancelled,
                                  "artifact.cancelled", "The artifact operation was cancelled."},
    terminal_presentation::Policy{ArtifactTerminalOutcome::CancellationRequested, terminal_presentation::Classification::Cancelled,
                                  "artifact.cancellation_requested", "Artifact cancellation was requested."},
    terminal_presentation::Policy{ArtifactTerminalOutcome::Refused, terminal_presentation::Classification::Refused, "artifact.refused",
                                  "The artifact operation was refused."},
};
static_assert(terminal_presentation::complete(kArtifactTerminalPresentations));
[[nodiscard]] consteval const auto& materialized_terminal_presentation_policy(std::type_identity<ArtifactTerminalOutcome>) {
    return kArtifactTerminalPresentations;
}

struct ArtifactTerminal final {
    ArtifactTerminalOutcome outcome = ArtifactTerminalOutcome::Idle;
    [[= mmltk::frameworks::reflection::MaxBytes{kArtifactPathCapacity}]] std::string artifact;
    [[= mmltk::frameworks::reflection::MaxBytes{kArtifactErrorCapacity}]] std::string detail;
    [[nodiscard]] bool valid() const noexcept {
        if (artifact.size() > kArtifactPathCapacity || detail.size() > kArtifactErrorCapacity) return false;
        switch (outcome) {
            case ArtifactTerminalOutcome::Idle:
            case ArtifactTerminalOutcome::CancellationRequested:
                return artifact.empty() && detail.empty();
            case ArtifactTerminalOutcome::Succeeded:
                return !artifact.empty() && detail.empty();
            case ArtifactTerminalOutcome::Failed:
            case ArtifactTerminalOutcome::Refused:
                return artifact.empty();
            case ArtifactTerminalOutcome::Cancelled:
                return artifact.empty() && detail.empty();
        }
        return false;
    }
    bool operator==(const ArtifactTerminal&) const = default;
};

struct ArtifactUiState final {
    std::uint64_t generation = 0U;

    // CLEANUP-IGNORE: Artifact compilation has a distinct inspection-bearing operation snapshot.
    [[= reflection::OperationStateField{reflection::OperationStateSemantic::Active}]] bool active = false;
    // CLEANUP-IGNORE: Artifact inspection is the domain payload that distinguishes this operation snapshot.
    ArtifactInspection inspection{};

    [[= reflection::OperationStateField{reflection::OperationStateSemantic::Progress}]] ArtifactProgress progress{};

    [[= reflection::OperationStateField{reflection::OperationStateSemantic::Terminal}]] ArtifactTerminal terminal{};
    bool operator==(const ArtifactUiState&) const = default;
};

MMLTK_REFLECT_FIELDS(ArtifactSplitFact)
MMLTK_REFLECT_FIELDS(ArtifactInspection)
MMLTK_REFLECT_FIELDS(ArtifactProgress)
MMLTK_REFLECT_FIELDS(ArtifactTerminal)
MMLTK_REFLECT_FIELDS(ArtifactUiState)
MMLTK_REFLECT_ENUM(ArtifactTerminalOutcome)

}  // namespace mmltk::controller::contracts
