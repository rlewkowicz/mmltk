#pragma once

#include "browser/host_api_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mmltk::browser {

enum class BrowserFileDialogBinding : std::uint8_t {
    Unknown = 0,
    TrainTrainCompiledPath,
    TrainValCompiledPath,
    TrainTestCompiledPath,
    TrainWeights,
    TrainResumePath,
    TrainOutputDir,
    PredictSingleImage,
    PredictImageFolder,
    PredictWeights,
    PredictOnnx,
    PredictTensorRt,
    PredictOutputPath,
    AnnotateSingleImage,
    AnnotateImageFolder,
    AnnotateWeights,
    AnnotateOnnx,
    AnnotateTensorRt,
    AnnotateOutputDir,
    ValidateCompiledPath,
    ValidateSourceRoot,
    ValidateOnnx,
    ValidateTensorRt,
    ValidateSaveEngine,
    ValidateReportJson,
    ExportWeights,
    ExportOnnx,
    ExportOutputPath,
};

struct BrowserNativeFileDialogContractSpec {
    std::string_view id;
    Workflow workflow;
    FileDialogMode mode;
    std::string_view field;
    std::string_view title;
    std::string_view filter_name;
    std::array<std::string_view, 5U> filter_patterns;
    std::size_t filter_pattern_count;
    BrowserFileDialogBinding binding;
};

