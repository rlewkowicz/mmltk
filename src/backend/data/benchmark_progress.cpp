#include "detail/benchmark_progress.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
namespace {
std::uint64_t replace_progress(const std::uint64_t aggregate, const std::uint64_t before, const std::uint64_t after) {
 if (before > aggregate) { throw std::underflow_error("benchmark artifact progress underflow"); }
 const auto remaining = aggregate - before;
 if (after > std::numeric_limits<std::uint64_t>::max() - remaining) { throw std::overflow_error("benchmark artifact progress overflow"); }
 return remaining + after;
}
}  // namespace
using Clock = std::chrono::steady_clock;
ProgressReporter::ProgressReporter(BenchmarkProgressCallback callback, const BenchmarkTraceSink& trace, std::span<const BenchmarkDatasetSource> sources)
    : callback_(std::move(callback)), trace_(&trace) {
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
ProgressReporter::~ProgressReporter() = default;
IndexingProgressTotals* ProgressReporter::indexing(std::span<const std::uint64_t> totals) {
 if (!callback_) return nullptr;
 const std::lock_guard lock(mutex_);
 if (!indexing_) indexing_ = std::make_unique<IndexingProgressTotals>(totals);
 return indexing_.get();
}
void ProgressReporter::invalidate_indexing(std::uint64_t count) {
 if (!callback_ || count == 0) return;
 const std::lock_guard lock(mutex_);
 if (count > indexed_completed_) throw std::underflow_error("benchmark indexing invalidation underflow");
 indexed_completed_ -= count;
 add_progress(state_.tracks.labels.invalidated, count, "benchmark indexing invalidation overflow");
 update_labels();
 if (state_.phase == DatasetCompilePhase::Indexing) state_.completed = indexed_completed_;
 emit();
}
void ProgressReporter::phase(const DatasetCompilePhase phase, const std::uint64_t completed, const std::uint64_t total) {
 if (!callback_ && !*trace_) { return; }
 const std::lock_guard lock(mutex_);
 const bool background_indexing = phase == DatasetCompilePhase::Indexing && (state_.phase == DatasetCompilePhase::Extracting || state_.phase == DatasetCompilePhase::Pixels);
 if (phase == DatasetCompilePhase::Indexing) {
  indexed_completed_ = std::max(indexed_completed_, completed);
  indexed_total_ = std::max(indexed_total_, total);
  indexing_known_ = true;
  update_labels();
 }
 if (background_indexing) {
  emit();
  return;
 }
 const bool phase_changed = state_.phase != phase;
 state_.phase = phase;
 if ((phase == DatasetCompilePhase::Downloading || phase == DatasetCompilePhase::Extracting) && !state_.tracks.acquisition.complete) {
  state_.tracks.acquisition.active = true;
  state_.tracks.acquisition.complete = false;
  state_.tracks.acquisition.activity = DatasetCompileActivity::Acquiring;
 }
 if (phase == DatasetCompilePhase::Syncing || phase == DatasetCompilePhase::Publishing) {
  for (auto* track : {&state_.tracks.acquisition, &state_.tracks.labels, &state_.tracks.pixels}) {
   track->active = false;
   track->complete = true;
   track->activity = DatasetCompileActivity::Complete;
  }
 }
 state_.completed = phase == DatasetCompilePhase::Indexing ? indexed_completed_ : completed;
 state_.total = phase == DatasetCompilePhase::Indexing ? indexed_total_ : total;
 if (phase == DatasetCompilePhase::Pixels) {
  state_.completed = state_.tracks.pixels.completed;
  state_.total = state_.tracks.pixels.total;
 }
 if (phase_changed || state_.activity.empty()) {
  state_.current_source.reset();
  set_activity_unlocked(default_phase_activity(phase));
 }
 emit();
}
void ProgressReporter::update_labels() {
 if (state_.phase == DatasetCompilePhase::Labels) {
  state_.completed = label_completed_;
  state_.total = label_plans_.size();
 }
 auto& labels = state_.tracks.labels;
 labels.completed = indexed_completed_;
 add_progress(labels.completed, label_completed_, "benchmark labels completed overflow");
 labels.total = indexed_total_;
 add_progress(labels.total, label_plans_.size(), "benchmark labels total overflow");
 labels.total_known = indexing_known_;
 labels.complete = labels_admitted_ && indexing_known_ && labels.completed == labels.total;
 labels.active = !labels.complete && (indexed_completed_ < indexed_total_ || label_active_ != 0);
 labels.activity = labels.complete                        ? DatasetCompileActivity::Complete
                   : label_active_ != 0                   ? DatasetCompileActivity::Preparing
                    : indexed_completed_ < indexed_total_ ? DatasetCompileActivity::Normalizing
                                                          : DatasetCompileActivity::Waiting;
}
void ProgressReporter::label_plans(std::size_t total) {
 if (!callback_) return;
 const std::lock_guard lock(mutex_);
 if (labels_admitted_) throw std::logic_error("benchmark label plans already admitted");
 label_plans_.assign(total, LabelPlan::Waiting);
 labels_admitted_ = true;
 update_labels();
 emit();
}
void ProgressReporter::label_plan_started(std::size_t slot) {
 if (!callback_) return;
 const std::lock_guard lock(mutex_);
 auto& plan = label_plans_.at(slot);
 if (plan == LabelPlan::Active) return;
 if (plan == LabelPlan::Complete) {
  --label_completed_;
  add_progress(state_.tracks.labels.invalidated, 1, "benchmark label invalidation overflow");
 }
 plan = LabelPlan::Active;
 ++label_active_;
 update_labels();
 emit();
}
void ProgressReporter::label_plan_completed(std::size_t slot) {
 if (!callback_) return;
 const std::lock_guard lock(mutex_);
 auto& plan = label_plans_.at(slot);
 if (plan == LabelPlan::Complete) return;
 if (plan != LabelPlan::Active) throw std::logic_error("benchmark label plan has not started");
 plan = LabelPlan::Complete;
 --label_active_;
 ++label_completed_;
 update_labels();
 emit();
}
void ProgressReporter::discard_label_plans() {
 if (!callback_) return;
 const std::lock_guard lock(mutex_);
 add_progress(state_.tracks.labels.invalidated, label_completed_, "benchmark label invalidation overflow");
 label_completed_ = label_active_ = 0;
 label_plans_.clear();
 labels_admitted_ = false;
 update_labels();
 emit();
}
void ProgressReporter::activity(std::string activity) {
 if (!callback_ && !*trace_) { return; }
 const std::lock_guard lock(mutex_);
 state_.current_source.reset();
 set_activity_unlocked(std::move(activity));
 emit();
}
void ProgressReporter::pixels(const std::uint64_t completed, const std::uint64_t total) {
 if (!pixel_observer_enabled()) return;
 const std::lock_guard lock(mutex_);
 auto& pixels = state_.tracks.pixels;
 if (completed > total) throw std::logic_error("benchmark pixel count exceeds total");
 if (completed < pixels.completed) add_progress(pixels.invalidated, pixels.completed - completed, "benchmark invalidated pixels overflow");
 pixels.completed = completed;
 pixels.total = total;
 pixels.total_known = true;
 pixels.complete = completed == total;
 pixels.active = !pixels.complete;
 pixels.activity = pixels.complete ? DatasetCompileActivity::Complete : DatasetCompileActivity::Compiling;
 if (*trace_ && pixel_started_.time_since_epoch().count() == 0) pixel_started_ = Clock::now();
 emit();
}
void ProgressReporter::invalidate_pixels(std::uint64_t count) {
 if (!pixel_observer_enabled() || !count) return;
 const std::lock_guard lock(mutex_);
 auto& pixels = state_.tracks.pixels;
 if (count > pixels.completed) throw std::underflow_error("benchmark pixel invalidation underflow");
 pixels.completed -= count;
 add_progress(pixels.invalidated, count, "benchmark invalidated pixels overflow");
 pixels.complete = false;
 pixels.active = true;
 pixels.activity = DatasetCompileActivity::Compiling;
 emit();
}
void ProgressReporter::pixel_completed() {
 if (!pixel_observer_enabled()) return;
 const std::lock_guard lock(mutex_);
 auto& pixels = state_.tracks.pixels;
 if (pixels.completed >= pixels.total) throw std::overflow_error("benchmark pixel completion exceeds total");
 ++pixels.completed;
 if (state_.phase == DatasetCompilePhase::Pixels) {
  state_.completed = pixels.completed;
  state_.total = pixels.total;
 }
 pixels.complete = pixels.completed == pixels.total;
 pixels.active = !pixels.complete;
 pixels.activity = pixels.complete ? DatasetCompileActivity::Complete : DatasetCompileActivity::Compiling;
 constexpr std::uint64_t kPixelProgressQuantum = 64U;
 if (pixels.completed != 1 && pixels.completed % kPixelProgressQuantum != 0 && !pixels.complete) return;
 emit();
 if (*trace_) {
  const double elapsed = std::chrono::duration<double>(Clock::now() - pixel_started_).count();
  trace_benchmark_event(*trace_, "benchmark.pixel_compile.throughput", [&] {
   return nlohmann::json{{"completed_images", pixels.completed}, {"total_images", pixels.total}, {"elapsed_seconds", elapsed}, {"split", "train and val"},
    {"images_per_second", elapsed > 0 ? static_cast<double>(pixels.completed) / elapsed : 0},
    {"eta_seconds", pixels.completed != 0 ? static_cast<double>(pixels.total - pixels.completed) * elapsed / static_cast<double>(pixels.completed) : 0}};
  });
 }
}
void ProgressReporter::acquisition_complete() {
 if (!callback_) return;
 const std::lock_guard lock(mutex_);
 if (state_.tracks.acquisition.complete) return;
 state_.tracks.acquisition.active = false;
 state_.tracks.acquisition.complete = true;
 state_.tracks.acquisition.activity = DatasetCompileActivity::Complete;
 emit();
}
void ProgressReporter::source_activity(const BenchmarkDatasetSource source, std::string activity, bool foreground) {
 if (!callback_ && !*trace_) { return; }
 const std::lock_guard lock(mutex_);
 if (!callback_) {
  if (state_.activity != activity || state_.current_source != source) { trace_activity(source, activity); }
  if (foreground) {
   state_.current_source = source;
   state_.activity = std::move(activity);
  }
  return;
 }
 set_source_activity_unlocked(source, std::move(activity), foreground);
 emit();
}
void ProgressReporter::set_source_activity_unlocked(const BenchmarkDatasetSource source, std::string activity, bool foreground) {
 BenchmarkSourceProgress& source_state = source_progress(source);
 if (source_state.activity != activity || state_.current_source != source) { trace_activity(source, activity); }
 if (source_state.activity != activity) source_state.transfer.reset();
 source_state.complete = false;
 source_state.activity = activity.substr(0, kDatasetCompileProgressTextCapacity);
 if (foreground) {
  state_.current_source = source;
  set_activity_unlocked(std::move(activity));
 }
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
void ProgressReporter::source_images(const BenchmarkDatasetSource source, const std::uint64_t completed, const std::uint64_t total, std::string activity) {
 if (!callback_) { return; }
 const std::lock_guard lock(mutex_);
 BenchmarkSourceProgress& progress = source_progress(source);
 if (completed < progress.completed_images) add_progress(progress.invalidated_images, progress.completed_images - completed, "benchmark source invalidation overflow");
 if (!activity.empty()) set_source_activity_unlocked(source, std::move(activity));
 progress.completed_images = completed;
 progress.total_images = total;
 select_acquisition_source(source);
 update_acquisition();
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
void ProgressReporter::source_transfer(const DownloadProgress& update, const BenchmarkSourceProgress& aggregate) {
 if (!callback_) { return; }
 const std::lock_guard lock(mutex_);
 const auto source = update.source;
 auto& progress = source_progress(source);
 std::string operation;
 if (update.cache_hit) {
  operation = "Reusing cached ";
 } else {
  switch (update.phase) {
   case DownloadProgressPhase::kDownloading: operation = update.redownload ? (update.resumed ? "Resuming re-download of " : "Re-downloading ") : (update.resumed ? "Resuming " : "Downloading "); break;
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
 progress.activity = operation.substr(0, kDatasetCompileProgressTextCapacity);
 progress.transfer = BenchmarkTransferProgress{.completed_bytes = update.completed_bytes, .total_bytes = update.total_bytes,
  .retained_bytes = update.retained_bytes, .attempt = update.attempt, .cache_hit = update.cache_hit, .resumed = update.resumed};
 progress.completed_bytes = aggregate.completed_bytes;
 progress.total_bytes = aggregate.total_bytes;
 progress.byte_total_known = aggregate.byte_total_known;
 progress.retry_count = aggregate.retry_count;
 progress.cache_hit = aggregate.cache_hit;
 progress.resumed = aggregate.resumed;
 update_acquisition();
 emit();
}
void ProgressReporter::source_complete(const BenchmarkDatasetSource source, const bool cache_hit) {
 if (!callback_) { return; }
 const std::lock_guard lock(mutex_);
 BenchmarkSourceProgress& progress = source_progress(source);
 progress.transfer.reset();
 progress.complete = true;
 progress.cache_hit = cache_hit;
 progress.activity = cache_hit ? "Cache hit" : "Complete";
 update_acquisition();
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
void ProgressReporter::update_acquisition() {
 auto& acquisition = state_.tracks.acquisition;
 const auto previous = acquisition.completed;
 acquisition.completed = 0;
 acquisition.total = 0;
 acquisition.total_known = true;
 for (const auto& source : state_.sources) {
  acquisition.total_known = acquisition.total_known && source.byte_total_known;
  add_progress(acquisition.completed, source.completed_bytes, "benchmark acquisition count overflow");
  add_progress(acquisition.total, source.total_bytes, "benchmark acquisition total overflow");
 }
 if (acquisition.completed < previous) add_progress(acquisition.invalidated, previous - acquisition.completed, "benchmark acquisition invalidation overflow");
 if (!acquisition.total_known) acquisition.total = 0;
 acquisition.complete = false;
 acquisition.active = true;
 acquisition.activity = DatasetCompileActivity::Acquiring;
 if (state_.phase == DatasetCompilePhase::Downloading || state_.phase == DatasetCompilePhase::Extracting) {
  state_.completed = acquisition.completed;
  state_.total = acquisition.total;
 }
}
IndexingProgressTotals::IndexingProgressTotals(std::span<const std::uint64_t> totals) {
 releases_.reserve(totals.size());
 for (const auto total : totals) {
  if (total > std::numeric_limits<std::uint64_t>::max() - total_) throw std::overflow_error("benchmark indexing total overflow");
  total_ += total;
  releases_.push_back({0, total});
 }
}
void IndexingProgressTotals::update(std::size_t release, std::uint64_t completed, ProgressReporter& reporter) {
 if (!reporter.normalization_observer_enabled()) return;
 const std::lock_guard lock(mutex_);
 auto& observation = releases_.at(release);
 // Ordinary observations remain monotonic within a retained release product.
 // Replacement explicitly withdraws that product before new callbacks begin.
 const auto admitted = std::max(observation.completed, std::min(completed, observation.total));
 completed_ += admitted - observation.completed;
 observation.completed = admitted;
 reporter.phase(DatasetCompilePhase::Indexing, completed_, total_);
}
void IndexingProgressTotals::invalidate(std::size_t release, ProgressReporter& reporter) {
 if (!reporter.normalization_observer_enabled()) return;
 const std::lock_guard lock(mutex_);
 auto& observation = releases_.at(release);
 const auto withdrawn = observation.completed;
 completed_ -= withdrawn;
 observation.completed = 0;
 reporter.invalidate_indexing(withdrawn);
}
void ArtifactProgressTotals::update(const DownloadProgress& update, ProgressReporter& reporter) {
 if (!reporter.transfer_observer_enabled()) { return; }
 const std::lock_guard lock(mutex_);
 const auto source = update.source;
 auto& totals = sources_[source];
 const auto previous = totals.artifacts.find(update.artifact_id);
 const bool observed = previous != totals.artifacts.end();
 const Observation old = observed ? previous->second : Observation{};
 const auto completed = replace_progress(totals.completed, old.completed, update.completed_bytes);
 const auto known_total = replace_progress(totals.known_total, old.total, update.total_bytes);
 const auto unknown_count = replace_progress(totals.unknown_count, observed && old.total == 0U ? 1U : 0U, update.total_bytes == 0U ? 1U : 0U);
 const auto retries = std::max(old.retries, update.attempt > 0 ? static_cast<std::uint64_t>(update.attempt - 1U) : 0U);
 const bool resumed = old.resumed || update.resumed;
 const auto source_retries = replace_progress(totals.retries, old.retries, retries);
 const auto source_resumed = replace_progress(totals.resumed, old.resumed, resumed);
 const auto source_cached = replace_progress(totals.cached, old.cached, update.cache_hit);
 totals.artifacts.insert_or_assign(update.artifact_id, Observation{update.completed_bytes, update.total_bytes, retries, resumed, update.cache_hit});
 totals.retries = source_retries;
 totals.resumed = source_resumed;
 totals.cached = source_cached;
 totals.completed = completed;
 totals.known_total = known_total;
 totals.unknown_count = unknown_count;
 reporter.source_transfer(update, BenchmarkSourceProgress{.source = source,
                                   .activity = {},
                                   .completed_bytes = completed,
                                   .total_bytes = unknown_count == 0U ? known_total : 0U,
                                   .retry_count = source_retries,
                                   .cache_hit = source_cached == totals.artifacts.size(),
                                   .resumed = source_resumed != 0,
                                   .byte_total_known = unknown_count == 0U});
}
void ArtifactProgressTotals::images(BenchmarkDatasetSource source, const std::string& artifact, std::uint64_t completed, std::uint64_t source_total, ProgressReporter& reporter, std::string activity) {
 if (!reporter.transfer_observer_enabled()) return;
 const std::lock_guard lock(mutex_);
 auto& source_state = sources_[source];
 const auto found = source_state.images.find(artifact);
 const auto previous = found == source_state.images.end() ? 0 : found->second;
 const auto aggregate = replace_progress(source_state.completed_images, previous, completed);
 if (aggregate > source_total) throw std::overflow_error("benchmark source images exceed admitted total");
 source_state.images.insert_or_assign(artifact, completed);
 source_state.completed_images = aggregate;
 reporter.source_images(source, aggregate, source_total, std::move(activity));
}
void ProgressReporter::emit() {
 if (callback_) {
  if (activity_started_.time_since_epoch().count() != 0) {
   state_.activity_elapsed_seconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - activity_started_).count());
  }
  callback_(state_);
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
