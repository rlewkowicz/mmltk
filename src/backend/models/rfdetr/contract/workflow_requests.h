#pragma once
#include "src/backend/imaging/resample/image_resize.h"
#include "src/backend/models/rfdetr/contract/prediction_limits.h"
#include "src/backend/data/data_loading_options.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include <cstddef>
#include "src/backend/models/rfdetr/contract/execution_plan.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include "mmltk/frameworks/reflection/materializer.h"
#include "mmltk/frameworks/reflection/member_relation.h"
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
namespace mmltk::backend::models::rfdetr {
inline constexpr std::size_t kMaximumCliImageInputs = 4096U;
inline constexpr std::size_t kMaximumTrainingDevices = 16U;
struct ModelArtifactRequest {
 // CLEANUP-IGNORE: Each canonical artifact path carries the same reflected path capacity for generated consumers.
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path weights_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path class_layout_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path onnx_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path tensorrt_path;
 [[= mmltk::frameworks::reflection::MaxBytes{
  mmltk::frameworks::reflection::kMaximumNameBytes}]][[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Preset>{}]][
  [= mmltk::frameworks::reflection::CatalogProvider<RfdetrPresetCatalog>{}]] std::string preset_name;
 [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int resolution = 0;
 [[nodiscard]] std::size_t selected_input_count() const noexcept {
  return static_cast<std::size_t>(!weights_path.empty()) + static_cast<std::size_t>(!onnx_path.empty()) + static_cast<std::size_t>(!tensorrt_path.empty());
 }
};
struct DeviceExecutionConfig {
 [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int device_id = 0;
};
struct InferenceExecutionConfig : DeviceExecutionConfig, mmltk::backend::data::DataLoadingOptions {
 CompilationMode compilation_mode = CompilationMode::kSelective;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string cpu_affinity;
 [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int workers = 0;
 bool allow_fp16 = true;
};
struct ModelArtifactOutputRequest : ModelArtifactRequest, DeviceExecutionConfig {
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path output_path;
};
struct BuildEngineRequest : ModelArtifactOutputRequest {
 bool allow_fp16 = true;
};
struct ExportOnnxRequest : ModelArtifactOutputRequest {
 [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int opset_version = 19;
 bool simplify = false;
};
enum class PredictSourceKind : std::uint8_t {
 CompiledDataset,
 ImageFiles,
 VideoFile,
};
struct PredictImageInput {
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path image_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string source_name;
 std::int64_t image_id = 0;
};
struct PredictRequest : ModelArtifactRequest, InferenceExecutionConfig {
 PredictSourceKind source_kind = PredictSourceKind::CompiledDataset;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path video_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path compiled_path;
 [[= mmltk::frameworks::reflection::MaxItems{mmltk::backend::models::rfdetr::kMaximumCliImageInputs}]] std::vector<PredictImageInput> image_inputs;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]][[= mmltk::frameworks::reflection::RuntimeDestination{}]] std::filesystem::path output_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string backend = "auto";
 [[= mmltk::frameworks::reflection::Minimum<std::size_t>{1U}]] std::size_t batch_size = 1U;
 [[= mmltk::frameworks::reflection::Minimum<std::size_t>{0U}]][[= mmltk::frameworks::reflection::Maximum<std::size_t>{kMaximumPredictionCandidates}]] std::size_t max_dets_per_image = 500U;
 [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int lanes = 1;
 [[= mmltk::frameworks::reflection::Minimum<float>{0.0F}]][[= mmltk::frameworks::reflection::Maximum<float>{1.0F}]][[= mmltk::frameworks::reflection::Finite{}]] float threshold = 0.0F;
 std::size_t limit_images = 0U;
 bool include_masks = true;
 bool progress_bar = true;
};
enum class ValidationLogMode : std::uint8_t {
 Quiet,
 Interactive,
};
// CLEANUP-IGNORE: Validation and training are distinct canonical requests even where their reflected path fields align.
struct ValidateRequest : ModelArtifactRequest, InferenceExecutionConfig {
 [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int lanes = 1;
 // CLEANUP-IGNORE: Distinct canonical path fields share constraints, not duplicated runtime behavior.
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path compiled_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path source_dir;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path save_engine_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]][[= mmltk::frameworks::reflection::RuntimeDestination{}]] std::filesystem::path report_json_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string eval_order = "onnx,tensorrt";
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string split;
 [[= mmltk::frameworks::reflection::Minimum<std::size_t>{1U}]] std::size_t batch_size = 1U;
 std::size_t limit_images = 0U;
 std::size_t candidate_count = 0U;
 std::size_t eval_max_dets = 0U;
 std::size_t alignment_images = 16U;
 [[= mmltk::frameworks::reflection::Minimum<std::size_t>{1U}]] std::size_t prefetch_factor = 2U;
 mmltk::backend::imaging::resample::ImageResizeMode compile_resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
 [[= mmltk::frameworks::reflection::Minimum<int>{-1}]] int compile_workers = -1;
 [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int compile_cuda_mask_batch_size = 0;
 [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int compile_cuda_device_id = 0;
 bool recompile = false;
 bool profile = false;
 bool write_report_json = true;
 ValidationLogMode log_mode = ValidationLogMode::Interactive;
};
inline constexpr float kAugmentationScalarMinimum = 0.0F;
inline constexpr float kAugmentationScalarMaximum = 1.0F;
struct AugmentationGroupConfig {
 // CLEANUP-IGNORE: Defaulted aggregate equality and distinct scalar policy declarations are not duplicated runtime
 // behavior.
 bool operator==(const AugmentationGroupConfig&) const = default;
 // CLEANUP-IGNORE: Every augmentation scalar deliberately exposes the same reflected unit-interval policy.
 [[= mmltk::frameworks::reflection::Minimum<float>{kAugmentationScalarMinimum}]][[= mmltk::frameworks::reflection::Maximum<float>{kAugmentationScalarMaximum}]][
  [= mmltk::frameworks::reflection::Finite{}]][[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::UnitInterval>{}]] float probability = 0.0F;
 [[= mmltk::frameworks::reflection::Minimum<float>{kAugmentationScalarMinimum}]][[= mmltk::frameworks::reflection::Maximum<float>{kAugmentationScalarMaximum}]][
  [= mmltk::frameworks::reflection::Finite{}]][[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::UnitInterval>{}]] float min_strength = 0.0F;
 [[= mmltk::frameworks::reflection::Minimum<float>{kAugmentationScalarMinimum}]][[= mmltk::frameworks::reflection::Maximum<float>{kAugmentationScalarMaximum}]][
  [= mmltk::frameworks::reflection::Finite{}]][[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::UnitInterval>{}]] float max_strength = 0.0F;
};
struct GpuAugmentationConfig {
 bool operator==(const GpuAugmentationConfig&) const = default;
 bool enabled = false;
 bool perceptual_downscale = false;
 AugmentationGroupConfig geometry{0.50F, 0.05F, 0.50F};
 AugmentationGroupConfig resize{0.50F, 0.05F, 0.50F};
 AugmentationGroupConfig color{0.50F, 0.05F, 0.50F};
 AugmentationGroupConfig noise{0.50F, 0.05F, 0.50F};
 AugmentationGroupConfig blur{0.50F, 0.05F, 0.50F};
 // CLEANUP-IGNORE: Occlusion remains a named generated field rather than an opaque indexed augmentation entry.
 AugmentationGroupConfig occlusion{0.50F, 0.05F, 0.50F};
 [[= mmltk::frameworks::reflection::Minimum<float>{kAugmentationScalarMinimum}]][[= mmltk::frameworks::reflection::Maximum<float>{kAugmentationScalarMaximum}]][
  [= mmltk::frameworks::reflection::Finite{}]][[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::UnitInterval>{}]] float copy_paste_probability = 0.80F;
};
// CLEANUP-IGNORE: Train is the canonical reflected training request; coincident field shapes remain domain-named.
struct TrainRequest : mmltk::backend::data::DataLoadingOptions {
 bool operator==(const TrainRequest&) const = default;
 // Rank-ordered overrides correspond exactly to device_ids; -1 selects automatic locality.
 [[= mmltk::frameworks::reflection::MaxItems{mmltk::backend::models::rfdetr::kMaximumTrainingDevices}]] std::vector<int>
  // CLEANUP-IGNORE: The ranked NUMA vector and following Train paths are distinct canonical generated fields.
  numa_nodes;
 // CLEANUP-IGNORE: Training input paths each retain the shared path constraint in the authoritative declaration.
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path train_compiled_path;
 // CLEANUP-IGNORE: Validation and training paths remain separately reflected domain fields with stable generated
 // identities.
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path val_compiled_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path weights_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path class_layout_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path resume_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]][[= mmltk::frameworks::reflection::RuntimeDestination{}]] std::filesystem::path output_dir;
 [[= mmltk::frameworks::reflection::MaxBytes{
  mmltk::frameworks::reflection::kMaximumNameBytes}]][[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Preset>{}]]
                                                     // CLEANUP-IGNORE: Preset metadata is deliberately repeated on the Train field consumed by reflection.
                                                     [[= mmltk::frameworks::reflection::CatalogProvider<RfdetrPresetCatalog>{}]] std::string preset_name;
 // CLEANUP-IGNORE: Optional training artifact paths share a capacity but retain distinct generated identities.
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path test_compiled_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path distributed_store_path;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string cpu_affinity;
 [[= mmltk::frameworks::reflection::Minimum<std::size_t>{1U}]] std::size_t batch_size = 1U;
 std::size_t val_batch_size = 0U;
 std::size_t num_queries = 0U;
 std::size_t eval_max_dets = 0U;
 [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int epochs = 1;
 [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int grad_accum_steps = 1;
 [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int print_freq = 100;
 [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int prefetch_factor = 2;
 // CLEANUP-IGNORE: Seed and worker settings are separate generated fields despite adjacent scalar defaults.
 int seed = 42;
 [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int workers = 0;
 [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int lanes = 1;
 [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int resolution = 0;
 [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int device_id = 0;
 [[= mmltk::frameworks::reflection::MaxItems{mmltk::backend::models::rfdetr::kMaximumTrainingDevices}]] std::vector<int> device_ids;
 [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int distributed_rank = 0;
 [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int distributed_world_size = 1;
 // CLEANUP-IGNORE: EMA and optimizer fields retain explicit reflected constraints at their canonical declarations.
 [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int ema_tau = 100;
 [[= mmltk::frameworks::reflection::Minimum<double>{
  0.0}]][[= mmltk::frameworks::reflection::Finite{}]][[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Norm>{}]] double
  // CLEANUP-IGNORE: Gradient clipping is a nonnegative norm; adjacent optimizer scalars are bounded decay
  // fractions.
  clip_max_norm = 0.1;
 [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]][
  [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double ema_decay = 0.993;
 TrainRecipeSettings recipe;
 TrainLaneConfiguration lane_configuration;
 TrainDataPolicy data_policy;
 [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int validation_lanes = 1;
 [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int unfreeze_encoder_last_epochs = 0;
 [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int disable_augmentation_last_epochs = 0;
 GpuAugmentationConfig gpu_augmentation;
 TrainingSupervisionConfig training_supervision;
 bool use_ema = false;
 bool validation_loss = false;
 bool validation_profile = false;
 bool amp = true;
 bool progress_bar = true;
 bool freeze_encoder = false;
 bool fused_optimizer = true;
 bool distributed_worker = false;
 CompilationMode compilation_mode = CompilationMode::kSelective;
};
[[nodiscard]] std::string encode_train_request_json(const TrainRequest&);
[[nodiscard]] TrainRequest decode_train_request_json(std::string_view);
MMLTK_REFLECT_FIELDS(TrainRequest)
}  // namespace mmltk::backend::models::rfdetr
namespace mmltk::backend::models::rfdetr {
MMLTK_REFLECT_FIELDS(ModelArtifactRequest)
MMLTK_REFLECT_FIELDS(DeviceExecutionConfig)
MMLTK_REFLECT_FIELDS(InferenceExecutionConfig)
MMLTK_REFLECT_FIELDS(ModelArtifactOutputRequest)
MMLTK_REFLECT_FIELDS(BuildEngineRequest)
MMLTK_REFLECT_FIELDS(ExportOnnxRequest)
MMLTK_REFLECT_FIELDS(PredictImageInput)
MMLTK_REFLECT_FIELDS(PredictRequest)
MMLTK_REFLECT_FIELDS(ValidateRequest)
// CLEANUP-IGNORE: Reflection registration and policy audits are declarative inventories of different canonical types.
MMLTK_REFLECT_FIELDS(AugmentationGroupConfig)
// CLEANUP-IGNORE: Reflection inventory entries intentionally share one declarative spelling per canonical type.
MMLTK_REFLECT_FIELDS(GpuAugmentationConfig)
static_assert(mmltk::frameworks::reflection::reflected_policies_are_valid<AugmentationGroupConfig>());
static_assert(mmltk::frameworks::reflection::reflected_policies_are_valid<GpuAugmentationConfig>());
static_assert(mmltk::frameworks::reflection::reflected_defaults_are_valid<AugmentationGroupConfig>());
static_assert(mmltk::frameworks::reflection::reflected_defaults_are_valid<GpuAugmentationConfig>());
[[nodiscard]] constexpr bool augmentation_group_relationship_valid(const AugmentationGroupConfig& group) noexcept { return group.min_strength <= group.max_strength; }
[[nodiscard]] constexpr bool gpu_augmentation_relationships_valid(const GpuAugmentationConfig& config) noexcept {
 return augmentation_group_relationship_valid(config.geometry) && augmentation_group_relationship_valid(config.resize) && augmentation_group_relationship_valid(config.color) &&
        augmentation_group_relationship_valid(config.noise) && augmentation_group_relationship_valid(config.blur) && augmentation_group_relationship_valid(config.occlusion);
}
[[nodiscard]] constexpr bool gpu_augmentation_config_valid(const GpuAugmentationConfig& config) noexcept {
 return !mmltk::frameworks::reflection::validate_reflected_fields(config).has_value() && gpu_augmentation_relationships_valid(config);
}
MMLTK_REFLECT_ENUM(PredictSourceKind)
MMLTK_REFLECT_ENUM(ValidationLogMode)
static_assert(mmltk::frameworks::reflection::ingress_policies_are_valid<ModelArtifactRequest>());
static_assert(mmltk::frameworks::reflection::ingress_policies_are_valid<DeviceExecutionConfig>());
static_assert(mmltk::frameworks::reflection::ingress_policies_are_valid<InferenceExecutionConfig>());
static_assert(mmltk::frameworks::reflection::ingress_policies_are_valid<ModelArtifactOutputRequest>());
static_assert(mmltk::frameworks::reflection::ingress_policies_are_valid<BuildEngineRequest>());
static_assert(mmltk::frameworks::reflection::ingress_policies_are_valid<ExportOnnxRequest>());
static_assert(mmltk::frameworks::reflection::ingress_policies_are_valid<PredictRequest>());
static_assert(mmltk::frameworks::reflection::ingress_policies_are_valid<ValidateRequest>());
static_assert(mmltk::frameworks::reflection::ingress_policies_are_valid<TrainRequest>());
void validate_build_engine_request(const BuildEngineRequest& request);
void validate_export_onnx_request(const ExportOnnxRequest& request);
void validate_predict_request(const PredictRequest& request);
void validate_validate_request(const ValidateRequest& request);
void validate_train_placement(const TrainRequest& request);
void validate_train_request(const TrainRequest& request);
}  // namespace mmltk::backend::models::rfdetr
