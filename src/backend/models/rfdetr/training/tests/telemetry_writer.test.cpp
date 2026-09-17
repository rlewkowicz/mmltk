#include "src/backend/models/rfdetr/training/telemetry_writer.h"
#include "src/frameworks/serialization/reflected_json.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <array>
#include <cerrno>
#include <iterator>
#include <sstream>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace {
namespace r = mmltk::backend::models::rfdetr;
void test_telemetry_persistence_failure_is_nonfatal() {
    mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-failure"};
    r::TrainingRun run;
    run.configuration.output_dir = temp.path() / "missing" / "directory";
    r::TrainingTelemetryWriter writer(run);
    r::TrainingMetricProgress progress;
    progress.phase = r::TrainingPhase::Completed;
    writer.Submit(progress, r::TrainingRecordRole::Epoch);
    writer.Finish(progress, {});
    writer.Close();
    REQUIRE(writer.persistence().degraded);
    REQUIRE_FALSE(writer.persistence().error.empty());
}
void test_telemetry_pressure_preserves_a_terminal_boundary() {
    mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-pressure"};
    r::TrainingRun run;
    run.configuration.output_dir = temp.path();
    r::TrainingTelemetryWriter writer(run);
    r::TrainingMetricProgress progress;
    progress.phase = r::TrainingPhase::Train;
    for (int sample = 0; sample < 4096; ++sample) {
        progress.optimizer_steps = sample;
        writer.Submit(progress, r::TrainingRecordRole::Live);
    }
    progress.phase = r::TrainingPhase::Completed;
    writer.Finish(progress, {});
    writer.Close();
    std::ifstream stream(temp.path() / "metrics.jsonl");
    std::string line;
    std::optional<r::TrainingRecord> previous;
    while (std::getline(stream, line)) {
        auto record = mmltk::frameworks::serialization::decode_reflected_json<r::TrainingRecord>(
            line, {.max_bytes = r::kTrainingRecordBytes, .max_items = 8192, .max_depth = 32});
        if (previous) REQUIRE(record.sequence > previous->sequence);
        previous = std::move(record);
    }
    REQUIRE(previous.has_value());
    REQUIRE(previous->role == r::TrainingRecordRole::Terminal);
    REQUIRE(previous->sequence == 4096);
    REQUIRE(previous->dropped_before == writer.persistence().dropped_records);
}
}  // namespace
TEST_CASE("test_telemetry_persistence_failure_is_nonfatal", "[model][rfdetr][training][telemetry]") { test_telemetry_persistence_failure_is_nonfatal(); }
TEST_CASE("test_telemetry_pressure_preserves_a_terminal_boundary", "[model][rfdetr][training][telemetry]") {
    test_telemetry_pressure_preserves_a_terminal_boundary();
}
namespace {
// The writer's actual Initialize open blocks on this FIFO until Release. No
// sleeps, scheduler assumptions or production-only persistence hooks are used.
class HeldHistory final {
   public:
    explicit HeldHistory(const std::filesystem::path& directory) : path_(directory / "metrics.jsonl") {
        if (::mkfifo(path_.c_str(), 0600) != 0) throw std::runtime_error("create telemetry history FIFO");
        r::TrainingRun run;
        run.configuration.output_dir = directory;
        writer = std::make_unique<r::TrainingTelemetryWriter>(std::move(run));
    }
    ~HeldHistory() { Drain(); }
    void Drain() {
        if (drained_) return;
        const int stream = ::open(path_.c_str(), O_RDWR | O_CLOEXEC);
        if (stream < 0) std::terminate();  // fixture cannot leave its writer blocked
        std::jthread reader([&] {
            std::array<char, 4096> bytes{};
            for (;;) {
                const auto count = ::read(stream, bytes.data(), bytes.size());
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) return;
                for (ssize_t index = 0; index < count; ++index) {
                    if (bytes[index] == '\0') return;
                    history.push_back(bytes[index]);
                }
            }
        });
        writer->Close();
        const char end = '\0';
        while (::write(stream, &end, 1) < 0 && errno == EINTR) {}
        reader.join();
        ::close(stream);
        drained_ = true;
    }
    std::unique_ptr<r::TrainingTelemetryWriter> writer;
    std::string history;

