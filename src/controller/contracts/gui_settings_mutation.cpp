#include "src/controller/contracts/gui_settings_mutation.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <expected>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
#include "src/controller/contracts/gui_settings_states.h"
#include "src/controller/contracts/model_selection.h"
#include "src/controller/contracts/settings_commands.h"
#include "src/controller/contracts/settings_vocabulary.h"
#include "src/controller/contracts/view_state.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/serialization/serialization.h"
namespace mmltk::controller::contracts {
namespace {
inline constexpr std::size_t kGuiSettingsMutableLeafCount = settings_vocabulary::mutable_leaf_count<GuiSettingsState>();
static_assert(mmltk::frameworks::reflection::reflected_defaults_are_valid<UiSettingsState>());
template <class T>
using IsStdArray = settings_vocabulary::is_array<T>;
template <class T>
using IsOptional = settings_vocabulary::is_optional<T>;
template <class T>
using IsStdVector = settings_vocabulary::is_vector<T>;
template <class T, class Source>
[[nodiscard]] std::expected<void, SettingsMutationError> assign_flat_scalar(T& destination, const Source& source) {
    using U = std::remove_cvref_t<T>;
    using V = std::remove_cvref_t<Source>;
    if constexpr (std::same_as<U, bool>) {
        if constexpr (std::same_as<V, bool>) {
            destination = source;
            return {};
        }
    } else if constexpr (std::same_as<U, std::string>) {
        if constexpr (std::same_as<V, std::string>) {
            destination = source;
            return {};
        }
    } else if constexpr (std::same_as<U, std::filesystem::path>) {
        if constexpr (std::same_as<V, std::string>) {
            destination = source;
            return {};
        }
    } else if constexpr (std::is_enum_v<U>) {
        if constexpr (std::same_as<V, std::string>) {
            bool matched = false;
            settings_vocabulary::for_each_enumerator<U>([&](const std::string_view name, const U enumerator) {
                if (source == name) {
                    destination = enumerator;
                    matched = true;
                }
            });
            return matched ? std::expected<void, SettingsMutationError>{} : std::unexpected(SettingsMutationError::OutOfRange);
        } else if constexpr (std::same_as<V, std::int64_t> || std::same_as<V, std::uint64_t>) {
            using Storage = std::underlying_type_t<U>;
            if (!std::in_range<Storage>(source)) return std::unexpected(SettingsMutationError::OutOfRange);
            const U candidate = static_cast<U>(static_cast<Storage>(source));
            bool known = false;
            settings_vocabulary::for_each_enumerator<U>([&](const std::string_view, const U enumerator) { known = known || candidate == enumerator; });
            if (!known) return std::unexpected(SettingsMutationError::OutOfRange);
            destination = candidate;
            return {};
        }
    } else if constexpr (std::integral<U>) {
        if constexpr (std::same_as<V, std::int64_t> || std::same_as<V, std::uint64_t>) {
            if (!std::in_range<U>(source)) return std::unexpected(SettingsMutationError::OutOfRange);
            destination = static_cast<U>(source);
            return {};
        }
    } else if constexpr (std::floating_point<U>) {
        if constexpr (std::same_as<V, double> || std::same_as<V, std::int64_t> || std::same_as<V, std::uint64_t>) {
            const double converted = static_cast<double>(source);
            if (!std::isfinite(converted) || converted < static_cast<double>(std::numeric_limits<U>::lowest()) ||
                converted > static_cast<double>(std::numeric_limits<U>::max())) {
                return std::unexpected(SettingsMutationError::OutOfRange);
            }
            const U narrowed = static_cast<U>(source);
            if constexpr (!std::same_as<V, double>) {
                if (static_cast<long double>(narrowed) != static_cast<long double>(source)) return std::unexpected(SettingsMutationError::OutOfRange);
            }
            if (!std::isfinite(narrowed)) { return std::unexpected(SettingsMutationError::OutOfRange); }
            destination = narrowed;
            return {};
        }
    }
    return std::unexpected(SettingsMutationError::TypeMismatch);
}
template <class T>
[[nodiscard]] std::expected<void, SettingsMutationError> assign_flat_leaf(T& destination, const mmltk::frameworks::serialization::wire::FlatValue& value) {
    using U = std::remove_cvref_t<T>;
    return value.visit([&]<class Source>(const Source& source) -> std::expected<void, SettingsMutationError> {
        using V = std::remove_cvref_t<Source>;
        if constexpr (requires(const V& values) {
                          values.begin();
                          values.end();
                      } && !std::same_as<V, std::string> && !std::same_as<V, mmltk::frameworks::serialization::wire::ByteBuffer>) {
            if constexpr (IsStdArray<U>::value) {
                if (source.size() != IsStdArray<U>::size) { return std::unexpected(SettingsMutationError::OutOfRange); }
                U candidate{};
                for (std::size_t index = 0U; index < source.size(); ++index) {
                    const auto result = std::visit([&](const auto& scalar) { return assign_flat_scalar(candidate[index], scalar); }, source[index]);
                    if (!result) return result;
                }
                destination = std::move(candidate);
                return {};
            } else if constexpr (IsStdVector<U>::value) {
                U candidate;
                if constexpr (requires { candidate.reserve(source.size()); }) candidate.reserve(source.size());
                for (const auto& item : source) {
                    typename U::value_type converted{};
                    const auto result = std::visit([&](const auto& scalar) { return assign_flat_scalar(converted, scalar); }, item);
                    if (!result) return result;
                    candidate.push_back(std::move(converted));
                }
                destination = std::move(candidate);
                return {};
            }
            return std::unexpected(SettingsMutationError::TypeMismatch);
        } else {
            return assign_flat_scalar(destination, source);
        }
    });
}
template <class T>
[[nodiscard]] std::expected<void, SettingsMutationError> assign_flat_path(T& destination, const std::string_view path,
                                                                          const mmltk::frameworks::serialization::wire::FlatValue& value) {
    if (path.empty()) return assign_flat_leaf(destination, value);
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_arithmetic_v<U> || std::is_enum_v<U> || std::same_as<U, std::string> || std::same_as<U, std::filesystem::path> ||
                  IsOptional<U>::value || IsStdArray<U>::value || IsStdVector<U>::value) {
        return std::unexpected(SettingsMutationError::InvalidPath);
    } else {
        std::expected<void, SettingsMutationError> result = std::unexpected(SettingsMutationError::InvalidPath);
        const bool found =
            settings_vocabulary::visit_mutable_path(destination, path, [&](const std::string_view, auto& leaf) { result = assign_flat_leaf(leaf, value); });
        return found ? result : std::unexpected(SettingsMutationError::InvalidPath);
    }
}
[[nodiscard]] bool valid_source(const SourceSelectionState& source) noexcept {
    return (source.crop_width == 0 || (source.crop_width <= source.capture_width && source.crop_x <= source.capture_width - source.crop_width)) &&
           (source.crop_height == 0 || (source.crop_height <= source.capture_height && source.crop_y <= source.capture_height - source.crop_height));
}
[[nodiscard]] bool valid_train(const TrainViewState& train) noexcept {
    const auto& request = train.request;
    const bool valid_distributed = request.distributed_worker
                                       ? request.distributed_rank >= 0 && request.distributed_world_size > 1 && !request.distributed_store_path.empty()
                                       : request.distributed_rank == 0 && request.distributed_world_size == 1 && request.distributed_store_path.empty();
    return !request.device_ids.empty() && request.resolution > 0 && !mmltk::frameworks::reflection::validate_reflected_fields(request) &&
           mmltk::frameworks::reflection::unique_nonnegative_identifiers(request.device_ids) &&
           mmltk::frameworks::reflection::enum_contains(request.lr_scheduler) &&
           mmltk::backend::models::rfdetr::gpu_augmentation_relationships_valid(request.gpu_augmentation) &&
           mmltk::backend::models::rfdetr::training_supervision_config_valid(request.training_supervision) && valid_distributed;
}
[[nodiscard]] bool valid_validate(const ValidateViewState& validate) noexcept {
    return !mmltk::frameworks::reflection::validate_reflected_fields(validate.request);
}
[[nodiscard]] bool valid_predict(const PredictViewState& predict) noexcept {
    const auto& request = predict.request;
    return !mmltk::frameworks::reflection::validate_reflected_fields(request) && request.resolution > 0 && request.compiled_path.empty() &&
           request.image_inputs.empty() && predict.live_split_count > 0 && valid_source(predict.source);
}
[[nodiscard]] bool selected_model_artifact_available(const ModelArtifactSelectionState& artifacts) noexcept {
    switch (artifacts.input) {
        case ModelArtifactInputKind::Weights: return !artifacts.weights_path.empty();
        case ModelArtifactInputKind::Onnx: return !artifacts.onnx_path.empty();
        case ModelArtifactInputKind::TensorRt: return !artifacts.tensorrt_path.empty();
        case ModelArtifactInputKind::None: return false;
    }
    return false;
}
[[nodiscard]] bool valid_model_selection_draft(const ModelArtifactSelectionState& artifacts, const bool concrete_compatible) noexcept {
    if (mmltk::backend::models::rfdetr::find_preset_catalog_entry(artifacts.preset_name) == nullptr || artifacts.resolution <= 0) { return false; }
    if (artifacts.input == ModelArtifactInputKind::None) return true;
    return concrete_compatible && (artifacts.source != ModelSelectionSource::Custom || selected_model_artifact_available(artifacts));
}
[[nodiscard]] bool valid_model_selection_relations(const GuiSettingsState& state) noexcept {
    std::array<bool, 4U> valid{};
    ModelSelectionRelation::VisitRows([&]<class Relation>(const ModelSelectionCompatibility& row) {
        const auto valid_index = [&]() -> std::optional<std::size_t> {
            switch (row.workflow) {
                case FeatureId::Train: return 0U;
                case FeatureId::Validate: return 1U;
                case FeatureId::Predict: return 2U;
                case FeatureId::Export: return 3U;
                case FeatureId::Annotate:
                case FeatureId::Live:
                case FeatureId::Explore: return std::nullopt;
            }
            return std::nullopt;
        }();
        if (!valid_index || valid[*valid_index]) return;
        if constexpr (std::tuple_size_v<decltype(Relation::predicate)> != 0U) {
            if (!row.required_export_build_tensorrt || std::get<0>(Relation::predicate)(state) != *row.required_export_build_tensorrt) return;
        }
        const auto source = Relation::source(state);
        const auto input = Relation::input(state);
        const auto& preset = Relation::preset(state);
        const auto resolution = Relation::resolution(state);
        const auto& artifact = Relation::artifact(state);
        if (mmltk::backend::models::rfdetr::find_preset_catalog_entry(preset) == nullptr || resolution <= 0) return;
        if (input == ModelArtifactInputKind::None) {
            valid[*valid_index] = true;
            return;
        }
        if (input != row.input || !model_selection_source_allowed(row, source) || (source == ModelSelectionSource::Custom && artifact.empty())) return;
        valid[*valid_index] = true;
    });
    return std::ranges::all_of(valid, std::identity{});
}
[[nodiscard]] bool valid_annotation_model_selection(const ModelArtifactSelectionState& artifacts) noexcept {
    const bool compatible = artifacts.source == ModelSelectionSource::Canonical
                                ? artifacts.input == ModelArtifactInputKind::Weights
                                : artifacts.source == ModelSelectionSource::Custom && artifacts.input != ModelArtifactInputKind::None;
    return valid_model_selection_draft(artifacts, compatible);
}
[[nodiscard]] bool valid_settings(const GuiSettingsState& state) noexcept {
    return !mmltk::frameworks::reflection::validate_reflected_fields(state).has_value() && valid_train(state.workflows.train) &&
           valid_validate(state.workflows.validate) && valid_predict(state.workflows.predict) && valid_source(state.workflows.annotate.source) &&
           state.workflows.explore.min_instances <= state.workflows.explore.max_instances &&
           state.workflows.explore.min_compiled_index <= state.workflows.explore.max_compiled_index &&
           valid_annotation_model_selection(model_artifacts(state.workflows.annotate)) && valid_model_selection_relations(state);
}
[[nodiscard]] std::filesystem::path effective_train_validation_path(const TrainViewState& train) {
    return train.use_compiled_directory_defaults ? std::filesystem::path{train.compiled_dataset_dir} / "val.bin" : train.request.val_compiled_path;
}
void apply_compiled_directory_defaults(TrainViewState& train) {
    if (!train.use_compiled_directory_defaults) return;
    const std::filesystem::path directory{train.compiled_dataset_dir};
    train.request.train_compiled_path = directory / "train.bin";
    train.request.val_compiled_path = effective_train_validation_path(train);
}
template <class Selection>
void normalize_canonical_source_transition(const Selection& installed, Selection& candidate) noexcept {
    if (installed.model_source != ModelSelectionSource::Canonical && candidate.model_source == ModelSelectionSource::Canonical) {
        candidate.model_input = ModelArtifactInputKind::Weights;
    }
}
void normalize_canonical_source_transitions(const GuiSettingsState& installed, GuiSettingsState& candidate) noexcept {
    normalize_canonical_source_transition(installed.workflows.train, candidate.workflows.train);
    normalize_canonical_source_transition(installed.workflows.validate, candidate.workflows.validate);
    normalize_canonical_source_transition(installed.workflows.predict, candidate.workflows.predict);
    normalize_canonical_source_transition(installed.workflows.annotate, candidate.workflows.annotate);
    normalize_canonical_source_transition(installed.workflows.export_state, candidate.workflows.export_state);
}
}  // namespace
std::expected<void, SettingsMutationError> apply_gui_settings_values(GuiSettingsState& state, const std::span<const SettingsValueUpdate> updates) {
    if (updates.empty() || updates.size() > kGuiSettingsMutableLeafCount) { return std::unexpected(SettingsMutationError::InvalidPath); }
    GuiSettingsState candidate = state;
    std::array<std::string_view, kGuiSettingsMutableLeafCount> paths{};
    std::size_t count = 0U;
    for (const SettingsValueUpdate& update : updates) {
        if (count == paths.size()) return std::unexpected(SettingsMutationError::InvalidPath);
        if (update.path.empty() || update.path.size() > kMaxSettingsPathBytes) { return std::unexpected(SettingsMutationError::InvalidPath); }
        for (std::size_t previous = 0; previous < count; ++previous) {
            if (paths[previous] == update.path) { return std::unexpected(SettingsMutationError::InvalidPath); }
        }
        if (auto result = assign_flat_path(candidate, update.path, update.value); !result) { return result; }
        paths[count++] = update.path;
    }
    const auto& installed_train = state.workflows.train.request;
    const auto& selected_train = candidate.workflows.train.request;
    if (selected_train.train_compiled_path != installed_train.train_compiled_path || selected_train.val_compiled_path != installed_train.val_compiled_path)
        candidate.workflows.train.use_compiled_directory_defaults = false;
    if (selected_train.output_dir != installed_train.output_dir && candidate.workflows.train.auto_output == state.workflows.train.auto_output)
        candidate.workflows.train.auto_output = false;
    apply_compiled_directory_defaults(candidate.workflows.train);
    normalize_canonical_source_transitions(state, candidate);
    if (!valid_settings(candidate)) return std::unexpected(SettingsMutationError::CrossFieldViolation);
    if (candidate.workflows.train.auto_output) candidate.workflows.train.request.output_dir.clear();
    state = std::move(candidate);
    return {};
}
bool gui_settings_valid(const GuiSettingsState& state) noexcept { return valid_settings(state); }
std::filesystem::path resolve_validation_source(const GuiSettingsState& state) {
    const auto& override_path = state.workflows.validate.request.compiled_path;
    if (!override_path.empty()) return override_path;
    return effective_train_validation_path(state.workflows.train);
}
ExploreSourceFact resolve_explore_source(const GuiSettingsState& state) {
    const auto& train = state.workflows.train;
    const auto& explore = state.workflows.explore;
    std::string compiled_source;
    switch (explore.dataset_source) {
        case ExploreDatasetSource::Train: compiled_source = train.request.train_compiled_path; break;
        case ExploreDatasetSource::Validation: compiled_source = train.request.val_compiled_path; break;
        case ExploreDatasetSource::Test: compiled_source = train.request.test_compiled_path; break;
        case ExploreDatasetSource::Custom: compiled_source = explore.custom_compiled_path; break;
    }
    const bool available = !compiled_source.empty();
    return {
        .selection = explore.dataset_source,
        .compiled_source = std::move(compiled_source),
        .available = available,
    };
}
}  // namespace mmltk::controller::contracts
