#pragma once
#include <vector>
#include "model_technical.h"
namespace mmltk::backend::models::rfdetr::testsupport {
inline std::vector<NormalizedModelStateEntry> clone_normalized_model_state(const torch::nn::Module& module, const bool copy_to_cpu = false) {
    const auto parameters = module.named_parameters(true);
    const auto buffers = module.named_buffers(true);
    std::vector<NormalizedModelStateEntry> state;
    state.reserve(parameters.size() + buffers.size());
    const auto append = [&](const auto& item) {
        auto tensor = item.value().detach();
        if (copy_to_cpu) { tensor = tensor.cpu(); }
        state.push_back({item.key(), tensor.clone()});
    };
    for (const auto& parameter : parameters) { append(parameter); }
    for (const auto& buffer : buffers) { append(buffer); }
    return state;
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
