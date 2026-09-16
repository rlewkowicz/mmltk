#pragma once
#include <filesystem>
#include <vector>
#include <memory>
#include <torch/serialize/input-archive.h>
#include "model_technical.h"
namespace mmltk::backend::models::rfdetr::detail {
struct ModelStateTechnicalOwner {
    std::vector<NormalizedModelStateEntry> entries;
    std::unique_ptr<torch::serialize::InputArchive> native_archive;
};
}  // namespace mmltk::backend::models::rfdetr::detail
