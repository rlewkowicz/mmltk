#include "gui/gui_settings.h"

#include <cassert>

namespace {

using namespace fastloader::gui;

void test_ui_settings_round_trip() {
    UiSettingsState ui;
    ui.ui_scale = 1.35f;
    ui.font_size = 18.0f;
    ui.secondary_font_size = 15.0f;
    ui.mono_font_size = 14.0f;
    ui.property_label_width = 192.0f;
    ui.density = UiDensity::Comfortable;

    TrainViewState train;
    ValidateViewState validate;
    PredictViewState predict;
    AnnotateViewState annotate;
    ExportViewState export_state;

    GuiSettingsSnapshot snapshot{
        View::Annotate,
        "rf-detr-seg-medium",
        &ui,
        &train,
        &validate,
        &predict,
        &annotate,
        &export_state,
    };

    const nlohmann::json saved = snapshot_gui_settings(snapshot);

    UiSettingsState loaded_ui{};
    TrainViewState loaded_train;
    ValidateViewState loaded_validate;
    PredictViewState loaded_predict;
    AnnotateViewState loaded_annotate;
    ExportViewState loaded_export;
    GuiSettingsSnapshot loaded{
        View::Train,
        {},
        &loaded_ui,
        &loaded_train,
        &loaded_validate,
        &loaded_predict,
        &loaded_annotate,
        &loaded_export,
    };

    apply_gui_settings(saved, loaded);

    assert(loaded.current_view == View::Annotate);
    assert(loaded.selected_preset == "rf-detr-seg-medium");
    assert(loaded_ui.ui_scale == 1.35f);
    assert(loaded_ui.font_size == 18.0f);
    assert(loaded_ui.secondary_font_size == 15.0f);
    assert(loaded_ui.mono_font_size == 14.0f);
    assert(loaded_ui.property_label_width == 192.0f);
    assert(loaded_ui.density == UiDensity::Comfortable);
}

} // namespace

int main() {
    test_ui_settings_round_trip();
    return 0;
}
