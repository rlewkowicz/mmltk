#pragma once
#include "src/backend/models/rfdetr/contract/prediction_limits.h"
#include "src/backend/data/data_loading_options.h"

#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "mmltk/frameworks/reflection/materializer.h"
#include "mmltk/frameworks/reflection/member_relation.h"

#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
namespace mmltk::backend::models::rfdetr {

struct ModelArtifactRequest {
    // CLEANUP-IGNORE: Each canonical artifact path carries the same reflected path capacity for generated consumers.
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path weights_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path onnx_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path tensorrt_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]]
        [[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Preset>{}]]
        [[= mmltk::frameworks::reflection::CatalogProvider<RfdetrPresetCatalog>{}]] std::string preset_name;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int resolution = 0;

    [[nodiscard]] std::size_t selected_input_count() const noexcept {
        return static_cast<std::size_t>(!weights_path.empty()) + static_cast<std::size_t>(!onnx_path.empty()) +
               static_cast<std::size_t>(!tensorrt_path.empty());
    }
};

struct DeviceExecutionConfig {
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int device_id = 0;
};

struct InferenceExecutionConfig : DeviceExecutionConfig, mmltk::backend::data::DataLoadingOptions {
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

using ModelExportRequest = std::variant<BuildEngineRequest, ExportOnnxRequest>;

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
    [[= mmltk::frameworks::reflection::MaxItems{mmltk::frameworks::reflection::kMaximumCliImageInputs}]] std::vector<PredictImageInput>
        image_inputs;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path output_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string backend = "auto";
    [[= mmltk::frameworks::reflection::Minimum<std::size_t>{1U}]] std::size_t batch_size = 1U;
    [[= mmltk::frameworks::reflection::Minimum<std::size_t>{1U}]]
    [[= mmltk::frameworks::reflection::Maximum<std::size_t>{kMaximumPredictionCandidates}]] std::size_t max_dets_per_image = 500U;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int lanes = 0;
    [[= mmltk::frameworks::reflection::Minimum<float>{
        0.0F}]][[= mmltk::frameworks::reflection::Maximum<float>{1.0F}]][[= mmltk::frameworks::reflection::Finite{}]] float threshold =
        0.0F;
    std::size_t limit_images = 0U;
    bool include_masks = true;
    bool progress_bar = true;
    CompilationMode compilation_mode = CompilationMode::kSelective;
};

enum class ValidationLogMode : std::uint8_t {
    Quiet,
    Interactive,
};

// CLEANUP-IGNORE: Validation and training are distinct canonical requests even where their reflected path fields align.
struct ValidateRequest : ModelArtifactRequest, InferenceExecutionConfig {
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path compiled_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path source_dir;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path save_engine_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path report_json_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string eval_order =
        "onnx,tensorrt";
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string split;
    [[= mmltk::frameworks::reflection::Minimum<std::size_t>{1U}]] std::size_t batch_size = 1U;
    std::size_t limit_images = 0U;
    std::size_t num_queries = 0U;
    std::size_t eval_max_dets = 0U;
    std::size_t alignment_images = 16U;
    [[= mmltk::frameworks::reflection::Minimum<std::size_t>{1U}]] std::size_t prefetch_factor = 2U;
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
    [[= mmltk::frameworks::reflection::Minimum<float>{kAugmentationScalarMinimum}]]
        [[= mmltk::frameworks::reflection::Maximum<float>{kAugmentationScalarMaximum}]][[= mmltk::frameworks::reflection::Finite{}]]
        [[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::UnitInterval>{}]] float
            probability = 0.0F;
    [[= mmltk::frameworks::reflection::Minimum<float>{kAugmentationScalarMinimum}]]
        [[= mmltk::frameworks::reflection::Maximum<float>{kAugmentationScalarMaximum}]][[= mmltk::frameworks::reflection::Finite{}]]
        [[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::UnitInterval>{}]] float
            min_strength = 0.0F;
    [[= mmltk::frameworks::reflection::Minimum<float>{kAugmentationScalarMinimum}]]
        [[= mmltk::frameworks::reflection::Maximum<float>{kAugmentationScalarMaximum}]][[= mmltk::frameworks::reflection::Finite{}]]
        [[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::UnitInterval>{}]] float
            max_strength = 0.0F;
};

