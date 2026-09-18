#include "src/backend/ml/cuda/numa_host_tensor.h"
#include "src/backend/models/rfdetr/core/sample_output.h"
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <stb_image_write.h>
#include <torch/torch.h>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/types/string_utils.h"
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/cuda_priority.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "src/backend/ml/cuda/shared_cuda_event.h"
import mmltk.backend.imaging.raster;
import mmltk.backend.ml.cuda.gpu_quiescence;
namespace mmltk::backend::models::rfdetr {
using mmltk::backend::ml::cuda::get_priority_cuda_stream;
using mmltk::frameworks::gpu::ensure_cuda_ok;
namespace {
struct PendingEvalSampleWrite {
    std::string output_path;
    int width = 0;
    int height = 0;
    torch::Tensor image_host;
    mmltk::backend::ml::cuda::CudaEventPool::Lease ready_event;
};
}  // namespace
struct EvaluationSampleWriter::Impl final {
   public:
    explicit Impl(const int device)
        : device_id(device),
          retirement_owner(1U),
          event_pool(mmltk::frameworks::gpu::make_cuda_device_owner<Impl, &Impl::record_failure>(this, device), 1U, retirement_owner) {
        mmltk::frameworks::gpu::CudaDeviceScope scope(device);
        ensure_cuda_ok(scope ? scope.FinalizeStatus(cudaStreamCreateWithFlags(&settlement_stream, cudaStreamNonBlocking)) : scope.Finalize(),
                       "create eval sample settlement stream");
    }
    ~Impl() noexcept {
        pool.wait_idle();
        if (settlement_stream != nullptr) {
            mmltk::frameworks::gpu::CudaDeviceScope scope(device_id);
            const cudaError_t status = scope ? cudaStreamDestroy(settlement_stream) : scope.status();
            static_cast<void>(scope.FinalizeStatus(status));
        }
    }
    void Enqueue(PendingEvalSampleWrite pending);
    void record_failure(const cudaError_t status) noexcept { mmltk::frameworks::gpu::record_first_cuda_failure(first_failure, status); }
    int device_id = -1;
    mmltk::common::concurrency::WorkerPool pool{1, {}, "rfdwrite"};
    std::future<void> future;
    std::atomic<cudaError_t> first_failure{cudaSuccess};
    mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_owner;
    mmltk::backend::ml::cuda::CudaEventPool event_pool;
    cudaStream_t settlement_stream = nullptr;
};
namespace {
void write_eval_sample_image(const PendingEvalSampleWrite& pending) {
    const std::string extension = mmltk::common::types::to_lower(std::filesystem::path(pending.output_path).extension().string());
    if (extension == ".jpg" || extension == ".jpeg") {
        if (stbi_write_jpg(pending.output_path.c_str(), pending.width, pending.height, 3, pending.image_host.data_ptr<uint8_t>(), 98) == 0) {
            throw std::runtime_error("failed to write eval sample image: " + pending.output_path);
        }
        return;
    }
    if (stbi_write_png(pending.output_path.c_str(), pending.width, pending.height, 3, pending.image_host.data_ptr<uint8_t>(), pending.width * 3) == 0) {
        throw std::runtime_error("failed to write eval sample image: " + pending.output_path);
    }
}
}  // namespace
void EvaluationSampleWriter::Impl::Enqueue(PendingEvalSampleWrite pending) {
    future = pool.enqueue([this, pending = std::move(pending)]() mutable {
        mmltk::frameworks::gpu::CudaDeviceScope scope(device_id);
        cudaError_t status = scope.status();
        if (scope) {
            try {
                pending.ready_event.wait(reinterpret_cast<std::uintptr_t>(settlement_stream), "wait for eval sample draw completion");
                status = cudaStreamSynchronize(settlement_stream);
            } catch (...) {
                static_cast<void>(scope.Finalize());
                throw;
            }
        }
        ensure_cuda_ok(scope.FinalizeStatus(status), "settle eval sample draw completion");
        pending.ready_event.retire();
        write_eval_sample_image(pending);
    });
}
EvaluationSampleWriter::EvaluationSampleWriter() = default;
EvaluationSampleWriter::~EvaluationSampleWriter() = default;
void build_instance_colors_async(const std::int32_t* labels, const std::size_t count, const int num_classes, std::uint8_t* colors_rgb,
                                 const cudaStream_t stream) {
    if (count == 0U) { return; }
    if (labels == nullptr || colors_rgb == nullptr || stream == nullptr) {
        throw std::invalid_argument("RF-DETR instance color generation requires device buffers and a stream");
    }
    ensure_cuda_ok(static_cast<cudaError_t>(mmltk::backend::imaging::raster::build_category_colors_cuda({labels, count, num_classes, colors_rgb, stream})),
                   "RF-DETR instance color generation");
}
void EvaluationSampleWriter::Draw(const at::Tensor& image_chw, const at::Tensor& result_boxes, const at::Tensor& result_labels, const at::Tensor& result_masks,
                                  const RenderSampleOptions& options) {
    if (image_chw.numel() == 0) { return; }
    const auto device = image_chw.device();
    if (!device.is_cuda()) throw std::invalid_argument("eval sample writer requires a CUDA image");
    if (!impl_) impl_ = std::make_unique<Impl>(device.index());
    if (impl_->device_id != device.index()) throw std::invalid_argument("eval sample writer cannot change its owning CUDA device");
    Flush();
    c10::cuda::CUDAGuard guard(device.index());
    const auto draw_stream = get_priority_cuda_stream(device.index(), mmltk::frameworks::gpu::current_cuda_highest_stream_priority());
    const int height = static_cast<int>(image_chw.size(1));
    const int width = static_cast<int>(image_chw.size(2));
    const auto labels_t = result_labels.to(torch::kCPU, torch::kInt64).contiguous();
    const int num_instances = static_cast<int>(labels_t.size(0));
    const int64_t* label_data = labels_t.data_ptr<int64_t>();
    std::vector<int> labels_int(num_instances);
    for (int i = 0; i < num_instances; ++i) { labels_int[static_cast<std::size_t>(i)] = static_cast<int>(label_data[i]); }
    std::vector<uint8_t> colors = mmltk::backend::imaging::raster::category_colors(labels_int, options.num_classes);
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
            masks_t = torch::zeros({num_instances, height, width}, torch::TensorOptions().dtype(torch::kBool).device(device));
        }
        colors_gpu = torch::tensor(colors, torch::TensorOptions().dtype(torch::kUInt8).device(device));
        labels_gpu = torch::tensor(labels_int, torch::TensorOptions().dtype(torch::kInt32).device(device));
        if (num_instances > 0) {
            ensure_cuda_ok(static_cast<cudaError_t>(mmltk::backend::imaging::raster::raster_mask_boxes_rgb({
                               {image_u8.data_ptr<uint8_t>(), width, height},
                               {
                                   masks_t.data_ptr<bool>(),
                                   boxes_t.data_ptr<float>(),
                                   colors_gpu.data_ptr<uint8_t>(),
                                   labels_gpu.data_ptr<int>(),
                                   num_instances,
                               },
                               options.mask_alpha,
                               static_cast<int>(options.box_thickness),
                               draw_stream.stream(),
                           })),
                           "RF-DETR eval sample raster");
        }
        image_host = mmltk::backend::ml::cuda::numa_empty({height, width, 3}, torch::kUInt8, device.index());
        image_host.copy_(image_u8, true);
        image_chw.record_stream(draw_stream);
        image_hwc.record_stream(draw_stream);
        image_u8.record_stream(draw_stream);
        boxes_t.record_stream(draw_stream);
        masks_t.record_stream(draw_stream);
        colors_gpu.record_stream(draw_stream);
        labels_gpu.record_stream(draw_stream);
    }
    auto ready_event = impl_->event_pool.record(reinterpret_cast<std::uintptr_t>(draw_stream.stream()), "record eval sample write event");
    if (!ready_event) throw std::runtime_error("eval sample writer CUDA event capacity is exhausted");
    impl_->Enqueue(PendingEvalSampleWrite{
        options.output_path.string(),
        width,
        height,
        std::move(image_host),
        std::move(*ready_event),
    });
}
void EvaluationSampleWriter::Flush() {
    if (impl_ && impl_->future.valid()) impl_->future.get();
}
}  // namespace mmltk::backend::models::rfdetr
