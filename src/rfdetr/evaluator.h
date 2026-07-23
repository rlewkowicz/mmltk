#pragma once

#include "mmltk/rfdetr/evaluation.h"

#include "rfdetr/postprocess.h"
#include <ATen/Context.h>
#include <cuda_runtime.h>
#include <torch/torch.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mmltk {
class DatasetLoader;
class WorkerPool;
}  

namespace mmltk::rfdetr {

struct EvaluationProfileRecord {
    std::filesystem::path jsonl_path;
    std::string event_source;
    std::string model_source;
    std::string precision;
    std::string box_precision;
    std::string expected_precision;
    bool sdp_flash_enabled = false;
    bool sdp_mem_efficient_enabled = false;
    bool sdp_math_enabled = false;
    bool sdp_cudnn_enabled = false;
    std::optional<int> validation_pass;
    EvaluationMetricSet metric_set = EvaluationMetricSet::BBox;
    size_t batch_size = 0;
    size_t image_height = 0;
    size_t image_width = 0;
    size_t query_count = 0;
    size_t class_count = 0;
    size_t transferred_bytes = 0;
    size_t mask_transferred_bytes = 0;
    size_t peak_in_flight_tasks = 0;
    size_t peak_in_flight_slots = 0;
    double loader_wait_seconds = 0.0;
    double preprocessing_seconds = 0.0;
    double model_forward_seconds = 0.0;
    double postprocess_seconds = 0.0;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
    std::atomic<uint64_t> d2h_wait_nanoseconds{0};
    std::atomic<uint64_t> cpu_encode_nanoseconds{0};
    std::atomic<uint64_t> cpu_match_nanoseconds{0};
    std::atomic<uint64_t> final_sort_ap_nanoseconds{0};
    std::atomic<size_t> iou_candidate_count{0};
    std::atomic<size_t> mask_iou_candidate_count{0};
    std::atomic<size_t> mask_task_count{0};
};

inline void capture_sdp_backend_flags(EvaluationProfileRecord& profile) {
    const auto& context = at::globalContext();
    profile.sdp_flash_enabled = context.userEnabledFlashSDP();
    profile.sdp_mem_efficient_enabled = context.userEnabledMemEfficientSDP();
    profile.sdp_math_enabled = context.userEnabledMathSDP();
    profile.sdp_cudnn_enabled = context.userEnabledCuDNNSDP();
}

inline const char* evaluation_precision_name(const at::ScalarType scalar_type) noexcept {
    switch (scalar_type) {
        case at::kFloat:
            return "fp32";
        case at::kHalf:
            return "fp16";
        case at::kBFloat16:
            return "bf16";
        default:
            return "other";
    }
}

class EvaluationCudaBatchTiming final {
   public:
    enum class Phase : std::uint8_t {
        Preprocessing = 0,
        ModelForward = 1,
        Postprocess = 2,
    };

    EvaluationCudaBatchTiming();
    ~EvaluationCudaBatchTiming();

    EvaluationCudaBatchTiming(const EvaluationCudaBatchTiming&) = delete;
    EvaluationCudaBatchTiming& operator=(const EvaluationCudaBatchTiming&) = delete;

    void record_start(Phase phase, cudaStream_t stream);
    void record_stop(Phase phase, cudaStream_t stream);
    void accumulate(EvaluationProfileRecord& profile) const;

   private:
    static constexpr size_t event_index(const Phase phase, const bool stop) {
        return static_cast<size_t>(phase) * 2U + static_cast<size_t>(stop);
    }

    double elapsed_seconds(Phase phase) const;
    void destroy_events() noexcept;

    std::array<cudaEvent_t, 6> events_{};
};

struct EvaluationCudaTimingLease {
    EvaluationCudaBatchTiming* timing = nullptr;
    size_t slot_index = 0;

    explicit operator bool() const noexcept {
        return timing != nullptr;
    }
};

class EvaluationCudaTimingPool final {
   public:
    explicit EvaluationCudaTimingPool(size_t slot_count);

    EvaluationCudaTimingLease acquire();
    void release(EvaluationCudaTimingLease& lease);