struct GpuAugmentationConfig {
    bool operator==(const GpuAugmentationConfig&) const = default;
    bool enabled = false;
    AugmentationGroupConfig geometry{0.50F, 0.05F, 0.50F};
    AugmentationGroupConfig resize{0.50F, 0.05F, 0.50F};
    AugmentationGroupConfig color{0.50F, 0.05F, 0.50F};
    AugmentationGroupConfig noise{0.50F, 0.05F, 0.50F};
    AugmentationGroupConfig blur{0.50F, 0.05F, 0.50F};
    // CLEANUP-IGNORE: Occlusion remains a named generated field rather than an opaque indexed augmentation entry.
    AugmentationGroupConfig occlusion{0.50F, 0.05F, 0.50F};
    [[= mmltk::frameworks::reflection::Minimum<float>{kAugmentationScalarMinimum}]]
        [[= mmltk::frameworks::reflection::Maximum<float>{kAugmentationScalarMaximum}]][[= mmltk::frameworks::reflection::Finite{}]]
        [[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::UnitInterval>{}]] float
            copy_paste_probability = 0.80F;
};

class[[= mmltk::frameworks::reflection::OpaqueRelationStorage{}]] TrainRecipeOverrideState final {
   public:
    constexpr TrainRecipeOverrideState() noexcept = default;
    constexpr bool operator==(const TrainRecipeOverrideState&) const noexcept = default;

   private:
    [[= mmltk::frameworks::reflection::Maximum<std::uint16_t>{std::uint16_t{0x07ffU}}]] std::uint16_t mask = 0U;

    friend struct mmltk::frameworks::reflection::catalog_provider_relation<TrainRecipeCatalog>;
};

