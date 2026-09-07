namespace mmltk::backend::models::rfdetr::evaluator_detail {
using CudaGuard = ::c10::cuda::CUDAGuard;
using CudaStreamGuard = ::c10::cuda::CUDAStreamGuard;
using DeviceIndex = ::c10::DeviceIndex;
using ::cudaError_t;
using ::cudaEvent_t;
using ::cudaEventCreate;
using ::cudaEventCreateWithFlags;
using ::cudaEventDestroy;
using ::cudaEventElapsedTime;
using ::cudaEventRecord;
using ::cudaEventSynchronize;
using ::cudaGetDevice;
using ::cudaSetDevice;
using ::cudaStream_t;
using ::cudaStreamSynchronize;
using ::cudaSuccess;
using ::c10::cuda::getStreamFromExternal;
using ::mmltk::backend::ml::cuda::checked_device_index;
inline constexpr unsigned int kCudaEventDisableTiming = cudaEventDisableTiming;
}  // namespace mmltk::backend::models::rfdetr::evaluator_detail

namespace mmltk::backend::models::rfdetr {

#if defined(MMLTK_TRAINING_EVALUATION_RUNTIME)
class EvaluationRunTechnicalOwner;

struct EvaluationRunConfig final {
    EvaluationMetricSet metric_set = EvaluationMetricSet::BBox;
    std::size_t batch_capacity = 1U;
    std::size_t prediction_capacity = 1U;
    std::size_t lane_count = 1U;
    std::size_t slots_per_lane = 1U;
    std::size_t encoding_capacity = 1U;
    std::uint32_t image_height = 0U;
    std::uint32_t image_width = 0U;
    int device_id = 0;
    bool profile = false;
};

struct EvaluationRunProgress final {
    std::size_t submitted_batches = 0U;
    std::size_t completed_images = 0U;
    std::size_t in_flight_batches = 0U;
    bool cancelled = false;
    bool terminal = false;
};

struct EvaluationRunTerminal final {
    EvalSummary summary{};
    std::size_t image_count = 0U;
    std::size_t category_count = 0U;
    bool cancelled = false;
};

class TrainingEvaluationRunOwner final {
   public:
    explicit TrainingEvaluationRunOwner(EvaluationRunConfig config);
    ~TrainingEvaluationRunOwner();
    TrainingEvaluationRunOwner(TrainingEvaluationRunOwner&&) noexcept;
    TrainingEvaluationRunOwner& operator=(TrainingEvaluationRunOwner&&) noexcept;
    TrainingEvaluationRunOwner(const TrainingEvaluationRunOwner&) = delete;
    TrainingEvaluationRunOwner& operator=(const TrainingEvaluationRunOwner&) = delete;

    void begin();
    void cancel() noexcept;
    [[nodiscard]] EvaluationRunProgress progress() const noexcept;
    [[nodiscard]] std::optional<EvaluationRunTerminal> terminal() const;
    [[nodiscard]] std::vector<int> image_ids() const;
    [[nodiscard]] EvaluationRunTechnicalOwner& operations() noexcept;
    [[nodiscard]] const EvaluationRunTechnicalOwner& operations() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct EvaluationProfileSetup {
    std::filesystem::path jsonl_path;
    std::string event_source;
    std::string model_source;
    std::string expected_precision;
    std::optional<int> validation_pass;
    EvaluationMetricSet metric_set = EvaluationMetricSet::BBox;
    size_t batch_size = 0;
    size_t image_height = 0;
    size_t image_width = 0;
    size_t query_count = 0;
    size_t persisted_max_instances_per_image = 0;
    size_t resolved_detection_cap = 0;
    size_t class_count = 0;
    bool query_count_automatic = false;
    bool detection_cap_automatic = false;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
};

struct EvaluationProfileRecord final : EvaluationProfileSetup {
    std::string precision;
    std::string box_precision;
    bool sdp_flash_enabled = false;
    bool sdp_mem_efficient_enabled = false;
    bool sdp_math_enabled = false;
    bool sdp_cudnn_enabled = false;
    size_t resolved_query_count = 0;
    size_t backend_query_count = 0;
    size_t transferred_bytes = 0;
    size_t mask_transferred_bytes = 0;
    size_t peak_in_flight_tasks = 0;
    size_t peak_in_flight_slots = 0;
    double loader_wait_seconds = 0.0;
    double preprocessing_seconds = 0.0;
    double model_forward_seconds = 0.0;
    double postprocess_seconds = 0.0;
    std::atomic<uint64_t> d2h_wait_nanoseconds{0};
    std::atomic<uint64_t> cpu_encode_nanoseconds{0};
    std::atomic<uint64_t> cpu_match_nanoseconds{0};
    std::atomic<uint64_t> final_sort_ap_nanoseconds{0};
    std::atomic<size_t> iou_candidate_count{0};
    std::atomic<size_t> mask_iou_candidate_count{0};
    std::atomic<size_t> mask_task_count{0};
};

void capture_sdp_backend_flags(EvaluationProfileRecord& profile);

// Accounts the device-to-host bytes moved for image_count postprocessed images: packed scores/labels/boxes plus
// bit-packed masks when present.
void accumulate_prediction_transfer_bytes(EvaluationProfileRecord& profile, const PostprocessedBatch& processed, size_t image_count);

const char* evaluation_precision_name(at::ScalarType scalar_type) noexcept;

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