   private:
    std::filesystem::path path_;
    bool drained_ = false;
};
class PersistenceNotice final {
   public:
    explicit PersistenceNotice(const std::filesystem::path& directory) : path_(directory / "stderr.txt") {
        saved_ = ::dup(STDERR_FILENO);
        const int output = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (saved_ < 0 || output < 0 || ::dup2(output, STDERR_FILENO) < 0) std::terminate();
        ::close(output);
    }
    ~PersistenceNotice() {
        ::dup2(saved_, STDERR_FILENO);
        ::close(saved_);
    }
    bool observed() const {
        std::ifstream stream(path_);
        const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>{}};
        return text.find(r::kTrainingPersistenceFailureLine) != std::string::npos;
    }

   private:
    std::filesystem::path path_;
    int saved_ = -1;
};
void test_telemetry_boundary_pressure_reserves_epoch_and_terminal() {
    mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-boundaries"};
    PersistenceNotice notice(temp.path());
    HeldHistory held(temp.path());
    r::TrainingMetricProgress progress;
    progress.phase = r::TrainingPhase::Validate;
    // With persistence held, these cannot drain. Later boundary attempts must
    // reject rather than replacing any earlier accepted non-live observation.
    for (int index = 0; index < 32; ++index) {
        progress.optimizer_steps = index;
        held.writer->Submit(progress, r::TrainingRecordRole::Boundary);
    }
    progress.phase = r::TrainingPhase::EpochComplete;
    progress.full_checkpoint_path = temp.path() / "checkpoint.pt";
    held.writer->Submit(progress, r::TrainingRecordRole::Epoch);
    held.writer->Submit(progress, r::TrainingRecordRole::Boundary);  // still full
    progress.phase = r::TrainingPhase::Completed;
    held.writer->Finish(progress, {});
    held.Drain();
    std::istringstream stream(held.history);
    std::string line;
    std::vector<r::TrainingRecord> records;
    while (std::getline(stream, line))
        records.push_back(mmltk::frameworks::serialization::decode_reflected_json<r::TrainingRecord>(
            line, {.max_bytes = r::kTrainingRecordBytes, .max_items = 8192, .max_depth = 32}));
    REQUIRE(records.size() > 2);
    for (std::size_t index = 1; index < records.size(); ++index) REQUIRE(records[index].sequence > records[index - 1].sequence);
    for (std::size_t index = 0; index + 2 < records.size(); ++index) {
        REQUIRE(records[index].role == r::TrainingRecordRole::Boundary);
        REQUIRE(records[index].sequence == index);
        REQUIRE(records[index].progress.optimizer_steps == static_cast<std::int64_t>(index));
    }
    REQUIRE(records[records.size() - 2].role == r::TrainingRecordRole::Epoch);
    REQUIRE(records[records.size() - 2].sequence == 32);
    REQUIRE(records.back().role == r::TrainingRecordRole::Terminal);
    REQUIRE(records.back().sequence == 34);
    REQUIRE(records.back().progress.full_checkpoint_path == progress.full_checkpoint_path);
    REQUIRE(records.back().dropped_before == 35 - records.size());
    REQUIRE(records.back().dropped_before == held.writer->persistence().dropped_records);
    REQUIRE(held.writer->persistence().degraded);
    REQUIRE(notice.observed());
    REQUIRE(std::filesystem::file_size(temp.path() / "log.txt") > 0);
}
void test_invalid_terminal_has_truthful_degraded_custody_and_notice() {
    mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-invalid-terminal"};
    PersistenceNotice notice(temp.path());
    HeldHistory held(temp.path());
    r::TrainingMetricProgress progress;
    progress.phase = r::TrainingPhase::Completed;
    progress.checkpoint_path = std::string(mmltk::frameworks::reflection::kMaximumPathBytes + 1, 'x');
    progress.full_checkpoint_path = temp.path() / "checkpoint.pt";
    held.writer->Finish(progress, {});
    held.Drain();
    const auto record = mmltk::frameworks::serialization::decode_reflected_json<r::TrainingRecord>(
        held.history, {.max_bytes = r::kTrainingRecordBytes, .max_items = 8192, .max_depth = 32});
    REQUIRE(record.role == r::TrainingRecordRole::Terminal);
    REQUIRE(record.progress.phase == r::TrainingPhase::Error);
    REQUIRE(record.progress.checkpoint_path.empty());
    REQUIRE(record.progress.full_checkpoint_path == progress.full_checkpoint_path);
    REQUIRE(record.dropped_before == 1);
    REQUIRE(held.writer->persistence().degraded);
    REQUIRE(notice.observed());
    REQUIRE_FALSE(std::filesystem::exists(temp.path() / "results.json"));
}
void test_rejected_only_submission_notifies_before_empty_close() {
    mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-empty-close"};
    PersistenceNotice notice(temp.path());
    HeldHistory held(temp.path());
    r::TrainingMetricProgress progress;
    progress.full_checkpoint_path = std::string(mmltk::frameworks::reflection::kMaximumPathBytes + 1, 'x');
    held.writer->Submit(progress, r::TrainingRecordRole::Boundary);
    held.Drain();
    REQUIRE(held.history.empty());
    REQUIRE(held.writer->persistence().dropped_records == 1);
    REQUIRE(held.writer->persistence().degraded);
    REQUIRE(notice.observed());
}
void test_unencodable_terminal_notifies_without_later_submission() {
    mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-unencodable-terminal"};
    PersistenceNotice notice(temp.path());
    HeldHistory held(temp.path());
    r::TrainingMetricProgress progress;
    progress.phase = static_cast<r::TrainingPhase>(255);
    held.writer->Finish(progress, {});
    held.Drain();
    REQUIRE(held.history.empty());
    REQUIRE(held.writer->persistence().degraded);
    REQUIRE(held.writer->persistence().dropped_records == 1);
    REQUIRE(notice.observed());
    REQUIRE_FALSE(std::filesystem::exists(temp.path() / "results.json"));
}
void test_telemetry_distinct_heads_drain_by_sequence() {
    mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-ordered-heads"};
    HeldHistory held(temp.path());
    r::TrainingMetricProgress progress;
    progress.phase = r::TrainingPhase::Train;
    for (const auto role : {r::TrainingRecordRole::Boundary, r::TrainingRecordRole::Epoch, r::TrainingRecordRole::Live, r::TrainingRecordRole::Boundary})
        held.writer->Submit(progress, role);
    progress.phase = r::TrainingPhase::Completed;
    held.writer->Finish(progress, {});
    held.Drain();
    std::istringstream stream(held.history);
    std::string line;
    std::uint64_t sequence = 0;
    while (std::getline(stream, line)) {
        const auto record = mmltk::frameworks::serialization::decode_reflected_json<r::TrainingRecord>(
            line, {.max_bytes = r::kTrainingRecordBytes, .max_items = 8192, .max_depth = 32});
        REQUIRE(record.sequence == sequence++);
        REQUIRE(record.dropped_before == 0);
    }
    REQUIRE(sequence == 5);
    REQUIRE_FALSE(held.writer->persistence().degraded);
}
}  // namespace
TEST_CASE("test_telemetry_boundary_pressure_reserves_epoch_and_terminal", "[model][rfdetr][training][telemetry]") {
    test_telemetry_boundary_pressure_reserves_epoch_and_terminal();
}
TEST_CASE("test_invalid_terminal_has_truthful_degraded_custody_and_notice", "[model][rfdetr][training][telemetry]") {
    test_invalid_terminal_has_truthful_degraded_custody_and_notice();
}
TEST_CASE("test_rejected_only_submission_notifies_before_empty_close", "[model][rfdetr][training][telemetry]") {
    test_rejected_only_submission_notifies_before_empty_close();
}
TEST_CASE("test_unencodable_terminal_notifies_without_later_submission", "[model][rfdetr][training][telemetry]") {
    test_unencodable_terminal_notifies_without_later_submission();
}
TEST_CASE("test_telemetry_distinct_heads_drain_by_sequence", "[model][rfdetr][training][telemetry]") { test_telemetry_distinct_heads_drain_by_sequence(); }