// CLEANUP-IGNORE: Train is the canonical reflected training request; coincident field shapes remain domain-named.
struct TrainRequest : mmltk::backend::data::DataLoadingOptions {
    // Rank-ordered overrides correspond exactly to device_ids; -1 selects automatic locality.
    [[= mmltk::frameworks::reflection::MaxItems{mmltk::frameworks::reflection::kMaximumTrainingDevices}]] std::vector<int>
        // CLEANUP-IGNORE: The ranked NUMA vector and following Train paths are distinct canonical generated fields.
        numa_nodes;
    // CLEANUP-IGNORE: Training input paths each retain the shared path constraint in the authoritative declaration.
    [[= mmltk::frameworks::reflection::MaxBytes{
        mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path train_compiled_path;
    // CLEANUP-IGNORE: Validation and training paths remain separately reflected domain fields with stable generated
    // identities.
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path val_compiled_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path weights_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path resume_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path output_dir;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]]
        [[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Preset>{}]]
        // CLEANUP-IGNORE: Preset metadata is deliberately repeated on the Train field consumed by reflection.
        [[= mmltk::frameworks::reflection::CatalogProvider<RfdetrPresetCatalog>{}]] std::string preset_name;
    // CLEANUP-IGNORE: Optional training artifact paths share a capacity but retain distinct generated identities.
    [[= mmltk::frameworks::reflection::MaxBytes{
        mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path test_compiled_path;
    [[= mmltk::frameworks::reflection::MaxBytes{
        mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path distributed_store_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string cpu_affinity;
    TrainLrSchedulerKind lr_scheduler = TrainLrSchedulerKind::Step;
    [[= mmltk::frameworks::reflection::Minimum<std::size_t>{1U}]] std::size_t batch_size = 1U;
    std::size_t val_batch_size = 0U;
    std::size_t num_queries = 0U;
    std::size_t eval_max_dets = 0U;
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int epochs = 1;
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int grad_accum_steps = 1;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int lr_drop = 100;
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int print_freq = 100;
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int prefetch_factor = 2;
    // CLEANUP-IGNORE: Seed and worker settings are separate generated fields despite adjacent scalar defaults.
    int seed = 42;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int workers = 0;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int lanes = 0;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int resolution = 0;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int device_id = 0;
    [[= mmltk::frameworks::reflection::MaxItems{mmltk::frameworks::reflection::kMaximumTrainingDevices}]] std::vector<int> device_ids;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int distributed_rank = 0;
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int distributed_world_size = 1;
    // CLEANUP-IGNORE: EMA and optimizer fields retain explicit reflected constraints at their canonical declarations.
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int ema_tau = 100;
    [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]][[= mmltk::frameworks::reflection::Finite{}]][
        [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::LearningRate>{}]] double lr =
        1.0e-4;
    [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]]
        [[= mmltk::frameworks::reflection::Finite{}]][[= mmltk::frameworks::reflection::Presentation<
            // CLEANUP-IGNORE: Each learning-rate field needs an independently addressable generated identity.
            mmltk::frameworks::reflection::PresentationKind::LearningRate>{}]] double lr_encoder = 1.5e-4;
    [[= mmltk::frameworks::reflection::Minimum<double>{
        0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]]
              [[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double
                  lr_component_decay = 0.7;
    [[= mmltk::frameworks::reflection::Minimum<double>{
        0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]]
              [[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double
                  encoder_layer_decay = 0.8;
    [[= mmltk::frameworks::reflection::Minimum<double>{
        0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]][
        [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double momentum = 0.95;
    [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]][[= mmltk::frameworks::reflection::Finite{}]][
        [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double weight_decay =
        1.0e-4;
    // CLEANUP-IGNORE: Warmup remains a separate constrained setting rather than an indexed optimizer scalar.
    [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]][[= mmltk::frameworks::reflection::Finite{}]] double warmup_epochs = 0.0;
    [[= mmltk::frameworks::reflection::Minimum<double>{
        0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]][
        [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double warmup_momentum =
        0.0;
    [[= mmltk::frameworks::reflection::Minimum<double>{
        0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]][
        [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double lr_min_factor =
        0.0;
    [[= mmltk::frameworks::reflection::Minimum<double>{0.0}]][[= mmltk::frameworks::reflection::Finite{}]][
        [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Norm>{}]] double
        // CLEANUP-IGNORE: Gradient clipping is a nonnegative norm; adjacent optimizer scalars are bounded decay
        // fractions.
        clip_max_norm = 0.1;
    [[= mmltk::frameworks::reflection::Minimum<double>{
        0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]][
        [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Decay>{}]] double ema_decay = 0.993;
    [[= mmltk::frameworks::reflection::CatalogProvider<TrainRecipeCatalog>{}]] TrainOptimizerKind optimizer = TrainOptimizerKind::AdamW;
    GpuAugmentationConfig gpu_augmentation;
    TrainingSupervisionConfig training_supervision;
    TrainRecipeOverrideState recipe_overrides;
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

MMLTK_REFLECT_FIELDS(TrainRecipeOverrideState)
MMLTK_REFLECT_FIELDS(TrainRequest)

}  // namespace mmltk::backend::models::rfdetr

namespace mmltk::frameworks::reflection {

template <>
struct catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog>
    : StaticMemberRelation<
          mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry, mmltk::backend::models::rfdetr::TrainRequest, 11U,
          MemberRelationEntry<member_path<&mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry::lr>,
                              member_path<&mmltk::backend::models::rfdetr::TrainRequest::lr>>,
          MemberRelationEntry<member_path<&mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry::lr_encoder>,
                              member_path<&mmltk::backend::models::rfdetr::TrainRequest::lr_encoder>>,
          MemberRelationEntry<member_path<&mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry::lr_component_decay>,
                              member_path<&mmltk::backend::models::rfdetr::TrainRequest::lr_component_decay>>,
          MemberRelationEntry<member_path<&mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry::encoder_layer_decay>,
                              member_path<&mmltk::backend::models::rfdetr::TrainRequest::encoder_layer_decay>>,
          MemberRelationEntry<member_path<&mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry::momentum>,
                              member_path<&mmltk::backend::models::rfdetr::TrainRequest::momentum>>,
          MemberRelationEntry<member_path<&mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry::weight_decay>,
                              member_path<&mmltk::backend::models::rfdetr::TrainRequest::weight_decay>>,  // CLEANUP-IGNORE: Each
                                                                                                          // relation row names a
                                                                                                          // distinct canonical source
                                                                                                          // and destination identity.
          MemberRelationEntry<member_path<&mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry::warmup_epochs>,
                              member_path<&mmltk::backend::models::rfdetr::TrainRequest::warmup_epochs>>,
          MemberRelationEntry<member_path<&mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry::warmup_momentum>,
                              member_path<&mmltk::backend::models::rfdetr::TrainRequest::warmup_momentum>>,
          MemberRelationEntry<member_path<&mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry::lr_min_factor>,
                              member_path<&mmltk::backend::models::rfdetr::TrainRequest::lr_min_factor>>,
          MemberRelationEntry<member_path<&mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry::lr_drop>,
                              member_path<&mmltk::backend::models::rfdetr::TrainRequest::lr_drop>>,
          MemberRelationEntry<member_path<&mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry::lr_scheduler>,
                              member_path<&mmltk::backend::models::rfdetr::TrainRequest::lr_scheduler>>> {
    using provider_type = mmltk::backend::models::rfdetr::TrainRecipeCatalog;
    using override_state_type = mmltk::backend::models::rfdetr::TrainRecipeOverrideState;
    inline static constexpr auto source_selector = member_path<&mmltk::backend::models::rfdetr::TrainRecipeCatalogEntry::optimizer>;
    inline static constexpr auto destination_selector = member_path<&mmltk::backend::models::rfdetr::TrainRequest::optimizer>;
    inline static constexpr auto destination_override_state = member_path<&mmltk::backend::models::rfdetr::TrainRequest::recipe_overrides>;
    inline static constexpr std::uint16_t valid_bits = 0x07ffU;

    template <auto Destination>
    [[nodiscard]] static constexpr bool overridden(const override_state_type& state) noexcept {
        return (state.mask & bit<Destination>()) != 0U;
    }

    template <auto Destination>
    static constexpr void set_override(override_state_type& state) noexcept {
        state.mask = static_cast<std::uint16_t>(state.mask | bit<Destination>());
    }

    template <auto Destination>
    static constexpr void clear_override(override_state_type& state) noexcept {
        state.mask = static_cast<std::uint16_t>(state.mask & ~bit<Destination>());
    }

    [[nodiscard]] static consteval bool audit() {
        if (!valid() || member_count != 11U) return false;
        if constexpr (!std::same_as<accessor_value_t<source_type, source_selector>,
                                    accessor_value_t<destination_type, destination_selector>>)
            return false;
        const auto source_selector_identity = accessor_member_identity<source_type, source_selector>();
        const auto destination_selector_identity = accessor_member_identity<destination_type, destination_selector>();
        bool selectors_are_not_members = true;
        std::uint16_t claimed = 0U;
        VisitMembers([&]<class Entry>() {
            selectors_are_not_members = selectors_are_not_members &&
                                        accessor_member_identity<source_type, Entry::source>() != source_selector_identity &&
                                        accessor_member_identity<destination_type, Entry::destination>() != destination_selector_identity;
            claimed = static_cast<std::uint16_t>(claimed | bit<Entry::destination>());
        });
        return selectors_are_not_members && claimed == valid_bits;
    }

   private:
    template <auto Destination>
    [[nodiscard]] static consteval std::uint16_t bit() {
        const auto identity = accessor_member_identity<destination_type, Destination>();
        std::size_t ordinal = 0U;
        std::size_t found = member_count;
        VisitMembers([&]<class Entry>() {
            if (accessor_member_identity<destination_type, Entry::destination>() == identity) found = ordinal;
            ++ordinal;
        });
        if (found == member_count) throw "destination accessor is not a Train recipe relation member";
        return static_cast<std::uint16_t>(std::uint16_t{1U} << found);
    }
};

static_assert(catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog>::audit());

}  // namespace mmltk::frameworks::reflection

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

[[nodiscard]] constexpr bool augmentation_group_relationship_valid(const AugmentationGroupConfig& group) noexcept {
    return group.min_strength <= group.max_strength;
}

[[nodiscard]] constexpr bool gpu_augmentation_relationships_valid(const GpuAugmentationConfig& config) noexcept {
    return augmentation_group_relationship_valid(config.geometry) && augmentation_group_relationship_valid(config.resize) &&
           augmentation_group_relationship_valid(config.color) && augmentation_group_relationship_valid(config.noise) &&
           augmentation_group_relationship_valid(config.blur) && augmentation_group_relationship_valid(config.occlusion);
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
