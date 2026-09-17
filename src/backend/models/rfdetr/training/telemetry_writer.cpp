#include "telemetry_writer.h"
#include <cerrno>
#include <atomic>
#include <fstream>
#include <format>
#include <inplace_vector>
#include <mutex>
#include <random>
#include <thread>
#include <unistd.h>
#include "src/common/io/json_file.h"
#include "src/frameworks/serialization/reflected_json.h"
namespace mmltk::backend::models::rfdetr {
namespace serial = mmltk::frameworks::serialization;
namespace {
constexpr std::size_t record_bytes = kTrainingRecordBytes;
constexpr serial::wire::Limits manifest_limits{.max_bytes = kTrainingManifestBytes, .max_items = 65536, .max_depth = 32};
constexpr serial::wire::Limits limits{.max_bytes = record_bytes, .max_items = 8192, .max_depth = 32};
constexpr std::size_t queue_bytes = 8U * 1024U * 1024U;
constexpr std::size_t epoch_capacity = 2;
constexpr std::size_t boundary_capacity = queue_bytes / record_bytes - epoch_capacity - 2;
// Boundary traffic cannot consume epoch or terminal reservations. Only Live
// records coalesce; every other accepted record retains independent custody.
bool bounded_paths(const TrainingMetricProgress& progress) {
    return progress.checkpoint_path.native().size() <= mmltk::frameworks::reflection::kMaximumPathBytes &&
           progress.full_checkpoint_path.native().size() <= mmltk::frameworks::reflection::kMaximumPathBytes;
}
std::string identity() {
    std::random_device random;
    return std::format("{:08x}{:08x}{:08x}{:08x}", random(), random(), random(), random());
}
std::string read_manifest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read current training manifest");
    std::string text(kTrainingManifestBytes + 1, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
    if (text.size() > kTrainingManifestBytes) throw std::runtime_error("training manifest exceeds byte limit");
    return text;
}
void write_file(const std::filesystem::path& path, const nlohmann::json& value) {
    mmltk::common::io::throw_on_json_write_failure(mmltk::common::io::write_json_file_atomic(path, value), path, "training telemetry");
}
void append_file(const std::filesystem::path& path, const nlohmann::json& value) {
    mmltk::common::io::throw_on_json_write_failure(mmltk::common::io::append_json_line(path, value), path, "training telemetry");
}
std::string_view legacy_phase(TrainingPhase phase) {
    switch (phase) {
        case TrainingPhase::Starting: return "starting";
        case TrainingPhase::Train: return "train";
        case TrainingPhase::Validate: return "validate";
        case TrainingPhase::EpochComplete: return "epoch_complete";
        case TrainingPhase::Completed: return "completed";
        case TrainingPhase::Error: return "error";
    }
    throw std::invalid_argument("invalid training phase");
}
}  // namespace
struct TrainingTelemetryWriter::Impl final {
    explicit Impl(TrainingRun value) : run(std::move(value)) {
        run.attempt_id = identity();
        worker = std::jthread([this] { Work(); });
    }
    TrainingRun run;
    std::mutex mutex;
    std::atomic<std::uint64_t> wake_generation{0};
    std::optional<TrainingRecord> live;
    std::inplace_vector<TrainingRecord, boundary_capacity> boundaries;
    std::inplace_vector<TrainingRecord, epoch_capacity> epochs;
    std::optional<TrainingRecord> terminal;
    std::optional<TrainingFinalFacts> final;
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<bool> degraded{false};
    std::atomic<bool> stopping{false};
    std::string error;
    std::array<std::byte, record_bytes> scratch{};
    std::vector<std::byte> manifest_scratch = std::vector<std::byte>(kTrainingManifestBytes);
    bool reported_failure = false;
    const int uncaught = std::uncaught_exceptions();
    TrainingMetricProgress last_progress;
    bool finished = false;
    std::jthread worker;
    void Wake() noexcept {
        wake_generation.fetch_add(1);
        wake_generation.notify_one();
    }
    void Drop() noexcept {
        dropped.fetch_add(1);
        degraded.store(true);
        Wake();
    }
    void Complete(TrainingMetricProgress progress, std::optional<TrainingFinalFacts> facts) noexcept {
        const auto next = sequence.fetch_add(1);
        try {
            // Training has ended. Only this bounded publication lock may wait;
            // serialization, filesystem access and pipe reporting stay on Work.
            std::lock_guard lock(mutex);
            if (finished || stopping.load()) {
                Drop();
                return;
            }
            if (!bounded_paths(progress)) {
                Drop();
                if (progress.checkpoint_path.native().size() > mmltk::frameworks::reflection::kMaximumPathBytes) progress.checkpoint_path.clear();
                if (progress.full_checkpoint_path.native().size() > mmltk::frameworks::reflection::kMaximumPathBytes) progress.full_checkpoint_path.clear();
                // Retain every representable fact, but do not emit success or
                // a results projection for an incomplete terminal payload.
                progress.phase = TrainingPhase::Error;
                facts.reset();
            }
            TrainingRecord record;
            record.sequence = next;
            record.dropped_before = dropped.load();
            record.role = TrainingRecordRole::Terminal;
            record.progress = std::move(progress);
            terminal = std::move(record);
            final = std::move(facts);
            finished = true;
            Wake();
        } catch (...) { Drop(); }
    }
    void Fail(std::string_view message) noexcept {
        degraded.store(true);
        {
            std::lock_guard lock(mutex);
            try {
                if (error.empty()) error.assign(message.substr(0, 1024));
            } catch (...) {}
        }
        if (!reported_failure) {
            // Product status travels through the already-captured child output.
            // Only this independent writer can wait for the pipe.
            constexpr std::string_view notice = kTrainingPersistenceFailureLine;
            std::size_t offset = 0;
            while (offset < notice.size()) {
                const auto written = ::write(STDERR_FILENO, notice.data() + offset, notice.size() - offset);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) break;
                offset += static_cast<std::size_t>(written);
            }
            reported_failure = true;
        }
    }
    void Initialize() {
        const auto directory = run.configuration.output_dir;
        const auto manifest = directory / "run.json";
        if (!run.configuration.resume_path.empty() && std::filesystem::exists(manifest)) {
            if (!std::filesystem::exists(directory / "metrics.jsonl") ||
                !std::filesystem::equivalent(std::filesystem::absolute(run.configuration.resume_path).parent_path(), directory))
                throw std::runtime_error("selected output history is not associated with the resume checkpoint");
            auto previous = serial::decode_reflected_json<TrainingRun>(read_manifest(manifest), manifest_limits);
            if (previous.format_version != kTrainingRunFormat || previous.run_id.empty() || previous.attempt_id.empty() ||
                previous.evaluated_weights != run.evaluated_weights || previous.class_layout != run.class_layout || run.source_checkpoint_attempt_id.empty() ||
                previous.checkpoint_attempt_id != run.source_checkpoint_attempt_id)
                throw std::runtime_error("resume history has an unsupported or incompatible run format");
            run.run_id = previous.run_id;
            run.checkpoint_attempt_id = previous.checkpoint_attempt_id;
        } else {
            run.run_id = identity();
            std::ofstream metrics(directory / "metrics.jsonl", std::ios::binary | std::ios::trunc);
            if (!metrics) throw std::runtime_error("cannot create training metric history");
        }
        write_file(manifest, serial::reflected_json(run, manifest_scratch, manifest_limits));
    }
    void Persist(TrainingRecord record, const std::optional<TrainingFinalFacts>& completed) {
        record.run_id = run.run_id;
        record.attempt_id = run.attempt_id;
        record.evaluated_weights = run.evaluated_weights;
        if (record.progress.phase == TrainingPhase::Starting) record.attempt_configuration = run.configuration;
        const auto directory = run.configuration.output_dir;
        append_file(directory / "metrics.jsonl", serial::reflected_json(record, scratch, limits));
        auto projection = serial::reflected_json(record.progress, scratch, limits);
        projection["record"] = serial::reflected_json(record, scratch, limits);
        projection["phase"] = legacy_phase(record.progress.phase);
        projection["eval_lanes"] = run.execution.eval_lanes;
        projection["effective_batch_per_rank"] = run.execution.effective_batch_per_rank;
        projection["effective_batch_global"] = run.execution.effective_batch_global;
        projection["training_supervision"] = serial::reflected_json(run.configuration.training_supervision, scratch, limits);
        projection["persistence_degraded"] = degraded.load();
        projection["dropped_records"] = dropped.load();
        write_file(directory / "progress.json", projection);
        if (record.role == TrainingRecordRole::Epoch) {
            run.checkpoint_attempt_id = run.attempt_id;
            write_file(directory / "run.json", serial::reflected_json(run, manifest_scratch, manifest_limits));
            if (record.progress.epoch_global_loss) projection["train_loss"] = *record.progress.epoch_global_loss;
            projection["evaluated_weights"] = run.evaluated_weights == EvaluatedWeights::Ema ? "ema" : "ordinary";
            append_file(directory / "log.txt", projection);
        }
        if (completed) {
            projection["preset_name"] = run.configuration.preset_name;
            projection["output_dir"] = directory.string();
            projection["checkpoint"] = (directory / "checkpoint.pt").string();
            projection["best_checkpoint"] = record.progress.checkpoint_path.string();
            projection["best_is_ema"] = run.evaluated_weights == EvaluatedWeights::Ema;
            projection["best_is_fallback"] = completed->fallback;
            projection["best_regular_metric"] = completed->best_regular ? nlohmann::json(*completed->best_regular) : nlohmann::json(nullptr);
            projection["best_ema_metric"] = completed->best_ema ? nlohmann::json(*completed->best_ema) : nlohmann::json(nullptr);
            projection["last_epoch"] = record.progress.epoch;
            projection["history_size"] = completed->history_size;
            const auto& bounds = run.execution.dataset_limits;
            projection["dataset_max_instances"] = {{"train", bounds.train_max_instances},
                                                   {"val", bounds.val_max_instances},
                                                   {"test", bounds.test_max_instances ? nlohmann::json(*bounds.test_max_instances) : nlohmann::json(nullptr)},
                                                   {"largest", bounds.largest_max_instances}};
            projection["query_resolution"] = {{"source", bounds.query_source},           {"resolved", bounds.resolved_num_queries},
                                              {"required", bounds.required_num_queries}, {"automatic_query_cap", bounds.automatic_num_queries_cap},
                                              {"automatic", bounds.automatic},           {"requested_override", bounds.requested_override}};
            projection["gpu_augmentation"] = serial::reflected_json(run.configuration.gpu_augmentation, scratch, limits);
            projection["test"] = record.progress.test ? serial::reflected_json(*record.progress.test, scratch, limits) : nlohmann::json(nullptr);
            write_file(directory / "results.json", projection);
        }
    }
    void Work() noexcept {
        bool initialized = false;
        try {
            Initialize();
            initialized = true;
        } catch (const std::exception& failure) { Fail(failure.what()); } catch (...) {
            Fail("cannot initialize training persistence");
        }
        for (;;) {
            // Snapshot before examining queues. Atomic wait cannot miss a
            // rejection or Close between the queue check and sleeping.
            const auto observed = wake_generation.load();
            std::optional<TrainingRecord> record;
            std::optional<TrainingFinalFacts> completed;
            bool stop = false;
            {
                std::lock_guard lock(mutex);
                TrainingRecord* oldest = live ? &*live : nullptr;
                const auto consider = [&](TrainingRecord* candidate) {
                    if (candidate && (!oldest || candidate->sequence < oldest->sequence)) oldest = candidate;
                };
                consider(boundaries.empty() ? nullptr : &boundaries.front());
                consider(epochs.empty() ? nullptr : &epochs.front());
                consider(terminal ? &*terminal : nullptr);
                if (oldest) {
                    record = std::move(*oldest);
                    if (live && oldest == &*live)
                        live.reset();
                    else if (!boundaries.empty() && oldest == &boundaries.front())
                        boundaries.erase(boundaries.begin());
                    else if (!epochs.empty() && oldest == &epochs.front())
                        epochs.erase(epochs.begin());
                    else {
                        terminal.reset();
                        completed = std::move(final);
                    }
                } else
                    stop = stopping.load();
            }
            // Failure notification precedes even an empty-queue shutdown.
            if (degraded.load() && !reported_failure) Fail("training telemetry is incomplete; history contains gaps");
            if (stop) return;
            if (record && !initialized) Drop();
            if (record && initialized) {
                try {
                    Persist(std::move(*record), completed);
                } catch (const std::exception& failure) {
                    Drop();
                    Fail(failure.what());
                } catch (...) {
                    Drop();
                    Fail("cannot persist training telemetry record");
                }
            }
            if (!record) wake_generation.wait(observed);
        }
    }
};
TrainingTelemetryWriter::TrainingTelemetryWriter(TrainingRun run) : impl_(std::make_unique<Impl>(std::move(run))) {}
TrainingTelemetryWriter::~TrainingTelemetryWriter() noexcept {
    if (!impl_->finished && std::uncaught_exceptions() > impl_->uncaught) {
        auto progress = std::move(impl_->last_progress);
        progress.phase = TrainingPhase::Error;
        impl_->Complete(std::move(progress), std::nullopt);
    }
    Close();
}
const std::string& TrainingTelemetryWriter::attempt_id() const noexcept { return impl_->run.attempt_id; }
void TrainingTelemetryWriter::Submit(TrainingMetricProgress progress, TrainingRecordRole role) noexcept {
    const auto next = impl_->sequence.fetch_add(1);
    try {
        if (!bounded_paths(progress) || (role != TrainingRecordRole::Live && role != TrainingRecordRole::Boundary && role != TrainingRecordRole::Epoch)) {
            impl_->Drop();
            return;
        }
        TrainingRecord record;
        record.sequence = next;
        impl_->last_progress = progress;
        record.role = role;
        record.progress = std::move(progress);
        std::unique_lock lock(impl_->mutex, std::try_to_lock);
        if (!lock.owns_lock() || impl_->finished || impl_->stopping.load()) {
            impl_->Drop();
            return;
        }
        if (role == TrainingRecordRole::Live) {
            if (impl_->live) impl_->Drop();
            record.dropped_before = impl_->dropped.load();
            impl_->live = std::move(record);
        } else if (role == TrainingRecordRole::Epoch) {
            if (impl_->epochs.size() == epoch_capacity) {
                impl_->Drop();
                return;
            }
            record.dropped_before = impl_->dropped.load();
            impl_->epochs.push_back(std::move(record));
        } else {
            if (impl_->boundaries.size() == boundary_capacity) {
                impl_->Drop();
                return;
            }
            record.dropped_before = impl_->dropped.load();
            impl_->boundaries.push_back(std::move(record));
        }
        impl_->Wake();
    } catch (...) { impl_->Drop(); }
}
void TrainingTelemetryWriter::Finish(TrainingMetricProgress progress, TrainingFinalFacts facts) noexcept {
    impl_->Complete(std::move(progress), std::move(facts));
}
void TrainingTelemetryWriter::Close() noexcept {
    impl_->stopping.store(true);
    impl_->Wake();
    if (impl_->worker.joinable()) impl_->worker.join();
}
TrainingPersistence TrainingTelemetryWriter::persistence() const {
    std::lock_guard lock(impl_->mutex);
    return {impl_->degraded.load(), impl_->dropped.load(), impl_->error};
}
}  // namespace mmltk::backend::models::rfdetr