   private:
    std::vector<std::unique_ptr<EvaluationCudaBatchTiming>> slots_;
    std::vector<size_t> free_slots_;
};

struct CompactImageMatchRecord {
    float score = 0.0F;
    std::uint32_t image_ordinal = 0;
    std::uint32_t prediction_ordinal = 0;
    std::uint16_t category_index = 0;
    std::uint16_t matched_threshold_bits = 0;
};

struct ImageEvaluationMatches {
    std::vector<CompactImageMatchRecord> bbox;
    std::optional<std::vector<CompactImageMatchRecord>> mask;
    size_t prediction_count = 0;
    size_t max_dets_per_image = 0;
};

struct PinnedBBoxPredictionBuffers {
    torch::Tensor scores_cpu;
    torch::Tensor labels_cpu;
    torch::Tensor boxes_cpu;
    int64_t batch_capacity = 0;
    int64_t prediction_capacity = 0;

    void ensure_capacity(int64_t batch_count, int64_t prediction_count);
};

struct PinnedMaskPredictionBuffers {
    torch::Tensor masks_cpu;
    torch::Tensor masks_gpu;
    uint32_t mask_height = 0;
    uint32_t mask_width = 0;
    int64_t packed_mask_bytes = 0;

    void ensure_capacity(int64_t batch_count, int64_t prediction_count, int64_t batch_capacity,
                         int64_t prediction_capacity, uint32_t height, uint32_t width, int device_id);
};

enum class PredictionSlotState : std::uint8_t {
    Free,
    GpuFilling,
    D2HPending,
    CpuMatching,
};

struct PinnedPredictionBuffers {
    PinnedBBoxPredictionBuffers bbox;
    std::optional<PinnedMaskPredictionBuffers> mask;
    cudaEvent_t ready_event = nullptr;
    int event_device_id = -1;
    std::atomic<PredictionSlotState> state{PredictionSlotState::Free};
    std::mutex consume_mutex;
    bool allow_mask_reconfiguration = false;

    ~PinnedPredictionBuffers();

    void ensure_ready_event(int device_id);
    void transition(PredictionSlotState expected, PredictionSlotState next, const char* operation);
};

struct PredictionBufferLease {
    std::shared_ptr<PinnedPredictionBuffers> buffers;
    std::shared_ptr<void> release_guard;
};

struct PredictionBufferConfig {
    int64_t batch_capacity = 0;
    int64_t prediction_capacity = 0;
    std::optional<std::pair<uint32_t, uint32_t>> mask_shape;
    int device_id = -1;
    bool allow_mask_reconfiguration = false;
};

class PredictionBufferSlotPool final : public std::enable_shared_from_this<PredictionBufferSlotPool> {
   public:
    PredictionBufferSlotPool(size_t slot_count, const PredictionBufferConfig& config);

    PredictionBufferLease acquire();

   private:
    void release(size_t slot_index);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<PinnedPredictionBuffers>> slots_;
    std::deque<size_t> free_slots_;
};

struct PredictionBatchMetadata {
    int64_t dataset_index = 0;
    int64_t image_id = 0;
    std::string source_name;
};

struct PredictionBatchItem : PredictionBatchMetadata {
    std::vector<Prediction> predictions;
    std::optional<ImageEvaluationMatches> evaluation_matches;
};

struct StagedBBoxPredictionBatch {
    torch::Tensor scores_cpu;
    torch::Tensor labels_cpu;
    torch::Tensor boxes_cpu;
};

struct StagedMaskPredictionBatch {
    torch::Tensor masks_cpu;
    uint32_t height = 0;
    uint32_t width = 0;
};

struct StagedPredictionBatch {
    std::vector<PredictionBatchMetadata> images;
    StagedBBoxPredictionBatch bbox;
    std::optional<StagedMaskPredictionBatch> mask;
    PostprocessedBatch pending_gpu;
    size_t category_count = 0;
    size_t max_dets_per_image = 0;
    size_t active_image_count = 0;
    PredictionBufferLease lease;
};

struct PendingPredictionBatchEncoding {
    std::vector<std::future<PredictionBatchItem>> images;
};

class CocoDataset {
   public:
    explicit CocoDataset(EvaluationMetricSet metric_set) : metric_set_(metric_set) {}
    CocoDataset(const CocoDataset& other);
    CocoDataset& operator=(const CocoDataset& other);
    CocoDataset(CocoDataset&&) noexcept = default;
    CocoDataset& operator=(CocoDataset&&) noexcept = default;

    static CocoDataset load_from_binary(const std::filesystem::path& path, EvaluationMetricSet metric_set);
    static CocoDataset load_from_loader(const mmltk::DatasetLoader& loader, EvaluationMetricSet metric_set);

