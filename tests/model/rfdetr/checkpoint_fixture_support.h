#pragma once

#include "mmltk/rfdetr/checkpoint.h"
#include "parity_fixture_support.h"
#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
#include "rfdetr/python_checkpoint_bridge.h"
#endif

#include <cstdio>
#include <filesystem>
#include <utility>
#include <vector>

namespace mmltk::rfdetr::testsupport {

#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
inline std::vector<StateDictEntry> collect_module_state(const torch::nn::Module& module) {
    std::vector<StateDictEntry> state_dict;
    for (const auto& item : module.named_parameters(true)) {
        state_dict.push_back({item.key(), item.value().detach().cpu().clone()});
    }
    for (const auto& item : module.named_buffers(true)) {
        state_dict.push_back({item.key(), item.value().detach().cpu().clone()});
    }
    return state_dict;
}

inline std::vector<StateDictEntry> make_minimal_upstream_checkpoint_state(const ParityFixtureCase& fixture) {
    std::vector<StateDictEntry> state_dict;
    state_dict.reserve(4);
    state_dict.push_back({"query_feat.weight", make_fixture_query_feat(fixture)});
    state_dict.push_back({"refpoint_embed.weight", make_fixture_refpoint_embed(fixture)});
    state_dict.push_back({"class_embed.weight", make_fixture_class_weight(fixture)});
    state_dict.push_back({"class_embed.bias", make_fixture_class_bias(fixture)});
    return state_dict;
}

inline void save_upstream_python_checkpoint(const std::filesystem::path& path,
                                            const std::vector<StateDictEntry>& state_dict) {
    mmltk::rfdetr::write_upstream_python_checkpoint(path, state_dict);
}

inline void write_minimal_upstream_checkpoint(const std::filesystem::path& path, const ParityFixtureCase& fixture) {
    save_upstream_python_checkpoint(path, make_minimal_upstream_checkpoint_state(fixture));
}

inline void write_module_upstream_checkpoint(const std::filesystem::path& path, const torch::nn::Module& module) {
    save_upstream_python_checkpoint(path, collect_module_state(module));
}
#endif

inline void log_fixture_phase(const char* test_name, size_t index, size_t total, const char* phase,
                              const char* preset_name) {
    std::fprintf(stderr, "%s: %zu/%zu %s %s\n", test_name, index, total, phase, preset_name);
    std::fflush(stderr);
}

}  // namespace mmltk::rfdetr::testsupport
