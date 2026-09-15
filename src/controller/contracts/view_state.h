#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"

#include "mmltk/frameworks/reflection/materializer.h"
#include "mmltk/frameworks/reflection/member_path.h"
#include "mmltk/frameworks/reflection/member_relation.h"

#include "src/backend/models/catalog/artifacts.h"
#include "src/backend/models/catalog/model_registry.h"
#include "src/backend/models/catalog/module.h"
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/controller/contracts/explore_filter.h"
#include "src/controller/contracts/annotation.h"
#include "src/controller/contracts/model_selection_types.h"
#include "src/controller/contracts/workflows.h"
namespace mmltk::controller::contracts {

enum class SourceKind : std::uint8_t {
    CompiledDataset,
    SingleImage,
    ImageFolder,
    VideoStream,
    VideoFile,
};

struct SourceSelectionState {
    SourceKind kind = SourceKind::CompiledDataset;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]]
        [[= reflection::FileDialog<"Select compiled dataset", "Compiled datasets", "*.mmltk *.bin">{
            .mode = FileDialogMode::OpenFile}]] std::string compiled_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]]
        [[= reflection::FileDialog<"Select image", "Images", "*.png *.jpg *.jpeg *.bmp *.webp">{
            .mode = FileDialogMode::OpenFile}]] std::string single_image_path;
    [[= mmltk::frameworks::reflection::MaxBytes{
        mmltk::frameworks::reflection::kMaximumPathBytes}]][[= reflection::FileDialog<"Select image directory", "Directories", "*">{
        .mode = FileDialogMode::OpenFolder}]] std::string image_directory;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]]
        [[= reflection::FileDialog<"Select video file", "Video files", "*.mp4 *.mkv *.mov *.avi *.webm *.m4v">{
            .mode = FileDialogMode::OpenFile}]] std::string video_file_path;
    bool recursive = false;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int device_index = 0;
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int capture_width = 1920;
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int capture_height = 1080;
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int capture_fps = 120;
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int v4l2_buffer_count = 4;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int crop_x = 0;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int crop_y = 0;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int crop_width = 0;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int crop_height = 0;
    bool operator==(const SourceSelectionState&) const = default;
};

struct ResolvedVideoCrop {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

enum class TrainExecutionTarget : std::uint8_t {
    Local = 0,
    Remote = 1,
};

enum class ExploreDatasetSource : std::uint8_t {
    Train = 0,
    Validation = 1,
    Test = 2,
    Custom = 3,
};

enum class ExploreDetailScaleMode : std::uint8_t {
    Fast = 0,
    Neural = 1,
    Basic = 2,
};

enum class WorkspaceAspectRatio : std::uint8_t {
    Widescreen = 0,
    Portrait = 1,
    Standard = 2,
    Photo = 3,
    Square = 4,
    SixteenTen = 5,
};

inline constexpr std::string_view kDefaultModelPresetName = mmltk::backend::models::rfdetr::kPresetCatalog.front().preset_name;
inline constexpr int kDefaultModelResolution = static_cast<int>(mmltk::backend::models::rfdetr::kPresetCatalog.front().resolution);
inline constexpr int kMaxExploreGridColumns = 99;

struct WorkflowModelSelectionState {
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]]
        [[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Preset>{}]]
        [[= mmltk::frameworks::reflection::CatalogProvider<mmltk::backend::models::rfdetr::RfdetrPresetCatalog>{}]] std::string preset_name{
            std::string(kDefaultModelPresetName)};
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int model_resolution = kDefaultModelResolution;
    ModelSelectionSource model_source = ModelSelectionSource::Canonical;
    ModelArtifactInputKind model_input = ModelArtifactInputKind::None;
};

