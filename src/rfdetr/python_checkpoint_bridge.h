#pragma once

#include "mmltk/rfdetr/checkpoint.h"

#include <filesystem>
#include <vector>

namespace mmltk::rfdetr {

NativeCheckpoint load_upstream_python_checkpoint(const std::filesystem::path& checkpoint_path);
std::vector<StateDictEntry> load_upstream_python_state_dict(const std::filesystem::path& checkpoint_path);
void write_upstream_python_checkpoint(const std::filesystem::path& checkpoint_path,
                                      const std::vector<StateDictEntry>& state_dict);

} // namespace mmltk::rfdetr
