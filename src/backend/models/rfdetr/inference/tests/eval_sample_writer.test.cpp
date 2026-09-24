#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include "src/backend/models/rfdetr/inference/evaluation.h"
#include "src/backend/models/rfdetr/inference/validate.h"
// RF-DETR inference sample-output coverage.
#include <cuda_runtime.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include "src/backend/models/rfdetr/core/sample_output.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "stb_image.h"
#include <torch/types.h>
#include <torch/serialize.h>
#include "src/backend/ml/cuda/torch_cuda_utils.h"
// CLEANUP-IGNORE: The evaluation writer's direct CUDA/render boundary is independent of the training fixture boundary.
import mmltk.backend.ml.cuda.gpu_quiescence;
namespace cuda_api = mmltk::backend::ml::cuda;
namespace fs = std::filesystem;
namespace {
int require_cuda_devices() {
 int device_count = 0;
 const cudaError_t status = ::cudaGetDeviceCount(&device_count);
 if (status != cudaSuccess || device_count <= 0) { throw std::runtime_error("test_rfdetr_eval_sample_writer requires at least one CUDA device"); }
 return device_count;
}
void require_eval_sample_output(mmltk::backend::models::rfdetr::EvaluationSampleWriter& writer, const int device) {
 cuda_api::TorchCudaDeviceGuard guard(cuda_api::checked_device_index(device));
 const mmltk::testsupport::ScopedTempDir temp_dir("mmltk_eval_sample_writer");
 const fs::path output_path = temp_dir.path() / "sample.png";
 auto image = torch::full({3, 16, 16}, 0.35f, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, device));
 auto boxes = torch::tensor({{2.0f, 2.0f, 13.0f, 13.0f}}, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, device));
 auto labels = torch::tensor({1}, torch::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA, device));
 auto masks = torch::zeros({1, 16, 16}, torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA, device));
 masks[0].slice(0, 4, 12).slice(1, 4, 12).fill_(true);
 mmltk::backend::models::rfdetr::RenderSampleOptions options;
 options.output_path = output_path;
 options.num_classes = 4;
 writer.Draw(image, boxes, labels, masks, options);
 writer.Flush();
 const bool output_exists = fs::exists(output_path);
 REQUIRE((output_exists));
 const auto output_size = fs::file_size(output_path);
 REQUIRE((output_size > 0));
 int width = 0;
 int height = 0;
 int channels = 0;
 stbi_uc* decoded = stbi_load(output_path.c_str(), &width, &height, &channels, 3);
 if (decoded == nullptr) { throw std::runtime_error("stbi_load failed for eval sample output"); }
 const int center_offset = ((height / 2) * width + (width / 2)) * 3;
 const int center_sum = static_cast<int>(decoded[center_offset]) + static_cast<int>(decoded[center_offset + 1]) + static_cast<int>(decoded[center_offset + 2]);
 stbi_image_free(decoded);
 REQUIRE((width == 16));
 REQUIRE((height == 16));
 REQUIRE((center_sum > 100));
}
}  // namespace
TEST_CASE("test_eval_sample_writer_flushes_output", "[model][rfdetr][eval_sample_writer]") {
 const int device_count = require_cuda_devices();
 for (int device = 0; device < device_count; ++device) {
  CAPTURE(device);
  mmltk::backend::models::rfdetr::EvaluationSampleWriter writer;
  require_eval_sample_output(writer, device);
  require_eval_sample_output(writer, device);
 }
}
