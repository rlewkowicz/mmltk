#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include "src/controller/contracts/gui_settings_mutation.h"
#include "src/controller/contracts/view_state.h"
namespace mmltk::controller::contracts {
#define MMLTK_GUI_SETTINGS_STATE_LIST(X) \
    X(SourceSelectionState)              \
    X(TrainViewState)                    \
    X(ValidateViewState)                 \
    X(PredictViewState)                  \
    X(UiSettingsState)                   \
    X(AnnotateViewState)                 \
    X(ExportViewState)                   \
    X(ExploreViewState)
inline constexpr std::uint32_t kGuiSettingsSchemaVersion = 8U;
[[nodiscard]] nlohmann::json normalize_gui_settings_document(const nlohmann::json& j);
#define MMLTK_GUI_SETTINGS_DECLARE_JSON(State)       \
    void to_json(nlohmann::json& j, const State& s); \
    void from_json(const nlohmann::json& j, State& s);
MMLTK_GUI_SETTINGS_STATE_LIST(MMLTK_GUI_SETTINGS_DECLARE_JSON)
#undef MMLTK_GUI_SETTINGS_DECLARE_JSON
#undef MMLTK_GUI_SETTINGS_STATE_LIST
[[nodiscard]] nlohmann::json snapshot_gui_settings(const GuiSettingsState& state);
void apply_gui_settings(const nlohmann::json& j, GuiSettingsState& state);
void to_json(nlohmann::json& json, const GuiSettingsState& state);
void from_json(const nlohmann::json& json, GuiSettingsState& state);
[[nodiscard]] nlohmann::json default_gui_settings_document();
[[nodiscard]] GuiSettingsState load_initial_gui_settings_state(const std::string& path);
// Typed parse and repair classification used by the application-owned
// SettingsStore boundary.
[[nodiscard]] bool load_gui_settings_file(const std::string& path, GuiSettingsState& state, nlohmann::json* normalized_document = nullptr,
                                          bool* repair_required = nullptr);
}  // namespace mmltk::controller::contracts
