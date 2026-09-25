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
   document.record.run_id = "session"; document.record.attempt_id = "attempt";
   auto& progress = document.record.progress;
   progress.phase = r::TrainingPhase::Train;
   progress.session_id = "session";
   progress.completed_images = 1; progress.total_images = 2;
   progress.checkpoint_path = "checkpoint.pt";
   document.record.sequence = 8;
   auto source_request = request;
   document.sources.catalog = r::training_source_catalog(source_request);
   document.sources.observations.push_back(document.record);
   auto& observation = document.sources.observations.back();
   observation.sequence = 3; observation.role = r::TrainingRecordRole::Epoch;
   observation.progress.phase = r::TrainingPhase::EpochComplete;
   observation.progress.artifact.emplace(); observation.progress.artifact->session_id = "session"; observation.progress.artifact->path = "scheduled-ema.pt";
   observation.progress.artifact->weights = request.use_ema ? r::EvaluatedWeights::Ema : r::EvaluatedWeights::Ordinary;
   observation.evaluated_weights = observation.progress.artifact->weights;
   observation.progress.val.emplace(); observation.progress.val->bbox.ap = .75;
   const auto selected_mode = std::string_view(mode) == "selected-result" || std::string_view(mode) == "premature-selected" ||
                             std::string_view(mode) == "missing-selected-validation" || std::string_view(mode) == "mismatched-selected-validation";
   if (selected_mode) {
    document.record.role = r::TrainingRecordRole::Terminal;
    progress.phase = r::TrainingPhase::Completed; progress.scope = r::TrainingRecordScope::SelectedOutput;
    progress.artifact.emplace(); progress.artifact->path = "frozen-selected.pt";
    auto& artifact = *progress.artifact;
    artifact.session_id = progress.session_id;
    artifact.initialization = artifact.configuration = artifact.content = artifact.sha256 = artifact.validation = std::string(64, 'a');
    artifact.selection_metric = .75; artifact.evaluation = observation.progress.val;
    document.final.emplace(); document.final->selected.emplace();
    auto& selected = *document.final->selected;
    selected.artifact = artifact; selected.validation = *artifact.evaluation;
    selected.best_individual_metric = .75; selected.ingredients.push_back({0, artifact.sha256, 1});
    document.sources.selected = selected;
    progress.val = selected.validation;
   }
   if (std::string_view(mode) == "remote-fatal") {
    progress.phase = r::TrainingPhase::Error; progress.model_id = 7;
    progress.failure = r::TrainingFailure{"session", 7, 19, "remote optimizer allocation failed"};
    document.record.role = r::TrainingRecordRole::Terminal;
   }
   if (std::string_view(mode) == "degraded") document.persistence = {true, 9, "epoch append failed"};
   if (std::string_view(mode) == "premature-selected") {
    document.record.role = r::TrainingRecordRole::Live;
    progress.phase = r::TrainingPhase::Train;
    document.final.reset();
   }
   if (std::string_view(mode) == "missing-selected-validation") progress.val.reset();
   if (std::string_view(mode) == "mismatched-selected-validation") progress.val->bbox.ap = .25;
   std::vector<std::byte> scratch(r::kTrainingProgressDocumentBytes);
   auto json = mmltk::frameworks::serialization::reflected_json(document, scratch, {.max_bytes = scratch.size(), .max_items = 8192, .max_depth = 32});
   if (std::string_view(mode) == "invalid-phase") json["record"]["progress"]["phase"] = std::string(4097, 'x');
   if (std::string_view(mode) == "invalid-observation") json["sources"]["observations"][0]["progress"]["artifact"]["path"] = std::string(4097, 'x');
   if (std::string_view(mode) == "future-observation") json["sources"]["observations"][0]["sequence"] = 9;
   if (std::string_view(mode) == "foreign-observation") json["sources"]["observations"][0]["attempt_id"] = "foreign";
   if (std::string_view(mode) == "duplicate-observation") json["sources"]["observations"].push_back(json["sources"]["observations"][0]);
   if (std::string_view(mode) == "inconsistent-observation") json["sources"]["observations"][0]["progress"]["artifact"]["session_id"] = "foreign";
   if (std::string_view(mode) == "foreign-current-session") {
    json["record"]["run_id"] = "foreign";
    for (auto& retained : json["sources"]["observations"]) retained["run_id"] = "foreign";
   }
   if (std::string_view(mode) == "invalid-path") json["record"]["progress"]["checkpoint_path"] = std::string(4097, 'x');
   std::ofstream(request.output_dir / (selected_mode ? "results.json" : "progress.json")) << json.dump();
  }
  return std::fputs(request.output_dir.c_str(), stdout) < 0 ? 3 : 0;
 } catch (const std::exception& error) {
  std::fprintf(stderr, "training request fixture: %s\n", error.what());
  return 1;
 }
}
