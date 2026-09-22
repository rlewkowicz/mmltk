module;
#include <cstddef>
#include <filesystem>
#include <memory>
#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
export module mmltk.backend.models.rfdetr.inference.loader;
export namespace mmltk::backend::models::rfdetr::inference_detail {
[[nodiscard]] std::unique_ptr<mmltk::backend::data::DatasetLoader> make_loader(const std::filesystem::path& compiled_path, std::size_t batch_size, const InferenceExecutionConfig& execution,
 std::size_t prefetch_factor, std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement = {});
}
