#include "ui_style.h"

#include <backends/imgui_impl_opengl3.h>
#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace fastloader::gui {

namespace {

UiSettingsState g_ui_settings{};
UiFontSet g_ui_fonts{};

float density_multiplier(const UiDensity mode) {
    switch (mode) {
    case UiDensity::Compact:
        return 0.88f;
    case UiDensity::Balanced:
        return 1.0f;
    case UiDensity::Comfortable:
        return 1.15f;
    }
    return 1.0f;
}

UiSettingsState sanitize_ui_settings(UiSettingsState settings) {
    settings.ui_scale = std::clamp(settings.ui_scale, 0.85f, 1.75f);
    settings.font_size = std::clamp(settings.font_size, 13.0f, 28.0f);
    settings.secondary_font_size = std::clamp(settings.secondary_font_size, 12.0f, 24.0f);
    settings.mono_font_size = std::clamp(settings.mono_font_size, 12.0f, 24.0f);
    settings.property_label_width = std::clamp(settings.property_label_width, 110.0f, 260.0f);
    return settings;
}

std::filesystem::path asset_font_path(const char* filename) {
#ifdef FASTLOADER_GUI_ASSET_ROOT
    return std::filesystem::path(FASTLOADER_GUI_ASSET_ROOT) / "fonts" / filename;
#else
    return std::filesystem::path("src/gui/res/fonts") / filename;
#endif
}

ImFont* add_font_or_throw(ImFontAtlas* atlas,
                          const std::filesystem::path& path,
                          float size_pixels,
                          const ImFontConfig& config) {
    if (atlas == nullptr) {
        throw std::runtime_error("ImGui font atlas is not initialized");
    }
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("missing GUI font asset: " + path.string());
    }
    ImFontConfig local_config = config;
    ImFont* font = atlas->AddFontFromFileTTF(path.c_str(), size_pixels, &local_config);
    if (font == nullptr) {
        throw std::runtime_error("failed to load GUI font: " + path.string());
    }
    return font;
}

void load_fonts(const UiSettingsState& settings) {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    if (io.Fonts == nullptr) {
        throw std::runtime_error("ImGui IO font atlas is missing");
    }

    io.Fonts->Clear();

    ImFontConfig primary_config;
    primary_config.OversampleH = 2;
    primary_config.OversampleV = 2;
    primary_config.RasterizerMultiply = 1.05f;

    ImFontConfig compact_config = primary_config;
    compact_config.RasterizerMultiply = 1.0f;

    ImFontConfig mono_config = primary_config;
    mono_config.RasterizerMultiply = 1.0f;

    const float scale = settings.ui_scale;
    g_ui_fonts.primary =
        add_font_or_throw(io.Fonts,
                          asset_font_path("SourceSansPro-Regular.otf"),
                          settings.font_size * scale,
                          primary_config);
    g_ui_fonts.compact =
        add_font_or_throw(io.Fonts,
                          asset_font_path("SourceSansPro-Regular.otf"),
                          settings.secondary_font_size * scale,
                          compact_config);
    g_ui_fonts.mono =
        add_font_or_throw(io.Fonts,
                          asset_font_path("SourceCodePro-Semibold.otf"),
                          settings.mono_font_size * scale,
                          mono_config);

    io.FontDefault = g_ui_fonts.primary;
    io.Fonts->Build();
}

void apply_palette(const UiSettingsState& settings) {
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);

    const float metric_scale = settings.ui_scale * density_multiplier(settings.density);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.WindowPadding = ImVec2(8.0f * metric_scale, 8.0f * metric_scale);
    style.FramePadding = ImVec2(6.0f * metric_scale, 5.0f * metric_scale);
    style.CellPadding = ImVec2(4.0f * metric_scale, 3.0f * metric_scale);
    style.ItemSpacing = ImVec2(6.0f * metric_scale, 5.0f * metric_scale);
    style.ItemInnerSpacing = ImVec2(5.0f * metric_scale, 4.0f * metric_scale);
    style.IndentSpacing = 14.0f * metric_scale;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;

    style.WindowRounding = 3.0f * metric_scale;
    style.ChildRounding = 3.0f * metric_scale;
    style.FrameRounding = 3.0f * metric_scale;
    style.PopupRounding = 3.0f * metric_scale;
    style.ScrollbarRounding = 3.0f * metric_scale;
    style.GrabRounding = 3.0f * metric_scale;
    style.TabRounding = 3.0f * metric_scale;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.94f, 0.95f, 0.97f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.66f, 0.71f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.13f, 0.13f, 0.14f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.09f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.21f, 0.22f, 0.24f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.18f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.22f, 0.25f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.13f, 0.14f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.24f, 0.28f, 0.95f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.30f, 0.36f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.18f, 0.24f, 0.29f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.24f, 0.29f, 0.96f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.31f, 0.37f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.16f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.94f, 0.73f, 0.37f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.94f, 0.73f, 0.37f, 0.92f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.99f, 0.79f, 0.43f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.23f, 0.25f, 0.28f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.21f, 0.22f, 0.24f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.18f, 0.19f, 0.21f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.19f, 0.22f, 0.25f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.25f, 0.29f, 0.33f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.28f, 0.33f, 0.37f, 1.00f);

    ImGui::GetStyle() = style;
}

} // namespace

void apply_ui_settings(const UiSettingsState& settings, const bool rebuild_font_texture) {
    g_ui_settings = sanitize_ui_settings(settings);
    load_fonts(g_ui_settings);
    apply_palette(g_ui_settings);
    if (rebuild_font_texture) {
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        (void)ImGui_ImplOpenGL3_CreateFontsTexture();
    }
}

const UiSettingsState& current_ui_settings() {
    return g_ui_settings;
}

const UiFontSet& current_ui_fonts() {
    return g_ui_fonts;
}

float ui_scaled(const float value) {
    return value * g_ui_settings.ui_scale;
}

float ui_label_width() {
    return g_ui_settings.property_label_width * g_ui_settings.ui_scale;
}

} // namespace fastloader::gui