inline constexpr std::array<BrowserNativeFileDialogContractSpec, 27U> kBrowserNativeFileDialogs{{
    {"train.dataset.train_compiled_path",
     Workflow::Train,
     FileDialogMode::OpenFile,
     "trainCompiledPath",
     "Choose train compiled dataset",
     "Compiled datasets",
     {"*.mmltk", "*.bin", "", "", ""},
     2U,
     BrowserFileDialogBinding::TrainTrainCompiledPath},
    {"train.dataset.val_compiled_path",
     Workflow::Train,
     FileDialogMode::OpenFile,
     "valCompiledPath",
     "Choose validation compiled dataset",
     "Compiled datasets",
     {"*.mmltk", "*.bin", "", "", ""},
     2U,
     BrowserFileDialogBinding::TrainValCompiledPath},
    {"train.dataset.test_compiled_path",
     Workflow::Train,
     FileDialogMode::OpenFile,
     "testCompiledPath",
     "Choose test compiled dataset",
     "Compiled datasets",
     {"*.mmltk", "*.bin", "", "", ""},
     2U,
     BrowserFileDialogBinding::TrainTestCompiledPath},
    {"train.model.weights",
     Workflow::Train,
     FileDialogMode::OpenFile,
     "weightsPath",
     "Choose training weights",
     "Weights",
     {"*.pt", "*.pth", "*.ckpt", "*.safetensors", ""},
     4U,
     BrowserFileDialogBinding::TrainWeights},
    {"train.training.resume_path",
     Workflow::Train,
     FileDialogMode::OpenFile,
     "resumePath",
     "Choose checkpoint to resume",
     "Checkpoints",
     {"*.pt", "*.pth", "*.ckpt", "*.safetensors", ""},
     4U,
     BrowserFileDialogBinding::TrainResumePath},
    {"train.training.output_dir",
     Workflow::Train,
     FileDialogMode::OpenFolder,
     "outputDir",
     "Choose training output directory",
     "",
     {"", "", "", "", ""},
     0U,
     BrowserFileDialogBinding::TrainOutputDir},
    {"predict.source.single_image",
     Workflow::Predict,
     FileDialogMode::OpenFile,
     "source.singleImagePath",
     "Select source image",
     "Images",
     {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.webp"},
     5U,
     BrowserFileDialogBinding::PredictSingleImage},
    {"predict.source.image_folder",
     Workflow::Predict,
     FileDialogMode::OpenFolder,
     "source.imageDirectory",
     "Select source folder",
     "",
     {"", "", "", "", ""},
     0U,
     BrowserFileDialogBinding::PredictImageFolder},
    {"predict.model.weights",
     Workflow::Predict,
     FileDialogMode::OpenFile,
     "weightsPath",
     "Choose model weights",
     "Weights",
     {"*.pt", "*.pth", "*.ckpt", "*.safetensors", ""},
     4U,
     BrowserFileDialogBinding::PredictWeights},
    {"predict.model.onnx",
     Workflow::Predict,
     FileDialogMode::OpenFile,
     "onnxPath",
     "Choose ONNX model",
     "ONNX",
     {"*.onnx", "", "", "", ""},
     1U,
     BrowserFileDialogBinding::PredictOnnx},
    {"predict.model.tensorrt",
     Workflow::Predict,
     FileDialogMode::OpenFile,
     "tensorrtPath",
     "Choose TensorRT engine",
     "TensorRT",
     {"*.engine", "*.trt", "", "", ""},
     2U,
     BrowserFileDialogBinding::PredictTensorRt},
    {"predict.output_path",
     Workflow::Predict,
     FileDialogMode::SaveFile,
     "outputPath",
     "Choose prediction output path",
     "JSON",
     {"*.json", "", "", "", ""},
     1U,
     BrowserFileDialogBinding::PredictOutputPath},
    {"annotate.source.single_image",
     Workflow::Annotate,
     FileDialogMode::OpenFile,
     "source.singleImagePath",
     "Select annotation image",
     "Images",
     {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.webp"},
     5U,
     BrowserFileDialogBinding::AnnotateSingleImage},
    {"annotate.source.image_folder",
     Workflow::Annotate,
     FileDialogMode::OpenFolder,
     "source.imageDirectory",
     "Select annotation folder",
     "",
     {"", "", "", "", ""},
     0U,
     BrowserFileDialogBinding::AnnotateImageFolder},
    {"annotate.model.weights",
     Workflow::Annotate,
     FileDialogMode::OpenFile,
     "weightsPath",
     "Choose annotate model weights",
     "Weights",
     {"*.pt", "*.pth", "*.ckpt", "*.safetensors", ""},
     4U,
     BrowserFileDialogBinding::AnnotateWeights},
    {"annotate.model.onnx",
     Workflow::Annotate,
     FileDialogMode::OpenFile,
     "onnxPath",
     "Choose annotate ONNX model",
     "ONNX",
     {"*.onnx", "", "", "", ""},
     1U,
     BrowserFileDialogBinding::AnnotateOnnx},
    {"annotate.model.tensorrt",
     Workflow::Annotate,
     FileDialogMode::OpenFile,
     "tensorrtPath",
     "Choose annotate TensorRT engine",
     "TensorRT",
     {"*.engine", "*.trt", "", "", ""},
     2U,
     BrowserFileDialogBinding::AnnotateTensorRt},
    {"annotate.output_dir",
     Workflow::Annotate,
     FileDialogMode::OpenFolder,
     "outputDir",
     "Choose annotation output directory",
     "",
     {"", "", "", "", ""},
     0U,
     BrowserFileDialogBinding::AnnotateOutputDir},
    {"validate.dataset.compiled_path",
     Workflow::Validate,
     FileDialogMode::OpenFile,
     "compiledPath",
     "Choose validation compiled dataset",
     "Compiled datasets",
     {"*.mmltk", "*.bin", "", "", ""},
     2U,
     BrowserFileDialogBinding::ValidateCompiledPath},
    {"validate.dataset.source_dir",
     Workflow::Validate,
     FileDialogMode::OpenFolder,
     "sourceDir",
     "Choose validation source root",
     "",
     {"", "", "", "", ""},
     0U,
     BrowserFileDialogBinding::ValidateSourceRoot},
    {"validate.model.onnx",
     Workflow::Validate,
     FileDialogMode::OpenFile,
     "onnxPath",
     "Choose validation ONNX",
     "ONNX",
     {"*.onnx", "", "", "", ""},
     1U,
     BrowserFileDialogBinding::ValidateOnnx},
    {"validate.model.tensorrt",
     Workflow::Validate,
     FileDialogMode::OpenFile,
     "tensorrtPath",
     "Choose validation TensorRT engine",
     "TensorRT",
     {"*.engine", "*.trt", "", "", ""},
     2U,
     BrowserFileDialogBinding::ValidateTensorRt},
    {"validate.output.save_engine_path",
     Workflow::Validate,
     FileDialogMode::SaveFile,
     "saveEnginePath",
     "Choose validation engine output",
     "TensorRT",
     {"*.engine", "*.trt", "", "", ""},
     2U,
     BrowserFileDialogBinding::ValidateSaveEngine},
    {"validate.report_json_path",
     Workflow::Validate,
     FileDialogMode::SaveFile,
     "reportJsonPath",
     "Choose validation report path",
     "JSON",
     {"*.json", "", "", "", ""},
     1U,
     BrowserFileDialogBinding::ValidateReportJson},
    {"export.model.weights",
     Workflow::Export,
     FileDialogMode::OpenFile,
     "weightsPath",
     "Choose export model weights",
     "Weights",
     {"*.pt", "*.pth", "*.ckpt", "*.safetensors", ""},
     4U,
     BrowserFileDialogBinding::ExportWeights},
    {"export.model.onnx",
     Workflow::Export,
     FileDialogMode::OpenFile,
     "onnxPath",
     "Choose export ONNX model",
     "ONNX",
     {"*.onnx", "", "", "", ""},
     1U,
     BrowserFileDialogBinding::ExportOnnx},
    {"export.output_path",
     Workflow::Export,
     FileDialogMode::SaveFile,
     "outputPath",
     "Choose export output path",
     "TensorRT",
     {"*.engine", "*.trt", "", "", ""},
     2U,
     BrowserFileDialogBinding::ExportOutputPath},
}};

[[nodiscard]] consteval bool browser_file_dialog_contract_is_valid() {
    for (std::size_t i = 0U; i < kBrowserNativeFileDialogs.size(); ++i) {
        const BrowserNativeFileDialogContractSpec& dialog = kBrowserNativeFileDialogs[i];
        if (dialog.id.empty() || dialog.field.empty() || dialog.title.empty() ||
            dialog.binding == BrowserFileDialogBinding::Unknown ||
            dialog.filter_pattern_count > dialog.filter_patterns.size()) {
            return false;
        }
        if (dialog.filter_pattern_count > 0U && dialog.filter_name.empty()) {
            return false;
        }
        for (std::size_t pattern_index = 0U; pattern_index < dialog.filter_pattern_count; ++pattern_index) {
            if (dialog.filter_patterns[pattern_index].empty()) {
                return false;
            }
        }
        for (std::size_t j = i + 1U; j < kBrowserNativeFileDialogs.size(); ++j) {
            if (dialog.id == kBrowserNativeFileDialogs[j].id ||
                dialog.binding == kBrowserNativeFileDialogs[j].binding) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] consteval auto browser_file_dialogs_sorted_by_id() {
    auto dialogs = kBrowserNativeFileDialogs;
    for (std::size_t i = 1U; i < dialogs.size(); ++i) {
        const BrowserNativeFileDialogContractSpec current = dialogs[i];
        std::size_t j = i;
        while (j > 0U && current.id < dialogs[j - 1U].id) {
            dialogs[j] = dialogs[j - 1U];
            --j;
        }
        dialogs[j] = current;
    }
    return dialogs;
}

static_assert(browser_file_dialog_contract_is_valid(), "native browser file-dialog contract is invalid");

inline constexpr auto kBrowserNativeFileDialogsById = browser_file_dialogs_sorted_by_id();

}  // namespace mmltk::browser
