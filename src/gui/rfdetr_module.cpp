#include "rfdetr_module.h"

#include "fastloader/rfdetr/model_config.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastloader::gui {

namespace {

class RfdetrModelModule final : public fastloader::runtime::ModelModule {
public:
    std::string_view module_id() const override {
        return "rfdetr";
    }

    std::string_view display_name() const override {
        return "RF-DETR";
    }

    std::vector<fastloader::runtime::ModelPresetDescriptor> presets() const override {
        std::vector<fastloader::runtime::ModelPresetDescriptor> descriptors;
        const auto& available_presets = fastloader::rfdetr::model_presets();
        descriptors.reserve(available_presets.size());
        for (const auto& preset : available_presets) {
            descriptors.push_back(fastloader::runtime::ModelPresetDescriptor{
                "rfdetr",
                std::string(preset.preset_name),
                std::string(preset.preset_name),
            });
        }
        return descriptors;
    }

    bool owns_preset(std::string_view preset_name) const override {
        return fastloader::rfdetr::find_model_preset(preset_name) != nullptr;
    }
};

} // namespace

std::shared_ptr<const fastloader::runtime::ModelModule> make_rfdetr_model_module() {
    return std::make_shared<RfdetrModelModule>();
}

} // namespace fastloader::gui
