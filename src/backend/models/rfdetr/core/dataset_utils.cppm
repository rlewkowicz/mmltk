module;
#include <cstdint>
#include <string>
#include <vector>

#include "src/backend/data/dataset_loader.h"

export module mmltk.backend.models.rfdetr.core.dataset_utils;

export namespace mmltk::backend::models::rfdetr {

struct PredictionBatchMetadata {
    int64_t dataset_index = 0;
    int64_t image_id = 0;
    std::string source_name;
};

inline std::vector<std::string> loader_class_names(const mmltk::backend::data::DatasetLoader& loader) {
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

// Projects a loader batch onto the per-image metadata every prediction and evaluation path consumes.
// Batch slot -> dataset index -> image id is one mapping; it lives here rather than being rebuilt at
// each call site.
inline std::vector<PredictionBatchMetadata> make_prediction_batch_metadata(const mmltk::backend::data::Batch& batch,
                                                                           const std::vector<int>& image_ids) {
    std::vector<PredictionBatchMetadata> metadata;
    metadata.reserve(batch.num_images);
    for (size_t image_index = 0; image_index < batch.num_images; ++image_index) {
        const auto dataset_index = static_cast<int64_t>(batch.image_indices[image_index]);
        metadata.push_back(PredictionBatchMetadata{
            dataset_index,
            image_id_for_dataset_index(image_ids, dataset_index),
            {},
        });
    }
    return metadata;
}

}  // namespace mmltk::backend::models::rfdetr
