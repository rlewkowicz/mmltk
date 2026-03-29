#pragma once

#include "fastloader/rfdetr/evaluation.h"

#include "rfdetr/postprocess.h"

#include <cuda_runtime.h>
#include <torch/torch.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastloader::rfdetr {

struct GroundTruthAnnotation {
    int image_id = 0;
    int category_id = 0;
    std::array<float, 4> bbox_xyxy{};
    EncodedMask mask;
    bool has_mask = false;
};

struct PinnedPredictionBuffers {
    torch::Tensor scores_cpu;
    torch::Tensor labels_cpu;
    torch::Tensor boxes_cpu;
    torch::Tensor masks_cpu;
    int64_t prediction_capacity = 0;
    uint32_t mask_height = 0;
    uint32_t mask_width = 0;

    void ensure_prediction_capacity(int64_t count);
    void ensure_mask_capacity(int64_t count, uint32_t height, uint32_t width);
};

struct PredictionBufferLease {
    std::shared_ptr<PinnedPredictionBuffers> buffers;
    std::shared_ptr<void> release_guard;
};

class PredictionBufferSlotPool final : public std::enable_shared_from_this<PredictionBufferSlotPool> {
public:
    explicit PredictionBufferSlotPool(size_t slot_count);

    PredictionBufferLease acquire();

private:
    void release(size_t slot_index);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<PinnedPredictionBuffers>> slots_;
    std::deque<size_t> free_slots_;
};

class CudaEventHandle {
public:
    explicit CudaEventHandle(cudaEvent_t event) : event_(event) {}

    ~CudaEventHandle() {
        if (event_ != nullptr) {
            cudaEventDestroy(event_);
        }
    }

    CudaEventHandle(const CudaEventHandle&) = delete;
    CudaEventHandle& operator=(const CudaEventHandle&) = delete;

    cudaEvent_t get() const { return event_; }

private:
    cudaEvent_t event_ = nullptr;
};

struct StagedPredictionBatch {
    std::vector<Prediction> predictions;
    torch::Tensor linear_masks_cpu;
    torch::Tensor pending_masks_gpu;
    uint32_t mask_height = 0;
    uint32_t mask_width = 0;
    std::unique_ptr<CudaEventHandle> ready_event;
    PredictionBufferLease lease;
};

class CocoDataset {
public:
    static CocoDataset load_from_binary(const std::filesystem::path& path);

    size_t num_images() const { return image_ids_.size(); }
    size_t num_categories() const { return category_names_.size(); }
    const std::vector<std::string>& category_names() const { return category_names_; }
    const std::vector<int>& image_ids() const { return image_ids_; }

    void limit_images(size_t limit);
    bool has_image(int image_id) const;

    void add_predictions(const std::vector<Prediction>& predictions);
    void add_predictions(std::vector<Prediction>&& predictions);
    EvalSummary evaluate(size_t max_dets_per_image) const;

private:
    std::vector<int> image_ids_;
    std::vector<std::string> category_names_;
    std::vector<GroundTruthAnnotation> ground_truths_;
    std::unordered_map<int, size_t> image_id_to_index_;
    std::vector<Prediction> predictions_;
};

std::vector<Prediction> result_to_predictions(int image_id,
                                              const TensorMap& result,
                                              size_t category_count,
                                              size_t max_dets_per_image);
StagedPredictionBatch stage_result_to_predictions(int image_id,
                                                  const TensorMap& result,
                                                  size_t category_count,
                                                  size_t max_dets_per_image,
                                                  PredictionBufferLease lease,
                                                  int device_id,
                                                  void* stream_handle);
std::vector<Prediction> encode_staged_predictions(StagedPredictionBatch&& staged);

AlignmentStats compare_top1(const TensorMap& lhs, const TensorMap& rhs, size_t category_count);

} // namespace fastloader::rfdetr
