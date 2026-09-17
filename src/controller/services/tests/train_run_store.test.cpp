#include "src/controller/services/train_run_store.h"
#include "src/backend/models/rfdetr/training/telemetry_writer.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
using mmltk::controller::services::TrainRunStore;
namespace {
void test_current_training_history_pages_and_attempt_configuration() {
    namespace r = mmltk::backend::models::rfdetr;
    mmltk::testsupport::ScopedTempDir temp{"mmltk-training-history"};
    r::TrainingRun run;
    run.configuration.train_compiled_path = temp.path() / "train.bin";
    run.configuration.val_compiled_path = temp.path() / "val.bin";
    run.configuration.weights_path = temp.path() / "weights.pt";
    run.configuration.output_dir = temp.path();
    r::TrainingTelemetryWriter writer(run);
    r::TrainingMetricProgress progress;
    progress.phase = r::TrainingPhase::Starting;
    writer.Submit(progress, r::TrainingRecordRole::Boundary);
    for (int epoch = 0; epoch < 4; ++epoch) {
        progress.epoch = epoch;
        progress.phase = r::TrainingPhase::EpochComplete;
        progress.scalars.total = 4.0 - epoch;
        writer.Submit(progress, r::TrainingRecordRole::Epoch);
    }
    progress.phase = r::TrainingPhase::Completed;
    writer.Finish(progress, {.history_size = 4});
    writer.Close();
    TrainRunStore store;
    const auto manifest = store.Open(temp.path());
    REQUIRE(manifest.configuration == run.configuration);
    REQUIRE(manifest.attempt_id == writer.attempt_id());
    std::uint64_t cursor = 0;
    std::optional<r::TrainingRecord> last;
    do {
        auto page = store.Read({store.generation(), cursor, 1});
        REQUIRE(page.records.size() <= 1);
        if (page.records.empty()) break;
        REQUIRE(page.next_cursor > cursor);
        cursor = page.next_cursor;
        last = page.records.back();
        if (!page.more) break;
    } while (true);
    REQUIRE(last.has_value());
    REQUIRE(last->role == r::TrainingRecordRole::Terminal);
    REQUIRE(last->progress.phase == r::TrainingPhase::Completed);
    REQUIRE(last->attempt_id == manifest.attempt_id);
    REQUIRE_THROWS(store.Read({store.generation() + 1, 0, 1}));
    REQUIRE_THROWS(store.Read({store.generation(), 1, 1}));
    REQUIRE_THROWS(store.Read({store.generation(), 0, 0}));
    {
        std::ofstream partial(temp.path() / "metrics.jsonl", std::ios::app);
        partial << "{\"format_version\":";
    }
    const auto partial = store.Read({store.generation(), cursor, 1});
    REQUIRE(partial.records.empty());
    REQUIRE(partial.next_cursor == cursor);
    {
        std::ofstream malformed(temp.path() / "metrics.jsonl", std::ios::app);
        malformed << "999}\n";
    }
    REQUIRE_THROWS(store.Read({store.generation(), cursor, 1}));
    const auto fresh = TrainRunStore::ResolveOutput(temp.path());
    REQUIRE(fresh != temp.path());
    REQUIRE(std::filesystem::is_empty(fresh));
}
}  // namespace
TEST_CASE("test_current_training_history_pages_and_attempt_configuration", "[gui][train][history]") {
    test_current_training_history_pages_and_attempt_configuration();
}
