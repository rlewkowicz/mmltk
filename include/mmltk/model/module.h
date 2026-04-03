#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::model {

struct ModelPresetDescriptor {
    std::string module_id;
    std::string preset_name;
    std::string display_name;
};

struct ModelModuleCapabilities {
    bool supports_weights = false;
    bool supports_onnx = false;
    bool supports_tensorrt = false;
    bool supports_training = false;
    bool supports_live = false;
};

class ModelModule {
public:
    virtual ~ModelModule() = default;

    [[nodiscard]] virtual std::string_view module_id() const = 0;
    [[nodiscard]] virtual std::string_view display_name() const = 0;
    [[nodiscard]] virtual std::vector<ModelPresetDescriptor> presets() const = 0;
    [[nodiscard]] virtual bool owns_preset(std::string_view preset_name) const = 0;
    [[nodiscard]] virtual std::string infer_preset_from_artifact_path(const std::filesystem::path& artifact_path) const;
    [[nodiscard]] virtual ModelModuleCapabilities capabilities() const;
};

class ModelRegistry {
public:
    void register_module(std::shared_ptr<const ModelModule> module);

    [[nodiscard]] const ModelModule* find_module(std::string_view module_id) const;
    [[nodiscard]] const ModelModule* find_module_for_preset(std::string_view preset_name) const;
    [[nodiscard]] const ModelModule* find_module_for_artifact_path(const std::filesystem::path& artifact_path) const;
    [[nodiscard]] std::vector<ModelPresetDescriptor> presets() const;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::vector<std::shared_ptr<const ModelModule>> modules_;
};

} // namespace mmltk::model
