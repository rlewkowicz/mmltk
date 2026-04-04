#pragma once

#include "view_state.h"

#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <thread>

#include <nlohmann/json.hpp>

namespace mmltk::gui {

inline constexpr std::uint32_t kGuiSettingsSchemaVersion = 3U;

[[nodiscard]] nlohmann::json normalize_gui_settings_document(const nlohmann::json& j);

void to_json(nlohmann::json& j, const SourceSelectionState& s);
void from_json(const nlohmann::json& j, SourceSelectionState& s);

void to_json(nlohmann::json& j, const TrainViewState& s);
void from_json(const nlohmann::json& j, TrainViewState& s);

void to_json(nlohmann::json& j, const ValidateViewState& s);
void from_json(const nlohmann::json& j, ValidateViewState& s);

void to_json(nlohmann::json& j, const PredictViewState& s);
void from_json(const nlohmann::json& j, PredictViewState& s);

void to_json(nlohmann::json& j, const UiSettingsState& s);
void from_json(const nlohmann::json& j, UiSettingsState& s);

void to_json(nlohmann::json& j, const AnnotateViewState& s);
void from_json(const nlohmann::json& j, AnnotateViewState& s);

void to_json(nlohmann::json& j, const ExportViewState& s);
void from_json(const nlohmann::json& j, ExportViewState& s);

struct WorkflowSettingsSnapshot {
    TrainViewState* train = nullptr;
    ValidateViewState* validate = nullptr;
    PredictViewState* predict = nullptr;
    AnnotateViewState* annotate = nullptr;
    ExportViewState* export_state = nullptr;
};

struct GuiSettingsSnapshot {
    View current_view = View::Train;
    std::string selected_preset;
    UiSettingsState* ui_settings = nullptr;
    WorkflowSettingsSnapshot workflows{};

    GuiSettingsSnapshot() = default;

    GuiSettingsSnapshot(View current_view_,
                        std::string selected_preset_,
                        UiSettingsState* ui_settings_,
                        const WorkflowSettingsSnapshot& workflows_)
        : current_view(current_view_),
          selected_preset(std::move(selected_preset_)),
          ui_settings(ui_settings_),
          workflows(workflows_) {}

    GuiSettingsSnapshot(View current_view_,
                        std::string selected_preset_,
                        UiSettingsState* ui_settings_,
                        TrainViewState* train_,
                        ValidateViewState* validate_,
                        PredictViewState* predict_,
                        AnnotateViewState* annotate_,
                        ExportViewState* export_state_)
        : GuiSettingsSnapshot(current_view_,
                              std::move(selected_preset_),
                              ui_settings_,
                              WorkflowSettingsSnapshot{train_, validate_, predict_, annotate_, export_state_}) {}
};

nlohmann::json snapshot_gui_settings(const GuiSettingsSnapshot& snap);
void apply_gui_settings(const nlohmann::json& j, GuiSettingsSnapshot& snap);

class GuiSettingsPersistence {
public:
    explicit GuiSettingsPersistence(std::string path);
    ~GuiSettingsPersistence();

    bool load(GuiSettingsSnapshot& snap);
    void notify_frame(const GuiSettingsSnapshot& snap);
    void flush();

private:
    void enqueue_save(nlohmann::json j);
    void writer_main();
    void save_to_disk(const nlohmann::json& j);

    std::string path_;
    nlohmann::json last_saved_;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point last_change_time_;
    std::mutex writer_mutex_;
    std::condition_variable writer_cv_;
    std::optional<nlohmann::json> pending_save_;
    bool save_in_flight_ = false;
    bool writer_should_stop_ = false;
    std::thread writer_thread_;
    static constexpr std::chrono::milliseconds kSaveDelay{200};
};

} // namespace mmltk::gui
