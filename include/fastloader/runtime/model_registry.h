#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastloader::runtime {

struct ModelPresetDescriptor {
    std::string module_id;
    std::string preset_name;
    std::string display_name;
};

class ModelModule {
public:
    virtual ~ModelModule() = default;

    virtual std::string_view module_id() const = 0;
    virtual std::string_view display_name() const = 0;
    virtual std::vector<ModelPresetDescriptor> presets() const = 0;
    virtual bool owns_preset(std::string_view preset_name) const = 0;
};

class ModelRegistry {
public:
    void register_module(std::shared_ptr<const ModelModule> module);

    const ModelModule* find_module(std::string_view module_id) const;
    const ModelModule* find_module_for_preset(std::string_view preset_name) const;
    std::vector<ModelPresetDescriptor> presets() const;
    bool empty() const noexcept;

private:
    std::vector<std::shared_ptr<const ModelModule>> modules_;
};

} // namespace fastloader::runtime
