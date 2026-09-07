#include "src/backend/models/catalog/module.h"

#include <array>
#include <cstddef>
#include <span>

#include "src/backend/models/catalog/artifacts.h"
#include "src/backend/models/catalog/model_registry.h"
#include "src/backend/models/rfdetr/contract/contract.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"

namespace mmltk::backend::models::catalog::detail {

namespace {

template <std::size_t PresetCount, class ContractContribution>
consteval auto project_presets(const ContractContribution& contribution) {
    std::array<ModelPresetDescriptor, PresetCount> presets{};
    if (contribution.presets.size() != PresetCount) throw "model contract preset extent does not match its catalog projection";
    for (std::size_t index = 0U; index < presets.size(); ++index) {
        const auto& source = contribution.presets[index];
        const auto task = model_task_name(source.task);
        if (!task) throw "model task lacks a catalog projection";
        presets[index] = {
            .model_id = contribution.model_id,
            .preset_name = source.preset_name,
            .display_name = source.display_name,
            .size_label = source.size_label,
            .task = *task,
            .canonical_weight_filename = source.canonical_weight_filename,
            .resolution = source.resolution,
        };
    }
    return presets;
}

template <class ContractContribution>
consteval ModelDescriptor project_model(const ContractContribution& contribution, const std::span<const ModelPresetDescriptor> presets) {
    return {
        .model_id = contribution.model_id,
        .display_name = contribution.display_name,
        .capabilities =
            {
                .weights = contribution.capabilities.weights,
                .onnx = contribution.capabilities.onnx,
                .tensorrt = contribution.capabilities.tensorrt,
                .training = contribution.capabilities.training,
                .live = contribution.capabilities.live,
            },
        .presets = presets,
        .infer_artifact_preset = contribution.infer_artifact_preset,
    };
}

inline constexpr auto kRfdetrPresets =
    project_presets<mmltk::backend::models::rfdetr::kPresetCatalog.size()>(mmltk::backend::models::rfdetr::kRfdetrContract);
inline constexpr std::array kModels{
    project_model(mmltk::backend::models::rfdetr::kRfdetrContract, kRfdetrPresets),
    // CLEANUP-IGNORE: The projected model inventory closes independently from validation of its source preset rows.
};

[[nodiscard]] consteval bool catalog_is_valid() {
    // CLEANUP-IGNORE: Model-catalog validation audits projected models; preset validation owns different invariants.
    for (std::size_t model_index = 0U; model_index < kModels.size(); ++model_index) {
        const auto& model = kModels[model_index];
        if (model.model_id.empty() || model.display_name.empty() || model.presets.empty() || model.infer_artifact_preset == nullptr) {
            return false;
        }
        for (std::size_t sibling_index = model_index + 1U; sibling_index < kModels.size(); ++sibling_index) {
            if (model.model_id == kModels[sibling_index].model_id) return false;
        }
        for (std::size_t preset_index = 0U; preset_index < model.presets.size(); ++preset_index) {
            const auto& preset = model.presets[preset_index];
            if (preset.model_id != model.model_id || preset.preset_name.empty() || preset.display_name.empty() ||
                preset.size_label.empty() || preset.task.empty() || preset.canonical_weight_filename.empty() || preset.resolution == 0U) {
                return false;
            }
            for (std::size_t sibling_model_index = model_index; sibling_model_index < kModels.size(); ++sibling_model_index) {
                const auto& sibling_model = kModels[sibling_model_index];
                const std::size_t first_sibling_preset = sibling_model_index == model_index ? preset_index + 1U : 0U;
                for (std::size_t sibling_preset_index = first_sibling_preset; sibling_preset_index < sibling_model.presets.size();
                     ++sibling_preset_index) {
                    if (preset.preset_name == sibling_model.presets[sibling_preset_index].preset_name) { return false; }
                }
            }
        }
    }
    return true;
}

static_assert(catalog_is_valid());

}  // namespace

std::span<const ModelDescriptor> models() noexcept { return kModels; }

}  // namespace mmltk::backend::models::catalog::detail
