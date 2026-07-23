#include "mmltk/rfdetr/draw_cuda.h"

#include <c10/cuda/CUDAGuard.h>
#include <cuda_runtime.h>
#include <torch/torch.h>

#include "support/catch2_compat.hpp"
#include "support/filesystem_test_utils.hpp"
#include <filesystem>
#include <stdexcept>
#include <string>

#include "stb_image.h"

namespace fs = std::filesystem;

namespace {

void require_cuda_device() {
    int device_count = 0;
    const cudaError_t status = ::cudaGetDeviceCount(&device_count);
    if (status != cudaSuccess || device_count <= 0) {
        throw std::runtime_error("test_rfdetr_eval_sample_writer requires at least one CUDA device");
    }
}

void test_eval_sample_writer_flushes_output() {
    require_cuda_device();
    c10::cuda::CUDAGuard guard(0);

    const mmltk::testsupport::ScopedTempDir temp_dir("mmltk_eval_sample_writer");
    const fs::path output_path = temp_dir.path() / "sample.png";

    auto image = torch::full({3, 16, 16}, 0.35f, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, 0));
    auto boxes = torch::tensor({{2.0f, 2.0f, 13.0f, 13.0f}},
                               torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, 0));
    auto labels = torch::tensor({1}, torch::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA, 0));
    auto masks = torch::zeros({1, 16, 16}, torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA, 0));
    masks[0].slice(0, 4, 12).slice(1, 4, 12).fill_(true);

    mmltk::rfdetr::RenderSampleOptions options;
    options.output_path = output_path;
    options.num_classes = 4;

    mmltk::rfdetr::draw_eval_sample_async_gpu(image, boxes, labels, masks, options);
    mmltk::rfdetr::flush_eval_sample_writes();

    const bool output_exists = fs::exists(output_path);
    assert(output_exists);
    const auto output_size = fs::file_size(output_path);
    assert(output_size > 0);

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(output_path.c_str(), &width, &height, &channels, 3);
    if (decoded == nullptr) {
        throw std::runtime_error("stbi_load failed for eval sample output");
    }

    const int center_offset = ((height / 2) * width + (width / 2)) * 3;
    const int center_sum = static_cast<int>(decoded[center_offset]) + static_cast<int>(decoded[center_offset + 1]) +
                           static_cast<int>(decoded[center_offset + 2]);
    stbi_image_free(decoded);
    assert(width == 16);
    assert(height == 16);
    assert(center_sum > 100);
}

}  

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][eval_sample_writer]", test_eval_sample_writer_flushes_output);