struct UiSettingsState {
    bool dark_mode = false;
    bool show_workspace_performance = false;
    [[= mmltk::frameworks::reflection::Minimum<float>{
        0.85F}]][[= mmltk::frameworks::reflection::Maximum<float>{1.75F}]][[= mmltk::frameworks::reflection::Finite{}]] float ui_scale =
        1.0f;
    [[= mmltk::frameworks::reflection::Minimum<float>{
        10.0F}]][[= mmltk::frameworks::reflection::Maximum<float>{32.0F}]][[= mmltk::frameworks::reflection::Finite{}]] float font_size =
        14.0f;
    [[= mmltk::frameworks::reflection::Minimum<float>{9.0F}]][[= mmltk::frameworks::reflection::Maximum<float>{28.0F}]][
        [= mmltk::frameworks::reflection::Finite{}]] float secondary_font_size = 12.0f;
    [[= mmltk::frameworks::reflection::Minimum<float>{9.0F}]][[= mmltk::frameworks::reflection::Maximum<float>{28.0F}]]
                                                             [[= mmltk::frameworks::reflection::Finite{}]] float mono_font_size = 12.0f;
    [[= mmltk::frameworks::reflection::Minimum<float>{9.0F}]][[= mmltk::frameworks::reflection::Maximum<float>{31.0F}]][
        [= mmltk::frameworks::reflection::Finite{}]] float text_input_font_size = 13.0f;
    [[= mmltk::frameworks::reflection::Minimum<float>{0.0F}]][[= mmltk::frameworks::reflection::Finite{}]] float crop_edge_hit_half_width =
        8.0f;
    [[= mmltk::frameworks::reflection::Minimum<float>{0.0F}]][[= mmltk::frameworks::reflection::Finite{}]] float crop_corner_hit_size =
        20.0f;
    [[= mmltk::frameworks::reflection::Minimum<float>{0.0F}]][[= mmltk::frameworks::reflection::Finite{}]] float crop_handle_radius = 6.0f;
    WorkspaceAspectRatio workspace_aspect_ratio = WorkspaceAspectRatio::Widescreen;
    [[= mmltk::frameworks::reflection::Minimum<int>{kMinAnnotationBrushRadius}]]
        [[= mmltk::frameworks::reflection::Maximum<int>{kMaxAnnotationBrushRadius}]] int annotation_brush_radius =
            kDefaultAnnotationBrushRadius;
    [[= mmltk::frameworks::reflection::Minimum<int>{kMinAnnotationMaskCleanupRadius}]]
        [[= mmltk::frameworks::reflection::Maximum<int>{kMaxAnnotationMaskCleanupRadius}]] int mask_cleanup_radius =
            kDefaultAnnotationMaskCleanupRadius;
};

struct ModelArtifactSelectionState {
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string weights_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string onnx_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string tensorrt_path;
    // CLEANUP-IGNORE: The normalized artifact projection and stored workflow selection have different field ownership.
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]]
        [[= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::Preset>{}]]
        [[= mmltk::frameworks::reflection::CatalogProvider<mmltk::backend::models::rfdetr::RfdetrPresetCatalog>{}]] std::string preset_name{
            std::string(kDefaultModelPresetName)};
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int resolution = kDefaultModelResolution;
    ModelSelectionSource source = ModelSelectionSource::Canonical;
    ModelArtifactInputKind input = ModelArtifactInputKind::None;
};

struct TrainExecutionPaneState {
    TrainExecutionTarget execution_target = TrainExecutionTarget::Local;
    std::array<bool, 5> remote_family_enabled{{true, true, true, true, true}};
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string remote_container_image;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string remote_launch_template;
};

struct TrainViewState : TrainExecutionPaneState {
    TrainViewState() {
        request.batch_size = 2;
        request.epochs = 12;
        request.workers = 16;
        request.prefetch_factor = 3;
        request.progress_bar = true;
        request.lanes = 3;
        request.val_batch_size = 8;
        request.device_ids = {0};
        request.train_compiled_path = "./compiled/train.bin";
        request.val_compiled_path = "./compiled/val.bin";
        request.output_dir = "./gui-train-output";
        request.preset_name = kDefaultModelPresetName;
        request.resolution = kDefaultModelResolution;
        request.gpu_augmentation.enabled = true;
    }

