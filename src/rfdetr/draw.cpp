#include "mmltk/rfdetr/draw_cuda.h"
#include "rfdetr/backends_internal.h"
#include "rfdetr/torch_cuda_utils.h"
#include "string_utils.h"
#include "worker_pool.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>

#include <deque>
#include <cmath>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <stb_image_write.h>

namespace mmltk::rfdetr {

namespace {

class CudaEventHandle final {
public:
    explicit CudaEventHandle(cudaEvent_t event) : event_(event) {}

    ~CudaEventHandle() {
        if (event_ != nullptr) {
            cudaEventDestroy(event_);
        }
    }

    CudaEventHandle(const CudaEventHandle&) = delete;
    CudaEventHandle& operator=(const CudaEventHandle&) = delete;

    [[nodiscard]] cudaEvent_t get() const { return event_; }

private:
    cudaEvent_t event_ = nullptr;
};

struct PendingEvalSampleWrite {
    std::string output_path;
    int width = 0;
    int height = 0;
    int device_id = 0;
    torch::Tensor image_host;
    std::shared_ptr<CudaEventHandle> ready_event;
};

struct EvalSampleWriterState {
    mmltk::WorkerPool pool{1, {}, "rfdwrite"};
    std::mutex mutex;
    std::deque<std::future<void>> futures;
};

EvalSampleWriterState& eval_sample_writer_state() {
    static EvalSampleWriterState state;
    return state;
}

void write_eval_sample_image(const PendingEvalSampleWrite& pending) {
    const std::string extension =
        strings::to_lower(std::filesystem::path(pending.output_path).extension().string());
    if (extension == ".jpg" || extension == ".jpeg") {
        if (stbi_write_jpg(pending.output_path.c_str(),
                           pending.width,
                           pending.height,
                           3,
                           pending.image_host.data_ptr<uint8_t>(),
                           98) == 0) {
            throw std::runtime_error("failed to write eval sample image: " + pending.output_path);
        }
        return;
    }

    if (stbi_write_png(pending.output_path.c_str(),
                       pending.width,
                       pending.height,
                       3,
                       pending.image_host.data_ptr<uint8_t>(),
                       pending.width * 3) == 0) {
        throw std::runtime_error("failed to write eval sample image: " + pending.output_path);
    }
}

void enqueue_eval_sample_write(PendingEvalSampleWrite pending) {
    auto& state = eval_sample_writer_state();
    std::future<void> future = state.pool.enqueue([pending = std::move(pending)]() mutable {
        ensure_cuda_ok(cudaSetDevice(pending.device_id), "cudaSetDevice for eval sample write");
        ensure_cuda_ok(cudaEventSynchronize(pending.ready_event->get()),
                       "cudaEventSynchronize for eval sample write");
        write_eval_sample_image(pending);
    });
    std::lock_guard<std::mutex> lock(state.mutex);
    state.futures.push_back(std::move(future));
}

} // namespace

// External CUDA kernel launcher
void launch_draw_masks_boxes(
    uint8_t* image_out,
    int width, int height,
    const bool* masks,
    const float* boxes,
    const uint8_t* colors,
    const int* labels,
    int num_instances,
    float mask_alpha,
    int box_thickness,
    cudaStream_t stream
);

static void hsv2rgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
    float c = v * s;
    float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rf, gf, bf;
    if (h >= 0 && h < 60) { rf = c; gf = x; bf = 0; }
    else if (h >= 60 && h < 120) { rf = x; gf = c; bf = 0; }
    else if (h >= 120 && h < 180) { rf = 0; gf = c; bf = x; }
    else if (h >= 180 && h < 240) { rf = 0; gf = x; bf = c; }
    else if (h >= 240 && h < 300) { rf = x; gf = 0; bf = c; }
    else { rf = c; gf = 0; bf = x; }
    r = static_cast<uint8_t>((rf + m) * 255.0f);
    g = static_cast<uint8_t>((gf + m) * 255.0f);
    b = static_cast<uint8_t>((bf + m) * 255.0f);
}

void build_instance_colors_from_zero_based_labels(const int* labels,
                                                  std::size_t count,
                                                  int num_classes,
                                                  std::vector<std::uint8_t>* colors_rgb) {
    if (colors_rgb == nullptr) {
        throw std::runtime_error("build_instance_colors_from_zero_based_labels requires colors_rgb");
    }

    const int safe_class_count = std::max(1, num_classes);
    colors_rgb->assign(count * 3U, 0U);
    std::vector<int> class_counts(static_cast<std::size_t>(safe_class_count), 0);

    for (std::size_t index = 0; index < count; ++index) {
        int label = labels[index];
        if (label < 0 || label >= safe_class_count) {
            label = 0;
        }

        const int rank = class_counts[static_cast<std::size_t>(label)]++;
        const float hue_step = 360.0f / static_cast<float>(safe_class_count);
        const float base_h = static_cast<float>(label) * hue_step;

        float h = base_h;
        float s = 1.0f;
        const float v = 1.0f;
        if (rank > 0) {
            const int ring = (rank - 1) / 10 + 1;
            const int sv_idx = ((rank - 1) % 10) / 2;
            const int sign = ((rank - 1) % 2 == 0) ? 1 : -1;
            s = std::fmax(0.0f, 1.0f - (static_cast<float>(sv_idx) * 0.05f));
            h = std::fmod(base_h + (static_cast<float>(sign) * static_cast<float>(ring) * 3.6f) + 360.0f,
                          360.0f);
        }

        const std::size_t color_offset = index * 3U;
        hsv2rgb(h,
                s,
                v,
                (*colors_rgb)[color_offset],
                (*colors_rgb)[color_offset + 1U],
                (*colors_rgb)[color_offset + 2U]);
    }
}