    explicit inline operator bool() const noexcept { return timing != nullptr; }
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
#endif

using CompactImageMatchRecord = EvaluationDatasetOwner::MatchRecord;
using ImageEvaluationMatches = EvaluationDatasetOwner::ImageMatches;

#if defined(MMLTK_TRAINING_EVALUATION_RUNTIME)
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

    void ensure_capacity(int64_t batch_count, int64_t prediction_count, int64_t batch_capacity, int64_t prediction_capacity,
                         uint32_t height, uint32_t width, int device_id);
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

struct EvaluationPredictionLane {
    mmltk::backend::ml::cuda::TorchCudaStream stream;
    std::shared_ptr<PredictionBufferSlotPool> slot_pool;
};

using EvaluationLaneWork = std::move_only_function<StagedPredictionBatch(EvaluationPredictionLane&)>;
#endif

#if defined(MMLTK_TRAINING_EVALUATION_RUNTIME)
std::vector<Prediction> result_to_predictions(int image_id, const TensorMap& result, size_t category_count, size_t max_dets_per_image,
                                              EvaluationProfileRecord* profile = nullptr);
StagedPredictionBatch stage_prediction_batch(std::vector<PredictionBatchMetadata> images, PostprocessedBatch batch, size_t category_count,
                                             size_t max_dets_per_image, PredictionBufferLease lease, int device_id, void* stream_handle);
PendingPredictionBatchEncoding enqueue_prediction_batch_encoding(mmltk::common::concurrency::WorkerPool& cpu_pool,
                                                                 StagedPredictionBatch&& staged, EvaluationProfileRecord* profile = nullptr,
                                                                 const EvaluationDatasetOwner* evaluation_dataset = nullptr);
std::vector<PredictionBatchItem> collect_prediction_batch_encoding(PendingPredictionBatchEncoding&& pending);

AlignmentStats compare_top1(const TensorMap& lhs, const TensorMap& rhs, size_t category_count);

class EvaluationRunTechnicalOwner {
   public:
    virtual ~EvaluationRunTechnicalOwner() = default;
    virtual void load_dataset(mmltk::backend::data::DatasetLoader& loader) = 0;
    [[nodiscard]] virtual EvaluationDatasetOwner& dataset() noexcept = 0;
    [[nodiscard]] virtual const EvaluationDatasetOwner& dataset() const noexcept = 0;
    [[nodiscard]] virtual std::size_t pending_lane_count() const noexcept = 0;
    [[nodiscard]] virtual std::size_t pending_encoding_count() const noexcept = 0;
    [[nodiscard]] virtual bool profiling() const noexcept = 0;
    virtual void configure_profile(EvaluationProfileSetup setup) = 0;
    virtual void record_loader_wait(double seconds) noexcept = 0;
    virtual void record_timing_start(EvaluationCudaTimingLease lease, EvaluationCudaBatchTiming::Phase phase, void* stream) = 0;
    virtual void record_timing_stop(EvaluationCudaTimingLease lease, EvaluationCudaBatchTiming::Phase phase, void* stream) = 0;
    virtual void record_model_output(std::string precision, std::string box_precision, std::size_t query_count,
                                     std::size_t class_count) = 0;
    virtual void record_prediction_transfer(const PostprocessedBatch& processed, std::size_t image_count) = 0;
    [[nodiscard]] virtual EvaluationCudaTimingLease acquire_timing() = 0;
    virtual void submit(mmltk::common::concurrency::WorkerPool& lane_pool, EvaluationLaneWork work, EvaluationCudaTimingLease timing) = 0;
    virtual void drain_lane(mmltk::common::concurrency::WorkerPool& cpu_pool) = 0;
    [[nodiscard]] virtual std::size_t drain_encoding() = 0;
    [[nodiscard]] virtual EvalSummary evaluate(std::size_t max_dets_per_image, mmltk::common::concurrency::WorkerPool& cpu_pool) = 0;
    virtual void settle(EvalSummary summary) = 0;
};
#endif

}  // namespace mmltk::backend::models::rfdetr
