#pragma once

#include "view_state.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mmltk::gui {

enum class ShellSlot : std::uint8_t {
    Workspace = 0,
    Sidebar = 1,
};

struct ShellColumnSpec {
    const char* id;
    const char* child_id;
    ShellSlot slot;
    float stretch = 1.0f;
};

struct ShellLayoutView {
    const char* table_id;
    const ShellColumnSpec* columns;
    std::size_t column_count;
};

template <std::size_t ColumnCount>
struct ViewShellLayout {
    const char* table_id;
    std::array<ShellColumnSpec, ColumnCount> columns;

    [[nodiscard]] constexpr ShellLayoutView view() const noexcept {
        return ShellLayoutView{table_id, columns.data(), ColumnCount};
    }
};

[[nodiscard]] constexpr ShellColumnSpec shell_column(const char* id,
                                                     const char* child_id,
                                                     ShellSlot slot,
                                                     float stretch) noexcept {
    return ShellColumnSpec{id, child_id, slot, stretch};
}

template <typename... Columns>
[[nodiscard]] constexpr auto make_view_shell_layout(const char* table_id,
                                                    Columns... columns) noexcept {
    static_assert(sizeof...(Columns) > 0, "Shell layouts require at least one column");
    return ViewShellLayout<sizeof...(Columns)>{table_id, {columns...}};
}

[[nodiscard]] constexpr auto make_workspace_sidebar_shell_layout(
    const char* table_id,
    const char* workspace_child_id,
    const char* sidebar_child_id,
    float workspace_stretch = 3.0f,
    float sidebar_stretch = 1.0f) noexcept {
    return make_view_shell_layout(table_id,
                                  shell_column("workspace",
                                               workspace_child_id,
                                               ShellSlot::Workspace,
                                               workspace_stretch),
                                  shell_column("sidebar",
                                               sidebar_child_id,
                                               ShellSlot::Sidebar,
                                               sidebar_stretch));
}

inline constexpr auto kTrainShellLayout =
    make_workspace_sidebar_shell_layout("train_shell",
                                        "train_workspace_pane",
                                        "train_sidebar_pane");
inline constexpr auto kValidateShellLayout =
    make_workspace_sidebar_shell_layout("validate_shell",
                                        "validate_workspace_pane",
                                        "validate_sidebar_pane");
inline constexpr auto kPredictShellLayout =
    make_workspace_sidebar_shell_layout("predict_shell",
                                        "predict_workspace_pane",
                                        "predict_sidebar_pane");
inline constexpr auto kAnnotateShellLayout =
    make_workspace_sidebar_shell_layout("annotate_shell",
                                        "annotate_workspace_pane",
                                        "annotate_sidebar_pane");
inline constexpr auto kExportShellLayout =
    make_workspace_sidebar_shell_layout("export_shell",
                                        "export_workspace_pane",
                                        "export_sidebar_pane");
inline constexpr auto kLiveShellLayout =
    make_workspace_sidebar_shell_layout("live_shell",
                                        "live_workspace_pane",
                                        "live_sidebar_pane");

[[nodiscard]] inline constexpr ShellLayoutView shell_layout_for_view(View view) noexcept {
    switch (view) {
    case View::Train:
        return kTrainShellLayout.view();
    case View::Validate:
        return kValidateShellLayout.view();
    case View::Predict:
        return kPredictShellLayout.view();
    case View::Annotate:
        return kAnnotateShellLayout.view();
    case View::Export:
        return kExportShellLayout.view();
    case View::Live:
        return kLiveShellLayout.view();
    }
    return kTrainShellLayout.view();
}

} // namespace mmltk::gui