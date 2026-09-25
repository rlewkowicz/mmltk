#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include "src/backend/models/rfdetr/contract/training_metrics.h"
#include "src/frameworks/serialization/reflected_json.h"
#include <exception>
#include <string_view>
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
int main(int argc, char** argv) {
 try {
  if (argc != 5 || std::string_view(argv[1]) != "rfdetr" || std::string_view(argv[2]) != "train" || std::string_view(argv[3]) != "--request-json") return 2;
  const auto request = mmltk::backend::models::rfdetr::decode_train_request_json(argv[4]);
  if (const auto* mode = std::getenv("MMLTK_TRAIN_FIXTURE_PROGRESS")) {
   namespace r = mmltk::backend::models::rfdetr;
   r::TrainingProgressDocument document;
   document.record.run_id = "run"; document.record.attempt_id = "attempt";
   auto& progress = document.record.progress;
   progress.phase = r::TrainingPhase::Train;
   progress.session_id = "session";
   progress.completed_images = 1; progress.total_images = 2;
   progress.checkpoint_path = "checkpoint.pt";
   document.record.sequence = 8;
   document.representative_observation = document.record;
   auto& observation = *document.representative_observation;
   observation.sequence = 3; observation.role = r::TrainingRecordRole::Epoch;
   observation.progress.phase = r::TrainingPhase::EpochComplete;
   observation.progress.artifact.emplace(); observation.progress.artifact->path = "scheduled-ema.pt";
   observation.progress.artifact->weights = r::EvaluatedWeights::Ema;
   observation.evaluated_weights = r::EvaluatedWeights::Ema;
   observation.progress.val.emplace(); observation.progress.val->bbox.ap = .75;
   const auto selected_mode = std::string_view(mode) == "selected-result";
   if (selected_mode) {
    progress.phase = r::TrainingPhase::Completed; progress.scope = r::TrainingRecordScope::SelectedOutput;
    progress.artifact.emplace(); progress.artifact->path = "frozen-selected.pt";
    document.final.emplace(); document.final->selected.emplace(); document.final->selected->artifact = *progress.artifact;
   }
   if (std::string_view(mode) == "remote-fatal") {
    progress.phase = r::TrainingPhase::Error; progress.model_id = 7;
    progress.failure = r::TrainingFailure{"session", 7, 19, "remote optimizer allocation failed"};
    document.record.role = r::TrainingRecordRole::Terminal;
   }
   if (std::string_view(mode) == "degraded") document.persistence = {true, 9, "epoch append failed"};
   std::vector<std::byte> scratch(r::kTrainingProgressDocumentBytes);
   auto json = mmltk::frameworks::serialization::reflected_json(document, scratch, {.max_bytes = scratch.size(), .max_items = 8192, .max_depth = 32});
   if (std::string_view(mode) == "invalid-phase") json["record"]["progress"]["phase"] = std::string(4097, 'x');
   if (std::string_view(mode) == "invalid-observation") json["representative_observation"]["progress"]["artifact"]["path"] = std::string(4097, 'x');
   if (std::string_view(mode) == "future-observation") json["representative_observation"]["sequence"] = 9;
   if (std::string_view(mode) == "invalid-path") json["record"]["progress"]["checkpoint_path"] = std::string(4097, 'x');
   std::ofstream(request.output_dir / (selected_mode ? "results.json" : "progress.json")) << json.dump();
  }
  return std::fputs(request.output_dir.c_str(), stdout) < 0 ? 3 : 0;
 } catch (const std::exception& error) {
  std::fprintf(stderr, "training request fixture: %s\n", error.what());
  return 1;
 }
}
