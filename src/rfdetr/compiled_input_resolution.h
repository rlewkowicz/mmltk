#pragma once

#include "compiled_file_utils.h"

#include <cstdint>
#include <filesystem>
#include <stdexcept>

namespace mmltk::rfdetr {

[[nodiscard]] inline std::uint32_t compiled_input_resolution(const std::filesystem::path& path) {
    const mmltk::CompiledDatasetInfo info = mmltk::inspect_compiled_dataset(path);
    if (info.width != info.height || info.channels != 3U) {
        throw std::runtime_error("compiled RF-DETR input must be square RGB: " + path.string());
    }
    return info.width;
}

}  
