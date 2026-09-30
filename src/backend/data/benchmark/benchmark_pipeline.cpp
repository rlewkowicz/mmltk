#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/detail/compile_pixel_workspace.h"
#include "src/backend/data/detail/compile_workspace.h"
#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/backend/data/benchmark/detail/benchmark_image_input.h"
#include "src/backend/data/benchmark/detail/benchmark_writer.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/backend/data/benchmark/detail/benchmark_curl.h"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {


using detail::CompileExecution;
using detail::CompileTaskKind;
using detail::CompileWorkClass;
using detail::CompileResources;
using detail::CompileAllowance;
struct BenchmarkCompilePipeline::Impl : std::enable_shared_from_this<Impl> {
 struct Slot;
 struct Label;
 struct LabelWork;
 struct Job {
  CompileTaskKind stage = CompileTaskKind::Header;
  CompileResources resources{};
  CompileAllowance parent, allowance;
  CompileExecution::Completion handle;
  CompileExecution::CompletionSource completion;
  Slot* slot = nullptr;
  Job* next = nullptr;
  bool queued = false, done = false, independent = false;
  std::exception_ptr failure;
 };
 struct Source;
 struct Slot {
  Job job;
  Source* source = nullptr;
  BenchmarkSplitWriter* writer = nullptr;
  std::size_t index = 0;
  std::uint64_t image_id = 0;
  BenchmarkSourceGeneration generation = 0;
  bool submitted = false, retiring = false, reopen = false;
  std::shared_ptr<const detail::CompilePixelWorkspacePlan> plan;
  CompileExecution::Producer headers, pixels;
  BenchmarkSourcePublication publication;
  std::shared_ptr<BenchmarkPixelInput> input;
  std::shared_ptr<const BenchmarkEncodedImage> payload;
  CompileAllowance pixel_allowance;
  std::mutex label_write;
  Label* selected_label = nullptr;
 };
 // One physical image can have independent annotation-edition products (for
 // example overlapping Large/XL membership). They remain under this sole image
 // owner; replacing one dependency never evicts another required conversion.
 struct Image : Slot {
  std::uint32_t width = 0, height = 0;
  std::shared_ptr<const BenchmarkEncodedImage> encoded;
  std::size_t pending_labels = 0;
  std::vector<std::shared_ptr<Label>> labels;
  [[nodiscard]] Label* find_labels(std::string_view dependency) const;
 };
 struct Label {
  Image* image = nullptr;
  std::string dependency;
  std::shared_ptr<const BenchmarkLabelChunk> product;
  std::optional<BenchmarkLabelInput> input;
  std::uint64_t attempt = 0, generation = 0;
  unsigned priority = 0;
  Label* previous = nullptr;
  Label* next = nullptr;
  bool waiting = false, retired = false;
  LabelWork* active = nullptr;
  CompileExecution::CompletionSource completion;
  CompileExecution::Producer producer;
  [[nodiscard]] bool needs_replay() const;
 };
 // Only admitted claims create execution records; pending products remain
 // source facts. The kernel bounds their combined active/suspended population.
 struct LabelWork {
  CompileExecution::Completion handle;
  Label* label = nullptr;
  std::optional<BenchmarkLabelInput> input;
  BenchmarkSourceGeneration generation = 0;
  std::uint32_t width = 0, height = 0;
 };
 struct Source {
  std::filesystem::path root;
  BenchmarkSourceGeneration generation = 1, counter = 1;
  std::uint64_t label_generation = 1, label_attempt = 0;
  std::unordered_map<std::uint64_t, BenchmarkSourceGeneration> replacements;
  BenchmarkSourceGeneration image_generation(std::uint64_t id) const {
   const auto found = replacements.find(id);
   return found == replacements.end() ? generation : found->second;
  }
  bool retiring = false;
  std::size_t pending = 0, pending_labels = 0;
  std::vector<BenchmarkSplitWriter*> writers;
  BenchmarkSplitWriter* incremental_writer = nullptr;
  AnnotationSource annotation_source = AnnotationSource::Generic;
  bool labels_retiring = false;
  std::unordered_map<std::uint64_t, Image> images;
 };
 // The sole mutable physical image owner. Scheduler jobs borrow stable slots;
 // source identity is bound once outside per-image production/registration.
 class ImageState {
 public:
  explicit ImageState(Impl& execution) : execution_(execution) {}
  Source& source(const std::filesystem::path&);
  [[nodiscard]] const Source* find(const std::filesystem::path&) const;
  // Caller holds the execution mutex; a current image need not have geometry yet.
  [[nodiscard]] const Image* find_image(const std::filesystem::path&, std::uint64_t) const;
  Slot* bind_image(Source&, std::uint64_t);
  void admit(Slot&, bool independent);
  void publish(Source&, std::uint64_t, BenchmarkSourceGeneration, BenchmarkSourcePublication, std::optional<std::pair<std::uint32_t, std::uint32_t>>, bool, std::uint64_t attempt,
   std::shared_ptr<const BenchmarkEncodedImage>, const std::filesystem::path&);
  void geometry_ready(Source&, std::uint64_t, BenchmarkSourceGeneration, std::pair<std::uint32_t, std::uint32_t>);
  [[nodiscard]] std::optional<BenchmarkImageGeometry> geometry(const std::filesystem::path&, std::uint64_t) const;
  void ready_labels(Label&);
  void unlink_labels(Label&);
  void withdraw_labels(Image&);
  void admit_labels();
  void withdraw_all_labels() {
   for (auto& [root, source] : sources_) {
    (void)root;
    for (auto& [id, image] : source.images) { (void)id; withdraw_labels(image); }
   }
  }
  [[nodiscard]] const Label* waiting_labels() const { return labels_head_; }
  void retire_image(const std::filesystem::path&, std::uint64_t);
  void retire_source(const std::filesystem::path&);
  [[nodiscard]] std::uint64_t attempt() const { return attempt_; }
  void close() {
   ++attempt_;
   for (auto& [root, source] : sources_) {
    (void)root;
    for (auto& [id, image] : source.images) {
     (void)id;
     withdraw_labels(image);
     image.selected_label = nullptr;
     image.labels.clear();
    }
    source.images.clear();
    source.writers.clear();
    source.incremental_writer = nullptr;
   }
   admitted_ = false;
   labels_head_ = labels_tail_ = nullptr;
  }