    size_t num_images() const {
        return image_ids_.size();
    }
    size_t num_categories() const {
        return category_names_.size();
    }
    const std::vector<std::string>& category_names() const {
        return category_names_;
    }
    const std::vector<int>& image_ids() const {
        return image_ids_;
    }
    EvaluationMetricSet metric_set() const {
        return metric_set_;
    }
    size_t ground_truth_count() const {
        return ground_truth_boxes_.size();
    }
    size_t prediction_count() const {
        return prediction_count_;
    }

    void limit_images(size_t limit);
    bool has_image(int image_id) const;

    [[nodiscard]] ImageEvaluationMatches match_staged_predictions(std::int64_t dataset_index,
                                                                  const BBoxPredictionView& bbox,
                                                                  const std::optional<PackedMaskPredictionView>& mask,
                                                                  size_t max_dets_per_image,
                                                                  EvaluationProfileRecord* profile = nullptr) const;
    void merge_matches(ImageEvaluationMatches&& matches);
    void clear_predictions();
    EvalSummary evaluate(size_t max_dets_per_image, EvaluationProfileRecord* profile = nullptr,
                         mmltk::WorkerPool* worker_pool = nullptr) const;

   private:
    struct GroundTruthSpan {
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
    };

    struct GroundTruthMask {
        std::uint32_t run_offset = 0;
        std::uint32_t run_count = 0;
        std::uint32_t area = 0;
    };

    struct CategoryReductionScratch {
        std::array<double, 10> average_precision{};
        std::array<size_t, 101> recall_indices{};
        std::vector<double> precisions;
    };

    struct MetricScratch {
        std::vector<CategoryReductionScratch> categories;

        void reset(size_t category_count) {
            categories.resize(category_count);
            for (CategoryReductionScratch& category : categories) {
                category.average_precision.fill(0.0);
            }
        }
    };

    [[nodiscard]] size_t ground_truth_span_index(size_t image_index, size_t category_index) const {
        return image_index * category_names_.size() + category_index;
    }
    void rebuild_ground_truth_totals();
    void reset_match_storage();

    EvaluationMetricSet metric_set_;
    std::vector<int> image_ids_;
    std::vector<std::string> category_names_;
    std::vector<GroundTruthSpan> ground_truth_spans_;
    std::vector<std::array<float, 4>> ground_truth_boxes_;
    std::vector<std::uint16_t> ground_truth_categories_;
    std::vector<std::uint32_t> ground_truth_ordinals_;
    std::optional<std::vector<GroundTruthMask>> ground_truth_masks_;
    std::optional<std::vector<std::pair<std::uint32_t, std::uint32_t>>> ground_truth_mask_runs_;
    std::uint32_t ground_truth_mask_height_ = 0;
    std::uint32_t ground_truth_mask_width_ = 0;
    std::vector<size_t> ground_truth_totals_;
    std::vector<size_t> ground_truth_nonempty_categories_;
    std::unordered_map<int, size_t> image_id_to_index_;
    mutable std::vector<std::vector<CompactImageMatchRecord>> bbox_matches_by_category_;
    mutable std::optional<std::vector<std::vector<CompactImageMatchRecord>>> mask_matches_by_category_;
    size_t prediction_count_ = 0;
    std::optional<size_t> matched_max_dets_per_image_;
    mutable MetricScratch bbox_metric_scratch_;
    mutable std::optional<MetricScratch> mask_metric_scratch_;
};

std::vector<Prediction> result_to_predictions(int image_id, const TensorMap& result, size_t category_count,
                                              size_t max_dets_per_image, EvaluationProfileRecord* profile = nullptr);
StagedPredictionBatch stage_prediction_batch(std::vector<PredictionBatchMetadata> images, PostprocessedBatch batch,
                                             size_t category_count, size_t max_dets_per_image,
                                             PredictionBufferLease lease, int device_id, void* stream_handle);
PendingPredictionBatchEncoding enqueue_prediction_batch_encoding(mmltk::WorkerPool& cpu_pool,
                                                                 StagedPredictionBatch&& staged,
                                                                 EvaluationProfileRecord* profile = nullptr,
                                                                 const CocoDataset* evaluation_dataset = nullptr);
std::vector<PredictionBatchItem> collect_prediction_batch_encoding(PendingPredictionBatchEncoding&& pending);

AlignmentStats compare_top1(const TensorMap& lhs, const TensorMap& rhs, size_t category_count);

}  
