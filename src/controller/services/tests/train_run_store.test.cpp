#include "src/frameworks/serialization/reflected_json.h"
#include <catch2/matchers/catch_matchers_string.hpp>
#include "src/controller/services/train_run_store.h"
#include "src/backend/models/rfdetr/training/telemetry_writer.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <array>
#include <future>
#include <set>
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
    progress.total_images = 35;
    writer.Submit(progress, r::TrainingRecordRole::Boundary);
    for (int epoch = 0; epoch < 4; ++epoch) {
        progress.epoch = epoch;
        progress.completed_images = 35;
        progress.phase = r::TrainingPhase::EpochComplete;
        progress.scalars.total = 4.0 - epoch;
        writer.Submit(progress, r::TrainingRecordRole::Epoch);
    }
    progress.phase = r::TrainingPhase::Completed;
    writer.Finish(progress, {.history_size = 4});
    writer.Close();
    TrainRunStore store;
    const auto opened = store.Open(temp.path());
    REQUIRE(opened.run.has_value());
    const auto& manifest = *opened.run;
    REQUIRE(manifest.configuration == run.configuration);
    REQUIRE(manifest.attempt_id == writer.attempt_id());
    std::uint64_t cursor = 0;
    std::optional<r::TrainingRecord> last;
    std::ifstream history(temp.path() / "metrics.jsonl");
    REQUIRE(history.good());
    do {
        auto page = store.Read({store.generation(), cursor, 1});
        REQUIRE(page.records.size() <= 1);
        if (page.records.empty()) break;
        REQUIRE(page.next_cursor > cursor);
        cursor = page.next_cursor;
        last = page.records.back();
        std::string line;
        REQUIRE(static_cast<bool>(std::getline(history, line)));
        const auto persisted = mmltk::frameworks::serialization::decode_reflected_json<r::TrainingRecord>(
            line, {.max_bytes = r::kTrainingRecordBytes, .max_items = 8192U, .max_depth = 32U});
        CHECK(*last == persisted);
        if (last->progress.phase == r::TrainingPhase::Starting) CHECK(last->attempt_configuration == run.configuration);
        if (!page.more) break;
    } while (true);
    REQUIRE(last.has_value());
    REQUIRE(last->role == r::TrainingRecordRole::Terminal);
    REQUIRE(last->progress.phase == r::TrainingPhase::Completed);
    REQUIRE(last->attempt_id == manifest.attempt_id);
    REQUIRE(last->format_version == 2);
    REQUIRE(last->progress.completed_images == 35);
    REQUIRE(last->progress.total_images == 35);
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
    {
        std::ofstream oversized(temp.path() / "metrics.jsonl", std::ios::trunc);
        oversized << std::string(r::kTrainingRecordBytes + 1U, ' ') << '\n';
    }
    REQUIRE_THROWS_WITH(store.Read({store.generation(), 0, 1}), Catch::Matchers::ContainsSubstring("training history record exceeds byte limit"));
    const auto fresh = TrainRunStore::ResolveOutput(temp.path());
    REQUIRE(fresh != temp.path());
    REQUIRE(fresh.filename() == "run-0001");
    REQUIRE(std::filesystem::is_empty(fresh));
    r::TrainingCheckpoint continuation;
    continuation.path = temp.path() / "checkpoint.pt";
    continuation.attempt_id = manifest.checkpoint_attempt_id;
    continuation.class_layout = manifest.class_layout;
    continuation.evaluated_weights = manifest.evaluated_weights;
    REQUIRE(TrainRunStore::ResolveOutput(temp.path(), continuation) == std::filesystem::absolute(temp.path()));
    continuation.attempt_id += "-different";
    REQUIRE(TrainRunStore::ResolveOutput(temp.path(), continuation).filename() == "run-0002");
    REQUIRE(TrainRunStore::ResolveOutput(fresh, continuation).filename() == "run-0001");
    const auto old_generation = store.generation();
    REQUIRE_FALSE(store.Open(fresh).run);
    REQUIRE_FALSE(store.run());
    REQUIRE_THROWS(store.Read({old_generation, 0, 1}));
}
}  // namespace
TEST_CASE("test_current_training_history_pages_and_attempt_configuration", "[gui][train][history]") {
    test_current_training_history_pages_and_attempt_configuration();
}
TEST_CASE("saved training format one is explicitly incompatible", "[gui][train][history]") {
    namespace r = mmltk::backend::models::rfdetr;
    mmltk::testsupport::ScopedTempDir temp{"training-old-format"};
    r::TrainingRun run;
    run.format_version = 1;
    run.run_id = "old-run";
    run.attempt_id = "old-attempt";
    std::ofstream(temp.path() / "metrics.jsonl");
    std::vector<std::byte> scratch(r::kTrainingManifestBytes);
    std::ofstream(temp.path() / "run.json") << mmltk::frameworks::serialization::reflected_json(
        run, scratch, {.max_bytes = r::kTrainingManifestBytes, .max_items = 65536, .max_depth = 32});
    TrainRunStore store;
    REQUIRE_THROWS_WITH(store.Open(temp.path()), Catch::Matchers::ContainsSubstring("unsupported or inconsistent training run format"));
}
TEST_CASE("automatic output reserves increasing directories across owners", "[gui][train][history]") {
    mmltk::testsupport::ScopedTempDir temp{"training-output-reservation"};
    const auto root = temp.path() / "output";
    CHECK(TrainRunStore::ResolveOutput(root, {}, true).filename() == "run-0001");
    std::filesystem::create_directory(root / "run-0041");
    std::ofstream(root / "run-0042");
    std::filesystem::create_directory(root / "unrelated");
    CHECK(TrainRunStore::ResolveOutput(root, {}, true).filename() == "run-0043");
    std::array<std::future<std::filesystem::path>, 8> starts;
    for (auto& start : starts) start = std::async(std::launch::async, [&] { return TrainRunStore::ResolveOutput(root, {}, true); });
    std::set<std::filesystem::path> paths;
    for (auto& start : starts) {
        const auto path = start.get();
        CHECK(std::filesystem::is_empty(path));
        CHECK(paths.insert(path).second);
    }
    CHECK(TrainRunStore::ResolveOutput(root, {}, true).filename() == "run-0052");
}
TEST_CASE("output opening distinguishes absent history and corrupt claimed history", "[gui][train][history]") {
    mmltk::testsupport::ScopedTempDir temp{"training-output-open"};
    TrainRunStore store;
    const auto empty = store.Open(temp.path());
    CHECK_FALSE(empty.run);
    std::ofstream(temp.path() / "notes.txt") << "unrelated";
    const auto unrelated = store.Open(temp.path());
    CHECK_FALSE(unrelated.run);
    CHECK(unrelated.generation > empty.generation);
    CHECK_THROWS(store.Read({unrelated.generation, 0, 1}));
    std::ofstream(temp.path() / "run.json") << "{}";
    CHECK_THROWS(store.Open(temp.path()));
}
