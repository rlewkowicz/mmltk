#pragma once

#include "ui_controls.h"

#include <array>
#include <cstddef>
#include <functional>
#include <utility>

namespace mmltk::gui {

struct GridColumnSpec {
    const char* id;
    float stretch = 1.0f;
    ImGuiTableColumnFlags flags = ImGuiTableColumnFlags_WidthStretch;
};

struct GridLayoutView {
    const char* table_id;
    const GridColumnSpec* columns;
    std::size_t column_count;
};

template <std::size_t ColumnCount>
struct GridLayout {
    const char* table_id;
    std::array<GridColumnSpec, ColumnCount> columns;

    [[nodiscard]] constexpr GridLayoutView view() const noexcept {
        return GridLayoutView{table_id, columns.data(), ColumnCount};
    }
};

[[nodiscard]] constexpr GridColumnSpec grid_column(
    const char* id,
    float stretch = 1.0f,
    ImGuiTableColumnFlags flags = ImGuiTableColumnFlags_WidthStretch) noexcept {
    return GridColumnSpec{id, stretch, flags};
}

template <typename... Columns>
[[nodiscard]] constexpr auto make_grid_layout(const char* table_id,
                                              Columns... columns) noexcept {
    static_assert(sizeof...(Columns) > 0, "Grid layouts require at least one column");
    return GridLayout<sizeof...(Columns)>{table_id, {columns...}};
}

[[nodiscard]] constexpr auto make_two_column_grid_layout(
    const char* table_id,
    const char* primary_column_id,
    const char* secondary_column_id,
    float primary_stretch = 1.05f,
    float secondary_stretch = 0.95f) noexcept {
    return make_grid_layout(table_id,
                            grid_column(primary_column_id, primary_stretch),
                            grid_column(secondary_column_id, secondary_stretch));
}

template <typename DrawCellFn>
void draw_grid(const GridLayoutView& layout,
               const ImVec2& size,
               const ImGuiTableFlags flags,
               DrawCellFn&& draw_cell_fn) {
    if (!ImGui::BeginTable(layout.table_id,
                           static_cast<int>(layout.column_count),
                           flags,
                           size)) {
        return;
    }

    for (std::size_t column_index = 0; column_index < layout.column_count;
         ++column_index) {
        const GridColumnSpec& column = layout.columns[column_index];
        ImGui::TableSetupColumn(column.id, column.flags, column.stretch);
    }

    if (size.y > 0.0f) {
        ImGui::TableNextRow(ImGuiTableRowFlags_None, size.y);
    } else {
        ImGui::TableNextRow();
    }

    for (std::size_t column_index = 0; column_index < layout.column_count;
         ++column_index) {
        ImGui::TableSetColumnIndex(static_cast<int>(column_index));
        std::forward<DrawCellFn>(draw_cell_fn)(layout.columns[column_index],
                                               column_index);
    }

    ImGui::EndTable();
}

template <std::size_t ColumnCount, typename DrawCellFn>
void draw_grid(const GridLayout<ColumnCount>& layout,
               const ImVec2& size,
               const ImGuiTableFlags flags,
               DrawCellFn&& draw_cell_fn) {
    draw_grid(layout.view(), size, flags, std::forward<DrawCellFn>(draw_cell_fn));
}

template <typename... DrawFns>
void draw_column(const char* id, DrawFns&&... draw_fns);

template <typename DrawPrimaryFn, typename DrawSecondaryFn>
void draw_two_column_grid(
    const GridLayoutView& layout,
    const char* primary_column_id,
    DrawPrimaryFn&& draw_primary_fn,
    const char* secondary_column_id,
    DrawSecondaryFn&& draw_secondary_fn,
    const ImVec2& size = ImVec2(0.0f, 0.0f),
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_NoSavedSettings) {
    draw_grid(layout, size, flags, [&](const GridColumnSpec&, const std::size_t column_index) {
        if (column_index == 0) {
            draw_column(primary_column_id, [&]() {
                std::invoke(draw_primary_fn);
            });
            return;
        }

        draw_column(secondary_column_id, [&]() {
            std::invoke(draw_secondary_fn);
        });
    });
}

template <std::size_t ColumnCount, typename DrawPrimaryFn, typename DrawSecondaryFn>
void draw_two_column_grid(
    const GridLayout<ColumnCount>& layout,
    const char* primary_column_id,
    DrawPrimaryFn&& draw_primary_fn,
    const char* secondary_column_id,
    DrawSecondaryFn&& draw_secondary_fn,
    const ImVec2& size = ImVec2(0.0f, 0.0f),
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_NoSavedSettings) {
    draw_two_column_grid(layout.view(),
                         primary_column_id,
                         std::forward<DrawPrimaryFn>(draw_primary_fn),
                         secondary_column_id,
                         std::forward<DrawSecondaryFn>(draw_secondary_fn),
                         size,
                         flags);
}

namespace detail {

template <typename DrawFn>
void draw_column_items(DrawFn&& draw_fn) {
    std::forward<DrawFn>(draw_fn)();
}

template <typename DrawFn, typename... Rest>
void draw_column_items(DrawFn&& draw_fn, Rest&&... rest) {
    std::forward<DrawFn>(draw_fn)();
    draw_column_items(std::forward<Rest>(rest)...);
}

template <typename DrawFn>
void draw_row_items(DrawFn&& draw_fn) {
    std::forward<DrawFn>(draw_fn)();
}

template <typename DrawFn, typename... Rest>
void draw_row_items(DrawFn&& draw_fn, Rest&&... rest) {
    std::forward<DrawFn>(draw_fn)();
    ImGui::SameLine();
    draw_row_items(std::forward<Rest>(rest)...);
}

} // namespace detail

template <typename DrawFn>
void draw_section_tile(const char* id, const char* heading, DrawFn&& draw_fn) {
    ImGui::PushID(id);
    if (heading != nullptr && heading[0] != '\0') {
        draw_section_heading(heading);
    }
    std::forward<DrawFn>(draw_fn)();
    ImGui::PopID();
}

template <typename... DrawFns>
void draw_row(const char* id, DrawFns&&... draw_fns) {
    ImGui::PushID(id);
    detail::draw_row_items(std::forward<DrawFns>(draw_fns)...);
    ImGui::PopID();
}

template <typename... DrawFns>
void draw_column(const char* id, DrawFns&&... draw_fns) {
    ImGui::PushID(id);
    detail::draw_column_items(std::forward<DrawFns>(draw_fns)...);
    ImGui::PopID();
}

template <typename DrawFn>
void draw_property_sheet_tile(const char* id,
                              const char* heading,
                              DrawFn&& draw_fn) {
    draw_section_tile(id, heading, std::forward<DrawFn>(draw_fn));
}

template <typename DrawFn>
void draw_console_tile(const char* id, const char* heading, DrawFn&& draw_fn) {
    draw_section_tile(id, heading, std::forward<DrawFn>(draw_fn));
}

template <typename DrawFn>
void draw_banner_tile(const char* id, const char* heading, DrawFn&& draw_fn) {
    draw_section_tile(id, heading, std::forward<DrawFn>(draw_fn));
}

template <typename DrawFn>
void draw_preview_tile(const char* id,
                       const char* heading,
                       const ImVec2& size,
                       DrawFn&& draw_fn,
                       const ImGuiWindowFlags child_flags =
                           ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoScrollWithMouse) {
    draw_section_tile(id, heading, [&]() {
        ImGui::BeginChild("preview", size, true, child_flags);
        std::forward<DrawFn>(draw_fn)();
        ImGui::EndChild();
    });
}

template <typename DrawTabsFn>
void draw_tab_tile(const char* id,
                   const char* heading,
                   DrawTabsFn&& draw_tabs_fn,
                   const ImGuiTabBarFlags flags = ImGuiTabBarFlags_None) {
    draw_section_tile(id, heading, [&]() {
        if (ImGui::BeginTabBar("tabs", flags)) {
            std::forward<DrawTabsFn>(draw_tabs_fn)();
            ImGui::EndTabBar();
        }
    });
}

template <typename DrawFn>
bool draw_tab_item(const char* label, DrawFn&& draw_fn) {
    if (!ImGui::BeginTabItem(label)) {
        return false;
    }
    std::forward<DrawFn>(draw_fn)();
    ImGui::EndTabItem();
    return true;
}

template <typename DrawFn>
void draw_toolbar_tile(const char* id,
                       const char* heading,
                       DrawFn&& draw_fn,
                       const ImVec2& item_spacing = ImVec2(8.0f, 8.0f)) {
    draw_section_tile(id, heading, [&]() {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ui_scaled(item_spacing.x),
                                   ui_scaled(item_spacing.y)));
        std::forward<DrawFn>(draw_fn)();
        ImGui::PopStyleVar();
    });
}

} // namespace mmltk::gui