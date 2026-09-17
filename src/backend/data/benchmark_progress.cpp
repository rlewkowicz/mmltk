#include "detail/benchmark_progress.h"
namespace mmltk::backend::data::benchmark_internal {
using Clock = std::chrono::steady_clock;
ProgressReporter::ProgressReporter(BenchmarkProgressCallback callback, const BenchmarkTraceSink& trace) : callback_(std::move(callback)), trace_(&trace) {
    if (!callback_) { return; }
    const auto source_progress = [](const BenchmarkDatasetSource source) {
        BenchmarkSourceProgress progress;
        progress.source = source;
        return progress;
    };
    state_.sources = {
        source_progress(BenchmarkDatasetSource::kCoco2017),
        source_progress(BenchmarkDatasetSource::kObjects365V2),
        source_progress(BenchmarkDatasetSource::kOpenImagesV7),
    };
}
void ProgressReporter::phase(const DatasetCompilePhase phase, const std::uint64_t completed, const std::uint64_t total) {
    if (!callback_) { return; }
    const std::lock_guard lock(mutex_);
    const bool phase_changed = state_.phase != phase;
    if (phase == DatasetCompilePhase::Extracting && state_.phase != DatasetCompilePhase::Extracting) {
        for (BenchmarkSourceProgress& source : state_.sources) {
            source.activity.clear();
            source.completed_bytes = 0U;
            source.total_bytes = 0U;
            source.completed_images = 0U;
            source.total_images = 0U;
            source.retry_count = 0U;
            source.cache_hit = false;
            source.resumed = false;
            source.complete = false;
        }
    }
    state_.phase = phase;
    state_.completed = completed;
    state_.total = total;
    if (phase_changed || state_.activity.empty()) { set_activity_unlocked(default_phase_activity(phase)); }
    emit();
}
void ProgressReporter::activity(std::string activity) {
    if (!callback_) { return; }
    const std::lock_guard lock(mutex_);
    set_activity_unlocked(std::move(activity));
    emit();
}
void ProgressReporter::pixel_attempt(const std::uint64_t completed, const std::uint64_t total, const std::string_view split, const std::uint64_t split_total) {
    if (!pixel_observer_enabled()) { return; }
    const std::lock_guard lock(mutex_);
    if (callback_ && state_.pixel_attempt == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("benchmark pixel attempt counter overflow");
    }
    if (callback_) {
        state_.phase = DatasetCompilePhase::Pixels;
        state_.completed = completed;
        state_.total = total;
        ++state_.pixel_attempt;
        state_.pixel_attempt_offset = completed;
        set_activity_unlocked(default_phase_activity(DatasetCompilePhase::Pixels));
    } else {
        activity_started_ = Clock::now();
    }
    pixel_completed_before_ = completed;
    pixel_split_ = split;
    pixel_split_total_ = split_total;
    pixel_completed_ = 0U;
    if (callback_) { emit(); }
}
void ProgressReporter::pixel_completed() {
    const std::lock_guard lock(mutex_);
    if (pixel_completed_ >= pixel_split_total_) { throw std::overflow_error("benchmark pixel completion exceeds its split total"); }
    ++pixel_completed_;
    constexpr std::uint64_t kPixelProgressQuantum = 64U;
    if ((pixel_completed_ % kPixelProgressQuantum) != 0U && pixel_completed_ != pixel_split_total_) { return; }
    if (callback_) {
        state_.completed = pixel_completed_before_ + pixel_completed_;
        state_.activity_elapsed_seconds =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - activity_started_).count());
        emit();
    }
    if (trace_ != nullptr && *trace_) {
        const double elapsed_seconds = std::chrono::duration<double>(Clock::now() - activity_started_).count();
        const double images_per_second = elapsed_seconds > 0.0 ? static_cast<double>(pixel_completed_) / elapsed_seconds : 0.0;
        const double eta_seconds = images_per_second > 0.0 ? static_cast<double>(pixel_split_total_ - pixel_completed_) / images_per_second : 0.0;
        trace_benchmark_event(*trace_, "benchmark.pixel_compile.throughput", [&] {
            return nlohmann::json{{"split", pixel_split_},
                                  {"completed_images", pixel_completed_},
                                  {"total_images", pixel_split_total_},
                                  {"elapsed_seconds", elapsed_seconds},
                                  {"images_per_second", images_per_second},
                                  {"eta_seconds", eta_seconds}};
        });
    }
}
void ProgressReporter::source_activity(const BenchmarkDatasetSource source, std::string activity) {
    if (!callback_) { return; }
    const std::lock_guard lock(mutex_);
    BenchmarkSourceProgress& source_state = source_progress(source);
    source_state.activity = activity;
    set_activity_unlocked(std::move(activity));
    emit();
}
bool ProgressReporter::pixel_observer_enabled() const noexcept { return callback_ || (trace_ != nullptr && static_cast<bool>(*trace_)); }
void ProgressReporter::projected(const std::uint64_t bytes) {
    if (!callback_) { return; }
    const std::lock_guard lock(mutex_);
    state_.projected_output_bytes = bytes;
    emit();
}
void ProgressReporter::rejected(const std::uint64_t dropped, const std::uint64_t quarantined) {
    if (!callback_) { return; }
    const std::lock_guard lock(mutex_);
    state_.dropped_instances = dropped;
    state_.quarantined_images = quarantined;
    emit();
}
void ProgressReporter::source_bytes(const BenchmarkDatasetSource source, const std::uint64_t completed, const std::uint64_t total, const std::uint32_t retry_count,
                      const bool cache_hit, const bool resumed) {
    if (!callback_) { return; }
    const std::lock_guard lock(mutex_);
    BenchmarkSourceProgress& progress = source_progress(source);
    progress.completed_bytes = completed;
    progress.total_bytes = total;
    progress.retry_count = retry_count;
    progress.cache_hit = cache_hit;
    progress.resumed = progress.resumed || resumed;
    update_source_phase_progress();
    emit();
}
void ProgressReporter::source_images(const BenchmarkDatasetSource source, const std::uint64_t completed, const std::uint64_t total) {
    if (!callback_) { return; }
    const std::lock_guard lock(mutex_);
    BenchmarkSourceProgress& progress = source_progress(source);
    progress.completed_images = completed;
    progress.total_images = total;
    update_source_phase_progress();
    emit();
}
void ProgressReporter::source_complete(const BenchmarkDatasetSource source, const bool cache_hit) {
    if (!callback_) { return; }
    const std::lock_guard lock(mutex_);
    BenchmarkSourceProgress& progress = source_progress(source);
    progress.complete = true;
    progress.cache_hit = cache_hit;
    progress.activity = cache_hit ? "Cache hit" : "Complete";
    update_source_phase_progress();
    emit();
}
std::string ProgressReporter::default_phase_activity(const DatasetCompilePhase phase) {
    // CLEANUP-IGNORE: exhaustive phase-to-activity descriptor; its switch shape intentionally follows the enum.
    switch (phase) {
        case DatasetCompilePhase::Idle: return "Compiling benchmark dataset";
        case DatasetCompilePhase::Planning: return "Planning benchmark dataset";
        case DatasetCompilePhase::Downloading: return "Acquiring benchmark metadata";
        case DatasetCompilePhase::Indexing: return "Indexing benchmark annotations";
        case DatasetCompilePhase::Extracting: return "Acquiring and extracting source images";
        case DatasetCompilePhase::Labels: return "Preparing compiled labels";
        case DatasetCompilePhase::Pixels: return "Compiling image pixels";
        case DatasetCompilePhase::Syncing: return "Validating and syncing compiled output";
        case DatasetCompilePhase::Publishing: return "Publishing benchmark dataset";
    }
    std::unreachable();
}
void ProgressReporter::set_activity_unlocked(std::string activity) {
    state_.activity = std::move(activity);
    state_.activity_elapsed_seconds = 0U;
    activity_started_ = Clock::now();
}
void ProgressReporter::add_progress(std::uint64_t& target, const std::uint64_t value, const char* context) {
    if (value > std::numeric_limits<std::uint64_t>::max() - target) { throw std::overflow_error(context); }
    target += value;
}
BenchmarkSourceProgress& ProgressReporter::source_progress(const BenchmarkDatasetSource source) {
    const auto found = std::ranges::find(state_.sources, source, &BenchmarkSourceProgress::source);
    if (found == state_.sources.end()) { throw std::runtime_error("benchmark progress references an unknown source"); }
    return *found;
}
void ProgressReporter::update_source_phase_progress() {
    if (state_.phase == DatasetCompilePhase::Downloading) {
        state_.completed = 0U;
        state_.total = 0U;
        for (const BenchmarkSourceProgress& source : state_.sources) {
            add_progress(state_.completed, source.completed_bytes, "benchmark download progress overflow");
            add_progress(state_.total, source.total_bytes, "benchmark download progress overflow");
        }
        return;
    }
    if (state_.phase != DatasetCompilePhase::Extracting) { return; }
    constexpr std::uint64_t kSourceProgressScale = 1'000'000U;
    state_.completed = 0U;
    state_.total = 0U;
    for (const BenchmarkSourceProgress& source : state_.sources) {
        const bool use_images = source.total_images > 0U;
        const std::uint64_t completed = use_images ? source.completed_images : source.completed_bytes;
        const std::uint64_t total = use_images ? source.total_images : source.total_bytes;
        if (total == 0U && !source.complete) { continue; }
        const std::uint64_t scaled_completed =
            total == 0U ? kSourceProgressScale
                        : static_cast<std::uint64_t>(static_cast<long double>(std::min(completed, total)) * static_cast<long double>(kSourceProgressScale) /
                                                     static_cast<long double>(total));
        add_progress(state_.completed, scaled_completed, "benchmark extraction progress overflow");
        add_progress(state_.total, kSourceProgressScale, "benchmark extraction progress overflow");
    }
}
void ProgressReporter::emit() const {
    if (callback_) { callback_(state_); }
}
}
