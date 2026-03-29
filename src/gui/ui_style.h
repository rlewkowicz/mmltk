#pragma once

#include "view_state.h"

struct ImFont;

namespace fastloader::gui {

struct UiFontSet {
    ImFont* primary = nullptr;
    ImFont* compact = nullptr;
    ImFont* mono = nullptr;
};

void apply_ui_settings(const UiSettingsState& settings, bool rebuild_font_texture);
const UiSettingsState& current_ui_settings();
const UiFontSet& current_ui_fonts();
float ui_scaled(float value);
float ui_label_width();

} // namespace fastloader::gui
