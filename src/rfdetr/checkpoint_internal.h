#pragma once

#include "mmltk/rfdetr/checkpoint.h"

#include <filesystem>
#include <torch/serialize.h>
#include <string_view>

namespace mmltk::rfdetr::detail {

inline std::filesystem::path canonical_checkpoint_path(const std::filesystem::path& checkpoint_path) {
    return std::filesystem::absolute(checkpoint_path).lexically_normal();
}

inline std::string canonical_checkpoint_path_string(const std::filesystem::path& checkpoint_path) {
    return canonical_checkpoint_path(checkpoint_path).string();
}

inline bool is_supported_native_checkpoint_format(std::string_view format) {
    return format == kNativeCheckpointFormat || format == "fastloader.rfdetr.native_checkpoint";
}

torch::Tensor prepare_tensor_for_checkpoint_write(const torch::Tensor& tensor);
void write_state_archive(torch::serialize::OutputArchive& archive,
                         const char* key,
                         const std::vector<StateDictEntry>& state_dict);

} // namespace mmltk::rfdetr::detail
