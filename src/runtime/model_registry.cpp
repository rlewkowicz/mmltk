#include "fastloader/runtime/model_registry.h"

#include <stdexcept>
#include <utility>

namespace fastloader::runtime {

void ModelRegistry::register_module(std::shared_ptr<const ModelModule> module) {
    if (!module) {
        throw std::runtime_error("model registry requires a non-null module");
    }
    if (find_module(module->module_id()) != nullptr) {
        throw std::runtime_error("duplicate model module registration: " + std::string(module->module_id()));
    }
    modules_.push_back(std::move(module));
}

const ModelModule* ModelRegistry::find_module(std::string_view module_id) const {
    for (const auto& module : modules_) {
        if (module->module_id() == module_id) {
            return module.get();
        }
    }
    return nullptr;
}

const ModelModule* ModelRegistry::find_module_for_preset(std::string_view preset_name) const {
    for (const auto& module : modules_) {
        if (module->owns_preset(preset_name)) {
            return module.get();
        }
    }
    return nullptr;
}

std::vector<ModelPresetDescriptor> ModelRegistry::presets() const {
    std::vector<ModelPresetDescriptor> descriptors;
    for (const auto& module : modules_) {
        std::vector<ModelPresetDescriptor> module_presets = module->presets();
        descriptors.insert(descriptors.end(),
                           std::make_move_iterator(module_presets.begin()),
                           std::make_move_iterator(module_presets.end()));
    }
    return descriptors;
}

bool ModelRegistry::empty() const noexcept {
    return modules_.empty();
}

} // namespace fastloader::runtime
