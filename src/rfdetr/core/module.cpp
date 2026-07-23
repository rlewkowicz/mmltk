#include "mmltk/rfdetr/module.h"

#include "mmltk/rfdetr/model_config.h"
#include "mmltk/rfdetr/preset_catalog.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::rfdetr {

namespace {

class RfdetrModelModule final : public mmltk::model::ModelModule {
   public:
    [[nodiscard]] std::string_view module_id() const override {
        return "rfdetr";
    }

    [[nodiscard]] std::string_view display_name() const override {
        return "RF-DETR";
    }

    [[nodiscard]] std::vector<mmltk::model::ModelPresetDescriptor> presets() const override {
        std::vector<mmltk::model::ModelPresetDescriptor> descriptors;
        const auto& available_presets = mmltk::rfdetr::model_presets();
        descriptors.reserve(available_presets.size());
        for (const auto& preset : available_presets) {
            const auto* metadata = mmltk::rfdetr::find_preset_catalog_entry(preset.preset_name);
            if (metadata == nullptr || metadata->canonical_weight_filename != preset.canonical_weight_filename ||
                metadata->resolution != static_cast<std::uint32_t>(preset.resolution) ||
                (metadata->task == ModelTask::Segmentation) != preset.segmentation_head) {
                throw std::logic_error("RF-DETR runtime preset does not match the canonical catalog: " +
                                       std::string(preset.preset_name));
            }
            descriptors.push_back(mmltk::model::ModelPresetDescriptor{
                "rfdetr",
                std::string(preset.preset_name),
                std::string(metadata->display_name),
                std::string(metadata->size_label),
                std::string(model_task_name(metadata->task)),
                std::string(metadata->canonical_weight_filename),
                metadata->resolution,
            });
        }
        return descriptors;
    }

    [[nodiscard]] bool owns_preset(std::string_view preset_name) const override {
        return mmltk::rfdetr::find_model_preset(preset_name) != nullptr;
    }

    [[nodiscard]] std::string infer_preset_from_artifact_path(
        const std::filesystem::path& artifact_path) const override {
        if (const auto* preset = mmltk::rfdetr::infer_model_preset_from_path(artifact_path)) {
            return std::string(preset->preset_name);
        }
        return {};
    }

    [[nodiscard]] mmltk::model::ModelModuleCapabilities capabilities() const override {
        return {
            true, true, true, true, true,
        };
    }
};

}  

std::shared_ptr<const mmltk::model::ModelModule> make_model_module() {
    return std::make_shared<RfdetrModelModule>();
}

}  
