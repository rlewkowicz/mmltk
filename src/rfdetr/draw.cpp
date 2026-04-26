#include "mmltk/rfdetr/draw_cuda.h"
#include "rfdetr/backends_internal.h"
#include "rfdetr/draw_color_utils.h"
#include "rfdetr/torch_cuda_utils.h"
#include "rfdetr/shared_cuda_event.h"
#include "string_utils.h"
#include "worker_pool.h"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>

#include <deque>
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

struct PendingEvalSampleWrite {
    std::string output_path;
    int width = 0;
    int height = 0;
    int device_id = 0;
    torch::Tensor image_host;
    std::shared_ptr<SharedCudaEvent> ready_event;
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
    const std::string extension = strings::to_lower(std::filesystem::path(pending.output_path).extension().string());
    if (extension == ".jpg" || extension == ".jpeg") {
        if (stbi_write_jpg(pending.output_path.c_str(), pending.width, pending.height, 3,
                           pending.image_host.data_ptr<uint8_t>(), 98) == 0) {
            throw std::runtime_error("failed to write eval sample image: " + pending.output_path);
        }
        return;
    }

    if (stbi_write_png(pending.output_path.c_str(), pending.width, pending.height, 3,
                       pending.image_host.data_ptr<uint8_t>(), pending.width * 3) == 0) {
        throw std::runtime_error("failed to write eval sample image: " + pending.output_path);
    }
}

void enqueue_eval_sample_write(PendingEvalSampleWrite pending) {
    auto& state = eval_sample_writer_state();
    std::future<void> future = state.pool.enqueue([pending = std::move(pending)]() mutable {
        ensure_cuda_ok(cudaSetDevice(pending.device_id), "cudaSetDevice for eval sample write");
        ensure_cuda_ok(cudaEventSynchronize(pending.ready_event->get()), "cudaEventSynchronize for eval sample write");
        write_eval_sample_image(pending);
    });
    std::lock_guard<std::mutex> lock(state.mutex);
    state.futures.push_back(std::move(future));
}

}  // namespace

void build_instance_colors_from_zero_based_labels(const int* labels, std::size_t count, int num_classes,
                                                  std::vector<std::uint8_t>* colors_rgb) {
    if (colors_rgb == nullptr) {
        throw std::runtime_error("build_instance_colors_from_zero_based_labels requires colors_rgb");
    }

    const int safe_class_count = draw_color::safe_class_count(num_classes);
    colors_rgb->assign(count * 3U, 0U);
    std::vector<int> class_counts(static_cast<std::size_t>(safe_class_count), 0);

    for (std::size_t index = 0; index < count; ++index) {
        const int label = draw_color::normalize_label(labels[index], safe_class_count);

        const int rank = class_counts[static_cast<std::size_t>(label)]++;
        const std::size_t color_offset = index * 3U;
        draw_color::instance_color_from_rank(label, rank, safe_class_count, (*colors_rgb)[color_offset],
                                             (*colors_rgb)[color_offset + 1U], (*colors_rgb)[color_offset + 2U]);
    }
}

void draw_eval_sample_async_gpu(const torch::Tensor& image_chw, const torch::Tensor& result_boxes,
                                const torch::Tensor& result_labels, const torch::Tensor& result_masks,
                                const RenderSampleOptions& options) {
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
    build_instance_colors_from_zero_based_labels(labels_int.data(), static_cast<std::size_t>(num_instances),
                                                 options.num_classes, &colors);

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
            masks_t =
                torch::zeros({num_instances, height, width}, torch::TensorOptions().dtype(torch::kBool).device(device));
        }
        colors_gpu = torch::tensor(colors, torch::TensorOptions().dtype(torch::kUInt8).device(device));
        labels_gpu = torch::tensor(labels_int, torch::TensorOptions().dtype(torch::kInt32).device(device));

        if (num_instances > 0) {
            launch_draw_masks_boxes(draw_launch::MaskBoxLabelRgbLaunch{
                draw_launch::make_packed_image(image_u8.data_ptr<uint8_t>(), width, height),
                draw_launch::MaskBoxLabelInputs{
                    masks_t.data_ptr<bool>(),
                    boxes_t.data_ptr<float>(),
                    colors_gpu.data_ptr<uint8_t>(),
                    labels_gpu.data_ptr<int>(),
                    num_instances,
                },
                options.mask_alpha,
                static_cast<int>(options.box_thickness),
                draw_stream.stream(),
            });
        }

        image_host = torch::empty({height, width, 3},
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

    enqueue_eval_sample_write(PendingEvalSampleWrite{
        options.output_path.string(),
        width,
        height,
        device.index(),
        std::move(image_host),
        record_shared_cuda_event(draw_stream.stream(), "eval sample write event"),
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

}  // namespace mmltk::rfdetr
