#pragma once
#include <fstream>
#include <optional>
#include "src/common/io/file_digest.h"
#include "src/backend/models/rfdetr/contract/training_metrics.h"
namespace mmltk::controller::services {
// TrainingSystem owns this file reader. A cursor is a byte boundary in the
// selected current-format stream; every query has fixed record and byte limits.
class TrainRunStore final {
public:
 [[nodiscard]] mmltk::backend::models::rfdetr::TrainingOpenedRun Open(const std::filesystem::path&);
 [[nodiscard]] mmltk::backend::models::rfdetr::TrainingHistoryPage Read(const mmltk::backend::models::rfdetr::TrainingHistoryQuery&);
 [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
 [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
 [[nodiscard]] const std::optional<mmltk::backend::models::rfdetr::TrainingRun>& run() const noexcept { return run_; }
 [[nodiscard]] static std::filesystem::path ResolveOutput(const std::filesystem::path&, const std::optional<mmltk::backend::models::rfdetr::TrainingCheckpoint>& resume = {}, bool automatic = false);

private:
 std::filesystem::path directory_;
 std::optional<mmltk::backend::models::rfdetr::TrainingRun> run_;
 std::ifstream metrics_;
 mmltk::common::io::FileSnapshot metrics_identity_;
 std::uint64_t generation_ = 0;
 std::string line_;
};
}  // namespace mmltk::controller::services
