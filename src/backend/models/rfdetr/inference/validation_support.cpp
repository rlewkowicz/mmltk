module;
#include <filesystem>
#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"

module mmltk.backend.models.rfdetr.inference.loader;

namespace mmltk::backend::models::rfdetr::inference_detail {

std::unique_ptr<mmltk::backend::data::DatasetLoader> make_loader(const std::filesystem::path& compiled_path, const std::size_t batch_size,
                                                                 const InferenceExecutionConfig& execution,
                                                                 const std::size_t prefetch_factor) {
    if (compiled_path.empty() || batch_size == 0 || prefetch_factor == 0 ||
        prefetch_factor > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("invalid RF-DETR inference loader setup");
    }
    mmltk::backend::data::DatasetLoader::Config config;
    config.compiled_path = std::filesystem::absolute(compiled_path).string();
    config.batch_size = batch_size;
    config.shuffle = false;
    config.device_id = execution.device_id;
    config.loading = execution;
    config.cpu_affinity = execution.cpu_affinity;
    config.prefetch_factor = static_cast<int>(prefetch_factor);
    config.gather_workers = execution.workers == 0 ? config.prefetch_factor : std::min(execution.workers, config.prefetch_factor);
    config.drop_last = false;
    return std::make_unique<mmltk::backend::data::DatasetLoader>(std::move(config));
}

}  // namespace mmltk::backend::models::rfdetr::inference_detail
