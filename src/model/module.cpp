#include "mmltk/model/module.h"

#include <stdexcept>
#include <utility>

namespace mmltk::model {

namespace {

template <typename Predicate>
const ModelModule* find_module_if(const std::vector<std::shared_ptr<const ModelModule>>& modules,
                                  Predicate&& predicate) {
    for (const auto& module : modules) {
        if (predicate(*module)) {
            return module.get();
        }
    }
    return nullptr;
}

}  

std::string ModelModule::infer_preset_from_artifact_path(const std::filesystem::path&) const {
    return {};
}

ModelModuleCapabilities ModelModule::capabilities() const {
    return {};
}

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
    return find_module_if(modules_, [module_id](const ModelModule& module) { return module.module_id() == module_id; });
}

const ModelModule* ModelRegistry::find_module_for_preset(std::string_view preset_name) const {
    return find_module_if(modules_,
                          [preset_name](const ModelModule& module) { return module.owns_preset(preset_name); });
}

const ModelModule* ModelRegistry::find_module_for_artifact_path(const std::filesystem::path& artifact_path) const {
    return find_module_if(modules_, [&artifact_path](const ModelModule& module) {
        return !module.infer_preset_from_artifact_path(artifact_path).empty();
    });
}

std::vector<ModelPresetDescriptor> ModelRegistry::presets() const {
    std::vector<ModelPresetDescriptor> descriptors;
    for (const auto& module : modules_) {
        std::vector<ModelPresetDescriptor> module_presets = module->presets();
        descriptors.insert(descriptors.end(), std::make_move_iterator(module_presets.begin()),
                           std::make_move_iterator(module_presets.end()));
    }
    return descriptors;
}

bool ModelRegistry::empty() const noexcept {
    return modules_.empty();
}

}  