    mmltk::backend::models::rfdetr::TrainRequest request;
    ModelSelectionSource model_source = ModelSelectionSource::Canonical;
    ModelArtifactInputKind model_input = ModelArtifactInputKind::Weights;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]]
        [[= mmltk::controller::contracts::reflection::FileDialog<"Select dataset source", "Directories", "*">{
            .mode = mmltk::controller::contracts::FileDialogMode::OpenFolder}]] std::string dataset_source_dir = "./dataset";
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]]
        [[= mmltk::controller::contracts::reflection::FileDialog<"Select compiled dataset", "Directories", "*">{
            .mode = mmltk::controller::contracts::FileDialogMode::OpenFolder}]] std::string compiled_dataset_dir = "./compiled";
    bool use_compiled_directory_defaults = true;
    bool overwrite_compiled_dataset = false;
    bool compile_dimensions = false;
    bool compile_benchmark_dataset_override = false;
    bool visualize_augmentation_in_explore = false;
};

struct ValidateViewState {
    ValidateViewState() {
        request.log_mode = mmltk::backend::models::rfdetr::ValidationLogMode::Quiet;
        request.report_json_path = "./rfdetr-validation-report.json";
        request.split = "val";
        request.preset_name = kDefaultModelPresetName;
        request.resolution = kDefaultModelResolution;
    }

    mmltk::backend::models::rfdetr::ValidateRequest request;
    ModelSelectionSource model_source = ModelSelectionSource::Canonical;
    ModelArtifactInputKind model_input = ModelArtifactInputKind::Weights;
};

struct PredictViewState {
    PredictViewState() {
        request.output_path = "./predictions.json";
        request.batch_size = 1;
        request.threshold = 0.25F;
        request.preset_name = kDefaultModelPresetName;
        request.resolution = kDefaultModelResolution;
    }

    mmltk::backend::models::rfdetr::PredictRequest request;
    SourceSelectionState source;
    ModelSelectionSource model_source = ModelSelectionSource::Canonical;
    ModelArtifactInputKind model_input = ModelArtifactInputKind::Weights;
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int live_split_count = 1;
};

struct AnnotateViewState : WorkflowModelSelectionState {
    AnnotateViewState() { source.kind = SourceKind::ImageFolder; }

    SourceSelectionState source;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]]
        [[= mmltk::controller::contracts::reflection::FileDialog<"Select annotation weights", "Weights", "*.pt *.pth *.ckpt *.safetensors">{
            .mode = mmltk::controller::contracts::FileDialogMode::OpenFile}]] std::string weights_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]]
        [[= mmltk::controller::contracts::reflection::FileDialog<"Select annotation ONNX", "ONNX files", "*.onnx">{
            .mode = mmltk::controller::contracts::FileDialogMode::OpenFile}]] std::string onnx_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]]
        [[= mmltk::controller::contracts::reflection::FileDialog<"Select annotation engine", "TensorRT files",
                                                                 // CLEANUP-IGNORE: Each annotation artifact field owns
                                                                 // a distinct reflected dialog identity.
                                                                 "*.engine *.trt">{
            .mode = mmltk::controller::contracts::FileDialogMode::OpenFile}]] std::string tensorrt_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]]
        [[= mmltk::controller::contracts::reflection::FileDialog<"Select annotation output", "Directories", "*">{
            .mode = mmltk::controller::contracts::FileDialogMode::OpenFolder}]] std::string output_dir = "./annotated-scenes";
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string split = "train";
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string backend = "auto";
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int device_id = 0;
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int max_dets_per_image = 300;
    [[= mmltk::frameworks::reflection::Minimum<float>{
        0.0F}]][[= mmltk::frameworks::reflection::Maximum<float>{1.0F}]][[= mmltk::frameworks::reflection::Finite{}]][
        [= mmltk::frameworks::reflection::Presentation<mmltk::frameworks::reflection::PresentationKind::UnitInterval>{}]] float threshold =
        0.25f;
    bool allow_fp16 = true;
    bool full_frame = false;
    mmltk::backend::models::rfdetr::CompilationMode compile_mode = mmltk::backend::models::rfdetr::CompilationMode::kSelective;
};

