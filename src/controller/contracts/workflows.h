#pragma once
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <string_view>
#include <type_traits>
#include <vector>
#include "mmltk/frameworks/reflection/materializer.h"
namespace mmltk::controller::contracts {
enum class FeatureId : std::uint8_t {
    Train = 0U,
    Validate = 1U,
    Predict = 2U,
    Annotate = 3U,
    Export = 4U,
    Live = 5U,
    Explore = 6U,
};
MMLTK_REFLECT_ENUM(FeatureId)
inline constexpr std::size_t kFeatureCount = static_cast<std::size_t>(FeatureId::Explore) + 1U;
[[nodiscard]] constexpr bool valid_feature(const FeatureId feature) noexcept { return static_cast<std::size_t>(feature) < kFeatureCount; }
[[nodiscard]] inline constexpr bool valid_foreground_workflow(const std::uint32_t workflow) noexcept {
    switch (static_cast<FeatureId>(workflow)) {
        case FeatureId::Validate:
        case FeatureId::Predict:
        case FeatureId::Annotate:
        case FeatureId::Live:
        case FeatureId::Explore: return true;
        case FeatureId::Train:
        case FeatureId::Export: return false;
    }
    return false;
}
}  // namespace mmltk::controller::contracts
namespace mmltk::controller::contracts::reflection {
using mmltk::frameworks::reflection::reflected_annotation_count;
using mmltk::frameworks::reflection::reflected_annotations;
template <std::size_t Count>
struct FeatureScope final {
    std::array<mmltk::controller::contracts::FeatureId, Count> values;
    constexpr bool operator==(const FeatureScope&) const noexcept = default;
};
template <class>
inline constexpr bool is_feature_scope_annotation = false;
template <std::size_t Count>
inline constexpr bool is_feature_scope_annotation<FeatureScope<Count>> = true;
using AllFeatureScope = FeatureScope<mmltk::controller::contracts::kFeatureCount>;
[[nodiscard]] consteval AllFeatureScope all_feature_scope() {
    AllFeatureScope result{};
    for (std::size_t index = 0U; index < result.values.size(); ++index) { result.values[index] = static_cast<mmltk::controller::contracts::FeatureId>(index); }
    return result;
}
template <class... Rest>
    requires(std::same_as<mmltk::controller::contracts::FeatureId, Rest> && ...)
[[nodiscard]] consteval auto feature_scope(const mmltk::controller::contracts::FeatureId first, const Rest... rest) {
    return FeatureScope<1U + sizeof...(Rest)>{{first, rest...}};
}
// The action and state method both name the concrete Process work declaration.
// Reflection can therefore prove the Work payload, its singular ProgressFor
// endpoint, and its browser projection without a parallel operation catalog.
template <class Work>
struct OperationProgressFor final {
    using work_type = Work;
};
template <class>
inline constexpr bool is_operation_progress_annotation = false;
template <class Work>
inline constexpr bool is_operation_progress_annotation<OperationProgressFor<Work>> = true;
enum class OperationStateSemantic : std::uint8_t {
    Active,
    Progress,
    Terminal,
};
MMLTK_REFLECT_ENUM(OperationStateSemantic)
struct OperationStateField final {
    OperationStateSemantic semantic = OperationStateSemantic::Progress;
    constexpr bool operator==(const OperationStateField&) const noexcept = default;
};
enum class ProgressFieldSemantic : std::uint8_t {
    StageOrStatus,
    ActivityOrDetail,
    Completed,
    Total,
    Elapsed,
    Remaining,
    Throughput,
    ProjectedOutput,
    Dropped,
    Quarantined,
};
MMLTK_REFLECT_ENUM(ProgressFieldSemantic)
struct ProgressField final {
    ProgressFieldSemantic semantic = ProgressFieldSemantic::StageOrStatus;
    constexpr bool operator==(const ProgressField&) const noexcept = default;
};
[[nodiscard]] constexpr bool feature_allowed(const mmltk::controller::contracts::FeatureId feature, const auto& scope) noexcept {
    for (const auto candidate : scope.values) {
        if (candidate == feature) return true;
    }
    return false;
}
template <class ActionType>
[[nodiscard]] constexpr bool feature_allowed(const mmltk::controller::contracts::FeatureId feature) {
    bool allowed = false;
    static constexpr auto annotations = reflected_annotations<^^ActionType>();
    template for (constexpr auto annotation : annotations) {
        using Annotation = std::remove_cvref_t<typename[:std::meta::type_of(annotation):]>;
        if constexpr (is_feature_scope_annotation<Annotation>) {
            constexpr Annotation policy = std::meta::extract<Annotation>(annotation);
            allowed = allowed || feature_allowed(feature, policy);
        }
    }
    return allowed;
}
// One request declaration owns its workflow policy. Schema, CLI, diagnostics,
// and Rust descriptors consume this value directly.
struct ReflectedWorkflowPolicy final {
    std::array<mmltk::controller::contracts::FeatureId, mmltk::controller::contracts::kFeatureCount> workflows{};
    std::size_t count = 0U;
    [[nodiscard]] constexpr bool allows(const mmltk::controller::contracts::FeatureId workflow) const noexcept {
        for (std::size_t index = 0U; index < count; ++index) {
            if (workflows[index] == workflow) return true;
        }
        return false;
    }
    [[nodiscard]] constexpr bool valid() const noexcept {
        if (count == 0U || count > workflows.size()) return false;
        for (std::size_t left = 0U; left < count; ++left) {
            if (!mmltk::controller::contracts::valid_feature(workflows[left])) return false;
            for (std::size_t right = left + 1U; right < count; ++right) {
                if (workflows[left] == workflows[right]) return false;
            }
        }
        return true;
    }
    constexpr bool operator==(const ReflectedWorkflowPolicy&) const noexcept = default;
};
template <std::meta::info Target>
[[nodiscard]] consteval ReflectedWorkflowPolicy reflected_workflow_policy_of() {
    ReflectedWorkflowPolicy result{};
    std::size_t annotation_count = 0U;
    bool overflow = false;
    template for (constexpr auto annotation : reflected_annotations<Target>()) {
        using Annotation = std::remove_cvref_t<typename[:std::meta::type_of(annotation):]>;
        if constexpr (is_feature_scope_annotation<Annotation>) {
            ++annotation_count;
            const Annotation policy = std::meta::extract<Annotation>(annotation);
            for (const mmltk::controller::contracts::FeatureId feature : policy.values) {
                if (result.count == result.workflows.size()) {
                    overflow = true;
                    continue;
                }
                result.workflows[result.count++] = feature;
            }
        }
    }
    if (annotation_count != 1U || overflow) result.count = 0U;
    return result;
}
template <class ActionType>
[[nodiscard]] consteval ReflectedWorkflowPolicy reflected_workflow_policy() {
    return reflected_workflow_policy_of<^^ActionType>();
}
}  // namespace mmltk::controller::contracts::reflection
namespace mmltk::controller::contracts {
template <mmltk::controller::contracts::FeatureId... Features>
    requires(sizeof...(Features) != 0U)
struct[[= mmltk::controller::contracts::reflection::feature_scope(Features...)]] WorkflowIntent final {
    static constexpr std::array<mmltk::controller::contracts::FeatureId, sizeof...(Features)> features{Features...};
};
struct[[= reflection::feature_scope(FeatureId::Train)]] TrainWorkflowIntent final {};
struct[[= reflection::feature_scope(FeatureId::Validate)]] ValidateWorkflowIntent final {};
struct[[= reflection::feature_scope(FeatureId::Predict)]] PredictWorkflowIntent final {};
struct[[= reflection::feature_scope(FeatureId::Annotate)]] AnnotateWorkflowIntent final {};
struct[[= reflection::feature_scope(FeatureId::Export)]] ExportWorkflowIntent final {};
struct[[= reflection::feature_scope(FeatureId::Live)]] LiveWorkflowIntent final {};
struct[[= reflection::feature_scope(FeatureId::Explore)]] ExploreWorkflowIntent final {};
// Workflow selection applies to every feature and therefore carries explicit
// all-feature scope.
struct[[= mmltk::controller::contracts::reflection::all_feature_scope()]] WorkflowRequest final {
    mmltk::controller::contracts::FeatureId workflow = mmltk::controller::contracts::FeatureId::Train;
    [[nodiscard]] bool valid() const noexcept { return mmltk::controller::contracts::valid_feature(workflow); }
};
template <FeatureId... Features>
[[nodiscard]] consteval const auto& materialized_field_policies(std::type_identity<WorkflowIntent<Features...>>) {
    return mmltk::frameworks::reflection::kReflectedFieldPolicies<WorkflowIntent<Features...>>;
}
MMLTK_REFLECT_FIELDS(TrainWorkflowIntent)
MMLTK_REFLECT_FIELDS(ValidateWorkflowIntent)
MMLTK_REFLECT_FIELDS(PredictWorkflowIntent)
MMLTK_REFLECT_FIELDS(AnnotateWorkflowIntent)
MMLTK_REFLECT_FIELDS(ExportWorkflowIntent)
MMLTK_REFLECT_FIELDS(LiveWorkflowIntent)
MMLTK_REFLECT_FIELDS(ExploreWorkflowIntent)
MMLTK_REFLECT_FIELDS(WorkflowRequest)
}  // namespace mmltk::controller::contracts
namespace mmltk::controller::contracts {
enum class FileDialogMode : std::uint8_t {
    OpenFile,
    OpenFolder,
    SaveFile,
};
MMLTK_REFLECT_ENUM(FileDialogMode)
}  // namespace mmltk::controller::contracts
namespace mmltk::controller::contracts::reflection {
struct PersistenceMetadata final {
    constexpr bool operator==(const PersistenceMetadata&) const noexcept = default;
};
template <std::size_t Extent>
struct FileDialogLiteral final {
    char value[Extent]{};
    consteval FileDialogLiteral(const char (&source)[Extent]) {
        for (std::size_t index = 0U; index < Extent; ++index) value[index] = source[index];
    }
    [[nodiscard]] constexpr std::string_view view() const noexcept {
        static_assert(Extent > 0U);
        return {value, Extent - 1U};
    }
    constexpr bool operator==(const FileDialogLiteral&) const noexcept = default;
};
template <FileDialogLiteral Title, FileDialogLiteral Filter, FileDialogLiteral Pattern>
struct FileDialog final {
    mmltk::controller::contracts::FileDialogMode mode = mmltk::controller::contracts::FileDialogMode::OpenFile;
    static constexpr std::string_view title = Title.view();
    static constexpr std::string_view filter = Filter.view();
    static constexpr std::string_view pattern = Pattern.view();
    constexpr bool operator==(const FileDialog&) const noexcept = default;
};
template <class>
inline constexpr bool is_file_dialog_annotation = false;
template <FileDialogLiteral Title, FileDialogLiteral Filter, FileDialogLiteral Pattern>
inline constexpr bool is_file_dialog_annotation<FileDialog<Title, Filter, Pattern>> = true;
}  // namespace mmltk::controller::contracts::reflection