void draw_eval_sample_async_gpu(
    const torch::Tensor& image_chw,
    const torch::Tensor& result_boxes,
    const torch::Tensor& result_labels,
    const torch::Tensor& result_masks,
    const RenderSampleOptions& options
) {
    if (image_chw.numel() == 0) {
        return;
    }

    const auto device = image_chw.device();
    c10::cuda::CUDAGuard guard(device.index());
    const auto draw_stream = get_high_priority_cuda_stream(device.index());

    const int height = static_cast<int>(image_chw.size(1));
    const int width = static_cast<int>(image_chw.size(2));
    const auto labels_t = result_labels.to(torch::kCPU, torch::kInt64).contiguous();
    const int num_instances = static_cast<int>(labels_t.size(0));

    const int64_t* label_data = labels_t.data_ptr<int64_t>();
    std::vector<int> labels_int(num_instances);
    for (int i = 0; i < num_instances; ++i) {
        labels_int[static_cast<std::size_t>(i)] = static_cast<int>(label_data[i]);
    }
    std::vector<uint8_t> colors;
    build_instance_colors_from_zero_based_labels(
        labels_int.data(),
        static_cast<std::size_t>(num_instances),
        options.num_classes,
        &colors);

    std::filesystem::create_directories(options.output_path.parent_path());

    torch::Tensor image_hwc;
    torch::Tensor image_u8;
    torch::Tensor boxes_t;
    torch::Tensor masks_t;
    torch::Tensor colors_gpu;
    torch::Tensor labels_gpu;
    torch::Tensor image_host;
    {
        c10::cuda::CUDAStreamGuard stream_guard(draw_stream);
        image_hwc = image_chw.permute({1, 2, 0}).contiguous();
        const auto image_max = image_hwc.max().item<double>();
        if (image_max <= 1.0) {
            image_u8 = image_hwc.mul(255.0f).clamp(0, 255).to(torch::kUInt8);
        } else {
            image_u8 = image_hwc.clamp(0, 255).to(torch::kUInt8);
        }
        boxes_t = result_boxes.to(device, torch::kFloat32).contiguous();
        if (result_masks.defined()) {
            masks_t = result_masks.to(device, torch::kBool).contiguous();
        } else {
            masks_t = torch::zeros({num_instances, height, width},
                                   torch::TensorOptions().dtype(torch::kBool).device(device));
        }
        colors_gpu = torch::tensor(colors, torch::TensorOptions().dtype(torch::kUInt8).device(device));
        labels_gpu = torch::tensor(labels_int, torch::TensorOptions().dtype(torch::kInt32).device(device));

        if (num_instances > 0) {
            launch_draw_masks_boxes(
                image_u8.data_ptr<uint8_t>(),
                width,
                height,
                masks_t.data_ptr<bool>(),
                boxes_t.data_ptr<float>(),
                colors_gpu.data_ptr<uint8_t>(),
                labels_gpu.data_ptr<int>(),
                num_instances,
                options.mask_alpha,
                static_cast<int>(options.box_thickness),
                draw_stream.stream());
        }

        image_host = torch::empty(
            {height, width, 3},
            torch::TensorOptions().dtype(torch::kUInt8).device(torch::kCPU).pinned_memory(true));
        image_host.copy_(image_u8, true);

        image_chw.record_stream(draw_stream);
        image_hwc.record_stream(draw_stream);
        image_u8.record_stream(draw_stream);
        boxes_t.record_stream(draw_stream);
        masks_t.record_stream(draw_stream);
        colors_gpu.record_stream(draw_stream);
        labels_gpu.record_stream(draw_stream);
    }

    cudaEvent_t ready_event = nullptr;
    ensure_cuda_ok(cudaEventCreateWithFlags(&ready_event, cudaEventDisableTiming),
                   "cudaEventCreateWithFlags for eval sample write");
    auto ready = std::make_shared<CudaEventHandle>(ready_event);
    ensure_cuda_ok(cudaEventRecord(ready->get(), draw_stream.stream()),
                   "cudaEventRecord for eval sample write");

    enqueue_eval_sample_write(PendingEvalSampleWrite{
        options.output_path.string(),
        width,
        height,
        device.index(),
        std::move(image_host),
        std::move(ready),
    });
}

void flush_eval_sample_writes() {
    auto& state = eval_sample_writer_state();
    state.pool.wait_idle();
    std::deque<std::future<void>> futures;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        futures.swap(state.futures);
    }
    for (auto& future : futures) {
        future.get();
    }
}

} // namespace mmltk::rfdetr