// CLEANUP-IGNORE: Export owns distinct reflected artifact inputs that share the canonical path constraint.
struct ExportViewState : WorkflowModelSelectionState {
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string weights_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string onnx_input_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]]
        [[= mmltk::controller::contracts::reflection::FileDialog<"Choose ONNX export", "ONNX files", "*.onnx">{
            .mode = mmltk::controller::contracts::FileDialogMode::SaveFile}]] std::string onnx_output_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]]
        [[= mmltk::controller::contracts::reflection::FileDialog<"Choose TensorRT export", "TensorRT files", "*.engine *.trt">{
            .mode = mmltk::controller::contracts::FileDialogMode::SaveFile}]] std::string output_path = "./rfdetr-engine.trt";
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int device_id = 0;
    [[= mmltk::frameworks::reflection::Minimum<int>{1}]] int opset_version = 19;
    bool allow_fp16 = true;
    bool build_tensorrt = true;
    bool simplify = false;
};

struct ExploreViewState : mmltk::backend::data::DataLoadingOptions {
    ExploreViewState() {
        sample_classes.fill(true);
        overlay_classes.fill(true);
    }

    ExploreDatasetSource dataset_source = ExploreDatasetSource::Train;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]]
        [[= mmltk::controller::contracts::reflection::FileDialog<"Select compiled dataset", "Compiled datasets", "*.mmltk *.bin">{
            .mode = mmltk::controller::contracts::FileDialogMode::OpenFile}]] std::string custom_compiled_path;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int device_id = 0;
    [[= mmltk::frameworks::reflection::Minimum<int>{
        1}]][[= mmltk::frameworks::reflection::Maximum<int>{kMaxExploreGridColumns}]] int grid_width = 3;
    ExploreOrder order = ExploreOrder::Sequential;
    std::uint64_t shuffle_seed = 0U;
    bool require_boxes = false;
    bool require_masks = false;
    [[= mmltk::frameworks::reflection::Maximum<std::uint32_t>{10'000U}]] std::uint32_t min_instances = 0U;
    [[= mmltk::frameworks::reflection::Maximum<std::uint32_t>{10'000U}]] std::uint32_t max_instances = 10'000U;
    std::uint64_t min_compiled_index = 0U;
    std::uint64_t max_compiled_index = std::numeric_limits<std::uint64_t>::max();
    [[= reflection::PersistenceMetadata{}]] ExploreClassCatalogIdentity class_catalog_identity = 0U;
    std::array<bool, kExploreClassCapacity> sample_classes{};
    std::array<bool, kExploreClassCapacity> overlay_classes{};
    bool show_boxes = true;
    bool show_masks = true;
    bool show_labels = true;
    bool show_original_dimensions = false;
    ExploreDetailScaleMode detail_scale_mode = ExploreDetailScaleMode::Basic;
};

template <typename State>
concept ModelArtifactSelectionView = requires(State& s) {
    s.model_source;
    s.model_input;
} && (requires(State& s) { s.request.weights_path; } || requires(State& s) { s.weights_path; });

template <typename State>
[[nodiscard]] decltype(auto) canonical_request_or_state(State& state) {
    if constexpr (requires { state.request; }) {
        return (state.request);
    } else {
        return (state);
    }
}

// Views expose whichever artifact slots their workflow accepts: train takes weights only,
// export adds ONNX, and the inference views add TensorRT on top.
template <ModelArtifactSelectionView State>
[[nodiscard]] inline ModelArtifactSelectionState model_artifacts(const State& s) {
    const auto path_text = [](const auto& path) {
        if constexpr (std::same_as<std::remove_cvref_t<decltype(path)>, std::filesystem::path>) {
            return path.string();
        } else {
            return std::string(path);
        }
    };
    const auto& source = canonical_request_or_state(s);
    const std::string& preset_name = [&]() -> const std::string& {
        if constexpr (requires { s.request; })
            return source.preset_name;
        else
            return s.preset_name;
    }();
    const int resolution = [&] {
        if constexpr (requires { s.request; })
            return static_cast<int>(source.resolution);
        else
            return s.model_resolution;
    }();
    ModelArtifactSelectionState artifact_state{.weights_path = path_text(source.weights_path),
                                               .onnx_path = {},
                                               .tensorrt_path = {},
                                               .preset_name = preset_name,
                                               .resolution = resolution,
                                               .source = s.model_source,
                                               .input = s.model_input};
    if constexpr (requires { source.onnx_input_path; }) {
        artifact_state.onnx_path = path_text(source.onnx_input_path);
    } else if constexpr (requires { source.onnx_path; }) {
        artifact_state.onnx_path = path_text(source.onnx_path);
    }
    if constexpr (requires { source.tensorrt_path; }) { artifact_state.tensorrt_path = path_text(source.tensorrt_path); }
    return artifact_state;
}

template <ModelArtifactSelectionView State>
inline void apply_model_artifacts(State& s, const ModelArtifactSelectionState& artifact_state) {
    auto& destination = canonical_request_or_state(s);
    destination.weights_path = artifact_state.weights_path;
    if constexpr (requires { s.request; }) {
        destination.preset_name = artifact_state.preset_name;
        destination.resolution = static_cast<decltype(destination.resolution)>(artifact_state.resolution);
    } else {
        s.preset_name = artifact_state.preset_name;
        s.model_resolution = artifact_state.resolution;
    }
    s.model_source = artifact_state.source;
    s.model_input = artifact_state.input;
    if constexpr (requires { destination.onnx_input_path; }) {
        destination.onnx_input_path = artifact_state.onnx_path;
    } else if constexpr (requires { destination.onnx_path; }) {
        destination.onnx_path = artifact_state.onnx_path;
    }
    if constexpr (requires { destination.tensorrt_path; }) { destination.tensorrt_path = artifact_state.tensorrt_path; }
}

// CLEANUP-IGNORE: The view-state reflection inventory is authoritative declarative schema, not executable duplication.
MMLTK_REFLECT_FIELDS(SourceSelectionState)
// CLEANUP-IGNORE: ResolvedVideoCrop remains a separately named reflected settings type.
MMLTK_REFLECT_FIELDS(ResolvedVideoCrop)
MMLTK_REFLECT_FIELDS(WorkflowModelSelectionState)
MMLTK_REFLECT_FIELDS(UiSettingsState)
MMLTK_REFLECT_FIELDS(ModelArtifactSelectionState)
MMLTK_REFLECT_FIELDS(TrainExecutionPaneState)
MMLTK_REFLECT_FIELDS(TrainViewState)
MMLTK_REFLECT_FIELDS(ValidateViewState)
MMLTK_REFLECT_FIELDS(PredictViewState)
MMLTK_REFLECT_FIELDS(AnnotateViewState)
MMLTK_REFLECT_FIELDS(ExportViewState)
MMLTK_REFLECT_FIELDS(ExploreViewState)
MMLTK_REFLECT_ENUM(SourceKind)
MMLTK_REFLECT_ENUM(TrainExecutionTarget)
MMLTK_REFLECT_ENUM(ExploreDatasetSource)
MMLTK_REFLECT_ENUM(ExploreDetailScaleMode)
MMLTK_REFLECT_ENUM(WorkspaceAspectRatio)

struct ExploreSettingsProjection final
    : mmltk::frameworks::reflection::StaticMemberRelation<
          // CLEANUP-IGNORE: Each relation entry identifies a different persisted field and destination; generic
          // projection already owns the repeated machinery.
          ExploreViewState, ExploreFilterUpdate, 11U,
          mmltk::frameworks::reflection::MemberRelationEntry<
              mmltk::frameworks::reflection::member_path<&ExploreViewState::require_boxes>,
              mmltk::frameworks::reflection::member_path<&ExploreFilterUpdate::filter, &ExploreFilter::require_boxes>>,
          mmltk::frameworks::reflection::MemberRelationEntry<
              mmltk::frameworks::reflection::member_path<&ExploreViewState::require_masks>,
              mmltk::frameworks::reflection::member_path<&ExploreFilterUpdate::filter, &ExploreFilter::require_masks>>,
          mmltk::frameworks::reflection::MemberRelationEntry<
              mmltk::frameworks::reflection::member_path<&ExploreViewState::min_instances>,
              mmltk::frameworks::reflection::member_path<&ExploreFilterUpdate::filter, &ExploreFilter::minimum_instances>>,
          mmltk::frameworks::reflection::MemberRelationEntry<
              mmltk::frameworks::reflection::member_path<&ExploreViewState::max_instances>,
              mmltk::frameworks::reflection::member_path<&ExploreFilterUpdate::filter, &ExploreFilter::maximum_instances>>,
          mmltk::frameworks::reflection::MemberRelationEntry<
              mmltk::frameworks::reflection::member_path<&ExploreViewState::min_compiled_index>,
              mmltk::frameworks::reflection::member_path<&ExploreFilterUpdate::filter, &ExploreFilter::minimum_compiled_index>>,
          mmltk::frameworks::reflection::MemberRelationEntry<
              mmltk::frameworks::reflection::member_path<&ExploreViewState::max_compiled_index>,
              mmltk::frameworks::reflection::member_path<&ExploreFilterUpdate::filter, &ExploreFilter::maximum_compiled_index>>,
          mmltk::frameworks::reflection::MemberRelationEntry<
              mmltk::frameworks::reflection::member_path<&ExploreViewState::order>,
              mmltk::frameworks::reflection::member_path<&ExploreFilterUpdate::filter, &ExploreFilter::order>>,
          mmltk::frameworks::reflection::MemberRelationEntry<
              mmltk::frameworks::reflection::member_path<&ExploreViewState::shuffle_seed>,
              mmltk::frameworks::reflection::member_path<&ExploreFilterUpdate::filter, &ExploreFilter::shuffle_seed>>,
          mmltk::frameworks::reflection::MemberRelationEntry<
              mmltk::frameworks::reflection::member_path<&ExploreViewState::show_boxes>,
              mmltk::frameworks::reflection::member_path<&ExploreFilterUpdate::overlay, &ExploreOverlay::show_boxes>>,
          mmltk::frameworks::reflection::MemberRelationEntry<
              mmltk::frameworks::reflection::member_path<&ExploreViewState::show_masks>,
              mmltk::frameworks::reflection::member_path<&ExploreFilterUpdate::overlay, &ExploreOverlay::show_masks>>,
          mmltk::frameworks::reflection::MemberRelationEntry<
              mmltk::frameworks::reflection::member_path<&ExploreViewState::show_labels>,
              mmltk::frameworks::reflection::member_path<&ExploreFilterUpdate::overlay, &ExploreOverlay::show_labels>>> {
    template <class Visitor>
    static constexpr void Visit(Visitor&& visitor) {
        VisitMembers([&]<class Entry>() { visitor.template operator()<Entry::source, Entry::destination>(); });
    }
};

[[nodiscard]] consteval bool explore_settings_projection_is_complete() {
    using namespace mmltk::frameworks::reflection;
    return ExploreSettingsProjection::valid();
}

static_assert(explore_settings_projection_is_complete(),
              "Explore settings projection must cover eleven unique typed source and destination members");

}  // namespace mmltk::controller::contracts
