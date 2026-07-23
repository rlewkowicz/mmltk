#include "mmltk/runtime/model_registry.h"

#include "support/catch2_compat.hpp"
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace {

class FakeModule final : public mmltk::runtime::ModelModule {
   public:
    [[nodiscard]] std::string_view module_id() const override {
        return "fake";
    }

    [[nodiscard]] std::string_view display_name() const override {
        return "Fake";
    }

    [[nodiscard]] std::vector<mmltk::runtime::ModelPresetDescriptor> presets() const override {
        return {
            {
                .module_id = "fake",
                .preset_name = "preset-a",
                .display_name = "Preset A",
                .size_label = "A",
                .task = "fake",
                .canonical_weight_filename = "preset-a.fakept",
                .resolution = 32,
            },
            {
                .module_id = "fake",
                .preset_name = "preset-b",
                .display_name = "Preset B",
                .size_label = "B",
                .task = "fake",
                .canonical_weight_filename = "preset-b.fakept",
                .resolution = 64,
            },
        };
    }

    [[nodiscard]] bool owns_preset(std::string_view preset_name) const override {
        return preset_name == "preset-a" || preset_name == "preset-b";
    }

    [[nodiscard]] std::string infer_preset_from_artifact_path(
        const std::filesystem::path& artifact_path) const override {
        if (artifact_path.extension() == ".fakept") {
            return "preset-a";
        }
        return {};
    }
};

void test_registry_resolves_modules_and_presets() {
    mmltk::runtime::ModelRegistry registry;
    registry.register_module(std::make_shared<FakeModule>());

    assert(!registry.empty());
    assert(registry.find_module("fake") != nullptr);
    assert(registry.find_module("missing") == nullptr);
    assert(registry.find_module_for_preset("preset-a") != nullptr);
    assert(registry.find_module_for_preset("missing") == nullptr);
    assert(registry.find_module_for_artifact_path("/tmp/model.fakept") != nullptr);
    assert(registry.find_module_for_artifact_path("/tmp/model.onnx") == nullptr);

    const auto presets = registry.presets();
    assert(presets.size() == 2U);
    assert(presets[0].module_id == "fake");
    assert(presets[0].preset_name == "preset-a");
    assert(presets[1].preset_name == "preset-b");
}

}  

MMLTK_REGISTER_TEST_CASE("[core][model_registry]", test_registry_resolves_modules_and_presets);
