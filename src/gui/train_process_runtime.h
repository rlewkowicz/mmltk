#pragma once

#include "train_command.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace mmltk::gui {

struct TrainArtifactProgress {
    std::string phase;
    int epoch = -1;
    int total_epochs = 0;
    int completed_batches = 0;
    int total_batches = 0;
    int completed_waves = 0;
    int optimizer_steps = 0;
    int steps_per_epoch = 0;
    int train_lanes = 0;
    double train_loss = 0.0;
    double class_loss = 0.0;
    double box_loss = 0.0;
    double step_loss = 0.0;
    double step_class_loss = 0.0;
    double step_box_loss = 0.0;
    double batches_per_second = 0.0;
    double images_per_second = 0.0;
    double elapsed_seconds = 0.0;
    double val_loss = 0.0;
    double val_bbox_ap = 0.0;
    double val_mask_ap = 0.0;
    std::string checkpoint_path;
};

struct LocalTrainSessionState {
    bool running = false;
    bool stop_requested = false;
    bool force_kill_requested = false;
    std::string label;
    std::string last_summary;
    std::string last_error;
    int pid = -1;
    int process_group_id = -1;
    int exit_code = -1;
    std::string output_tail;
    std::filesystem::path output_dir;
    std::optional<TrainArtifactProgress> progress;
};

class LocalTrainSession {
   public:
    LocalTrainSession();
    ~LocalTrainSession();

    LocalTrainSession(const LocalTrainSession&) = delete;
    LocalTrainSession& operator=(const LocalTrainSession&) = delete;

    void start(const mmltk::rfdetr::TrainRequest& request, const std::filesystem::path& cli_path,
               std::string fallback_preset_name);
    void request_stop(bool force);
    void shutdown();

    [[nodiscard]] LocalTrainSessionState snapshot() const;
    [[nodiscard]] bool running() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::gui
