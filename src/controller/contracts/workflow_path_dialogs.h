#pragma once
#include <concepts>
#include <optional>
#include <string_view>
#include <type_traits>
#include "mmltk/frameworks/reflection/member_path.h"
#include "src/controller/contracts/gui_settings_states.h"
#include "src/controller/contracts/workflows.h"
namespace mmltk::controller::contracts {
// Controller-owned dialog metadata for paths declared by backend request types.
struct WorkflowPathDialog final {
    std::string_view title;
    std::string_view filter;
    std::string_view pattern;
    FileDialogMode mode = FileDialogMode::OpenFile;
};
template <auto Path>
[[nodiscard]] consteval std::optional<WorkflowPathDialog> workflow_path_dialog() {
    using mmltk::frameworks::reflection::member_path;
    if constexpr (std::same_as<
                      std::remove_cv_t<decltype(Path)>,
                      std::remove_cv_t<decltype(member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::validate, &ValidateViewState::request,
                                                            &mmltk::backend::models::rfdetr::ValidateRequest::compiled_path>)>>) {
        return WorkflowPathDialog{"Select validation dataset", "Compiled datasets", "*.mmltk *.bin", FileDialogMode::OpenFile};
    }
    if constexpr (std::same_as<std::remove_cv_t<decltype(Path)>,
                               std::remove_cv_t<decltype(member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train, &TrainViewState::request,
                                                                     &mmltk::backend::models::rfdetr::TrainRequest::output_dir>)>>) {
        return WorkflowPathDialog{"Select training output", "Directories", "*", FileDialogMode::OpenFolder};
    }
    if constexpr (std::same_as<std::remove_cv_t<decltype(Path)>,
                               std::remove_cv_t<decltype(member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train, &TrainViewState::request,
                                                                     &mmltk::backend::models::rfdetr::TrainRequest::resume_path>)>>) {
        return WorkflowPathDialog{"Select resume checkpoint", "Checkpoints", "*.pt *.pth *.ckpt", FileDialogMode::OpenFile};
    }
    if constexpr (std::same_as<std::remove_cv_t<decltype(Path)>,
                               std::remove_cv_t<decltype(member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train, &TrainViewState::request,
                                                                     &mmltk::backend::models::rfdetr::TrainRequest::train_compiled_path>)>>) {
        return WorkflowPathDialog{"Select training dataset", "Compiled datasets", "*.mmltk *.bin", FileDialogMode::OpenFile};
    }
    if constexpr (std::same_as<std::remove_cv_t<decltype(Path)>,
                               std::remove_cv_t<decltype(member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train, &TrainViewState::request,
                                                                     &mmltk::backend::models::rfdetr::TrainRequest::val_compiled_path>)>>) {
        return WorkflowPathDialog{"Select validation dataset", "Compiled datasets", "*.mmltk *.bin", FileDialogMode::OpenFile};
    }
    if constexpr (std::same_as<std::remove_cv_t<decltype(Path)>,
                               std::remove_cv_t<decltype(member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train, &TrainViewState::request,
                                                                     &mmltk::backend::models::rfdetr::TrainRequest::test_compiled_path>)>>) {
        return WorkflowPathDialog{"Select test dataset", "Compiled datasets", "*.mmltk *.bin", FileDialogMode::OpenFile};
    }
    if constexpr (std::same_as<std::remove_cv_t<decltype(Path)>,
                               std::remove_cv_t<decltype(member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::train, &TrainViewState::request,
                                                                     &mmltk::backend::models::rfdetr::TrainRequest::class_layout_path>)>> ||
                  std::same_as<
                      std::remove_cv_t<decltype(Path)>,
                      std::remove_cv_t<decltype(member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::validate, &ValidateViewState::request,
                                                            &mmltk::backend::models::rfdetr::ValidateRequest::class_layout_path>)>> ||
                  std::same_as<std::remove_cv_t<decltype(Path)>,
                               std::remove_cv_t<decltype(member_path<&GuiSettingsState::workflows, &WorkflowSettingsState::predict, &PredictViewState::request,
                                                                     &mmltk::backend::models::rfdetr::PredictRequest::class_layout_path>)>>) {
        return WorkflowPathDialog{"Select class layout", "Class descriptors", "*.classes.json *.json", FileDialogMode::OpenFile};
    }
    return std::nullopt;
}
}  // namespace mmltk::controller::contracts
