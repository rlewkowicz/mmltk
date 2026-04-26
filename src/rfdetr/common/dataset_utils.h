#pragma once

#include "dataset_loader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mmltk::rfdetr {

inline std::vector<std::string> loader_class_names(const DatasetLoader& loader) {
    std::vector<std::string> names;
    names.reserve(loader.num_classes());
    for (uint32_t index = 0; index < loader.num_classes(); ++index) {
        names.emplace_back(loader.class_name(index));
    }
    return names;
}

inline int64_t image_id_for_dataset_index(const std::vector<int>& image_ids, int64_t dataset_index) {
    if (dataset_index >= 0 && static_cast<size_t>(dataset_index) < image_ids.size()) {
        return image_ids[static_cast<size_t>(dataset_index)];
    }
    return dataset_index + 1;
}

}  // namespace mmltk::rfdetr
