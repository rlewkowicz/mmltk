namespace mmltk::backend::models::rfdetr {

using CompactImageMatchRecord = EvaluationDatasetOwner::MatchRecord;
using ImageEvaluationMatches = EvaluationDatasetOwner::ImageMatches;

class CocoDataset {
   public:
    explicit inline CocoDataset(EvaluationMetricSet metric_set) : metric_set_(metric_set) {}
    CocoDataset(const CocoDataset& other);
    CocoDataset& operator=(const CocoDataset& other);
    CocoDataset(CocoDataset&&) noexcept = default;
    CocoDataset& operator=(CocoDataset&&) noexcept = default;

    static CocoDataset load_from_loader(const mmltk::backend::data::DatasetLoader& loader, EvaluationMetricSet metric_set);

    inline size_t num_images() const { return image_ids_.size(); }
    inline size_t num_categories() const { return category_names_.size(); }
    inline const std::vector<std::string>& category_names() const { return category_names_; }
    inline const std::vector<int>& image_ids() const { return image_ids_; }
    inline EvaluationMetricSet metric_set() const { return metric_set_; }
    inline size_t ground_truth_count() const { return ground_truth_boxes_.size(); }
    inline size_t prediction_count() const { return prediction_count_; }
    inline size_t mask_rle_pair_count() const { return ground_truth_mask_runs_ ? ground_truth_mask_runs_->size() : 0U; }

    void limit_images(size_t limit);
    bool has_image(int image_id) const;
    [[nodiscard]] ImageEvaluationMatches match_staged_predictions(std::int64_t dataset_index, const BBoxPredictionView& bbox,
                                                                  const std::optional<PackedMaskPredictionView>& mask,
                                                                  size_t max_dets_per_image) const;
    void merge_matches(ImageEvaluationMatches&& matches);
    void clear_predictions();
    EvalSummary evaluate(size_t max_dets_per_image, mmltk::common::concurrency::WorkerPool* worker_pool = nullptr) const;

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

        inline void reset(size_t category_count) {
            categories.resize(category_count);
            for (CategoryReductionScratch& category : categories)
                category.average_precision.fill(0.0);
        }
    };

    [[nodiscard]] inline size_t ground_truth_span_index(size_t image_index, size_t category_index) const {
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

}  // namespace mmltk::backend::models::rfdetr
