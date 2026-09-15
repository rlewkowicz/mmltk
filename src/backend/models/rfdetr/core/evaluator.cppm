module;
#include <array>
#include <span>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"

export module mmltk.backend.models.rfdetr.core.evaluator;

export namespace mmltk::backend::models::rfdetr {

class EvaluationDatasetOwner final {
   public:
    EvaluationDatasetOwner(mmltk::backend::data::DatasetLoader& loader, EvaluationMetricSet metric_set);
    ~EvaluationDatasetOwner();
    EvaluationDatasetOwner(const EvaluationDatasetOwner&);
    EvaluationDatasetOwner& operator=(const EvaluationDatasetOwner&);
    EvaluationDatasetOwner(EvaluationDatasetOwner&&) noexcept;
    EvaluationDatasetOwner& operator=(EvaluationDatasetOwner&&) noexcept;

    void clear_predictions();
    void limit_images(std::size_t limit);
    void merge_bbox_predictions(std::int64_t dataset_index, BBoxPredictionView predictions, std::size_t max_dets_per_image);
    struct MatchRecord final {
        float score = 0.0F;
        std::uint32_t image_ordinal = 0U;
        std::uint32_t prediction_ordinal = 0U;
        std::uint16_t category_index = 0U;
        std::uint32_t category_rank = 0U;
        std::array<std::uint16_t, 4> area_matched_bits{};
        std::array<std::uint16_t, 4> area_ignored_bits{};
    };
    struct ImageMatches final {
        std::vector<MatchRecord> bbox;
        std::optional<std::vector<MatchRecord>> mask;
        std::size_t prediction_count = 0U;
        std::size_t max_dets_per_image = 0U;
        std::size_t bbox_iou_candidate_count = 0U;
        std::size_t mask_iou_candidate_count = 0U;
    };
    struct Facts final {
        EvaluationMetricSet metric_set = EvaluationMetricSet::BBox;
        std::size_t prediction_count = 0U;
        std::size_t ground_truth_count = 0U;
        std::size_t mask_rle_pair_count = 0U;
    };
    [[nodiscard]] ImageMatches match_predictions(std::int64_t dataset_index, BBoxPredictionView predictions,
                                                 std::optional<PackedMaskPredictionView> masks, std::size_t max_dets_per_image,
                                                 std::span<const Prediction> encoded_masks = {}) const;
    void merge_matches(ImageMatches matches);
    [[nodiscard]] EvalSummary evaluate(std::size_t max_dets_per_image) const;
    [[nodiscard]] EvalSummary evaluate(std::size_t max_dets_per_image, mmltk::common::concurrency::WorkerPool& worker_pool) const;
    [[nodiscard]] std::vector<EvaluationMetricDetail> take_details();
    [[nodiscard]] std::vector<int> image_ids() const;
    [[nodiscard]] std::size_t image_count() const noexcept;
    [[nodiscard]] std::size_t category_count() const noexcept;
    [[nodiscard]] const std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog>& class_catalog() const noexcept;
    [[nodiscard]] Facts facts() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::backend::models::rfdetr
