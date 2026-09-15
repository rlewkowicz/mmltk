#include "train_run_store.h"
#include <chrono>
#include <format>
#include <limits>
#include <stdexcept>
#include "src/frameworks/serialization/reflected_json.h"

namespace mmltk::controller::services {
namespace r = mmltk::backend::models::rfdetr;
namespace serial = mmltk::frameworks::serialization;
namespace {
constexpr std::size_t manifest_bytes = r::kTrainingManifestBytes;
constexpr std::size_t line_bytes = r::kTrainingRecordBytes;
constexpr std::size_t page_bytes = 2U * r::kTrainingRecordBytes;
constexpr serial::wire::Limits record_limits{.max_bytes = line_bytes, .max_items = 8192, .max_depth = 32};
r::TrainingRun ReadRun(const std::filesystem::path& directory) {
    std::ifstream input(directory / "run.json", std::ios::binary);
    if (!input || !std::filesystem::is_regular_file(directory / "metrics.jsonl"))
        throw std::runtime_error("output directory has no supported current run.json/metrics.jsonl history");
    std::string text(manifest_bytes + 1, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
    const auto run = serial::decode_reflected_json<r::TrainingRun>(
        text, {.max_bytes = manifest_bytes, .max_items = 65536, .max_depth = 32});
    if (run.format_version != r::kTrainingRunFormat || run.run_id.empty() || run.attempt_id.empty() ||
        run.evaluated_weights != (run.configuration.use_ema ? r::EvaluatedWeights::Ema : r::EvaluatedWeights::Ordinary))
        throw std::runtime_error("unsupported or inconsistent training run format");
    return run;
}
}  // namespace
r::TrainingRun TrainRunStore::Open(const std::filesystem::path& directory) {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("training history generation exhausted");
    auto canonical = std::filesystem::canonical(directory);
    auto run = ReadRun(canonical);
    const auto identity = mmltk::common::io::FileSnapshot::Read(canonical / "metrics.jsonl");
    std::ifstream stream(canonical / "metrics.jsonl", std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open current training history");
    const auto after = mmltk::common::io::FileSnapshot::Read(canonical / "metrics.jsonl");
    if (identity.device != after.device || identity.inode != after.inode) throw std::runtime_error("history changed while opening");
    metrics_identity_ = after;
    directory_ = std::move(canonical);
    run_ = run;
    metrics_ = std::move(stream);
    ++generation_;
    line_.clear();
    line_.reserve(line_bytes);
    return run;
}
r::TrainingHistoryPage TrainRunStore::Read(const r::TrainingHistoryQuery& query) {
    if (!run_ || query.generation != generation_) throw std::runtime_error("training history query refers to a stale directory");
    if (!query.count || query.count > r::kTrainingHistoryPageSize) throw std::invalid_argument("invalid training history page size");
    const auto identity = mmltk::common::io::FileSnapshot::Read(directory_ / "metrics.jsonl");
    if (identity.device != metrics_identity_.device || identity.inode != metrics_identity_.inode || identity.bytes < metrics_identity_.bytes)
        throw std::runtime_error("opened history was replaced or truncated");
    metrics_identity_ = identity;
    const auto size = identity.bytes;
    if (query.cursor > size || query.cursor > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        throw std::invalid_argument("training history cursor is outside the stream");
    metrics_.clear();
    if (query.cursor) {
        metrics_.seekg(static_cast<std::streamoff>(query.cursor - 1));
        if (metrics_.get() != '\n') throw std::invalid_argument("training history cursor is not a record boundary");
    } else metrics_.seekg(0);
    r::TrainingHistoryPage page;
    page.generation = generation_;
    page.next_cursor = query.cursor;
    page.records.reserve(query.count);
    std::size_t consumed = 0;
    while (page.records.size() < query.count && consumed < page_bytes) {
        line_.clear();
        bool complete = false;
        char byte{};
        while (metrics_.get(byte)) {
            ++consumed;
            if (byte == '\n') { complete = true; break; }
            if (line_.size() == line_bytes) throw std::runtime_error("training history record exceeds byte limit");
            line_.push_back(byte);
        }
        if (!complete) break; // A partial append is retried from its initial byte.
        if (consumed > page_bytes) break;
        auto record = serial::decode_reflected_json<r::TrainingRecord>(line_, record_limits);
        if (record.format_version != r::kTrainingRunFormat || record.run_id != run_->run_id || record.attempt_id.empty() ||
            record.evaluated_weights != run_->evaluated_weights)
            throw std::runtime_error("training history record belongs to an incompatible run");
        page.next_cursor += line_.size() + 1;
        page.records.push_back(std::move(record));
    }
    page.more = page.next_cursor < size && ((!page.records.empty() && consumed >= page_bytes) || page.records.size() == query.count);
    return page;
}
std::filesystem::path TrainRunStore::ResolveOutput(const std::filesystem::path& selected, const std::optional<r::TrainingCheckpoint>& resume) {
    const auto root = std::filesystem::absolute(selected).lexically_normal();
    if (resume && std::filesystem::exists(root / "run.json") && std::filesystem::exists(root / "metrics.jsonl")) {
        std::optional<r::TrainingRun> run;
        try { run = ReadRun(root); } catch (const std::exception&) {}
        if (run && (std::filesystem::weakly_canonical(resume->path.parent_path()) == std::filesystem::weakly_canonical(root) &&
            resume->path.filename() == "checkpoint.pt" && resume->attempt_id == run->checkpoint_attempt_id &&
            resume->evaluated_weights == run->evaluated_weights && resume->class_layout == run->class_layout)) return root;
        (void)run;
    }
    if (!std::filesystem::exists(root) || std::filesystem::is_empty(root)) {
        std::filesystem::create_directories(root);
        return root;
    }
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    for (unsigned suffix = 0; suffix < 1024; ++suffix) {
        const auto candidate = root / std::format("run-{}-{}", stamp, suffix);
        if (std::filesystem::create_directory(candidate)) return candidate;
    }
    throw std::runtime_error("cannot allocate a fresh training output directory");
}
}  // namespace mmltk::controller::services
