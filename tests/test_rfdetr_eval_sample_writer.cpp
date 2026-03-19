#include "fastloader/rfdetr/draw_cuda.h"

#include <c10/cuda/CUDAGuard.h>
#include <cuda_runtime.h>
#include <torch/torch.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* name_prefix) {
        std::string pattern = (fs::temp_directory_path() / (std::string(name_prefix) + ".XXXXXX")).string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        }
        path_ = fs::path(created);
    }

    ~ScopedTempDir() {
        if (!path_.empty()) {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
    }

    const fs::path& path() const {
        return path_;
    }

private:
    fs::path path_;
};

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

    const ScopedTempDir temp_dir("fastloader_eval_sample_writer");
    const fs::path output_path = temp_dir.path() / "sample.jpg";

    auto image = torch::full({3, 16, 16},
                             96.0f,
                             torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, 0));
    auto boxes = torch::tensor({{2.0f, 2.0f, 13.0f, 13.0f}},
                               torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA, 0));
    auto labels = torch::tensor({1},
                                torch::TensorOptions().dtype(torch::kInt64).device(torch::kCUDA, 0));
    auto masks = torch::zeros({1, 16, 16},
                              torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA, 0));
    masks[0].slice(0, 4, 12).slice(1, 4, 12).fill_(true);

    fastloader::rfdetr::RenderSampleOptions options;
    options.output_path = output_path;
    options.num_classes = 4;

    fastloader::rfdetr::draw_eval_sample_async_gpu(image, boxes, labels, masks, options);
    fastloader::rfdetr::flush_eval_sample_writes();

    const bool output_exists = fs::exists(output_path);
    assert(output_exists);
    const auto output_size = fs::file_size(output_path);
    assert(output_size > 0);
}

} // namespace

int main() {
    try {
        test_eval_sample_writer_flushes_output();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_rfdetr_eval_sample_writer error: %s\n", error.what());
    } catch (...) {
        std::fputs("test_rfdetr_eval_sample_writer error: unknown exception\n", stderr);
    }
    return 1;
}