 private:
  Impl& execution_;
  std::unordered_map<std::filesystem::path, Source> sources_;
  std::uint64_t attempt_ = 1;
  bool admitted_ = false;
  Label* labels_head_ = nullptr;
  Label* labels_tail_ = nullptr;
 };
 CompileExecution execution;
 std::mutex mutex;
 std::condition_variable changed;
 ImageState images{*this};
 std::deque<Job*> pixels;
 struct RetiredCompletion { CompileExecution::CompletionSource completion; CompileExecution::Producer producer; };
 std::vector<RetiredCompletion> retired_completions;
 std::size_t pending = 0;
 bool membership_pending = true, stopping = false, labels_first = true;
 std::exception_ptr failure;
 const std::exception_ptr retired_failure = std::make_exception_ptr(std::runtime_error("benchmark work retired before completion"));
 mmltk::common::concurrency::CancellationObservation cancellation;
 std::uint32_t label_resolution = 0;
 mmltk::backend::imaging::resample::ImageResizeMode label_resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
 std::function<void(const std::filesystem::path&, std::uint64_t, const BenchmarkLabelWorkspace&)> label_started;
 std::function<void(const std::filesystem::path&, std::uint64_t)> label_completed;
 std::vector<BenchmarkLabelWorkspace> label_workspaces;
 std::vector<std::unique_ptr<detail::CompilePixelWorkspace>> pixel_workspaces;
 std::mutex curl_mutex;
 std::unique_ptr<BenchmarkCurl> curl;
 StorageReservationPool storage{".", {}};
 std::unique_ptr<CompileExecution::Readiness> readiness;
 Impl(std::size_t workers, std::span<const int> cpus, detail::CompileExecutionLimits limits, mmltk::common::concurrency::CancellationObservation cancel, BenchmarkTraceSink trace)
 : execution(workers, cpus, limits, cancel, trace ? CompileExecution::Observer{[trace](std::string_view event, const detail::CompileExecutionSnapshot& value) {
    if (value.reservation) {
     trace_benchmark_event(trace, "benchmark.resource.wait", [&] {
      const auto& request = *value.reservation;
      nlohmann::json result{{"file", request.location.file_name()}, {"line", request.location.line()}, {"function", request.location.function_name()},
       {"requested_bytes", request.requested.bytes}, {"requested_descriptors", request.requested.descriptors}, {"requested_continuation", request.requested.continuation_descriptors},
       {"producer", request.requested.producer}, {"held_bytes", value.resources.bytes}, {"byte_target", value.byte_target}, {"held_descriptors", value.resources.descriptors},
       {"committed_descriptors", value.resources.continuation_descriptors}, {"descriptor_limit", value.descriptor_limit}, {"parents", nlohmann::json::array()}, {"owners", nlohmann::json::array()}};
      for (const auto& credit : request.parents)
       result["parents"].push_back({{"bytes", credit.resources.bytes}, {"descriptors", credit.resources.descriptors}, {"available", credit.available}, {"borrowed", credit.borrowed},
        {"descriptors_retired", credit.descriptors_retired}, {"continuations_released", credit.continuations_released}});
      for (const auto& credit : request.owners)
       result["owners"].push_back({{"id", credit.id}, {"parent", credit.parent}, {"file", credit.location.file_name()}, {"line", credit.location.line()},
        {"bytes", credit.resources.bytes}, {"descriptors", credit.resources.descriptors}, {"available", credit.available}, {"borrowed", credit.borrowed}});
      return result;
     });
     return;
    }
    trace_benchmark_event(trace, event, [&] { return nlohmann::json{{"live", value.live}, {"baseline", value.baseline}, {"elastic", value.elastic}, {"ready", value.ready},
      {"parked", value.parked}, {"running", value.running}, {"active", value.active}, {"external", value.external}, {"services", value.services},
      {"bytes", value.resources.bytes}, {"byte_target", value.byte_target}, {"normal_locked",value.normal_locked}, {"resident",value.resident}, {"leased",value.leased}, {"exceptional",value.exceptional}, {"exceptional_peak",value.exceptional_peak}, {"opaque_library",value.opaque_library}, {"protected_workspace",value.protected_workspace}, {"shared_workspace",value.shared_workspace}, {"compaction_workspace",value.compaction_workspace}, {"workspace_contexts",value.workspace_contexts}, {"context_capacity",value.context_capacity}, {"exceptional_reserved",value.exceptional_reserved}, {"workspace_reuses",value.workspace_reuses}, {"physical_retirements",value.physical_retirements}, {"descriptors", value.resources.descriptors}, {"descriptor_limit", value.descriptor_limit}, {"descriptor_promises", value.resources.continuation_descriptors},
      {"records", value.records}, {"record_limit", value.record_limit}, {"admitted", value.admitted}, {"completed", value.completed}}; });
   }} : CompileExecution::Observer{}, std::make_exception_ptr(std::runtime_error("benchmark dataset compilation cancelled"))), cancellation(cancel), label_workspaces(execution.workers()) {
  for (std::size_t i=0; i<execution.workers(); ++i) pixel_workspaces.push_back(std::make_unique<detail::CompilePixelWorkspace>());
 }
 template <class Predicate> void wait(std::unique_lock<std::mutex>& lock, Predicate ready) {
  if (execution.on_lane()) throw std::logic_error("benchmark dependency wait inside a CPU lane");
  while (!ready()) {
   if (cancellation.descriptor() >= 0) changed.wait(lock);
   else { changed.wait_for(lock, std::chrono::milliseconds(100)); if (cancellation.requested()) execution.notify_admission_change(); }
  }
 }
 void check_admission() const {
  if (failure) std::rethrow_exception(failure);
  throw_if_benchmark_cancelled(cancellation);
  if (stopping) throw std::runtime_error("benchmark admission after retirement");
 }
 void push(Job& job) { job.queued = true; pixels.push_back(&job); execution.notify_admission_change(); }
 void remove(Job& job) { std::erase(pixels, &job); job.queued = false; }
 void settle(Job&, std::exception_ptr = {}, bool recoverable = false) noexcept;
 void settle_list(Job*, std::exception_ptr) noexcept;
 void dispatch();
 void header_input(Slot&);
 [[nodiscard]] bool submit_pixel(Job&);
 void run_pixel(Slot&, std::size_t, const CompileAllowance&);
 void run_label(LabelWork&, std::size_t);
};
struct BenchmarkSourcePublication::State {
 std::weak_ptr<BenchmarkCompilePipeline::Impl> execution;
 BenchmarkCompilePipeline::Impl::Source* source;
 std::shared_ptr<const ArtifactLease> custody;
 std::uint64_t attempt;
 BenchmarkSourceGeneration generation;
 std::optional<std::pair<std::uint64_t, BenchmarkSourceGeneration>> replacement;
 bool defer_pixels;
 CompileExecution::Producer headers, pixels;
 [[nodiscard]] BenchmarkSourceGeneration image_generation(std::uint64_t id) const { return replacement && replacement->first == id ? replacement->second : generation; }
};
void BenchmarkCompilePipeline::membership_ready() {
 const std::lock_guard lock(impl_->mutex);
 impl_->membership_pending = false;
}
BenchmarkCompilePipeline::Impl::Source& BenchmarkCompilePipeline::Impl::ImageState::source(const std::filesystem::path& root) {
 auto [position, inserted] = sources_.try_emplace(root);
 if (inserted) position->second.root = root;
 return position->second;
}
const BenchmarkCompilePipeline::Impl::Source* BenchmarkCompilePipeline::Impl::ImageState::find(const std::filesystem::path& root) const {
 const auto found = sources_.find(root);
 return found == sources_.end() ? nullptr : &found->second;
}
void BenchmarkCompilePipeline::bind_slot(const std::filesystem::path& root, std::uint64_t id, BenchmarkSplitWriter& writer, std::uint32_t index) {
 const std::lock_guard lock(impl_->mutex);
 auto& source = impl_->images.source(root);
 auto& slot = source.images[id];
 if (slot.writer && (slot.writer != &writer || slot.index != index)) throw std::logic_error("benchmark identity already owns a different slot");
 if (std::ranges::find(source.writers, &writer) == source.writers.end()) source.writers.push_back(&writer);
 slot.source = &source;
 slot.writer = &writer;
 slot.index = index;
 slot.image_id = id;
 slot.generation = source.image_generation(id);
 slot.payload = slot.encoded;
 slot.job.slot = &slot;
}
BenchmarkCompilePipeline::Impl::Slot* BenchmarkCompilePipeline::Impl::ImageState::bind_image(Source& source, std::uint64_t id) {
 if (!source.incremental_writer) return nullptr;
 auto [position, inserted] = source.images.try_emplace(id);
 auto& slot = position->second;
 if (inserted || !slot.writer) {
  slot.source = &source;
  slot.writer = source.incremental_writer;
  slot.image_id = id;
  slot.generation = source.image_generation(id);
  slot.index = slot.writer->allocate_slot({.source_image_id = id, .annotation_source = source.annotation_source}, {source.root});
  slot.job.slot = &slot;
 }
 return &slot;
}
void BenchmarkCompilePipeline::bind_source(const std::filesystem::path& root, BenchmarkSplitWriter& writer, AnnotationSource annotation_source) {
 const std::lock_guard lock(impl_->mutex);
 auto& source = impl_->images.source(root);
 if (source.incremental_writer && source.incremental_writer != &writer) throw std::logic_error("benchmark source has two output owners");
 source.incremental_writer = &writer;
 source.annotation_source = annotation_source;
 if (std::ranges::find(source.writers, &writer) == source.writers.end()) source.writers.push_back(&writer);
}
std::optional<std::uint32_t> BenchmarkCompilePipeline::image_slot(const std::filesystem::path& root, std::uint64_t id) const {
 const std::lock_guard lock(impl_->mutex);
 const auto* source = impl_->images.find(root);
 if (!source) return {};
 const auto found = source->images.find(id);
 return found == source->images.end() || !found->second.writer ? std::nullopt : std::optional(static_cast<std::uint32_t>(found->second.index));
}
void BenchmarkCompilePipeline::wait_image(const std::filesystem::path& root, std::uint64_t id) {
 std::unique_lock lock(impl_->mutex);
 auto& source = impl_->images.source(root);
 impl_->wait(lock, [&] {
  impl_->check_admission();
  const auto found = source.images.find(id);
  return found != source.images.end() && found->second.submitted && found->second.job.done;
 });
 if (source.images.at(id).job.failure) std::rethrow_exception(source.images.at(id).job.failure);
}
void BenchmarkCompilePipeline::Impl::header_input(Slot& slot) {
 auto& job = slot.job;
 job.resources = slot.payload && !slot.payload->encoded().empty() ? CompileResources{} : CompileResources::handles(1, false, 1);
 job.parent = slot.payload ? slot.payload->allowance() : CompileAllowance{};
 if (!job.parent && slot.publication.state_ && slot.publication.state_->custody) job.parent = slot.publication.state_->custody->allowance();
}
void BenchmarkCompilePipeline::Impl::ImageState::admit(Slot& slot, bool independent) {
 slot.headers = execution_.execution.open_class(CompileWorkClass::Extraction);
 slot.pixels = execution_.execution.open_class(CompileWorkClass::Pixels);
 slot.submitted = true;
 auto& job = slot.job;
 job.stage = CompileTaskKind::Header;
 execution_.header_input(slot);
 job.done = false;
 job.failure = {};
 job.completion = CompileExecution::CompletionSource{&execution_.execution};
 job.independent = independent;
 admitted_ = admitted_ || independent;
 ++execution_.pending;
 ++slot.source->pending;
 execution_.push(job);
}
BenchmarkCompilePipeline::Impl::Label* BenchmarkCompilePipeline::Impl::Image::find_labels(std::string_view dependency) const {
 const auto found = std::ranges::find_if(labels, [&](const auto& label) { return label->dependency == dependency; });
 return found == labels.end() ? nullptr : found->get();
}
bool BenchmarkCompilePipeline::Impl::Label::needs_replay() const {
 // Incremental writes release compiled vectors. A settled sibling claim may
 // still be indexed even though another claim replaced or cleared its bytes.
 return image->source->incremental_writer && product && !active && image->selected_label != this;
}
void BenchmarkCompilePipeline::Impl::ImageState::ready_labels(Label& label) {
 auto& image = *label.image;
 if (!label.input || (!image.width && !label.input->index().image(label.input->position()).width) || label.product || label.active || label.waiting || label.retired || image.retiring ||
     image.source->retiring || image.source->labels_retiring || execution_.stopping)
  return;
 label.previous = labels_tail_;
 label.next = nullptr;
 label.waiting = true;
 if (labels_tail_)
  labels_tail_->next = &label;
 else
  labels_head_ = &label;
 labels_tail_ = &label;
 execution_.execution.notify_admission_change();
}
void BenchmarkCompilePipeline::Impl::ImageState::unlink_labels(Label& label) {
 if (!label.waiting) return;
 if (label.previous)
  label.previous->next = label.next;
 else
  labels_head_ = label.next;
 if (label.next)
  label.next->previous = label.previous;
 else
  labels_tail_ = label.previous;
 label.previous = label.next = nullptr;
 label.waiting = false;
}
void BenchmarkCompilePipeline::Impl::ImageState::withdraw_labels(Image& image) {
 for (auto& label : image.labels) {
  if (label->retired) continue;
  unlink_labels(*label);
  label->retired = true;
  label->input.reset();
  label->product.reset();
  if (!label->active) { execution_.retired_completions.push_back({label->completion, std::move(label->producer)}); }
  else label->active->handle.cancel();
 }
}
void BenchmarkCompilePipeline::Impl::ImageState::retire_image(const std::filesystem::path& root, std::uint64_t id) {
 auto& execution = execution_;
 std::unique_lock lock(execution.mutex);
 auto& value = source(root);
 auto [replacement, inserted] = value.replacements.try_emplace(id, 0);
 (void)inserted;
 const auto generation = ++value.counter;
 replacement->second = generation;
 execution.execution.notify_admission_change();
 const auto found_image = value.images.find(id);
 auto* image = found_image == value.images.end() ? nullptr : &found_image->second;
 if (image) {
  image->retiring = true;
  withdraw_labels(*image);
 }
 const auto found = value.images.find(id);
 auto* slot = found == value.images.end() || !found->second.writer ? nullptr : &found->second;
 Job* detached = nullptr;
 if (slot) {
  slot->retiring = true;
  slot->job.handle.cancel();
  if (slot->job.queued) {
   execution.remove(slot->job);
   slot->job.next = detached;
   detached = &slot->job;
  }
 }
 lock.unlock();
 execution.settle_list(detached, execution.retired_failure);
 lock.lock();
 execution.changed.notify_all();
 execution.wait(lock, [&] { return (!slot || !slot->submitted || slot->job.done) && (!image || image->pending_labels == 0); });
 if (image) {
  image->width = image->height = 0;
  image->encoded.reset();
  image->plan.reset();
  image->selected_label = nullptr;
  image->labels.clear();
  image->generation = generation;
 }
 if (slot) {
  slot->submitted = false;
  slot->job.failure = {};
  slot->generation = generation;
  lock.unlock();
  slot->writer->invalidate_image(slot->index);
  slot->writer->invalidate_labels(static_cast<std::uint32_t>(slot->index));
  lock.lock();
 }
 if (image) image->retiring = false;
 lock.unlock();
 execution.changed.notify_all();
}
void BenchmarkCompilePipeline::Impl::ImageState::geometry_ready(Source& source, std::uint64_t id, BenchmarkSourceGeneration generation, std::pair<std::uint32_t, std::uint32_t> dimensions) {
 if (execution_.stopping || source.retiring || source.image_generation(id) != generation) return;
 const auto slot = source.images.find(id);
 if (slot != source.images.end() && slot->second.retiring) return;
 if (!dimensions.first || !dimensions.second) throw std::invalid_argument("benchmark geometry is empty");
 auto [position, inserted] = source.images.try_emplace(id);
 if (!inserted && position->second.width && (position->second.width != dimensions.first || position->second.height != dimensions.second))
  throw std::runtime_error("benchmark source generation has contradictory geometry");
 auto& image = position->second;
 image.generation = generation;
 image.width = dimensions.first;
 image.height = dimensions.second;
 image.source = &source;
 image.image_id = id;
 for (auto& label : image.labels) ready_labels(*label);
}
std::optional<BenchmarkImageGeometry> BenchmarkCompilePipeline::Impl::ImageState::geometry(const std::filesystem::path& root, std::uint64_t id) const {
 const std::lock_guard lock(execution_.mutex);
 const auto* image = find_image(root, id);
 if (!image || !image->width) return std::nullopt;
 return BenchmarkImageGeometry{image->source->root, id, image->generation, image->width, image->height};
}
const BenchmarkCompilePipeline::Impl::Image* BenchmarkCompilePipeline::Impl::ImageState::find_image(const std::filesystem::path& root, std::uint64_t id) const {
 const auto* source = find(root);
 if (!source || source->retiring) return nullptr;
 const auto found = source->images.find(id);
 if (found == source->images.end() || found->second.generation != source->image_generation(id)) return nullptr;
 return &found->second;
}
void BenchmarkCompilePipeline::Impl::ImageState::publish(Source& source, std::uint64_t id, BenchmarkSourceGeneration generation, BenchmarkSourcePublication publication,
 std::optional<std::pair<std::uint32_t, std::uint32_t>> dimensions, bool defer_pixels, std::uint64_t attempt, std::shared_ptr<const BenchmarkEncodedImage> payload,
 const std::filesystem::path& input_root) {
 auto& execution = execution_;
 Job* accepted = nullptr;
 {
  const std::lock_guard lock(execution.mutex);
  // Check captured attempt before consulting borrowed writers or reporting a
  // new attempt's failures to an old producer. Shutdown also closes tickets.
  if (attempt != attempt_ || execution.stopping) return;
  if (execution.failure) std::rethrow_exception(execution.failure);
  if (source.retiring || generation != source.image_generation(id)) return;
  if (dimensions) geometry_ready(source, id, generation, *dimensions);
  if (payload) {
   geometry_ready(source, id, generation, {payload->header().width, payload->header().height});
   source.images.at(id).encoded = payload->storage() == BenchmarkEncodedImage::Storage::HeaderOnly && !payload->allowance() ? payload : payload->header_only();
  }
  (void)bind_image(source, id);
  const auto found = source.images.find(id);
  if (found == source.images.end() || !found->second.writer || found->second.submitted || found->second.retiring) {
   execution.changed.notify_all();
   return;
  }
  auto& slot = found->second;
  slot.generation = generation;
  if (!input_root.empty()) slot.writer->input_root(static_cast<std::uint32_t>(slot.index), input_root);
  slot.publication = std::move(publication);
  slot.payload = payload ? std::move(payload) : source.images.contains(id) ? source.images.at(id).encoded : nullptr;
  admit(slot, true);
  // Only a bounded admitted Header record may retain charged source bytes
  // before its pixel plan exists. A full record bank must not strand a source
  // arena behind parked pixels that need that same arena to drain.
  if (slot.payload && slot.payload->charged_bytes()) {
   if (execution.submit_pixel(slot.job)) {
    execution.pixels.pop_back();
    slot.job.queued = false;
   } else {
    slot.payload = slot.payload->file_backing() ? slot.payload->file_backing() : slot.encoded;
    execution.header_input(slot);
   }
  }
  accepted = &slot.job;
 }
 execution.changed.notify_all();
 if (execution.execution.workers() == 1 && !defer_pixels && !execution.execution.on_lane()) {
  std::unique_lock lock(execution.mutex);
  execution.wait(lock, [&] { return attempt != attempt_ || accepted->done; });
  if (execution.failure) std::rethrow_exception(execution.failure);
 }
}
void BenchmarkCompilePipeline::Impl::ImageState::retire_source(const std::filesystem::path& root) {
 auto& execution = execution_;
 Job* retired = nullptr;
 std::unique_lock lock(execution.mutex);
 auto& source = this->source(root);
 source.retiring = true;
 source.generation = ++source.counter;
 source.replacements.clear();
 execution.execution.notify_admission_change();
 for (auto& [id, image] : source.images) {
  (void)id;
  withdraw_labels(image);
 }
 // Every canonical source slot already owns its intrusive job handle. Visit
 // only affected work; unrelated stage queues and writer slots stay untouched.
 for (auto& [id, slot] : source.images) {
  (void)id;
  slot.job.handle.cancel();
  if (!slot.job.queued) continue;
  execution.remove(slot.job);
  slot.job.next = retired;
  retired = &slot.job;
 }
 lock.unlock();
 execution.settle_list(retired, execution.retired_failure);
 lock.lock();
 execution.changed.notify_all();
 execution.wait(lock, [&] { return source.pending == 0; });
 for (auto& [id, image] : source.images) {
  (void)id;
  image.width = image.height = 0;
  image.encoded.reset();
  image.plan.reset();
  image.selected_label = nullptr;
  image.labels.clear();
 }
 for (auto& [id, slot] : source.images) {
  (void)id;
  slot.submitted = false;
  slot.job.failure = {};
  slot.generation = source.generation;
 }
 lock.unlock();
 // The source flag remains closed across callbacks, which may throw. Completed
 // siblings in other sources are never invalidated by this owner.
 for (auto* writer : source.writers) writer->invalidate_source(root);
 for (auto& [id, slot] : source.images) {
  (void)id;
  if (!slot.writer) continue;
  slot.writer->invalidate_labels(static_cast<std::uint32_t>(slot.index));
 }
 lock.lock();
 source.retiring = false;
 lock.unlock();
 execution.changed.notify_all();
}
void BenchmarkCompilePipeline::retire_labels(const std::filesystem::path& root, std::uint64_t id, std::string_view dependency) {
 std::unique_lock lock(impl_->mutex);
 auto& source = impl_->images.source(root);
 const auto found = source.images.find(id);
 if (found == source.images.end()) return;
 auto& image = found->second;
 const auto position = std::ranges::find_if(image.labels, [&](const auto& value) { return value->dependency == dependency; });
 if (position == image.labels.end()) return;
 // Keep the exact dependency visible until its write and invalidation settle.
 // A simultaneous generation retirement may clear the image's references,
 // while this shared custody still protects the borrower and selected identity.
 auto label = *position;
 impl_->images.unlink_labels(*label);
 label->retired = true;
 label->input.reset();
 label->product.reset();
 if (label->active) label->active->handle.cancel();
 else { impl_->retired_completions.push_back({label->completion, std::move(label->producer)}); }
 impl_->execution.notify_admission_change();
 impl_->changed.notify_all();
 impl_->wait(lock, [&] { return label->active == nullptr; });
 lock.unlock();
 {
  const std::lock_guard write(image.label_write);
  const std::lock_guard state(impl_->mutex);
  if (image.writer && image.selected_label == label.get()) {
   image.writer->invalidate_labels(static_cast<std::uint32_t>(image.index));
   image.selected_label = nullptr;
  }
  std::erase(image.labels, label);
 }
 lock.lock();
 impl_->execution.notify_admission_change();
 impl_->changed.notify_all();
}
void BenchmarkCompilePipeline::original_generation(const std::filesystem::path& root, std::uint64_t original_generation, bool withdrawn) {
 std::unique_lock lock(impl_->mutex);
 auto& source = impl_->images.source(root);
 if (source.label_attempt == impl_->images.attempt() && original_generation < source.label_generation) return;
 source.label_attempt = impl_->images.attempt();
 const bool changed = source.label_generation != original_generation;
 source.label_generation = original_generation;
 impl_->execution.notify_admission_change();
 if (withdrawn || changed) {
  source.labels_retiring = true;
  for (auto& [id, image] : source.images) {
   (void)id;
   impl_->images.withdraw_labels(image);
  }
  impl_->changed.notify_all();
  // Only label consumers retire. Geometry, encoded custody and pixel jobs stay live.
  impl_->wait(lock, [&] { return source.pending_labels == 0; });
  for (auto& [id, image] : source.images) {
   (void)id;
   image.selected_label = nullptr;
   image.labels.clear();
   if (const auto found = source.images.find(id); found != source.images.end() && found->second.writer) { found->second.writer->invalidate_labels(static_cast<std::uint32_t>(found->second.index)); }
  }
  source.labels_retiring = false;
 }
 impl_->changed.notify_all();
}
void BenchmarkCompilePipeline::label_configuration(std::uint32_t resolution, mmltk::backend::imaging::resample::ImageResizeMode mode,
 std::function<void(const std::filesystem::path&, std::uint64_t, const BenchmarkLabelWorkspace&)> observer, std::function<void(const std::filesystem::path&, std::uint64_t)> completed) {
 const std::lock_guard lock(impl_->mutex);
 impl_->label_resolution = resolution;
 impl_->label_resize_mode = mode;
 impl_->label_started = std::move(observer);
 impl_->label_completed = std::move(completed);
}
std::shared_ptr<const BenchmarkLabelChunk> BenchmarkCompilePipeline::image_labels(const std::filesystem::path& root, std::uint64_t id, std::string_view dependency) const {
 const std::lock_guard lock(impl_->mutex);
 const auto* image = impl_->images.find_image(root, id);
 const auto* label = image ? image->find_labels(dependency) : nullptr;
 return label && !label->retired ? label->product : nullptr;
}
bool BenchmarkCompilePipeline::has_image_labels(const std::filesystem::path& root, std::uint64_t id, std::string_view dependency, std::uint64_t original_generation) const {
 const std::lock_guard lock(impl_->mutex);
 impl_->check_admission();
 const auto* source = impl_->images.find(root);
 if (!source || source->retiring || source->labels_retiring || (original_generation && original_generation != source->label_generation)) return false;
 const auto found = source->images.find(id);
 if (found == source->images.end() || found->second.retiring || found->second.generation != source->image_generation(id)) return false;
 const auto* label = found->second.find_labels(dependency);
 return label && !label->retired && label->attempt == impl_->images.attempt() && label->generation == source->label_generation && !label->needs_replay();
}
std::shared_ptr<const BenchmarkLabelChunk> BenchmarkCompilePipeline::wait_image_labels(
 const std::filesystem::path& root, std::uint64_t id, std::string_view dependency, std::uint64_t original_generation) {
 if (impl_->execution.on_lane()) throw std::logic_error("benchmark label dependency wait inside a CPU lane");
 std::unique_lock lock(impl_->mutex);
 const auto attempt = impl_->images.attempt();
 std::shared_ptr<const BenchmarkLabelChunk> product;
 impl_->wait(lock, [&] {
  product.reset();
  impl_->check_admission();
  const auto* source = impl_->images.find(root);
  if (attempt != impl_->images.attempt() || !source || source->retiring || source->labels_retiring || (original_generation && original_generation != source->label_generation)) return true;
  const auto found = source->images.find(id);
  if (found == source->images.end() || found->second.generation != source->image_generation(id)) return true;
  const auto& image = found->second;
  const auto slot = source->images.find(id);
  if (!image.width && slot != source->images.end() && slot->second.job.done && slot->second.job.failure) std::rethrow_exception(slot->second.job.failure);
  const auto* label = image.find_labels(dependency);
  if (image.retiring || !label || label->retired || label->attempt != attempt || label->generation != source->label_generation) return true;
  product = label->product;
  return (product && !label->active) || (!impl_->membership_pending && !image.writer && !image.width);
 });
 return product;
}
BenchmarkSourceGeneration BenchmarkCompilePipeline::source_generation(const std::filesystem::path& root) {
 const std::lock_guard lock(impl_->mutex);
 return impl_->images.source(root).generation;
}
BenchmarkSourceGeneration BenchmarkCompilePipeline::image_generation(const std::filesystem::path& root, std::uint64_t id) {
 const std::lock_guard lock(impl_->mutex);
 return impl_->images.source(root).image_generation(id);
}
void BenchmarkCompilePipeline::retire_image(const std::filesystem::path& root, std::uint64_t id) { impl_->images.retire_image(root, id); }
void BenchmarkCompilePipeline::retire_source(const std::filesystem::path& root) { impl_->images.retire_source(root); }
void BenchmarkCompilePipeline::geometry_ready(const BenchmarkImageGeometry& fact) {
 const std::lock_guard lock(impl_->mutex);
 impl_->images.geometry_ready(impl_->images.source(fact.root), fact.image_id, fact.generation, {fact.width, fact.height});
 impl_->changed.notify_all();
}
std::shared_ptr<const BenchmarkEncodedImage> BenchmarkCompilePipeline::image_input(const std::filesystem::path& root, std::uint64_t id) const {
 const std::lock_guard lock(impl_->mutex);
 const auto* image = impl_->images.find_image(root, id);
 return image ? image->encoded : nullptr;
}
std::optional<BenchmarkImageGeometry> BenchmarkCompilePipeline::geometry(const std::filesystem::path& root, std::uint64_t id) const { return impl_->images.geometry(root, id); }
bool BenchmarkCompilePipeline::labels_ready(
 const BenchmarkSourcePublication& publication, std::uint64_t id, BenchmarkLabelInput input, std::string_view dependency, std::uint64_t original_generation, unsigned priority) {
 if (!publication.state_ || publication.state_->execution.lock() != impl_) return false;
 const auto& ticket = *publication.state_;
 const auto generation = ticket.image_generation(id);
 auto& source = *ticket.source;
 {
  const std::lock_guard lock(impl_->mutex);
  throw_if_benchmark_cancelled(impl_->cancellation);
  if (!impl_->label_resolution || impl_->stopping || ticket.attempt != impl_->images.attempt() || source.retiring || source.labels_retiring || generation != source.image_generation(id) ||
      (original_generation && original_generation != source.label_generation))
   return false;
  impl_->check_admission();
  if (input.index().image(input.position()).source_image_id != id) throw std::invalid_argument("benchmark label input has a different physical image");
  auto& image = source.images.try_emplace(id).first->second;
  if (image.retiring) return false;
  auto* label = image.find_labels(dependency);
  if (label) {
   if (label->retired || label->attempt != ticket.attempt || label->generation != source.label_generation) return false;
   if (!label->needs_replay()) return true;
   label->product.reset();
   label->completion = CompileExecution::CompletionSource{&impl_->execution};
  } else {
   (void)impl_->images.bind_image(source, id);
   image.labels.push_back(std::make_shared<Impl::Label>());
   label = image.labels.back().get();
   label->image = &image;
   label->attempt = ticket.attempt;
   label->generation = source.label_generation;
   label->dependency = dependency;
   label->completion = CompileExecution::CompletionSource{&impl_->execution};
  }
  label->producer = impl_->execution.open_class(CompileWorkClass::Annotations);
  label->priority = priority;
  image.generation = generation;
  image.source = &source;
  image.image_id = id;
  label->input = std::move(input);
  impl_->images.ready_labels(*label);
 }
 impl_->changed.notify_all();
 return true;
}
BenchmarkSourcePublication BenchmarkCompilePipeline::source_publication(
 const std::filesystem::path& root, std::shared_ptr<const ArtifactLease> custody, std::optional<std::uint64_t> repaired_image, bool defer_pixels) {
 const std::lock_guard lock(impl_->mutex);
 impl_->check_admission();
 auto& source = impl_->images.source(root);
 return BenchmarkSourcePublication(std::make_shared<BenchmarkSourcePublication::State>(impl_, &source, std::move(custody), impl_->images.attempt(), source.generation,
  repaired_image ? std::optional(std::pair{*repaired_image, source.image_generation(*repaired_image)}) : std::nullopt, defer_pixels,
  source.incremental_writer || !source.writers.empty() ? impl_->execution.open_class(CompileWorkClass::Extraction) : CompileExecution::Producer{},
  source.incremental_writer || !source.writers.empty() ? impl_->execution.open_class(CompileWorkClass::Pixels) : CompileExecution::Producer{}));
}
void BenchmarkSourcePublication::operator()(const CachedImageReady& ready) const {
 if (!state_) return;
 if (auto execution = state_->execution.lock())
  execution->images.publish(
   *state_->source, ready.image_id, state_->image_generation(ready.image_id), *this, ready.dimensions, ready.defer_pixels || state_->defer_pixels, state_->attempt, ready.payload, ready.input_root);
}
bool BenchmarkSourcePublication::consume(const CachedImageReady& ready) const {
 if (!state_) return false;
 auto execution = state_->execution.lock();
 if (!execution) return false;
 {
  const std::lock_guard lock(execution->mutex);
  if (state_->attempt != execution->images.attempt()) throw std::runtime_error("benchmark repaired image attempt retired");
  if (!state_->source->images.contains(ready.image_id) || !state_->source->images.at(ready.image_id).writer) return false;
 }
 execution->images.publish(*state_->source, ready.image_id, state_->image_generation(ready.image_id), *this, ready.dimensions, false, state_->attempt, ready.payload, ready.input_root);
 std::unique_lock lock(execution->mutex);
 const auto retired = [&] {
  return execution->stopping || state_->attempt != execution->images.attempt() || state_->source->retiring ||
         state_->source->image_generation(ready.image_id) != state_->image_generation(ready.image_id);
 };
 execution->wait(lock, [&] { return retired() || state_->source->images.at(ready.image_id).job.done; });
 if (retired()) throw std::runtime_error("benchmark repaired image generation retired");
 if (const auto failure = state_->source->images.at(ready.image_id).job.failure) std::rethrow_exception(failure);
 return true;
}
bool BenchmarkSourcePublication::subscribe(std::uint64_t id, std::function<void(std::exception_ptr)> callback) const {
 if (!state_) return false;
 auto owner = state_->execution.lock(); if (!owner) return false;
 CompileExecution::Completion completion;
 {
  const std::lock_guard lock(owner->mutex);
  if (owner->stopping || state_->attempt != owner->images.attempt() || state_->source->retiring || state_->source->image_generation(id) != state_->image_generation(id)) return false;
  const auto image = state_->source->images.find(id);
  if (image == state_->source->images.end() || !image->second.submitted || image->second.retiring) return false;
  completion = image->second.job.completion.completion();
 }
 completion.subscribe(std::move(callback)); return true;
}
bool BenchmarkCompilePipeline::subscribe_labels(const BenchmarkSourcePublication& publication, std::uint64_t id, std::string_view dependency, std::uint64_t original_generation, std::function<void(std::exception_ptr)> callback) const {
 if (!publication.state_ || publication.state_->execution.lock() != impl_) return false;
 CompileExecution::Completion completion;
 {
  const std::lock_guard lock(impl_->mutex);
  const auto& ticket = *publication.state_;
  if (ticket.attempt != impl_->images.attempt() || ticket.image_generation(id) != ticket.source->image_generation(id)) return false;
  const auto* image = impl_->images.find_image(ticket.source->root, id);
  const auto* label = image ? image->find_labels(dependency) : nullptr;
  if (!label || label->retired || label->attempt != ticket.attempt || label->generation != ticket.source->label_generation || label->generation != original_generation) return false;
  completion = label->completion.completion();
 }
 completion.subscribe(std::move(callback)); return true;
}
void BenchmarkSourcePublication::geometry_ready(std::uint64_t id, std::pair<std::uint32_t, std::uint32_t> dimensions) const {
 if (!state_) return;
 if (auto execution = state_->execution.lock()) {
  const std::lock_guard lock(execution->mutex);
  if (state_->attempt != execution->images.attempt()) return;
  execution->images.geometry_ready(*state_->source, id, state_->image_generation(id), dimensions);
  execution->changed.notify_all();
 }
}
BenchmarkCompilePipeline::BenchmarkCompilePipeline(std::size_t workers, std::span<const int> cpus, detail::CompileExecutionLimits limits,
 mmltk::common::concurrency::CancellationObservation cancellation, BenchmarkTraceSink trace) {
 if (!limits.workspace_plan) limits.workspace_plan=std::make_shared<detail::CompileWorkspacePlan>(detail::compile_workspace_plan(workers,1,false,0,limits.transient_bytes,BenchmarkEncodedImage::normal_window_bytes));
 impl_=std::make_shared<Impl>(workers,cpus,limits,cancellation,std::move(trace));
 impl_->readiness = std::make_unique<CompileExecution::Readiness>(impl_->execution, [weak = std::weak_ptr<Impl>(impl_)](CompileExecution::Readiness::Changes) {
  if (auto owner = weak.lock()) {
   try { owner->dispatch(); }
   catch (...) {
    const auto error = std::current_exception();
    { const std::lock_guard lock(owner->mutex); if (!owner->failure) owner->failure = error; }
    owner->execution.fail(error); owner->dispatch();
   }
   owner->changed.notify_all();
  }
 });
}
BenchmarkCompilePipeline::~BenchmarkCompilePipeline() { impl_->curl.reset(); retire_attempt(); impl_->readiness.reset(); }
CompileExecution& BenchmarkCompilePipeline::execution() noexcept { return impl_->execution; }
BenchmarkCurl& BenchmarkCompilePipeline::curl() {
 const std::lock_guard lock(impl_->curl_mutex);
 if (!impl_->curl) impl_->curl = std::make_unique<BenchmarkCurl>(impl_->execution.workers(), &impl_->execution);
 return *impl_->curl;
}
StorageReservationPool& BenchmarkCompilePipeline::storage() noexcept { return impl_->storage; }
void BenchmarkCompilePipeline::Impl::settle(Job& job, std::exception_ptr error, bool recoverable) noexcept {
 auto& slot = *job.slot;
 slot.input.reset(); slot.payload.reset(); slot.pixel_allowance = {}; slot.publication = {}; job.allowance = {}; job.parent = {};
 auto completion = job.completion;
 CompileExecution::Producer headers, pixels;
 {
  const std::lock_guard lock(mutex);
  if (error && !recoverable && job.independent && !stopping && !slot.retiring && !slot.source->retiring && !failure) { failure = error; execution.fail(error); }
  job.failure = error; job.done = true; job.queued = false;
  --pending; --slot.source->pending; headers = std::move(slot.headers); pixels = std::move(slot.pixels);
 }
 completion.settle(error);
 execution.notify_admission_change(); changed.notify_all();
}
void BenchmarkCompilePipeline::Impl::settle_list(Job* jobs, std::exception_ptr error) noexcept {
 while (jobs) { auto* job = jobs; jobs = job->next; job->next = nullptr; settle(*job, error, true); }
}
void BenchmarkCompilePipeline::Impl::run_pixel(Slot& slot, std::size_t lane, const CompileAllowance& allowance) {
 BenchmarkSplitWriter* writer;
 std::size_t index;
 std::uint64_t attempt;
 BenchmarkSourceGeneration generation;
 {
  const std::lock_guard lock(mutex);
  if (stopping || slot.retiring || slot.source->retiring) std::rethrow_exception(retired_failure);
  writer = slot.writer; index = slot.index; attempt = images.attempt(); generation = slot.generation;
 }
 // Source/attempt retirement drains this admitted Header/Pixels record before
 // clearing its slot or destroying its writer. All fallible planning runs here,
 // outside facts/admission locks and on the existing Header CPU grant.
 const auto current = [&] {
  return !stopping && !slot.retiring && !slot.source->retiring && attempt == images.attempt() &&
   generation == slot.source->image_generation(slot.image_id) && slot.generation == generation && slot.writer == writer && slot.index == index;
 };
 throw_if_benchmark_cancelled(cancellation);
 if (slot.job.stage != CompileTaskKind::Header) {
  writer->write_pixel(index, lane, slot.input, allowance, *pixel_workspaces[lane]);
  return;
 }
 auto payload = slot.payload;
 auto plan = slot.plan;
 if (payload && !plan) plan = writer->prepare_pixel_plan(index, payload->header());
 CompileAllowance pixel_allowance;
 bool reopen = false;
 if (payload && payload->charged_bytes()) {
  // The plan is complete before nonblocking input/work admission. Never park
  // a pixel demand while retaining a normal source arena that must drain for it.
  if (auto granted = execution.try_reserve(plan->resources(), payload->allowance())) pixel_allowance = std::move(*granted);
  else {
   payload = payload->file_backing() ? payload->file_backing() : payload->header_only();
   reopen = payload->encoded().empty();
  }
 }
 {
  const std::lock_guard lock(mutex);
  if (!current()) std::rethrow_exception(retired_failure);
  slot.plan = std::move(plan);
  slot.payload = std::move(payload);
  slot.pixel_allowance = std::move(pixel_allowance);
  slot.reopen = reopen;
 }
 // Reopening needs its own descriptor admission. Return the current Header
 // grant and retain only the immutable plan/file fact and publication custody.
 if (reopen) return;
 auto input = writer->prepare_pixel(index, lane, slot.publication, allowance, slot.payload, slot.plan);
 const auto dimensions = input ? writer->header_dimensions(index) : std::nullopt;
 {
  const std::lock_guard lock(mutex);
  if (!current()) std::rethrow_exception(retired_failure);
  slot.input = std::move(input);
  if (dimensions) images.geometry_ready(*slot.source, slot.image_id, generation, *dimensions);
 }
}
bool BenchmarkCompilePipeline::Impl::submit_pixel(Job& job) {
 auto& slot = *job.slot;
 const auto weak = weak_from_this();
 auto task = execution.try_submit({.kind = job.stage, .resources = job.resources,
  .work = [weak, &slot](std::size_t lane, const CompileAllowance& allowance) { if (auto owner = weak.lock()) owner->run_pixel(slot, lane, allowance); },
  .allowance = job.stage == CompileTaskKind::Pixels ? slot.pixel_allowance : CompileAllowance{}, .parent = job.parent,
  .finite_consumer = job.stage == CompileTaskKind::Pixels, .retain_workspace = false,
  .completed = [weak, &slot](std::exception_ptr error) {
   auto owner = weak.lock(); if (!owner) return;
   bool recoverable = false;
   if (error) try { std::rethrow_exception(error); } catch (const BenchmarkImageReadError&) { recoverable = true; } catch (...) {}
   {
    const std::lock_guard completed_lock(owner->mutex);
    if (!error && (owner->stopping || slot.retiring || slot.source->retiring || slot.generation != slot.source->image_generation(slot.image_id))) { error = owner->retired_failure; recoverable = true; }
    if (!error && slot.job.stage == CompileTaskKind::Header && slot.reopen) {
     slot.reopen = false;
     owner->header_input(slot);
     owner->push(slot.job); owner->changed.notify_all(); return;
    }
    if (!error && slot.job.stage == CompileTaskKind::Header && slot.input && !owner->stopping && !slot.retiring && !slot.source->retiring) {
     slot.headers.close();
     slot.job.stage = CompileTaskKind::Pixels;
     slot.job.resources = slot.writer->pixel_resources(*slot.input);
     slot.job.parent = slot.writer->pixel_input_allowance(*slot.input);
     owner->push(slot.job); owner->changed.notify_all(); return;
    }
   }
   owner->settle(slot.job, error, recoverable);
  }});
 if (!task) return false;
 job.handle = std::move(*task);
 slot.pixel_allowance = {};
 return true;
}
void BenchmarkCompilePipeline::Impl::dispatch() {
 std::unique_lock lock(mutex);
 if (!retired_completions.empty()) {
  auto completions = std::exchange(retired_completions, {});
  lock.unlock(); for (auto& completion : completions) completion.completion.settle(retired_failure); lock.lock();
 }
 if (stopping || failure || cancellation.requested()) {
  images.withdraw_all_labels();
  auto completions = std::exchange(retired_completions, {});
  Job* retired = nullptr;
  while (!pixels.empty()) { auto* job = pixels.front(); pixels.pop_front(); job->queued = false; job->next = retired; retired = job; }
  const auto error = failure ? failure : retired_failure;
  lock.unlock();
  for (auto& completion : completions) completion.completion.settle(error);
  settle_list(retired, error); changed.notify_all(); return;
 }
 for (;;) {
  const auto before = pending;
  if (labels_first) images.admit_labels();
  bool accepted = false;
  if (!pixels.empty()) {
   auto& job = *pixels.front();
   if (submit_pixel(job)) { pixels.pop_front(); job.queued = false; accepted = true; }
  }
  if (!labels_first) images.admit_labels();
  labels_first = !labels_first;
  if (!accepted && before == pending) break;
 }
}
void BenchmarkCompilePipeline::Impl::ImageState::admit_labels() {
 auto& execution = execution_;
 // Every eligible claim uses ordinary kernel admission. A blocked claim cannot
 // hide a ready sibling, and only one claim per image may own the selected slot.
 for (auto* candidate = labels_head_; candidate;) {
  auto* next = candidate->next;
  auto& label = *candidate;
  if (label.image->pending_labels) { candidate = next; continue; }
  auto work = std::make_shared<LabelWork>();
  work->label = &label;
  work->input = label.input;
  work->generation = label.image->generation;
  const auto& canvas = work->input->index().image(work->input->position());
  work->width = canvas.width ? canvas.width : label.image->width;
  work->height = canvas.height ? canvas.height : label.image->height;
  const auto weak = execution.weak_from_this();
  auto task = execution.execution.try_submit({.kind = CompileTaskKind::Labels, .resources = {.bytes=BenchmarkLabelWorkspace::required_bytes(work->width),.supplied_workspace=true,.completion_workspace=true},
   .work = [weak, work](std::size_t lane, const CompileAllowance&) { if (auto owner = weak.lock()) owner->run_label(*work, lane); },
   .finite_consumer = true, .retain_workspace = false,
   .completed = [weak, work](std::exception_ptr error) {
    auto owner = weak.lock(); if (!owner) return;
    work->input.reset();
    auto& product = *work->label;
    auto completion = product.completion;
    CompileExecution::Producer producer;
    {
     const std::lock_guard lock(owner->mutex);
     auto& image = *product.image;
     const bool retired = owner->stopping || product.retired || image.retiring || image.source->retiring || product.attempt != owner->images.attempt();
     if (retired && !error) error = owner->retired_failure;
     if (error && !retired && !owner->failure) { owner->failure = error; owner->execution.fail(error); }
     --image.source->pending; --image.source->pending_labels; --image.pending_labels; --owner->pending;
     product.active = nullptr; producer = std::move(product.producer);
    }
    completion.settle(error); owner->execution.notify_admission_change(); owner->changed.notify_all();
   }});
  if (!task) return;
  work->handle = std::move(*task);
  unlink_labels(label); label.input.reset(); label.active = work.get();
  ++execution.pending; ++label.image->source->pending; ++label.image->source->pending_labels; ++label.image->pending_labels;
  candidate = next;
 }
}
void BenchmarkCompilePipeline::Impl::run_label(LabelWork& label, std::size_t lane) {
 {
  const std::lock_guard lock(mutex);
  if (stopping || label.label->retired || label.label->image->retiring || label.label->image->source->retiring) std::rethrow_exception(retired_failure);
 }
 throw_if_benchmark_cancelled(cancellation);
    label_workspaces[lane].prepare(label.width,execution.current_allowance());
    struct ReleaseBorrow { BenchmarkLabelWorkspace& workspace; ~ReleaseBorrow() { workspace.finish(); } } release{label_workspaces[lane]};
    if (label_started) label_started(label.label->image->source->root, label.label->image->image_id, label_workspaces[lane]);
    const auto& input = *label.input;
    auto& result = *label.label;
    auto& image = *result.image;
    Slot* target = nullptr;
    {
     const std::lock_guard lock(mutex);
     if (image.source->incremental_writer) target = images.bind_image(*image.source, image.image_id);
    }
    auto product = std::make_shared<BenchmarkLabelChunk>();
    const auto projected = compile_benchmark_image_labels_reused(input.index(),input.position(),{label.width,label.height},label_resolution,label_resize_mode,label_workspaces[lane],cancellation);
    product->width = projected.width; product->height = projected.height; product->dropped = projected.dropped;
    auto output = label_workspaces[lane].release_output();
    if (!target) {
     product->labels.assign(output.labels.begin(),output.labels.end()); product->runs.assign(output.runs.begin(),output.runs.end());
    }
    if (target) {
     const std::lock_guard write(target->label_write);
     bool current;
     {
      const std::lock_guard lock(mutex);
      current = !stopping && !result.retired && !image.retiring && !image.source->retiring && result.attempt == images.attempt() &&
                label.generation == image.source->image_generation(image.image_id) && result.generation == image.source->label_generation &&
                (!target->selected_label || result.priority >= target->selected_label->priority);
     }
     if (current) {
      ImageEntry entry{};
      entry.source_image_id = image.image_id;
      entry.has_source_image_id = 1;
      entry.source = image.source->annotation_source;
      entry.original_width = product->width;
      entry.original_height = product->height;
      target->writer->write_labels(static_cast<std::uint32_t>(target->index), entry, {output.labels, output.runs}, lane);
      {
       const std::lock_guard lock(mutex);
       target->selected_label = &result;
      }
      if (label_completed) label_completed(image.source->root, image.image_id);
     }
     // The frame owns this lane until the direct write and observation finish.
     // Only the small immutable result survives; reusable output remains charged
     // as lane scratch and mapped input remains the authority for claim replay.
    }
    const std::lock_guard lock(mutex);
    if (!stopping && !result.retired && !image.retiring && !image.source->retiring && result.attempt == images.attempt() && label.generation == image.source->image_generation(image.image_id) &&
        result.generation == image.source->label_generation)
     result.product = std::move(product);

}
void BenchmarkCompilePipeline::write_remaining(BenchmarkSplitWriter& writer, const PreparedBenchmarkSplit& split, std::span<const std::size_t> slots) {
 for (std::size_t i = 0; i < split.images.size(); ++i) {
  const auto& image = split.images[i]; const auto& root = split.sources.at(image.source_index).root;
  if (!image_slot(root, image.source_image_id)) bind_slot(root, image.source_image_id, writer, static_cast<std::uint32_t>(i));
 }
 const auto withdraw = [&] {
  Impl::Job* discarded = nullptr;
  {
   const std::lock_guard lock(impl_->mutex);
   for (auto index : slots) {
    const auto [root, id] = writer.identity(index);
    auto& slot = impl_->images.source(root).images.at(id);
    if (!slot.submitted || slot.job.done || slot.job.independent) continue;
    slot.job.handle.cancel();
    if (slot.job.queued) { impl_->remove(slot.job); slot.job.next = discarded; discarded = &slot.job; }
   }
  }
  impl_->settle_list(discarded, impl_->retired_failure);
 };
 std::exception_ptr failure;
 try {
  const std::lock_guard lock(impl_->mutex); impl_->check_admission();
  for (auto index : slots) {
   const auto [root, id] = writer.identity(index);
   auto& slot = impl_->images.source(root).images.at(id);
   if (slot.job.done && slot.job.failure) std::rethrow_exception(slot.job.failure);
   if ((slot.submitted && !slot.job.done) || writer.image_complete(index)) continue;
   impl_->images.admit(slot, false);
  }
 } catch (...) { failure = std::current_exception(); withdraw(); }
 impl_->execution.notify_admission_change();
 for (auto index : slots) {
  const auto [root, id] = writer.identity(index);
  try { image_completion(root, id).wait(); }
  catch (...) { if (!failure) { failure = std::current_exception(); withdraw(); } }
 }
 if (failure) std::rethrow_exception(failure);
}
CompileExecution::Completion BenchmarkCompilePipeline::image_completion(const std::filesystem::path& root, std::uint64_t id) const {
 const std::lock_guard lock(impl_->mutex);
 const auto* source = impl_->images.find(root); if (!source) return {};
 const auto image = source->images.find(id);
 return image == source->images.end() || !image->second.submitted ? CompileExecution::Completion{} : image->second.job.completion.completion();
}
CompileExecution::Completion BenchmarkCompilePipeline::label_completion(const std::filesystem::path& root, std::uint64_t id, std::string_view dependency) const {
 const std::lock_guard lock(impl_->mutex);
 const auto* image = impl_->images.find_image(root, id);
 const auto* label = image ? image->find_labels(dependency) : nullptr;
 return label ? label->completion.completion() : CompileExecution::Completion{};
}
void BenchmarkCompilePipeline::drain() {
 std::unique_lock lock(impl_->mutex);
 impl_->wait(lock, [&] { return impl_->pending == 0 && (impl_->failure || impl_->cancellation.requested() || !impl_->images.waiting_labels()); });
 throw_if_benchmark_cancelled(impl_->cancellation);
 if (impl_->failure) std::rethrow_exception(impl_->failure);
}
void BenchmarkCompilePipeline::retire_attempt() noexcept {
 Impl::Job* discarded = nullptr;
 {
  const std::lock_guard lock(impl_->mutex); impl_->stopping = true;
  while (!impl_->pixels.empty()) { auto* job = impl_->pixels.front(); impl_->pixels.pop_front(); job->queued = false; job->next = discarded; discarded = job; }
 }
 impl_->settle_list(discarded, impl_->retired_failure);
 impl_->execution.retire_attempt();
 {
  std::unique_lock lock(impl_->mutex);
  impl_->wait(lock, [&] { return impl_->pending == 0; });
  impl_->images.close();
  auto completions = std::exchange(impl_->retired_completions, {});
  lock.unlock(); for (auto& completion : completions) completion.completion.settle(impl_->retired_failure); lock.lock();
  impl_->membership_pending = impl_->labels_first = true; impl_->failure = {}; impl_->stopping = false;
 }
 impl_->changed.notify_all();
}
}  // namespace mmltk::backend::data::benchmark_internal
