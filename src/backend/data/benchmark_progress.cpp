#include "detail/benchmark_progress.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
using Clock = std::chrono::steady_clock;
ProgressReporter::ProgressReporter(BenchmarkProgressCallback callback, const BenchmarkTraceSink& trace, std::span<const BenchmarkDatasetSource> sources) : callback_(std::move(callback)), trace_(&trace) {
    if (!callback_) { return; }
    const auto source_progress = [](const BenchmarkDatasetSource source) {
        BenchmarkSourceProgress progress;
        progress.source = source;
        return progress;
    };
    constexpr BenchmarkDatasetSource custom_sources[]{BenchmarkDatasetSource::kCoco2017, BenchmarkDatasetSource::kObjects365V2, BenchmarkDatasetSource::kOpenImagesV7};
    if (sources.empty()) sources = custom_sources;
    for (const auto source : sources) state_.sources.push_back(source_progress(source));
}
void ProgressReporter::phase(const DatasetCompilePhase phase, const std::uint64_t completed, const std::uint64_t total) {
    if (!callback_ && !*trace_) { return; }
    const std::lock_guard lock(mutex_);
    const bool phase_changed = state_.phase != phase;
    if (phase == DatasetCompilePhase::Extracting && state_.phase != DatasetCompilePhase::Extracting) {
        for (BenchmarkSourceProgress& source : state_.sources) {
            source.activity.clear();
            source.completed_bytes = 0U;
            source.total_bytes = 0U;
            source.byte_total_known = true;
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
    if (phase_changed || state_.activity.empty()) {
        state_.current_source.reset();
        set_activity_unlocked(default_phase_activity(phase));
    }
    emit();
}
void ProgressReporter::activity(std::string activity) {
    if (!callback_ && !*trace_) { return; }
    const std::lock_guard lock(mutex_);
    state_.current_source.reset();
    set_activity_unlocked(std::move(activity));
    emit();
}
void ProgressReporter::pixel_attempt(const std::uint64_t completed, const std::uint64_t total, const std::string_view split, const std::uint64_t split_total) {
    if (!pixel_observer_enabled()) { return; }
    const std::lock_guard lock(mutex_);
    if (callback_ && state_.pixel_attempt == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("benchmark pixel attempt counter overflow");
    }
    state_.current_source.reset();
    state_.phase = DatasetCompilePhase::Pixels;
    set_activity_unlocked(default_phase_activity(DatasetCompilePhase::Pixels));
    activity_started_ = Clock::now();
    if (callback_) {
        state_.completed = completed;
        state_.total = total;
        ++state_.pixel_attempt;
        state_.pixel_attempt_offset = completed;
        state_.activity_elapsed_seconds = 0U;
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
    if (!callback_ && !*trace_) { return; }
    const std::lock_guard lock(mutex_);
    if (!callback_) {
        if (state_.activity != activity || state_.current_source != source) { trace_activity(source, activity); }
        state_.current_source = source;
        state_.activity = std::move(activity);
        return;
    }
    set_source_activity_unlocked(source, std::move(activity));
    emit();
}
void ProgressReporter::set_source_activity_unlocked(const BenchmarkDatasetSource source, std::string activity) {
    BenchmarkSourceProgress& source_state = source_progress(source);
    if (source_state.activity != activity || state_.current_source != source) { trace_activity(source, activity); }
    source_state.complete = false;
    source_state.cache_hit = false;
    source_state.resumed = false;
    source_state.retry_count = 0U;
    source_state.activity = activity;
    state_.current_source = source;
    set_activity_unlocked(std::move(activity));
}
bool ProgressReporter::transfer_observer_enabled() const noexcept { return static_cast<bool>(callback_); }
bool ProgressReporter::normalization_observer_enabled() const noexcept { return static_cast<bool>(callback_); }
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
void ProgressReporter::source_images(const BenchmarkDatasetSource source, const std::uint64_t completed, const std::uint64_t total) {
    if (!callback_) { return; }
    const std::lock_guard lock(mutex_);
    BenchmarkSourceProgress& progress = source_progress(source);
    progress.completed_images = completed;
    progress.total_images = total;
    select_acquisition_source(source);
    update_source_phase_progress();
    emit();
}
void ProgressReporter::select_acquisition_source(const BenchmarkDatasetSource source) {
    if (state_.phase == DatasetCompilePhase::Downloading || state_.phase == DatasetCompilePhase::Extracting) { state_.current_source = source; }
}
void ProgressReporter::trace_activity(const std::optional<BenchmarkDatasetSource> source, const std::string_view activity) {
    trace_benchmark_event(*trace_, "benchmark.progress.activity", [&] {
        nlohmann::json fields{{"activity", activity}, {"phase", static_cast<unsigned>(state_.phase)}};
        if (source) { fields["source"] = static_cast<unsigned>(*source); }
        return fields;
    });
}
void ProgressReporter::source_transfer(const DownloadProgress& update, const std::uint64_t completed,
                                       const std::uint64_t total) {
    if (!callback_) { return; }
    const std::lock_guard lock(mutex_);
    const auto source = update.source;
    auto& progress = source_progress(source);
    std::string operation;
    if (update.cache_hit) {
        operation = "Reusing cached ";
    } else {
        switch (update.phase) {
            case DownloadProgressPhase::kDownloading:
                operation = update.redownload ? (update.resumed ? "Resuming re-download of " : "Re-downloading ")
                                              : (update.resumed ? "Resuming " : "Downloading ");
                break;
            case DownloadProgressPhase::kVerifyingCachedArtifact: operation = "Verifying cached "; break;
            case DownloadProgressPhase::kVerifyingDownloadedArtifact: operation = "Verifying downloaded "; break;
        }
    }
    operation += update.artifact_id;
    select_acquisition_source(source);
    if (state_.current_source == source) {
        if (state_.activity != operation) { trace_activity(source, operation); }
        set_activity_unlocked(operation);
    }
    progress.complete = false;
    progress.activity = operation + " · " + std::to_string(update.completed_bytes);
    if (update.total_bytes != 0U) { progress.activity += " / " + std::to_string(update.total_bytes); }
    progress.activity += update.cache_hit ? " bytes reused" : " bytes";
    if (!update.cache_hit && update.total_bytes == 0U) { progress.activity += " (total unknown)"; }
    if (!update.cache_hit && update.attempt != 0U) { progress.activity += " · attempt " + std::to_string(update.attempt); }
    if (update.resumed) { progress.activity += " · retained " + std::to_string(update.retained_bytes) + " bytes"; }
    progress.completed_bytes = completed;
    progress.total_bytes = total;
    progress.byte_total_known = total != 0U;
    progress.retry_count = !update.cache_hit && update.attempt > 0U ? update.attempt - 1U : 0U;
    progress.cache_hit = update.cache_hit;
    progress.resumed = update.resumed;
    update_source_phase_progress();
    emit();
}
void ProgressReporter::add_source_images(const BenchmarkDatasetSource source, const std::uint64_t count, const std::uint64_t total, std::string activity) {
    if (!callback_) { return; }
    const std::lock_guard lock(mutex_);
    auto& progress = source_progress(source);
    add_progress(progress.completed_images, count, "benchmark source image count overflow");
    if (!activity.empty()) { set_source_activity_unlocked(source, std::move(activity)); }
    progress.total_images = total;
    select_acquisition_source(source);
    update_source_phase_progress();
    emit();
}
void ProgressReporter::rollback_source_images(const BenchmarkDatasetSource source, const std::uint64_t count, const std::uint64_t total, std::string activity) {
    if (!callback_) { return; }
    const std::lock_guard lock(mutex_);
    auto& progress = source_progress(source);
    if (count > progress.completed_images) { throw std::underflow_error("benchmark source image count underflow"); }
    progress.completed_images -= count;
    if (!activity.empty()) { set_source_activity_unlocked(source, std::move(activity)); }
    progress.total_images = total;
    select_acquisition_source(source);
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
    if (state_.activity == activity) { return; }
    if (!state_.current_source) { trace_activity(std::nullopt, activity); }
    state_.activity = std::move(activity);
    state_.activity_elapsed_seconds = 0U;
    if (callback_) { activity_started_ = Clock::now(); }
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
        bool all_known = true;
        for (const BenchmarkSourceProgress& source : state_.sources) {
            all_known = all_known && source.byte_total_known;
            add_progress(state_.completed, source.completed_bytes, "benchmark download progress overflow");
            add_progress(state_.total, source.total_bytes, "benchmark download progress overflow");
        }
        if (!all_known) { state_.total = 0U; }
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
void ArtifactProgressTotals::update(const DownloadProgress& update, ProgressReporter& reporter) {
    if (!reporter.transfer_observer_enabled()) { return; }
    const std::lock_guard lock(mutex_);
    const auto source = update.source;
    auto& totals = sources_[source];
    const auto previous = totals.artifacts.find(update.artifact_id);
    const bool observed = previous != totals.artifacts.end();
    const Observation old = observed ? previous->second : Observation{};
    const auto replace = [](const std::uint64_t aggregate, const std::uint64_t before, const std::uint64_t after) {
        if (before > aggregate) { throw std::underflow_error("benchmark artifact progress underflow"); }
        const auto remaining = aggregate - before;
        if (after > std::numeric_limits<std::uint64_t>::max() - remaining) { throw std::overflow_error("benchmark artifact progress overflow"); }
        return remaining + after;
    };
    const auto completed = replace(totals.completed, old.completed, update.completed_bytes);
    const auto known_total = replace(totals.known_total, old.total, update.total_bytes);
    const auto unknown_count = replace(totals.unknown_count, observed && old.total == 0U ? 1U : 0U, update.total_bytes == 0U ? 1U : 0U);
    totals.artifacts.insert_or_assign(update.artifact_id, Observation{update.completed_bytes, update.total_bytes});
    totals.completed = completed;
    totals.known_total = known_total;
    totals.unknown_count = unknown_count;
    reporter.source_transfer(update, completed, unknown_count == 0U ? known_total : 0U);
}
void ProgressReporter::emit() {
    if (callback_) {
        if (activity_started_.time_since_epoch().count() != 0) {
            state_.activity_elapsed_seconds =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - activity_started_).count());
        }
        callback_(state_);
    }
}
}  // namespace mmltk::backend::data::benchmark_internal
