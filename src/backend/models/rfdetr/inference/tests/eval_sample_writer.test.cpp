#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include "src/backend/models/rfdetr/inference/evaluation.h"
#include "src/backend/models/rfdetr/inference/validate.h"
// RF-DETR inference sample-output coverage.
#include <cuda_runtime.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include "catch2_compat.hpp"
#include "draw.h"
#include "filesystem_test_utils.hpp"
#include "stb_image.h"
#include "torch_api.h"
#include "torch_cuda_utils.h"
// CLEANUP-IGNORE: The evaluation writer's direct CUDA/render boundary is independent of the training fixture boundary.
import mmltk.backend.ml.cuda.gpu_quiescence;
import mmltk.backend.models.rfdetr.inference.analysis_provider;
import mmltk.backend.models.rfdetr.inference.prediction;
import mmltk.backend.models.rfdetr.inference.runtime_backend;
namespace cuda_api = mmltk::backend::ml::cuda;
namespace tensor_api = mmltk::backend::ml::torch_api;
namespace fs = std::filesystem;
namespace {
void require_cuda_device() {
    int device_count = 0;
    const cudaError_t status = ::cudaGetDeviceCount(&device_count);
    if (status != cudaSuccess || device_count <= 0) { throw std::runtime_error("test_rfdetr_eval_sample_writer requires at least one CUDA device"); }
}
void test_eval_sample_writer_flushes_output() {
    require_cuda_device();
    cuda_api::TorchCudaDeviceGuard guard(static_cast<cuda_api::TorchDeviceIndex>(0));
    const mmltk::testsupport::ScopedTempDir temp_dir("mmltk_eval_sample_writer");
    const fs::path output_path = temp_dir.path() / "sample.png";
    auto image = tensor_api::full({3, 16, 16}, 0.35f, tensor_api::TensorOptions().dtype(tensor_api::kFloat32).device(tensor_api::kCUDA, 0));
    auto boxes = tensor_api::tensor({{2.0f, 2.0f, 13.0f, 13.0f}}, tensor_api::TensorOptions().dtype(tensor_api::kFloat32).device(tensor_api::kCUDA, 0));
    auto labels = tensor_api::tensor({1}, tensor_api::TensorOptions().dtype(tensor_api::kInt64).device(tensor_api::kCUDA, 0));
    auto masks = tensor_api::zeros({1, 16, 16}, tensor_api::TensorOptions().dtype(tensor_api::kBool).device(tensor_api::kCUDA, 0));
    masks[0].slice(0, 4, 12).slice(1, 4, 12).fill_(true);
    mmltk::backend::models::rfdetr::RenderSampleOptions options;
    options.output_path = output_path;
    options.num_classes = 4;
    mmltk::backend::models::rfdetr::draw_eval_sample_async_gpu(image, boxes, labels, masks, options);
    mmltk::backend::models::rfdetr::flush_eval_sample_writes();
    const bool output_exists = fs::exists(output_path);
    MMLTK_ASSERT(output_exists);
    const auto output_size = fs::file_size(output_path);
    MMLTK_ASSERT(output_size > 0);
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(output_path.c_str(), &width, &height, &channels, 3);
    if (decoded == nullptr) { throw std::runtime_error("stbi_load failed for eval sample output"); }
    const int center_offset = ((height / 2) * width + (width / 2)) * 3;
    const int center_sum =
        static_cast<int>(decoded[center_offset]) + static_cast<int>(decoded[center_offset + 1]) + static_cast<int>(decoded[center_offset + 2]);
    stbi_image_free(decoded);
    MMLTK_ASSERT(width == 16);
    MMLTK_ASSERT(height == 16);
    MMLTK_ASSERT(center_sum > 100);
}
}  // namespace
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][eval_sample_writer]", test_eval_sample_writer_flushes_output);
