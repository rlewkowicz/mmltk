module;
#include <ATen/Context.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>
#include <torch/torch.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "detail/detection_types.h"
#include "detail/postprocess.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "torch_api.h"
#include "torch_cuda_utils.h"

module mmltk.backend.models.rfdetr.core.evaluator;

import mmltk.backend.ml.cuda.gpu_quiescence;
import mmltk.backend.models.rfdetr.core.dataset_utils;

#define MMLTK_CORE_EVALUATION_DATASET
#include "detail/evaluator_dataset_private.h"
#undef MMLTK_CORE_EVALUATION_DATASET

namespace mmltk::backend::models::rfdetr {

struct EvaluationDatasetOwner::Impl final {
    Impl(mmltk::backend::data::DatasetLoader& loader, const EvaluationMetricSet metric_set)
        : dataset(CocoDataset::load_from_loader(loader, metric_set)), catalog(loader.class_catalog()) {}

    Impl(const Impl&) = default;

    CocoDataset dataset;
    std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> catalog;
};

EvaluationDatasetOwner::EvaluationDatasetOwner(mmltk::backend::data::DatasetLoader& loader, const EvaluationMetricSet metric_set)
    : impl_(std::make_unique<Impl>(loader, metric_set)) {}
EvaluationDatasetOwner::~EvaluationDatasetOwner() = default;
EvaluationDatasetOwner::EvaluationDatasetOwner(const EvaluationDatasetOwner& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
EvaluationDatasetOwner& EvaluationDatasetOwner::operator=(const EvaluationDatasetOwner& other) {
    if (this != &other) impl_ = std::make_unique<Impl>(*other.impl_);
    return *this;
}
EvaluationDatasetOwner::EvaluationDatasetOwner(EvaluationDatasetOwner&&) noexcept = default;
EvaluationDatasetOwner& EvaluationDatasetOwner::operator=(EvaluationDatasetOwner&&) noexcept = default;

void EvaluationDatasetOwner::clear_predictions() { impl_->dataset.clear_predictions(); }
void EvaluationDatasetOwner::limit_images(const std::size_t limit) { impl_->dataset.limit_images(limit); }
void EvaluationDatasetOwner::merge_bbox_predictions(const std::int64_t dataset_index, const BBoxPredictionView predictions,
                                                    const std::size_t max_dets_per_image) {
    impl_->dataset.merge_matches(impl_->dataset.match_staged_predictions(dataset_index, predictions, std::nullopt, max_dets_per_image));
}
EvaluationDatasetOwner::ImageMatches EvaluationDatasetOwner::match_predictions(const std::int64_t dataset_index,
                                                                               const BBoxPredictionView predictions,
                                                                               const std::optional<PackedMaskPredictionView> masks,
                                                                               const std::size_t max_dets_per_image, const std::span<const Prediction> encoded_masks) const {
    return impl_->dataset.match_staged_predictions(dataset_index, predictions, masks, max_dets_per_image, encoded_masks);
}
void EvaluationDatasetOwner::merge_matches(ImageMatches matches) { impl_->dataset.merge_matches(std::move(matches)); }
EvalSummary EvaluationDatasetOwner::evaluate(const std::size_t max_dets_per_image) const {
    return impl_->dataset.evaluate(max_dets_per_image);
}
std::vector<EvaluationMetricDetail> EvaluationDatasetOwner::take_details() { return impl_->dataset.take_details(); }
EvalSummary EvaluationDatasetOwner::evaluate(const std::size_t max_dets_per_image,
                                             mmltk::common::concurrency::WorkerPool& worker_pool) const {
    return impl_->dataset.evaluate(max_dets_per_image, &worker_pool);
}
std::vector<int> EvaluationDatasetOwner::image_ids() const { return impl_->dataset.image_ids(); }
std::size_t EvaluationDatasetOwner::image_count() const noexcept { return impl_->dataset.num_images(); }
const std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog>& EvaluationDatasetOwner::class_catalog() const noexcept { return impl_->catalog; }
std::size_t EvaluationDatasetOwner::category_count() const noexcept { return impl_->dataset.num_categories(); }
EvaluationDatasetOwner::Facts EvaluationDatasetOwner::facts() const noexcept {
    return Facts{
        impl_->dataset.metric_set(),
        impl_->dataset.prediction_count(),
        impl_->dataset.ground_truth_count(),
        impl_->dataset.mask_rle_pair_count(),
    };
}
}  // namespace mmltk::backend::models::rfdetr
