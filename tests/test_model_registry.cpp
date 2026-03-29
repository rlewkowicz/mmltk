#include "fastloader/runtime/model_registry.h"

#include <cassert>
#include <memory>
#include <string_view>
#include <vector>

namespace {

class FakeModule final : public fastloader::runtime::ModelModule {
public:
    std::string_view module_id() const override {
        return "fake";
    }

    std::string_view display_name() const override {
        return "Fake";
    }

    std::vector<fastloader::runtime::ModelPresetDescriptor> presets() const override {
        return {
            {"fake", "preset-a", "Preset A"},
            {"fake", "preset-b", "Preset B"},
        };
    }

    bool owns_preset(std::string_view preset_name) const override {
        return preset_name == "preset-a" || preset_name == "preset-b";
    }
};

void test_registry_resolves_modules_and_presets() {
    fastloader::runtime::ModelRegistry registry;
    registry.register_module(std::make_shared<FakeModule>());

    assert(!registry.empty());
    assert(registry.find_module("fake") != nullptr);
    assert(registry.find_module("missing") == nullptr);
    assert(registry.find_module_for_preset("preset-a") != nullptr);
    assert(registry.find_module_for_preset("missing") == nullptr);

    const auto presets = registry.presets();
    assert(presets.size() == 2U);
    assert(presets[0].module_id == "fake");
    assert(presets[0].preset_name == "preset-a");
    assert(presets[1].preset_name == "preset-b");
}

} // namespace

int main() {
    test_registry_resolves_modules_and_presets();
    return 0;
}
