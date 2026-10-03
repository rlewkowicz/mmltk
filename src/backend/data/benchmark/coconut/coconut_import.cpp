#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_native_image.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_annotation_identity.h"
#include "src/backend/data/detail/compile_json_number.h"
#include "src/backend/data/detail/staged_artifact.h"
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/detail/compile_workspace.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_input.h"
#include <system_error>
#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/backend/data/benchmark/detail/benchmark_json.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_json.h"
#include <tuple>
#include "src/backend/data/benchmark/coconut/detail/coconut_physical.h"
#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_import.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_staging.h"
#include "src/backend/data/detail/file_work_queue.h"
#include <exception>
#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_mask_recovery.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory_io.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/frameworks/serialization/json_scalar.h"
#include "src/pch_std.h"
#include <iterator>
#include <condition_variable>
#include <deque>
#include <thread>
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
using mmltk::backend::data::detail::CompileChange;
namespace mmltk::backend::data::benchmark_internal {
using detail::CompileAllowance;
using detail::CompileExecution;
using detail::CompileResources;
using detail::CompileTaskKind;
namespace {
using Cancellation = common::concurrency::CancellationObservation;
[[noreturn]] void invalid(std::string_view detail) { throw CoconutInputError("COCONut: " + std::string(detail)); }
}  // namespace
namespace {
using Archive = BenchmarkArchive;
namespace reflection = mmltk::frameworks::reflection;
template <class T>
struct JsonBorrowedScalar;
template <class... T>
struct JsonBorrowedScalar<std::variant<T...>> {
 using type = std::variant<std::conditional_t<std::same_as<T, std::string>, std::string_view, T>...>;
};
struct JsonAtom {
 bool present = false;
 std::optional<typename JsonBorrowedScalar<CoconutJsonScalar>::type> value;
 void reset() {
  present = false;
  value.reset();
 }
 [[nodiscard]] std::uint64_t retained_bytes() const noexcept { return 0; }
 explicit operator bool() const noexcept { return present; }
 template <class T>
 const T* get() const noexcept {
  return value ? std::get_if<T>(&*value) : nullptr;
 }
 bool is_null() const noexcept { return get<std::nullptr_t>() != nullptr; }
 bool is_bool() const noexcept { return get<bool>() != nullptr; }
 bool boolean() const {
  if (const auto* result = get<bool>()) return *result;
  invalid("flag must be boolean or integer");
 }
 std::uint64_t unsigned_integer() const {
  if (const auto* result = get<std::uint64_t>()) return *result;
  if (const auto* result = get<std::int64_t>(); result && *result >= 0) return static_cast<std::uint64_t>(*result);
  invalid("integral field must decode from a nonnegative integer JSON number");
 }
 double number() const {
  if (const auto* result = get<double>()) return *result;
  if (const auto* result = get<std::uint64_t>()) return static_cast<double>(*result);
  if (const auto* result = get<std::int64_t>()) return static_cast<double>(*result);
  invalid("JSON field must be numeric");
 }
 std::string_view text() const {
  if (const auto* result = get<std::string_view>()) return *result;
  invalid("JSON field must be text");
 }
};
// Accounting follows the field actually consumed, including exceptional unwind.
// Retained inactive slots are never visited to reset or total a smaller row.
template <class T>
struct JsonStorageAccounting {
 T& storage;
 std::uint64_t& total;
 std::uint64_t before;
 JsonStorageAccounting(T& value, std::uint64_t& bytes) : storage(value), total(bytes), before(value.retained_bytes()) {}
 ~JsonStorageAccounting() { total = total - before + storage.retained_bytes(); }
};
template <class T>
struct JsonArray {
 bool present = false, valid = false;
 std::vector<T> values;
 std::size_t length = 0;
 std::uint64_t nested_bytes = 0;
 void reset() {
  present = valid = false;
  length = 0;
 }
 [[nodiscard]] auto active() const { return std::span(values).first(length); }
 [[nodiscard]] std::uint64_t retained_bytes() const noexcept { return values.capacity() * sizeof(T) + nested_bytes; }
 T& next() {
  if (length == values.size()) values.emplace_back();
  auto& value = values[length++];
  value.reset();
  return value;
 }
};
template <class Shape>
class JsonObject;
template <class Value>
struct JsonStorage {
 using type = JsonObject<Value>;
};
template <>
struct JsonStorage<CoconutJsonScalar> {
 using type = JsonAtom;
};
template <class Value>
struct JsonStorage<std::vector<Value>> {
 using type = JsonArray<typename JsonStorage<Value>::type>;
};
template <class Fields>
struct JsonMemberTuple;
template <class Bases, class... Declaration>
struct JsonMemberTuple<reflection::MaterializedFieldPolicyProduct<Bases, Declaration...>> {
 using type = std::tuple<typename JsonStorage<typename Declaration::member_type>::type...>;
};
// The two wire declarations project reusable parser storage without reflecting
// presence/type/object state or retaining unknown values as a DOM.
template <class Shape>
class JsonObject {
 static_assert(std::same_as<Shape, CoconutJsonRow> || std::same_as<Shape, CoconutJsonSegment>);
 using Fields = std::remove_cvref_t<decltype(reflection::field_declarations<Shape>())>;
 typename JsonMemberTuple<Fields>::type fields_;
 std::uint64_t retained_ = 0;

public:
 bool object = false;
 [[nodiscard]] std::uint64_t retained_bytes() const noexcept { return retained_; }
 void reset() {
  object = false;
  Fields::Visit([&]<class Declaration, std::size_t Index>() { std::get<Index>(fields_).reset(); });
 }
 template <auto Member>
 const auto& get() const {
  return std::get<reflection::member_index<Member>(reflection::field_declarations<Shape>())>(fields_);
 }
 template <class Visitor>
 void select(std::string_view key, Visitor&& visitor) {
  bool selected = false;
  Fields::Visit([&]<class Declaration, std::size_t Index>() {
   if (!selected && key == reflection::field_declarations<Shape>()[Index].member_name) {
    auto& field = std::get<Index>(fields_);
    JsonStorageAccounting accounting(field, retained_);
    visitor(field);
    selected = true;
   }
  });
 }
};
using JsonSegment = JsonObject<CoconutJsonSegment>;
using JsonRow = JsonObject<CoconutJsonRow>;
struct JsonParser {
 simdjson::ondemand::parser parser;
 JsonRow row;
 std::atomic<std::uint64_t> retained{0};
 [[nodiscard]] std::uint64_t retained_bytes() const noexcept { return retained.load(std::memory_order_relaxed); }
 void account() noexcept { retained.store(row.retained_bytes() + parser.capacity() * 32ULL, std::memory_order_relaxed); }
 void retire() noexcept {
  row = {};
  parser = simdjson::ondemand::parser{};
  retained.store(0, std::memory_order_relaxed);
 }
};
struct JsonConsumption {
 std::size_t events = 0, maximum;
 bool limited;
 std::size_t segment_limit;
 void event(std::size_t depth) {
  if (!limited) return;
  if (depth > 16) invalid("annotation JSON nesting exceeds admission");
  if (++events > maximum) invalid("panoptic row exceeds segment admission");
 }
};
void consume_json_value(simdjson::ondemand::value value, JsonConsumption& admission, std::size_t depth, JsonAtom* atom = nullptr, JsonRow* row = nullptr, JsonSegment* segment = nullptr,
 JsonArray<JsonSegment>* segments = nullptr, JsonArray<JsonAtom>* coordinates = nullptr) {
 admission.event(depth);
 if (atom) {
  atom->present = true;
  atom->value.reset();
 }
 if (segments) {
  segments->present = true;
  segments->valid = false;
  segments->length = 0;
 }
 if (coordinates) {
  coordinates->present = true;
  coordinates->valid = false;
  coordinates->length = 0;
 }
 const auto type = value.type().value();
 if (type == simdjson::ondemand::json_type::object) {
  if (row) row->object = true;
  if (segment) segment->object = true;
  for (auto field : value.get_object().value()) {
   admission.event(depth + 1);
   const auto key = field.unescaped_key().value();
   bool selected = false;
   const auto select = [&]<class T>(T& target) {
    selected = true;
    if constexpr (std::same_as<T, JsonAtom>)
     consume_json_value(field.value(), admission, depth + 1, &target);
    else if constexpr (std::same_as<T, JsonArray<JsonSegment>>)
     consume_json_value(field.value(), admission, depth + 1, nullptr, nullptr, nullptr, &target);
    else if constexpr (std::same_as<T, JsonArray<JsonAtom>>)
     consume_json_value(field.value(), admission, depth + 1, nullptr, nullptr, nullptr, nullptr, &target);
   };
   if (row)
    row->select(key, select);
   else if (segment)
    segment->select(key, select);
   if (!selected) consume_json_value(field.value(), admission, depth + 1);
  }
  admission.event(depth);
 } else if (type == simdjson::ondemand::json_type::array) {
  if (segments) segments->valid = true;
  if (coordinates) coordinates->valid = true;
  for (auto child : value.get_array().value()) {
   if (segments) {
    if (segments->length < admission.segment_limit) {
     auto& target = segments->next();
     JsonStorageAccounting accounting(target, segments->nested_bytes);
     consume_json_value(child.value(), admission, depth + 1, nullptr, nullptr, &target);
    } else {
     ++segments->length;
     consume_json_value(child.value(), admission, depth + 1);
    }
   } else if (coordinates) {
    if (coordinates->length < 4) {
     auto& target = coordinates->next();
     consume_json_value(child.value(), admission, depth + 1, &target);
    } else {
     ++coordinates->length;
     consume_json_value(child.value(), admission, depth + 1);
    }
   } else
    consume_json_value(child.value(), admission, depth + 1);
  }
  admission.event(depth);
 } else if (type == simdjson::ondemand::json_type::string) {
  const auto text = value.get_string().value();
  if (admission.limited && text.size() > 4096) invalid("panoptic text exceeds admission");
  if (atom) atom->value = text;
 } else if (type == simdjson::ondemand::json_type::number) {
  detail::consume_compile_json_number(value, [&](auto number) {
   if (atom) atom->value = number;
  }, [](const char* error) { invalid(error); });
 } else if (type == simdjson::ondemand::json_type::boolean) {
  const auto boolean = value.get_bool().value();
  if (atom) atom->value = boolean;
 } else {
  if (!value.is_null().value()) invalid("invalid JSON value");
  if (atom) atom->value = nullptr;
 }
}
bool flag(const JsonAtom& field, bool required = false) {
 if (!field) {
  if (required) invalid("missing required flag");
  return false;
 }
 if (field.is_bool()) return field.boolean();
 const auto value = field.unsigned_integer();
 if (value > 1) invalid("invalid flag");
 return value != 0;
}
void segments_from_json(const JsonArray<JsonSegment>& input, CoconutRecord& record, const CoconutImportLimits& limits) {
 if (!input.present || !input.valid || input.length > limits.max_segments) invalid("segment list exceeds admission");
 record.segments.reserve(input.length);
 for (const auto& value : input.active()) {
  CoconutSegment segment;
  try {
   if (!value.object) invalid("segment must be an object");
   const auto id = value.get<&CoconutJsonSegment::id>().unsigned_integer();
   if (id == 0 || id > 0xffffffU) invalid("segment ID exceeds RGB24");
   segment.id = static_cast<std::uint32_t>(id);
   segment.category_id = value.get<&CoconutJsonSegment::category_id>().unsigned_integer();
   segment.isthing = flag(value.get<&CoconutJsonSegment::isthing>(), true);
   segment.crowd = flag(value.get<&CoconutJsonSegment::iscrowd>());
   segment.ignore = flag(value.get<&CoconutJsonSegment::ignore>());
   if (value.get<&CoconutJsonSegment::area>() && !value.get<&CoconutJsonSegment::area>().is_null()) {
    segment.area = value.get<&CoconutJsonSegment::area>().number();
    if (!std::isfinite(*segment.area) || *segment.area < 0) invalid("invalid supplied segment area");
   }
   if (value.get<&CoconutJsonSegment::bbox>().present) {
    if (!value.get<&CoconutJsonSegment::bbox>().valid || value.get<&CoconutJsonSegment::bbox>().length != 4) invalid("invalid supplied COCO bbox");
    segment.bbox.emplace();
    for (std::size_t i = 0; i < 4; ++i) (*segment.bbox)[i] = value.get<&CoconutJsonSegment::bbox>().values[i].number();
   }
  } catch (const CoconutInputError&) {
   segment.valid = false;
   segment.isthing = true;
  }
  record.segments.push_back(std::move(segment));
 }
}
struct PendingNativeImage {
 CoconutRecord record;
 std::size_t position = 0;
 CoconutPhysicalImage physical;
 dataset::MaskDimensions dimensions;
 std::uint32_t unlisted_segment = 0;
 CoconutNativeSupport support;
 std::shared_ptr<const CoconutNativeImage> product;
 BenchmarkSourcePublication publication;
 std::string provisional_dependency;
 std::uint64_t physical_generation = 0, original_generation = 0;
 CoconutPhysicalMembership::Registration physical_change;
 using OriginalWait = std::pair<CoconutImageNamespace, std::uint64_t>;
 std::optional<OriginalWait> original_wait;
 enum class Readiness { Ready, Checking, Parked, Working, Retired };
 Readiness readiness = Readiness::Parked;
 bool notified = false, recovered = false, counted = false;
 std::shared_ptr<PendingNativeImage> next_ready;
};
class Importer final {
 static constexpr std::size_t native_window = 64;
 struct Worker {
  CoconutNativeWorkspace native;
  CoconutNativeOutput output;
  std::shared_ptr<const CoconutNativeImage> complete;
  std::shared_ptr<PendingNativeImage> pending;
  BenchmarkSourcePublication publication;
  std::size_t position = 0;
  bool saved = false, reused = false;
  explicit Worker(const CoconutImportRequest& request) : native(request.limits, request.cancellation) { native.recovery(request.recovery); }
  ~Worker() {
   complete.reset();
   pending.reset();
   native.release_supplied();
  }
 };
 struct NativeJob {
  enum class State { Working, Prepare, Preparing, Copy, Copying, Done };
  CoconutRecord record;
  CoconutAnnotationInput input;
  CompileAllowance arena, descriptor;
  std::unique_ptr<Worker> worker;
  CompileTaskKind kind;
  CompileExecution::CompletionSource completion;
  CompileExecution::Continuation continuation;
  State state = State::Working;
  std::exception_ptr error;
  NativeJob(CompileExecution& execution, CompileTaskKind task_kind) : kind(task_kind), completion(&execution) {}
 };
 // The existing 64-entry normalization and recovery windows own these jobs.
 // Readiness only retries bounded admission; physical work and mapped copies
 // run on the existing file service and semantic CPU lanes respectively.
 void post_output(const std::shared_ptr<NativeJob>& job, NativeJob::State state, std::exception_ptr error) {
  {
   const std::lock_guard lock(outputs_mutex_);
   job->state = error ? NativeJob::State::Done : state;
   job->error = error;
  }
  output_readiness_->notify();
 }
 void pump_outputs() {
  auto& execution = request_.execution->execution();
  std::array<std::shared_ptr<NativeJob>, 2 * native_window> finished;
  std::size_t count = 0;
  {
   const std::lock_guard lock(outputs_mutex_);
   for (auto& entry : outputs_) {
    if (!entry) continue;
    const auto job = entry;
    if (job->state == NativeJob::State::Working || job->state == NativeJob::State::Preparing || job->state == NativeJob::State::Copying) continue;
    CompileExecution::Readiness::Attempt attempt(output_readiness_.get());
    try {
     if (job->state != NativeJob::State::Done) {
      if (auto error = execution.interruption()) std::rethrow_exception(error);
     }
     if (job->state == NativeJob::State::Prepare) {
      if (request_.records->staging->try_claim(job->worker->output.request()))
       job->state = NativeJob::State::Copy;
      else {
       if (!job->descriptor) {
        auto descriptor = execution.try_reserve({.descriptors = 1}, request_.parent_allowance);
        if (!descriptor) continue;
        job->descriptor = std::move(*descriptor);
       }
       auto operation = request_.execution->files().try_submit({.completed = [this, job](std::exception_ptr error) { post_output(job, NativeJob::State::Copy, error); }, .work = [this, job] {
        request_.records->staging->prepare(job->worker->output.request(), job->descriptor);
       }});
       if (!operation) continue;  // Full retains this exact request and allowance.
       job->state = NativeJob::State::Preparing;
       attempt.commit();
      }
     }
     if (job->state == NativeJob::State::Copy) {
      // Every accepted physical operation has returned and closed its handle.
      job->descriptor = {};
      auto copy = job->continuation.try_submit({.kind = job->kind, .work = [this, job](std::size_t, const CompileAllowance&) {
       auto& worker = *job->worker;
       worker.output.fill(request_.cancellation);
       worker.output = {};
       worker.native.release_supplied();
       if (worker.pending)
        native_staged(*worker.pending->product, job->kind);
       else if (worker.complete)
        native_staged(*worker.complete, job->kind);
      }, .allowance = job->arena, .finite_consumer = true, .retain_workspace = false, .completed = [this, job](std::exception_ptr error) { post_output(job, NativeJob::State::Done, error); }});
      if (!copy) continue;
      job->state = NativeJob::State::Copying;
      attempt.commit();
     }
    } catch (...) {
     job->error = std::current_exception();
     job->state = NativeJob::State::Done;
    }
    if (job->state == NativeJob::State::Done) finished[count++] = std::move(entry);
   }
  }
  for (const auto& job : std::span(finished).first(count)) {
   // No accepted operation or copy can still touch these PMR objects. Destroy
   // every view before returning its arena, and before controller observers.
   job->worker->output = {};
   job->worker->native.release_supplied();
   job->arena = {};
   job->descriptor = {};
   job->input = {};
   job->continuation = {};
   job->completion.settle(job->error);
  }
 }
 CompileExecution::Completion submit_native(const std::shared_ptr<NativeJob>& job, std::uint64_t bytes, std::function<void(const CompileAllowance&)> work) {
  // Acquire actual scratch on the input/recovery controller before consuming a
  // scheduler record. Byte-waiting first steps must not fill that bounded queue
  // and strand the copy continuation which returns their only fitting arena.
  auto& execution = request_.execution->execution();
  const CompileResources resources{.bytes = bytes, .supplied_workspace = true, .completion_workspace = true, .workspace_class = detail::work_class(job->kind)};
  auto arena = execution.try_reserve(resources, request_.parent_allowance);
  // Immutable input has already been staged. A source cursor that cannot lend
  // its complete next draw must release its decoder before admission can wait.
  if (!arena && job->input.retire_projection) job->input.retire_projection();
  job->input.retire_projection = {};
  job->arena = arena ? std::move(*arena) : execution.reserve(resources, request_.parent_allowance);
  {
   const std::lock_guard lock(outputs_mutex_);
   auto free = std::ranges::find_if(outputs_, [](const auto& value) { return !value; });
   if (free == outputs_.end()) throw std::logic_error("COCONut native window exceeded prepared output records");
   *free = job;
  }
  try {
   (void)request_.execution->execution().submit(
    {.kind = job->kind, .resources = {.bytes = bytes, .supplied_workspace = true}, .work = [job, &execution, work = std::move(work)](std::size_t, const CompileAllowance& allowance) {
    job->arena = allowance;
    job->worker->native.prepare_support(allowance.workspace());
    work(allowance);
    job->continuation = execution.retain_current(job->kind);
   }, .allowance = job->arena, .finite_consumer = true, .retain_workspace = false, .completed = [this, job](std::exception_ptr error) { post_output(job, NativeJob::State::Prepare, error); }});
  } catch (...) { post_output(job, NativeJob::State::Done, std::current_exception()); }
  return job->completion.completion();
 }
 template <class Settlement>
 static void track_native(BenchmarkCompletionWindow& window, const CompileExecution::Completion& completion, Settlement&& settlement) {
  try {
   window.push(completion, std::forward<Settlement>(settlement));
  } catch (...) {
   // Even allocation failure while registering controller settlement must join
   // a job whose file operation or mapped copy has already been accepted.
   const auto error = std::current_exception();
   try {
    completion.wait();
   } catch (...) {}
   std::rethrow_exception(error);
  }
 }

public:
 explicit Importer(const CoconutImportRequest& request)
     : request_(request),
       archive_worker_(request),
       assembly_workspace_(request.limits, request.cancellation),
       normalizations_(native_window, request.execution ? &request.execution->execution() : nullptr) {
  if (request.execution)
   output_readiness_ = std::make_unique<CompileExecution::Readiness>(request.execution->execution(), CompileChange::Resources | CompileChange::Records, [this](auto changes) {
    if (changes.retry || includes(changes.causes, CompileChange::Records | CompileChange::Local | CompileChange::Interruption)) pump_outputs();
   });
  if (request.input_identity.empty()) invalid("missing pinned input identity");
  if (!request.physical_membership) invalid("missing physical membership admission");
  assembly_workspace_.recovery(request.recovery);
  if (request_.progress) request_.progress(0);
 }
 ~Importer() {
  try {
   normalizations_.drain();
  } catch (...) {}
  stop_recovery();
  output_readiness_.reset();
 }
 void drain_normalization() { normalizations_.drain(); }
 void stop_recovery() {
  if (recovery_controller_.joinable()) {
   recovery_controller_.request_stop();
   recovery_signal_->notify();
   recovery_controller_.join();
  }
  // Accepted recovery work has joined. Withdraw parked registrations before
  // releasing their support, even when a provider never settles its generation.
  for (;;) {
   std::shared_ptr<PendingNativeImage> row;
   {
    const std::lock_guard lock(groups_mutex_);
    if (pending_.empty()) break;
    row = pending_.begin()->second;
   }
   retire_pending(row);
  }
  recovery_signal_->clear();
 }
 void retire_scratch() noexcept { archive_worker_.native.retire(); }
 std::optional<CoconutPhysicalImage> identify(const CoconutRecord& record, bool wait = false, bool report = false) {
  const auto reject = [&](std::string_view reason) {
   if (report && request_.rejected_object) {
    const auto sources = coconut_release_sources(request_.edition);
    const CoconutPhysicalImage available{record.namespace_hint.value_or(sources.front()), record.image_id, 0, record.file_name, request_.input_identity};
    request_.rejected_object(available, record, {}, reason, 0);
   }
  };
  try {
   return wait ? request_.physical_membership->resolve(request_.edition, record) : request_.physical_membership->identify(request_.edition, record);
  } catch (const CoconutInputError& error) {
   if (!request_.accept_partial && !request_.reject_invalid_identities) throw;
   reject(error.what());
  } catch (const BenchmarkArchiveError& error) {
   if (!request_.accept_partial && !request_.reject_invalid_identities) throw;
   reject(error.what());
  } catch (const CoconutPhysicalMembershipError& error) {
   if (!request_.accept_partial && !request_.reject_invalid_identities) throw;
   reject(error.what());
  }
  return std::nullopt;
 }
 dataset::MaskDimensions admit_png(CoconutNativeWorkspace& workspace, const CoconutRecord& record, std::span<const std::uint8_t> png, const CoconutPhysicalImage& physical) {
  try {
   return workspace.admit_png(record, png);
  } catch (const CoconutInputError& error) { throw CoconutInputError(physical.member + ": " + error.what()); }
 }
 void canonical_ordinals(std::vector<std::size_t> ordinals) {
  source_ordinals_ = std::move(ordinals);
  for (auto& [source, component] : components_) {
   (void)source;
   for (auto& image : component.inventory) image.source_ordinal = source_ordinals_.at(image.source_ordinal);
  }
 }
 bool stage_parquet_input(const CoconutRecord& record, std::span<const std::uint8_t> png) {
  if (!request_.execution || png.empty()) return false;
  // Concurrent source windows may occupy every ordinary context. Retain an
  // Arrow batch only when its descendants fit the protected completion arena;
  // otherwise transfer immutable input before asking for an ordinary arena.
  const auto& completion = request_.execution->execution().workspace_plan().completion;
  constexpr auto annotation_class = static_cast<std::size_t>(detail::CompileWorkClass::Annotations);
  const auto capacity = annotation_class < completion.size() ? completion[annotation_class] : 0;
  try {
   if (reusable_native(record)) return false;
   if (const auto physical = identify(record)) {
    std::size_t position = 0;
    if (const auto* reused = reusable_image(*physical, position)) return assembly_workspace_.reuse_workspace_bytes(record, reused->index(), position) > capacity;
   }
   const auto dimensions = assembly_workspace_.admit_png(record, png);
   const auto bytes = common::math::checked_add(assembly_workspace_.decoder_workspace_bytes(png),
    assembly_workspace_.support_workspace_bytes(record, std::uint64_t{dimensions.width} * dimensions.height, record.segments.size(), !request_.originals), "COCONut actual normalizer overflow");
   return bytes > capacity;
  } catch (const CoconutInputError&) { return false; }
 }
 void settle_worker(Worker& worker) {
  trace_benchmark_event(request_.trace, "benchmark.coconut.normalize_settled", [&] {
   const auto snapshot = request_.execution ? request_.execution->execution().snapshot() : detail::CompileExecutionSnapshot{};
   return nlohmann::json{
    {"source_ordinal", worker.position}, {"standalone_capacity", worker.native.retained_bytes()}, {"recovery_pending", bool(worker.pending)}, {"normal_locked", snapshot.normal_locked},
    {"leased", snapshot.leased}, {"exceptional", snapshot.exceptional}
   };
  });
  if (worker.pending) {
   auto native = std::move(worker.pending);
   auto provisional = std::move(native->product);
   native->provisional_dependency = provisional->input_identity() + ".provisional";
   publish_labels(provisional, native->publication, true);
   defer_native(std::move(native));
  }
  if (worker.complete) {
   auto complete = std::move(worker.complete);
   if (!worker.saved && !worker.reused) settle_report(*complete);
   if (!worker.saved)
    retain_image(worker.position, complete);
   else
    complete_row();
   publish_labels(complete, worker.publication);
  }
 }
 void consume_group(std::size_t group, const CoconutRecord& record, const CoconutAnnotationInput& input, std::size_t position = SIZE_MAX) {
  (void)group;
  if (position == SIZE_MAX) position = record.source_ordinal;
  if (input.membership) {
   if (request_.preparation_progress) {
    const std::lock_guard lock(progress_mutex_);
    ++parsed_rows_;
    if (parsed_rows_ == 1 || parsed_rows_ % kProgressQuantum == 0) request_.preparation_progress(CoconutPreparationStage::Annotations, parsed_rows_);
   }
   (void)identify(record);
   if (!request_.metadata_only) return;
  }
  if (!request_.execution || request_.metadata_only) {
   consume(record, input.png, {}, nullptr, position);
   return;
  }
  normalizations_.make_room();
  auto job = std::make_shared<NativeJob>(request_.execution->execution(), CompileTaskKind::Normalize);
  job->record = record;
  job->input = input;
  if (idle_.empty())
   job->worker = std::make_unique<Worker>(request_);
  else {
   job->worker = std::move(idle_.back());
   idle_.pop_back();
  }
  job->worker->position = position;
  auto& workspace = job->worker->native;
  if (request_.originals) workspace.originals({});
  std::size_t decode = 0;
  std::uint64_t support = 0;
  const auto physical = identify(record);
  std::size_t reused_position = 0;
  const auto* reused = physical ? reusable_image(*physical, reused_position) : nullptr;
  if (reusable_native(record, position)) { /* Its immutable product already owns every output range. */
  } else if (reused)
   support = workspace.reuse_workspace_bytes(record, reused->index(), reused_position);
  else if (!input.png.empty()) {
   try {
    const auto dimensions = workspace.admit_png(record, input.png);
    decode = workspace.decoder_workspace_bytes(input.png);
    support = workspace.support_workspace_bytes(record, std::uint64_t{dimensions.width} * dimensions.height, record.segments.size(), !request_.originals);
   } catch (const CoconutInputError&) { /* The ordinary row policy reports malformed PNG input. */
   }
  }
  const auto bytes = common::math::checked_add(std::uint64_t{decode}, support, "COCONut actual row workspace overflow");
  trace_benchmark_event(request_.trace, "benchmark.coconut.normalize_request", [&] {
   return nlohmann::json{
    {"image_id", record.image_id}, {"source_ordinal", record.source_ordinal}, {"png_bytes", input.png.size()}, {"input_backing_bytes", input.live_bytes}, {"decoder_bytes", decode},
    {"support_bytes", support}, {"workspace_bytes", bytes}
   };
  });
  job->input.admitted = {};
  auto completion = submit_native(job, bytes, [this, job, position](const CompileAllowance& allowance) { consume(job->record, job->input.png, allowance, job->worker.get(), position); });
  track_native(normalizations_, completion, [this, job](std::exception_ptr failure) {
   if (failure) {
    job->worker->complete.reset();
    job->worker->pending.reset();
    job->worker->native.release_supplied();
    std::rethrow_exception(failure);
   }
   settle_worker(*job->worker);
   idle_.push_back(std::move(job->worker));
  });
  if (input.admitted) input.admitted(completion);
 }
 void settle_normalizations() { normalizations_.reap(); }
 void collect_groups() {
  normalizations_.drain();
  offered_.clear();
 }
 bool reusable_native(const CoconutRecord& record, std::size_t position = SIZE_MAX) {
  const auto admitted = identify(record);
  if (!admitted) return false;
  const auto& physical = *admitted;
  const auto& row = request_.records->row(position == SIZE_MAX ? record.source_ordinal : position);
  if (!row.complete() || !row.native()) return false;
  auto original = original_input(physical.source);
  if (!original.terminal) return false;
  const auto& saved = row.native();
  const auto& lineage = saved->lineage().component;
  const auto identity = original.originals ? original.originals->identity(physical.source) : request_.recovery ? request_.recovery->original_identity(physical.source) : std::string_view{};
  return lineage.annotation_input_identity == request_.input_identity && saved->inventory().physical == physical && lineage.original_generation == original.generation &&
         lineage.original_annotation_identity == identity;
 }
 void consume(const CoconutRecord& record, std::span<const std::uint8_t> png, CompileAllowance allowance = {}, Worker* selected = nullptr, std::size_t position = SIZE_MAX,
  dataset::MaskDimensions dimensions = {}) {
  const auto admitted = identify(record, false, true);
  if (!admitted) {
   {
    const std::lock_guard lock(groups_mutex_);
    ++rows_;
   }
   retain_image(position == SIZE_MAX ? record.source_ordinal : position, {});
   return;
  }
  const auto& physical = *admitted;
  try {
   consume_image(record, physical, png, std::move(allowance), selected, position, dimensions);
  } catch (const CoconutInputError& error) {
   const bool required = !record.width || !record.height || std::ranges::any_of(record.segments, [](const auto& segment) { return segment.isthing || !segment.valid; });
   if (required && !request_.accept_partial) throw;
   if (position == SIZE_MAX) position = record.source_ordinal;
   if (request_.rejected_object) request_.rejected_object(physical, record, {}, error.what(), 0);
   auto product = CoconutNativeImage::rejected(record, physical, lineage_for(physical.source, nullptr, 0));
   {
    const std::lock_guard lock(groups_mutex_);
    ++rows_;
   }
   retain_image(position, std::move(product));
  }
 }
 void consume_image(const CoconutRecord& record, const CoconutPhysicalImage& physical, std::span<const std::uint8_t> png, CompileAllowance allowance, Worker* selected, std::size_t position,
  dataset::MaskDimensions dimensions) {
  if (position == SIZE_MAX) position = record.source_ordinal;
  auto& worker = selected ? *selected : archive_worker_;
  auto& workspace = worker.native;
  if (request_.membership_ready && request_.originals && !request_.metadata_only) {
   const std::lock_guard lock(groups_mutex_);
   auto& component = components_[physical.source];
   component.edition = request_.edition;
   component.source = physical.source;
   component.annotation_input_identity = component.input_identity = request_.input_identity;
   component.index.source = coconut_index_source(physical.source);
   component.index.split = coconut_component_split(request_.edition, physical.source);
   component.index.images.push_back({physical.image_id, 0, 0, record.width, record.height, physical.shard, 0});
   component.inventory.push_back({physical, record.image_id, record.source_ordinal});
  }
  const bool normalize = !request_.metadata_only && std::ranges::find(request_.retained_sources, physical.source) == request_.retained_sources.end();
  const auto original = normalize && request_.originals && physical.archive_identity == "pending" ? CoconutOriginalReadiness{{}, false, 0}
                        : normalize                                                               ? original_input(physical.source)
                                                                                                  : CoconutOriginalReadiness{{}, true, 0};
  bool deferred = !original.terminal;
  if (request_.originals) workspace.originals(original.originals);
  const auto lineage = lineage_for(physical.source, workspace.recovery(), original.generation);
  std::shared_ptr<const CoconutNativeImage> saved;
  {
   const std::lock_guard lock(groups_mutex_);
   const auto& candidate = request_.records->row(position).native();
   if (normalize && !deferred && candidate && candidate->inventory().physical == physical && candidate->lineage().component.annotation_input_identity == request_.input_identity &&
       candidate->lineage().component.original_generation == original.generation && candidate->lineage().component.original_annotation_identity == lineage->component.original_annotation_identity)
    saved = candidate;
  }
  std::size_t reused_position = 0;
  const auto* reused = normalize && !saved ? reusable_image(physical, reused_position) : nullptr;
  const auto publication = normalize ? request_.physical_membership->label_publication(request_.edition, physical) : BenchmarkSourcePublication{};
  if (normalize && !saved && !reused && png.empty()) throw CoconutInputError("missing required panoptic mask");
  std::size_t decode_bytes = 0;
  const auto inspect = [&](std::size_t) {
   if (!normalize || reused || saved) return;
   if (!dimensions.width) dimensions = admit_png(workspace, record, png, physical);
   decode_bytes = workspace.decoder_workspace_bytes(png);
  };
  if (request_.execution && !request_.execution->execution().on_lane())
   request_.execution->execution().run(CompileTaskKind::Projection, {}, inspect, allowance);
  else
   inspect(0);
  if (reused) dimensions = {reused->index().images()[reused_position].width, reused->index().images()[reused_position].height};
  auto upcoming = normalize && !saved ? workspace.support_workspace_bytes(record, std::uint64_t{dimensions.width} * dimensions.height, record.segments.size(), !deferred) : 0;
  const auto available = allowance && decode_bytes <= allowance.bytes() ? allowance.bytes() - decode_bytes : 0;
  // Originals may become ready after the actual PNG task was admitted. Keep
  // its support immutable and admit recovery independently if that new demand
  // cannot fit in the already supplied support range.
  if (!deferred && workspace.recovery() && allowance && upcoming > available) deferred = true;
  std::shared_ptr<const CoconutNativeImage> complete = saved;
  const auto work = [&](std::size_t) {
   throw_if_benchmark_cancelled(request_.cancellation);
   {
    const std::lock_guard lock(groups_mutex_);
    if (!offered_.insert(std::pair{physical.source, physical.image_id}).second) invalid("duplicate offered physical member: " + physical.member);
   }
   try {
    if (!normalize) {
     if (request_.metadata_only && std::ranges::find(request_.retained_sources, physical.source) == request_.retained_sources.end()) {
      const std::lock_guard lock(groups_mutex_);
      auto& component = components_[physical.source];
      static_cast<CoconutComponentMetadata&>(component) = lineage->component;
      static_cast<NormalizedAnnotationMetadata&>(component.index) = lineage->index;
      component.index.images.push_back({physical.image_id, 0, 0, record.width, record.height, physical.shard, 0});
      component.inventory.push_back({physical, record.image_id, record.source_ordinal});
      if (component.recovery_policy) component.recovery.push_back({physical.image_id, 0, {}});
     }
    } else if (!saved) {
     if (reused)
      complete =
       workspace.reuse(record, physical, reused->index(), reused_position, reused->inventory_image(reused_position), reused->recovery_policy() ? &reused->recovery_image(reused_position) : nullptr,
        lineage, reused->image_input_identity(reused_position), reused->required_labels_valid(reused_position), &worker.output);
     else {
      workspace.decode(record, png, dimensions, request_.execution ? request_.execution->execution().current_allowance().workspace() : std::span<std::byte>{});
      if (deferred) {
       auto native = std::make_shared<PendingNativeImage>();
       native->record = record;
       native->position = position;
       native->physical = physical;
       native->dimensions = dimensions;
       native->publication = publication;
       native->unlisted_segment = workspace.unlisted_segment();
       workspace.originals({});
       native->product = workspace.finish(record, physical, dimensions, lineage, {}, &worker.output);
       worker.output.retain_support(workspace.support(record.segments.size()), native->support);
       worker.pending = std::move(native);
      } else
       complete = workspace.finish(record, physical, dimensions, lineage, request_.rejected_object, &worker.output);
     }
    }
   } catch (const CoconutInputError& error) { throw CoconutInputError(physical.member + ": " + error.what()); }
   const std::lock_guard lock(groups_mutex_);
   ++rows_;
  };
  try {
   if (request_.execution && !request_.execution->execution().on_lane())
    request_.execution->execution().run(CompileTaskKind::Metadata, {}, work, allowance);
   else
    work(request_.execution ? request_.execution->execution().current_lane() : 0);
  } catch (...) {
   workspace.retire();
   throw;
  }
  worker.complete = std::move(complete);
  worker.publication = publication;
  worker.position = position;
  worker.saved = bool(saved);
  worker.reused = reused != nullptr;
  if (!normalize) complete_row();
  if (!request_.execution || !request_.execution->execution().on_lane()) {
   request_.records->staging->prepare(
    worker.output.request(), request_.execution ? &request_.execution->execution() : nullptr, request_.execution ? &request_.execution->files() : nullptr, request_.parent_allowance);
   worker.output.fill(request_.cancellation);
   worker.output = {};
   if (worker.pending)
    native_staged(*worker.pending->product, CompileTaskKind::Normalize);
   else if (worker.complete)
    native_staged(*worker.complete, CompileTaskKind::Normalize);
   settle_worker(worker);
  }
 }
 std::vector<CoconutComponent> finish(bool partial = false) {
  BenchmarkCostSpan drain(request_.trace, BenchmarkCostKind::RecoveryDrain, [&] {
   const std::lock_guard lock(groups_mutex_);
   return nlohmann::json{{"edition", request_.edition}, {"rows", rows_}, {"input_identity", request_.input_identity}};
  });
  normalizations_.drain();
  if (partial && rows_ == 0 && std::ranges::none_of(request_.records->rows(), [](const auto& row) { return bool(row.native()); })) return {};
  if (partial && !request_.records->groups.empty()) request_.records->rebase_parquet_segments();
  if (!partial && rows_ == 0) invalid("selected release contains no offered image rows");
  if (!partial && request_.expected_rows != 0 && rows_ != request_.expected_rows) invalid("offered row count does not match the selected release");
  if (request_.preparation_progress && request_.records->size() != 1 && request_.records->size() % kProgressQuantum != 0)
   request_.preparation_progress(CoconutPreparationStage::Annotations, request_.records->size());
  const bool early_membership = request_.membership_ready && !request_.metadata_only && recovery_controller_.joinable();
  if (!request_.metadata_only) {
   request_.records->archive.reset();
   request_.records->document_complete = request_.records->document_complete || bool(request_.records->parquet);
   request_.records->parquet.reset();
   if (request_.input_finished) request_.input_finished();
  }
  if (early_membership) request_.membership_ready(assemble(true));
  {
   const std::lock_guard lock(progress_mutex_);
   {
    const std::lock_guard facts_lock(groups_mutex_);
    input_done_ = true;
   }
   // Serialize the last row and source closure so exactly one reports the
   // final partial quantum before recovery's physical eligibility join.
   if (completed_rows_ == rows_ && completed_rows_ != 1 && completed_rows_ % kProgressQuantum != 0) report_rows();
  }
  recovery_signal_->notify();
  if (recovery_controller_.joinable()) recovery_controller_.join();
  if (recovery_failure_) std::rethrow_exception(recovery_failure_);
  drain.finish();
  throw_if_benchmark_cancelled(request_.cancellation);
  auto result = assemble(request_.metadata_only);
  if (request_.membership_ready && !request_.metadata_only && !early_membership) {
   std::vector<CoconutComponent> membership;
   membership.reserve(result.size());
   for (const auto& component : result) membership.push_back(component.membership());
   request_.membership_ready(std::move(membership));
  }
  if (!request_.metadata_only) request_.records->retire_native();
  request_.records->staging->retire();
  return result;
 }
 std::vector<CoconutComponent> assemble(bool metadata_only) {
  if (!metadata_only) {
   request_.parent_allowance.release_unused_continuation();
   BenchmarkCostSpan wait(
    request_.trace, BenchmarkCostKind::PhysicalWait, [&] { return nlohmann::json{{"edition", request_.edition}, {"rows", request_.records->size()}, {"input_identity", request_.input_identity}}; });
   trace_benchmark_event(request_.trace, "benchmark.annotations.physical_join_wait", [&] { return nlohmann::json{{"edition", request_.edition}, {"rows", request_.records->size()}}; });
   request_.physical_membership->wait_sources(request_.edition);
   wait.finish();
   trace_benchmark_event(request_.trace, "benchmark.annotations.physical_join_ready", [&] { return nlohmann::json{{"edition", request_.edition}, {"rows", request_.records->size()}}; });
   for (std::size_t position = 0; position < request_.records->size(); ++position) {
    const auto& row = request_.records->row(position);
    if (!row.native()) continue;
    if (!row.native()->image().width || !row.native()->image().height) {
     request_.records->native(position, {});
     continue;
    }
    const auto physical = identify(row.record(), true, true);
    if (physical) {
     const auto& lineage = row.native()->lineage().component;
     if (lineage.recovery_policy && lineage.source != physical->source) throw CoconutOriginalChanged(lineage.source);
     request_.records->native(position, CoconutNativeImage::bind(row.native(), *physical));
    } else {
     if (request_.execution) {
      const auto& previous = row.native()->inventory().physical;
      const auto root = request_.physical_membership->identity_root(previous.source);
      request_.execution->retire_labels(root, previous.image_id, row.native()->input_identity());
      request_.execution->retire_labels(root, previous.image_id, row.native()->input_identity() + ".provisional");
     }
     request_.records->native(position, {});
    }
   }
  }
  std::uint64_t image_count = 0, recovery_join_boxes = 0;
  BenchmarkCostSpan gather_cost(request_.trace, BenchmarkCostKind::ComponentGather, [&] {
   return nlohmann::json{
    {"edition", request_.edition}, {"rows_revisited", request_.records->size()}, {"images", image_count}, {"metadata_only", metadata_only}, {"input_identity", request_.input_identity}
   };
  });
  for (const auto& row : request_.records->rows())
   if (const auto& native = row.native(); !metadata_only && native && std::ranges::find(request_.retained_sources, native->lineage().component.source) == request_.retained_sources.end()) {
    ++image_count;
    if (native->lineage().component.recovery_policy) recovery_join_boxes = std::max(recovery_join_boxes, std::uint64_t{native->boxes().size()});
   }
  if (metadata_only)
   for (const auto& [source, component] : components_) {
    (void)source;
    image_count += component.inventory.size();
   }
  // Layout references retain their credit through final assembly and intervening
  // inventory I/O; only finite CPU work enters the shared lane.
  auto merge_allowance =
   request_.execution ? request_.execution->execution().reserve(coconut_component_assembly_resources(image_count, recovery_join_boxes), request_.parent_allowance) : CompileAllowance{};
  std::map<CoconutImageNamespace, std::vector<CoconutComponentImage>> images;
  std::map<CoconutImageNamespace, AnnotationRejectCounts> rejected;
  const auto gather = [&](CoconutComponentImage chunk, const AnnotationRejectCounts& counts) {
   const auto source = chunk.component->source;
   images[source].push_back(chunk);
   mmltk::frameworks::reflection::visit_materialized_members<AnnotationRejectCounts>([&]<class Declaration>(const auto&) {
    rejected[source].*Declaration::pointer = mmltk::common::math::checked_add(rejected[source].*Declaration::pointer, counts.*Declaration::pointer, "COCONut rejection count overflow");
   });
  };
  const auto gather_images = [&](std::size_t) {
   if (!metadata_only)
    for (const auto& row : request_.records->rows()) {
     const auto& native = row.native();
     if (!native || std::ranges::find(request_.retained_sources, native->lineage().component.source) != request_.retained_sources.end()) continue;
     if (row.segment_ordinal() < native->segment_begin()) invalid("native chunk ordinal exceeds its canonical row");
     gather({&native->lineage().component, &native->lineage().index, &native->image(), native->boxes(), native->runs(), &native->inventory(), &native->recovery(),
             row.segment_ordinal() - native->segment_begin(), &native->input_identity(), native->required_labels_valid(), row.segment_ordinal_complete(), native->admission()},
      native->rejected());
    }
   if (metadata_only)
    for (auto& [source, component] : components_) {
     (void)source;
     for (std::size_t i = 0; i < component.inventory.size(); ++i)
      gather({&component, &component.index, &component.index.images[i], {}, {}, &component.inventory[i], component.recovery_policy ? &component.recovery[i] : nullptr, 0}, {});
    }
  };
  if (request_.execution)
   request_.execution->execution().run(CompileTaskKind::Metadata, {}, gather_images, merge_allowance);
  else
   gather_images(0);
  std::vector<CoconutComponent> result;
  for (auto& [source, rows] : images) {
   throw_if_benchmark_cancelled(request_.cancellation);
   if (!metadata_only && request_.originals && (source == CoconutImageNamespace::CocoTrain || source == CoconutImageNamespace::CocoValidation)) {
    auto original = request_.originals(source, true, {});
    original_owner_ = std::move(original.originals);
    assembly_workspace_.originals(original_owner_);
   }
   auto header = *rows.front().component;
   if (!metadata_only && request_.originals && (source == CoconutImageNamespace::CocoTrain || source == CoconutImageNamespace::CocoValidation)) {
    const auto original = request_.originals(source, true, {});
    header = lineage_for(source, assembly_workspace_.recovery(), original.generation)->component;
    for (const auto& row : rows)
     if (row.component->original_generation != header.original_generation || row.component->original_annotation_identity != header.original_annotation_identity) throw CoconutOriginalChanged(source);
   }
   std::optional<std::uint64_t> source_extent;
   if ((request_.records->parquet || request_.records->document_complete) && std::ranges::all_of(request_.records->rows(), [](const auto& row) { return row.complete(); })) {
    source_extent = 0;
    for (const auto& row : request_.records->rows()) *source_extent = common::math::checked_add(*source_extent, std::uint64_t{row.record().segments.size()}, "COCONut source segment extent overflow");
   }
   auto component = assemble_coconut_component(header, rows, rejected[source], metadata_only, request_.records->size(), source_extent, source_ordinals_,
    request_.records->identity == request_.input_identity ? std::span<const CoconutComponent>(request_.records->inventories) : std::span<const CoconutComponent>{}, *request_.physical_membership,
    metadata_only ? nullptr : assembly_workspace_.recovery(), request_.inventory_directory, request_.cancellation, request_.execution ? &request_.execution->storage() : nullptr, request_.execution,
    merge_allowance, request_.accept_partial || request_.reject_invalid_identities, [&](const CoconutInventoryImage& image, std::string_view reason) {
    if (request_.rejected_object) {
     CoconutRecord available;
     available.image_id = image.release_image_id;
     available.file_name = image.physical.member;
     request_.rejected_object(image.physical, available, {}, reason, 0);
    }
   }, request_.trace);
   if (!component) continue;
   if (request_.records && metadata_only) request_.records->inventories.push_back(*component);
   result.push_back(std::move(*component));
  }
  throw_if_benchmark_cancelled(request_.cancellation);
  return result;
 }

private:
 static constexpr std::uint64_t kProgressQuantum = 64;
 CoconutOriginalReadiness original_input(CoconutImageNamespace source) const {
  if (request_.originals && (source == CoconutImageNamespace::CocoTrain || source == CoconutImageNamespace::CocoValidation)) return request_.originals(source, false, {});
  return {{}, true, 0};
 }
 std::shared_ptr<const CoconutNativeLineage> lineage_for(CoconutImageNamespace source, const CoconutMaskRecovery* recovery, std::uint64_t generation) {
  const auto original = recovery ? recovery->original_identity(source) : std::string_view{};
  const std::lock_guard lock(groups_mutex_);
  auto& lineage = lineages_[source];
  if (!lineage || lineage->component.original_generation != generation || lineage->component.original_annotation_identity != original) {
   auto next = std::make_shared<CoconutNativeLineage>();
   auto& component = next->component;
   component.edition = request_.edition;
   component.source = source;
   component.annotation_input_identity = component.input_identity = request_.input_identity;
   component.original_generation = generation;
   component.original_annotation_identity = original;
   if (!original.empty()) component.recovery_policy = kCoconutRecoveryPolicy;
   next->index.source = coconut_index_source(source);
   next->index.split = coconut_component_split(request_.edition, source);
   lineage = std::move(next);
  }
  return lineage;
 }
 const CoconutComponent* reusable_image(const CoconutPhysicalImage& physical, std::size_t& position) const {
  for (const auto& component : request_.reusable_images) {
   if (component.source() != physical.source) continue;
   const auto images = component.index().images();
   const auto found = std::ranges::lower_bound(images, physical.image_id, {}, &NormalizedImage::source_image_id);
   if (found == images.end() || (*found).source_image_id != physical.image_id) continue;
   position = static_cast<std::size_t>(found - images.begin());
   if (component.inventory_image(position).physical == physical) return &component;
  }
  return nullptr;
 }
 void retain_image(std::size_t position, std::shared_ptr<const CoconutNativeImage> product) {
  {
   const std::lock_guard lock(groups_mutex_);
   request_.records->native(position, std::move(product));
  }
  complete_row();
 }
 void report_rows() {
  if (request_.progress) request_.progress(completed_rows_);
  if (request_.metadata_only && request_.preparation_progress) request_.preparation_progress(CoconutPreparationStage::Membership, completed_rows_);
 }
 void complete_row() {
  const std::lock_guard lock(progress_mutex_);
  ++completed_rows_;
  if (completed_rows_ != 1 && completed_rows_ % kProgressQuantum != 0) {
   const std::lock_guard facts_lock(groups_mutex_);
   if (!input_done_ || completed_rows_ != rows_) return;
  }
  report_rows();
 }
 void native_staged(const CoconutNativeImage& image, CompileTaskKind kind) const {
  trace_benchmark_event(request_.trace, "benchmark.coconut.native_staged", [&] {
   const auto snapshot = request_.execution ? request_.execution->execution().snapshot() : detail::CompileExecutionSnapshot{};
   return nlohmann::json{
    {"image_id", image.inventory().physical.image_id}, {"kind", kind}, {"admitted_runs", image.admission() ? image.runs().size() : 0}, {"boxes", image.boxes().size()}, {"runs", image.runs().size()},
    {"normal_locked", snapshot.normal_locked}, {"leased", snapshot.leased}, {"exceptional", snapshot.exceptional}
   };
  });
 }
 void settle_report(const CoconutNativeImage& image) {
  // Recovery omissions are reported later by the recipe. Metadata, retained
  // products and healthy rows must not flush another producer's report bytes.
  if (image.rejected().degenerate_boxes && !image.lineage().component.recovery_policy && request_.image_terminal) request_.image_terminal();
 }
 void publish_labels(std::shared_ptr<const CoconutNativeImage> image, const BenchmarkSourcePublication& publication, bool provisional = false) {
  if (!request_.execution) return;
  const auto& physical = image->inventory().physical;
  const auto& lineage = image->lineage().component;
  request_.execution->labels_ready(publication, physical.image_id, CoconutNativeImage::labels(image), provisional ? image->input_identity() + ".provisional" : image->input_identity(),
   lineage.original_generation,
   provisional                                ? 0U
   : lineage.edition == CoconutEdition::Large ? 2U
                                              : 1U);
 }
 void defer_native(std::shared_ptr<PendingNativeImage> native) {
  const std::lock_guard lock(groups_mutex_);
  if (recovery_failure_) std::rethrow_exception(recovery_failure_);
  pending_.emplace(native->position, native);
  recovery_signal_->enqueue(native);
  if (!recovery_controller_.joinable())
   recovery_controller_ = std::jthread([this](std::stop_token stop) {
    try {
     recover_pending(stop);
    } catch (...) {
     const std::lock_guard failure_lock(groups_mutex_);
     recovery_failure_ = std::current_exception();
    }
    recovery_signal_->notify();
   });
  recovery_signal_->notify();
 }
 void recover_pending(std::stop_token stop) {
  const auto signal = recovery_signal_;
  const std::stop_callback stopping(stop, [signal] { signal->notify(); });
  std::unique_ptr<CompileExecution::Readiness> interrupted;
  if (request_.execution)
   interrupted = std::make_unique<CompileExecution::Readiness>(request_.execution->execution(), CompileChange::Interruption, [signal](auto change) {
    if (includes(change.causes, CompileChange::Interruption)) signal->notify();
   });
  std::vector<std::unique_ptr<Worker>> idle_recovery;
  BenchmarkCompletionWindow ready(native_window, request_.execution ? &request_.execution->execution() : nullptr);
  for (;;) {
   const auto observed = signal->revision.load(std::memory_order_acquire);
   ready.reap();
   if (stop.stop_requested()) {
    ready.drain();
    return;
   }
   if (request_.execution)
    if (auto error = request_.execution->execution().interruption()) std::rethrow_exception(error);
   throw_if_benchmark_cancelled(request_.cancellation);
   std::array<std::shared_ptr<PendingNativeImage>, native_window> pending;
   std::size_t count = 0;
   bool done = false, empty = false;
   {
    const std::lock_guard lock(groups_mutex_);
    done = input_done_;
    empty = pending_.empty();
   }
   while (count < pending.size()) {
    auto next = signal->take();
    if (!next) break;
    pending[count++] = std::move(next);
   }
   if (empty && done) {
    ready.drain();
    const std::lock_guard lock(groups_mutex_);
    if (pending_.empty()) return;
    continue;
   }
   bool progressed = false;
   for (const auto& native : std::span(pending).first(count)) {
    trace_benchmark_event(request_.trace, "benchmark.coconut.recovery_observed", [&] { return nlohmann::json{{"image_id", native->record.image_id}, {"source_ordinal", native->position}}; });
    const auto physical = request_.physical_membership->readiness(request_.edition, native->record);
    const auto source = physical.image ? physical.image->source : native->physical.source;
    auto original = original_input(source);
    // Physical custody is per row, including a terminal image awaiting its
    // originals. The snapshot already owns its next-change registration.
    if ((!physical.terminal || !original.terminal) && (!native->physical_change || native->physical_change.ready())) {
     native->physical_change = physical.changed;
     native->physical_change.subscribe([weak_signal = std::weak_ptr(signal), weak = std::weak_ptr(native)](std::exception_ptr) {
      if (auto ready_signal = weak_signal.lock())
       if (auto row = weak.lock()) ready_signal->enqueue(row);
     });
    }
    signal->observe_original(native, source, original);
    const bool recovered_identity = native->recovered && physical.image && *physical.image == native->physical;
    if (recovered_identity && original.generation != native->original_generation) throw CoconutOriginalChanged(source);
    if (recovered_identity && physical.terminal && original.terminal) {
     settle_report(*native->product);
     retire_pending(native);
     continue;
    }
    if ((!physical.image && !physical.terminal) || !original.terminal || recovered_identity) {
     trace_benchmark_event(request_.trace, "benchmark.coconut.recovery_parked", [&] {
      return nlohmann::json{
       {"image_id", native->record.image_id}, {"source", source}, {"physical_generation", physical.generation}, {"original_generation", original.generation}, {"physical_ready", bool(physical.image)},
       {"original_ready", original.terminal}
      };
     });
     signal->park(native);
     continue;
    }
    signal->working(native);
    progressed = true;
    if (native->recovered) {
     if (request_.execution) request_.execution->retire_labels(request_.physical_membership->identity_root(native->physical.source), native->physical.image_id, native->product->input_identity());
     native->product.reset();
     native->recovered = false;
    }
    if (!physical.image) {
     (void)identify(native->record, true, true);
     if (request_.execution) request_.execution->retire_labels(request_.physical_membership->identity_root(native->physical.source), native->physical.image_id, native->provisional_dependency);
     retain_pending(native, {});
     retire_pending(native);
     continue;
    }
    native->physical = *physical.image;
    native->physical_generation = physical.generation;
    native->original_generation = original.generation;
    native->publication = request_.physical_membership->label_publication(request_.edition, native->physical);
    ready.make_room();
    auto worker = idle_recovery.empty() ? std::make_unique<Worker>(request_) : std::move(idle_recovery.back());
    if (!idle_recovery.empty()) idle_recovery.pop_back();
    worker->native.originals(original.originals);
    const auto pixels = std::uint64_t{native->dimensions.width} * native->dimensions.height;
    const auto bytes = worker->native.support_workspace_bytes(native->record, pixels, native->record.segments.size());
    trace_benchmark_event(request_.trace, "benchmark.coconut.recovery_ready", [&] {
     return nlohmann::json{
      {"image_id", native->record.image_id}, {"source", source}, {"physical_generation", physical.generation}, {"original_generation", original.generation}, {"support_bytes", bytes}
     };
    });
    const auto recover = [this, native, original](Worker& target) {
     auto& workspace = target.native;
     workspace.borrow_support(native->record, native->support, native->dimensions, native->unlisted_segment);
     native->product =
      workspace.finish(native->record, native->physical, native->dimensions, lineage_for(native->physical.source, workspace.recovery(), original.generation), request_.rejected_object, &target.output);
     target.complete = native->product;
    };
    const auto settle = [this, native, original](std::exception_ptr failure) {
     if (failure) {
      native->product.reset();
      std::rethrow_exception(failure);
     }
     const auto current = original_input(native->physical.source);
     if (current.generation != original.generation) throw CoconutOriginalChanged(native->physical.source);
     const auto current_physical = request_.physical_membership->readiness(request_.edition, native->record);
     if (!current_physical.image || *current_physical.image != native->physical) {
      // The immutable PNG support remains valid while its physical join is
      // withdrawn. Re-arm that record without retaining the finished arena.
      native->product.reset();
      recovery_signal_->retry(native);
      return;
     }
     retain_pending(native, native->product);
     publish_labels(native->product, native->publication);
     native->recovered = true;
     if (current_physical.terminal) {
      settle_report(*native->product);
      retire_pending(native);
     } else {
      // Recovery's result is usable now, but its immutable input support and
      // subscription remain until physical eligibility admits that identity.
      // Rechecking through the ready queue closes changes during CPU work.
      recovery_signal_->retry(native);
     }
     trace_benchmark_event(request_.trace, "benchmark.coconut.recovery_settled",
      [&] { return nlohmann::json{{"image_id", native->record.image_id}, {"source", native->physical.source}, {"physical_terminal", current_physical.terminal}}; });
    };
    if (request_.execution) {
     auto job = std::make_shared<NativeJob>(request_.execution->execution(), CompileTaskKind::Recovery);
     job->worker = std::move(worker);
     auto completion = submit_native(job, bytes, [recover, job](const CompileAllowance&) { recover(*job->worker); });
     track_native(ready, completion, [&, job, settle](std::exception_ptr failure) {
      settle(failure);
      job->worker->complete.reset();
      idle_recovery.push_back(std::move(job->worker));
     });
     completion.subscribe([signal](std::exception_ptr) { signal->notify(); });
    } else {
     recover(*worker);
     request_.records->staging->prepare(worker->output.request());
     worker->output.fill(request_.cancellation);
     native_staged(*native->product, CompileTaskKind::Recovery);
     settle({});
     worker->complete.reset();
     idle_recovery.push_back(std::move(worker));
    }
   }
   if (!progressed && !count) {
    std::unique_lock lock(signal->mutex);
    signal->changed.wait(lock, [&] { return stop.stop_requested() || signal->revision.load(std::memory_order_acquire) != observed; });
   }
  }
 }
 const CoconutImportRequest& request_;
 Worker archive_worker_;
 CoconutNativeWorkspace assembly_workspace_;
 std::shared_ptr<const CoconutRecoveryOriginals> original_owner_;
 std::unordered_map<std::size_t, std::shared_ptr<PendingNativeImage>> pending_;
 // Intrusive ready links reuse each retained row: notification neither scans
 // the backlog nor allocates a second work record. All subscription callbacks
 // hold only weak row custody and this signal, never the importer.
 struct RecoverySignal : std::enable_shared_from_this<RecoverySignal> {
  using State = PendingNativeImage::Readiness;
  std::mutex mutex;
  std::condition_variable changed;
  std::atomic<std::uint64_t> revision{0};
  std::shared_ptr<PendingNativeImage> first;
  PendingNativeImage* last = nullptr;
  struct OriginalGroup {
   std::unordered_map<std::size_t, std::weak_ptr<PendingNativeImage>> rows;
   bool delivered = false;
  };
  std::map<PendingNativeImage::OriginalWait, OriginalGroup> originals;
  BenchmarkTraceSink trace;
  explicit RecoverySignal(BenchmarkTraceSink observer) : trace(std::move(observer)) {}
  ~RecoverySignal() { clear(); }
  void clear() {
   std::shared_ptr<PendingNativeImage> rows;
   {
    const std::lock_guard lock(mutex);
    originals.clear();
    rows = std::move(first);
    last = nullptr;
   }
   // Releasing immutable support can notify other resource owners. Do it
   // outside the readiness lock and break the shared links iteratively.
   while (rows) {
    auto row = std::move(rows);
    rows = std::move(row->next_ready);
   }
  }
  void remove_original(const std::shared_ptr<PendingNativeImage>& row) {
   if (!row->original_wait) return;
   const auto found = originals.find(*row->original_wait);
   if (found != originals.end()) {
    found->second.rows.erase(row->position);
    if (found->second.rows.empty()) found->second.rows = decltype(found->second.rows){};
   }
   row->original_wait.reset();
   // Retain one receipt per observed namespace/generation, never per retired
   // row. Later observations join its existing callback or immediate delivery.
  }
  void observe_original(const std::shared_ptr<PendingNativeImage>& row, CoconutImageNamespace source, const CoconutOriginalReadiness& original) {
   const PendingNativeImage::OriginalWait key{source, original.generation};
   bool subscribe = false;
   {
    const std::lock_guard lock(mutex);
    if (original.terminal || !original.changed) {
     remove_original(row);
     return;
    }
    if (row->original_wait == key) return;
    remove_original(row);
    auto [group, inserted] = originals.try_emplace(key);
    if (group->second.delivered) {
     enqueue_locked(row);
     return;
    }
    group->second.rows.emplace(row->position, row);
    row->original_wait = key;
    subscribe = inserted;
   }
   if (!subscribe) return;
   trace_benchmark_event(trace, "benchmark.coconut.original_subscribed", [&] { return nlohmann::json{{"source", source}, {"generation", original.generation}}; });
   // Membership precedes subscription. A completion that raced its snapshot
   // delivers immediately; callbacks never capture a row or importer.
   original.changed.subscribe([weak = weak_from_this(), key](std::exception_ptr) {
    if (auto signal = weak.lock()) signal->original_ready(key);
   });
  }
  void original_ready(PendingNativeImage::OriginalWait key) {
   std::size_t count = 0;
   {
    const std::lock_guard lock(mutex);
    const auto found = originals.find(key);
    if (found == originals.end()) return;
    if (trace) count = found->second.rows.size();
    for (const auto& [position, weak] : found->second.rows) {
     (void)position;
     if (auto row = weak.lock()) {
      row->original_wait.reset();
      enqueue_locked(row);
     }
    }
    found->second = OriginalGroup{.rows = {}, .delivered = true};
   }
   trace_benchmark_event(trace, "benchmark.coconut.original_notified", [&] { return nlohmann::json{{"source", key.first}, {"generation", key.second}, {"rows", count}}; });
  }
  void append(const std::shared_ptr<PendingNativeImage>& row) {
   row->readiness = State::Ready;
   if (last)
    last->next_ready = row;
   else
    first = row;
   last = row.get();
   revision.fetch_add(1, std::memory_order_release);
   changed.notify_all();
  }
  void enqueue_locked(const std::shared_ptr<PendingNativeImage>& row) {
   if (row->readiness == State::Checking)
    row->notified = true;
   else if (row->readiness == State::Parked)
    append(row);
  }
  void enqueue(const std::shared_ptr<PendingNativeImage>& row) {
   const std::lock_guard lock(mutex);
   enqueue_locked(row);
  }
  std::shared_ptr<PendingNativeImage> take() {
   const std::lock_guard lock(mutex);
   if (!first) return {};
   auto row = std::move(first);
   first = std::move(row->next_ready);
   if (!first) last = nullptr;
   row->readiness = State::Checking;
   row->notified = false;
   return row;
  }
  void park(const std::shared_ptr<PendingNativeImage>& row) {
   const std::lock_guard lock(mutex);
   if (row->notified)
    append(row);
   else
    row->readiness = State::Parked;
  }
  void working(const std::shared_ptr<PendingNativeImage>& row) {
   const std::lock_guard lock(mutex);
   row->readiness = State::Working;
  }
  void retry(const std::shared_ptr<PendingNativeImage>& row) {
   const std::lock_guard lock(mutex);
   append(row);
  }
  void retire(const std::shared_ptr<PendingNativeImage>& row) {
   const std::lock_guard lock(mutex);
   row->readiness = State::Retired;
   remove_original(row);
  }
  void notify() {
   {
    const std::lock_guard lock(mutex);
    revision.fetch_add(1, std::memory_order_release);
   }
   changed.notify_all();
  }
 };
 void retain_pending(const std::shared_ptr<PendingNativeImage>& native, std::shared_ptr<const CoconutNativeImage> product) {
  {
   const std::lock_guard lock(groups_mutex_);
   request_.records->native(native->position, std::move(product));
  }
  if (!std::exchange(native->counted, true)) complete_row();
 }
 void retire_pending(const std::shared_ptr<PendingNativeImage>& native) {
  recovery_signal_->retire(native);
  native->physical_change = {};
  {
   const std::lock_guard lock(groups_mutex_);
   pending_.erase(native->position);
  }
  trace_benchmark_event(request_.trace, "benchmark.coconut.recovery_retired", [&] { return nlohmann::json{{"image_id", native->record.image_id}, {"source_ordinal", native->position}}; });
 }
 std::shared_ptr<RecoverySignal> recovery_signal_ = std::make_shared<RecoverySignal>(request_.trace);
 std::exception_ptr recovery_failure_;
 bool input_done_ = false;
 std::jthread recovery_controller_;
 std::mutex groups_mutex_, progress_mutex_;
 std::uint64_t completed_rows_ = 0, rows_ = 0, parsed_rows_ = 0;
 std::mutex outputs_mutex_;
 std::array<std::shared_ptr<NativeJob>, 2 * native_window> outputs_{};
 std::unique_ptr<CompileExecution::Readiness> output_readiness_;
 BenchmarkCompletionWindow normalizations_;
 std::vector<std::unique_ptr<Worker>> idle_;
 std::set<std::pair<CoconutImageNamespace, std::uint64_t>> offered_;
 std::map<CoconutImageNamespace, std::shared_ptr<const CoconutNativeLineage>> lineages_;
 std::map<CoconutImageNamespace, CoconutComponentBuilder> components_;
 std::vector<std::size_t> source_ordinals_;
};
// One forward discovery cursor plus the archive's generation-bound locators.
// Ready annotation chunks consume their masks immediately; later chunks reuse
// encountered positions instead of repeating an archive census.
class AnnotationMaskStream final {
 struct Mask {
  std::size_t position;
  bool consumed = false;
 };

public:
 AnnotationMaskStream(const CoconutImportRequest& request, CoconutAnnotationRecords& records, Importer& importer)
     : request_(request),
       records_(records),
       importer_(importer),
       archive_(records.archive),
       prefix_(request.edition == CoconutEdition::XLarge  ? "coconuts_xlarge/panseg/"
               : request.edition == CoconutEdition::Large ? "panoptic_object365/"
                                                          : "panoptic_o365val_v3/") {}
 ~AnnotationMaskStream() {
  importer_.retire_scratch();
  if (archive_) archive_->pause();
 }
 void consume(std::size_t position) {
  const auto& record = records_.record(position);
  const auto member = prefix_ + record.physical_stem + ".png";
  const auto [mask, inserted] = wanted_.emplace(member, Mask{position});
  if (!inserted) invalid("duplicate offered mask: " + member);
  ensure();
  if (archive_->has_member(member) && !archive_->direct_member(member)) return;
  bool found = archive_->seek(member, request_.cancellation);
  if (!found) {
   if (forward_ && archive_->position() != forward_ordinal_) (void)archive_->seek(*forward_, request_.cancellation);
   while (archive_->next(request_.cancellation)) {
    forward_ = archive_->member_position();
    forward_ordinal_ = archive_->position();
    if (request_.preparation_progress && (forward_ordinal_ == 0 || (forward_ordinal_ + 1) % 64 == 0)) request_.preparation_progress(CoconutPreparationStage::Archive, forward_ordinal_ + 1);
    if (archive_->member() == member) {
     found = true;
     break;
    }
   }
  }
  if (!found)
   importer_.consume_group(0, record, {}, position);
  else
   consume_current(position);
  mask->second.consumed = true;
 }
 bool consume_known(std::size_t position) {
  const auto& record = records_.record(position);
  const auto member = prefix_ + record.physical_stem + ".png";
  auto& mask = wanted_.try_emplace(member, Mask{position}).first->second;
  if (mask.consumed) return true;
  if (!archive_->has_member(member)) return false;
  const auto cursor = archive_->member_position();
  const auto ordinal = archive_->position();
  const bool current = archive_->member() == member;
  if (!current && !archive_->direct_member(member)) return false;
  const bool retained_cursor = ensure();
  if ((!retained_cursor || !current) && !archive_->seek(member, request_.cancellation)) invalid("discovered mask disappeared from its archive generation");
  consume_current(position);
  mask.consumed = true;
  if (archive_->position() != ordinal) (void)archive_->seek(cursor, request_.cancellation);
  return true;
 }
 void metadata(std::uint64_t bytes, const std::function<void()>& work) {
  if (request_.execution && bytes > request_.execution->execution().workspace_capacity()) {
   if (archive_) pause();
   importer_.drain_normalization();
  }
  work();
 }
 void resume() { (void)ensure(); }
 void suspend() {
  if (archive_) pause();
 }
 void finish() {
  if (!archive_) return;
  std::vector<std::string> pending;
  for (const auto& [member, mask] : wanted_) {
   if (mask.consumed) continue;
   if (!archive_->has_member(member)) {
    importer_.consume_group(0, records_.record(mask.position), {}, mask.position);
    continue;
   }
   pending.push_back(member);
  }
  // Context-dependent and compressed inputs share one ordered remainder pass.
  // Never replay their prefix independently for each out-of-order annotation.
  archive_->visit_known(pending, [&](std::size_t i) {
   auto& mask = wanted_.at(pending[i]);
   consume_current(mask.position);
   mask.consumed = true;
  }, request_.cancellation);
  archive_->inspect_members([&](std::string_view member, bool conflict) {
   if (!member.starts_with(prefix_) || !member.ends_with(".png")) return;
   if (!wanted_.contains(std::string(member))) {
    if (request_.edition != CoconutEdition::XLarge) invalid("extra mask without annotation: " + std::string(member));
    return;
   }
   if (conflict) invalid("duplicate archive mask: " + std::string(member));
  });
  pause();
 }

private:
 void pause() {
  importer_.retire_scratch();
  archive_->suspend(request_.parent_allowance);
  paused_ = true;
 }
 void consume_current(std::size_t position) {
  const auto& record = records_.record(position);
  archive_->require_regular(request_.limits.max_png_bytes);
  const auto bytes = common::math::checked_cast<std::size_t>(archive_->size(), "COCONut PNG source range overflow");
  CoconutStaging::Request range(bytes, alignof(std::uint8_t));
  records_.staging->prepare(range, request_.execution ? &request_.execution->execution() : nullptr, request_.execution ? &request_.execution->files() : nullptr, request_.parent_allowance);
  const auto output = range.target();
  archive_->read_into({reinterpret_cast<std::uint8_t*>(output.data()), output.size()}, request_.cancellation);
  auto staged = range.bytes();
  CoconutAnnotationInput input{
   .png = {reinterpret_cast<const std::uint8_t*>(staged.values.data()), staged.values.size()}, .backing = std::move(staged.owner), .live_bytes = staged.values.size(), .retire_projection = [&] {
   pause();
  }
  };
  importer_.consume_group(0, record, input, position);
  if (paused_) (void)ensure();
 }
 bool ensure(std::uint64_t = 0) {
  // Only the actual bounded source read/parser window belongs to this cursor.
  // Semantic descendants own staged immutable input and reserve independently.
  constexpr std::uint64_t source_window = 0;
  if (!archive_) {
   archive_ =
    std::make_shared<Archive>(request_.mask_archive, request_.execution ? &request_.execution->execution() : nullptr, source_window, request_.parent_allowance, 1, true, CompileAllowance{}, 1024, 1);
   records_.archive = archive_;
   admitted_ = true;
  } else if (paused_ || !admitted_) {
   archive_->suspend(request_.parent_allowance);
   while (!archive_->try_resume(source_window, request_.parent_allowance, 1)) {
    const auto generation = request_.execution ? request_.execution->execution().admission_generation() : 0;
    importer_.drain_normalization();
    if (archive_->try_resume(source_window, request_.parent_allowance, 1)) break;
    if (request_.execution) {
     auto pressure = request_.execution->execution().defer_resources();
     request_.execution->execution().wait_for_admission_change(generation);
    }
   }
   admitted_ = true;
   paused_ = false;
   return false;
  }
  return true;
 }
 const CoconutImportRequest& request_;
 CoconutAnnotationRecords& records_;
 Importer& importer_;
 std::shared_ptr<Archive> archive_;
 std::string prefix_;
 std::unordered_map<std::string, Mask> wanted_;
 std::optional<Archive::MemberPosition> forward_;
 std::uint64_t forward_ordinal_ = 0;
 bool paused_ = false, admitted_ = false;
};
std::uint32_t dimension(const JsonAtom& field, const CoconutImportLimits& limits) {
 if (!field || field.is_null()) return 0;
 const auto value = field.unsigned_integer();
 if (value == 0 || value > limits.max_dimension) invalid("image dimension exceeds admission");
 return static_cast<std::uint32_t>(value);
}
std::uint64_t json_workspace_bytes(std::size_t bytes, const CoconutImportLimits& limits) {
 const auto nested = common::math::checked_multiply(
  std::min<std::uint64_t>(limits.max_segments, bytes / 2) + 1, std::uint64_t{3 * (sizeof(JsonSegment) + 4 * sizeof(JsonAtom))}, "COCONut JSON nested workspace overflow");
 return common::math::checked_add(nested,
  common::math::checked_add(std::uint64_t{65536}, common::math::checked_multiply(std::uint64_t{bytes}, 32U, "COCONut JSON workspace overflow"), "COCONut JSON workspace overflow"),
  "COCONut JSON workspace overflow");
}
void json_rows(const CoconutImportRequest& request, const PaddedMappedFile& input, std::span<const ByteRange> rows, std::vector<JsonParser>& parsers, CompileExecution::Workspace* workspace,
 const std::function<void(const JsonRow&, std::size_t)>& consume, const std::function<void(std::size_t, std::size_t)>& ready = {}, bool parallel = false,
 const std::function<bool(std::size_t)>& parsed = {}, AnnotationMaskStream* masks = nullptr) {
 // Source/lane parser capacity survives bounded callbacks under IdleScratch
 // custody. The typed row borrows it only during this synchronous callback.
 for (std::size_t first = 0; first < rows.size();) {
  auto end = first + 1;
  while (end < rows.size() && rows[end].end - rows[first].begin <= (256U << 10)) ++end;
  const auto parse = [&](std::size_t job) {
   auto& storage = parsers[request.execution ? request.execution->execution().current_lane() : 0];
   struct Account {
    JsonParser& parser;
    ~Account() { parser.account(); }
   } accounting{storage};
   for (auto i = parallel ? first + job : first; i < (parallel ? first + job + 1 : end); ++i) {
    throw_if_benchmark_cancelled(request.cancellation);
    if (parsed && parsed(i)) continue;
    const auto range = rows[i];
    auto document = storage.parser.iterate(simdjson::padded_string_view(input.data() + range.begin, range.end - range.begin, input.capacity_from(range.begin)));
    JsonConsumption admission{0, static_cast<std::size_t>(request.limits.max_segments) * 32U + 128U, true, request.limits.max_segments};
    auto& row = storage.row;
    row.reset();
    consume_json_value(document.get_value().value(), admission, 2, nullptr, &row);
    if (!row.object) invalid("panoptic row must be an object");
    consume(row, i);
    if (request.execution && !parallel && (i - first + 1) % 32 == 0) request.execution->execution().cooperate();
   }
  };
  // Previous nested high-water capacity is charged until real retirement.
  // Counting visits only the bounded lanes; each row stores its own O(1) total.
  std::uint64_t retained = 0, largest = 0;
  for (const auto& parser : parsers) retained = std::max(retained, parser.retained_bytes());
  for (auto i = first; i < end; ++i) largest = std::max(largest, std::uint64_t{rows[i].end - rows[i].begin});
  const auto demand = [&](std::size_t job) {
   const auto bytes = parallel ? rows[first + job].end - rows[first + job].begin : largest;
   const auto incoming = json_workspace_bytes(bytes, request.limits);
   return CompileResources{mmltk::common::math::checked_add(retained, incoming, "COCONut retained JSON workspace overflow"), 0};
  };
  const auto work = [&] {
   if (request.execution)
    request.execution->execution().for_each(CompileTaskKind::Metadata, parallel ? end - first : 1, demand, parse, *workspace);
   else if (parallel) {
    for (auto i = first; i < end; ++i) parse(i - first);
   } else
    parse(0);
  };
  if (masks && request.execution) {
   std::uint64_t largest_demand = 0;
   for (std::size_t i = 0; i < (parallel ? end - first : 1); ++i) largest_demand = std::max(largest_demand, demand(i).bytes);
   masks->metadata(largest_demand, work);
  } else
   work();
  if (ready) ready(first, end);
  first = end;
 }
}
void json_records(const CoconutImportRequest& request, CoconutAnnotationRecords& retained, AnnotationMaskStream* masks = nullptr) {
 throw_if_benchmark_cancelled(request.cancellation);
 const auto& categories = coconut_categories();
 NumericCategoryAdmission category_admission(categories);
 struct ImageRow {
  std::optional<std::uint64_t> id;
  std::string file_name;
  std::string physical_stem;
  std::uint32_t width = 0, height = 0;
 };
 std::vector<ImageRow> images;
 std::unordered_map<std::uint64_t, std::size_t> by_id;
 std::unordered_map<std::string, std::size_t> by_file, by_physical_stem;
 const auto physical_stem = [](const JsonRow& row) {
  std::string stem;
  for (const auto* field : {&row.get<&CoconutJsonRow::object365_file_name>(), &row.get<&CoconutJsonRow::object365_name>(), &row.get<&CoconutJsonRow::file_name>()}) {
   if (!*field || field->is_null()) continue;
   const auto name = field->text();
   if (name.empty() || (field == &row.get<&CoconutJsonRow::file_name>() && !std::filesystem::path(name).filename().string().starts_with("objects365_"))) continue;
   const auto parsed = parse_coconut_objects_member(name);
   if (!stem.empty() && stem != parsed.stem) invalid("contradictory declared Objects365 members");
   stem = parsed.stem;
  }
  return stem;
 };
 auto handles = request.execution ? request.execution->execution().reserve(CompileResources::handles(1, true), request.parent_allowance) : CompileAllowance{};
 const PaddedMappedFile input(request.annotation_json);
 std::vector<JsonParser> parsers(request.execution ? request.execution->execution().workers() : 1);
 std::optional<CompileExecution::Workspace> parser_workspace;
 if (request.execution) parser_workspace.emplace(request.execution->execution(), [&](std::size_t lane) noexcept { parsers[lane].retire(); });
 auto* workspace = parser_workspace ? &*parser_workspace : nullptr;
 constexpr std::array<std::string_view, 3> fields{"categories", "images", "annotations"};
 const auto category_row = [&](const JsonRow& row, std::size_t) {
  std::optional<std::uint32_t> id;
  std::optional<std::string> name;
  if (row.get<&CoconutJsonRow::id>()) id = mmltk::common::math::checked_cast<std::uint32_t>(row.get<&CoconutJsonRow::id>().unsigned_integer(), "category ID overflow");
  if (row.get<&CoconutJsonRow::name>()) name = std::string(row.get<&CoconutJsonRow::name>().text());
  category_admission.observe(id, name ? std::optional<std::string_view>(*name) : std::nullopt);
 };
 const auto image_row = [&](const JsonRow& row, std::size_t) {
  ImageRow image;
  if (row.get<&CoconutJsonRow::id>()) {
   image.id = row.get<&CoconutJsonRow::id>().unsigned_integer();
   if (!by_id.emplace(*image.id, images.size()).second) invalid("duplicate JSON image ID");
  }
  if (row.get<&CoconutJsonRow::file_name>()) {
   image.file_name = std::string(row.get<&CoconutJsonRow::file_name>().text());
   if (!by_file.emplace(image.file_name, images.size()).second) invalid("duplicate JSON image filename");
  }
  image.physical_stem = physical_stem(row);
  if (!image.physical_stem.empty() && !by_physical_stem.emplace(image.physical_stem, images.size()).second) invalid("duplicate JSON physical image: " + image.physical_stem);
  image.width = dimension(row.get<&CoconutJsonRow::width>(), request.limits);
  image.height = dimension(row.get<&CoconutJsonRow::height>(), request.limits);
  images.push_back(std::move(image));
 };
 std::size_t discovered_rows = 0;
 std::vector<bool> joined_images;
 std::uint64_t segment_ordinal = 0;
 std::size_t ready_records = 0;
 const auto annotations = [&](std::span<const ByteRange> ranges) {
  const auto base = discovered_rows;
  discovered_rows += ranges.size();
  if (retained.size() < discovered_rows) retained.resize(discovered_rows);
  const auto annotation_row = [&](const JsonRow& annotation, std::size_t position) {
   throw_if_benchmark_cancelled(request.cancellation);
   CoconutRecord record;
   record.source_ordinal = base + position;
   std::optional<std::size_t> image_index;
   const auto join = [&](const auto& lookup, const auto& key) {
    const auto found = lookup.find(key);
    if (found == lookup.end()) return;
    if (image_index && *image_index != found->second) invalid("contradictory JSON image joins: " + record.file_name);
    image_index = found->second;
   };
   if (annotation.get<&CoconutJsonRow::image_id>()) {
    record.image_id = annotation.get<&CoconutJsonRow::image_id>().unsigned_integer();
    join(by_id, record.image_id);
   }
   if (annotation.get<&CoconutJsonRow::file_name>()) {
    record.file_name = std::string(annotation.get<&CoconutJsonRow::file_name>().text());
    join(by_file, record.file_name);
   }
   record.physical_stem = physical_stem(annotation);
   if (!record.physical_stem.empty()) join(by_physical_stem, record.physical_stem);
   if (!image_index && request.edition == CoconutEdition::ObjectsValidation) invalid("validation annotation has no image-record join: " + record.file_name);
   if (image_index) {
    const auto& image = images[*image_index];
    if (image.id && annotation.get<&CoconutJsonRow::image_id>() && *image.id != record.image_id) invalid("annotation/image ID disagreement");
    if (!image.physical_stem.empty()) {
     if (!record.physical_stem.empty() && record.physical_stem != image.physical_stem) invalid("contradictory declared Objects365 members");
     record.physical_stem = image.physical_stem;
    }
    record.width = image.width;
    record.height = image.height;
   }
   if (record.physical_stem.empty()) invalid("unresolved offered JSON annotation: " + record.file_name);
   if (!annotation.get<&CoconutJsonRow::image_id>()) {
    const auto declared_id = image_index ? images[*image_index].id : std::nullopt;
    record.image_id = declared_id ? *declared_id : parse_coconut_objects_member(record.physical_stem).id;
   }
   segments_from_json(annotation.get<&CoconutJsonRow::segments_info>(), record, request.limits);
   retained.admit_json(base + position, std::move(record), image_index);
  };
  const auto ready = [&](std::size_t first, std::size_t end) {
   if (request.preparation_progress) request.preparation_progress(CoconutPreparationStage::Annotations, base + end);
   for (auto i = first; i < end; ++i) {
    retained.stage_segments(base + i, request.execution ? &request.execution->execution() : nullptr, request.execution ? &request.execution->files() : nullptr, request.parent_allowance);
    const auto& row = retained.row(base + i);
    const auto& record = row.record();
    if (row.image_join()) {
     if (joined_images[*row.image_join()]) invalid("multiple annotations join one image row: " + record.file_name);
     joined_images[*row.image_join()] = true;
    }
    retained.json_prefix(base + i, segment_ordinal);
    segment_ordinal = mmltk::common::math::checked_add(segment_ordinal, std::uint64_t{record.segments.size()}, "segment ordinal overflow");
   }
   // A ready chunk can start its physical images before later annotation rows
   // parse. Resolution may wait on sources, so it runs after CPU custody ends.
   while (ready_records < base + end) {
    if (masks)
     masks->consume(ready_records++);
    else
     (void)request.physical_membership->identify(request.edition, retained.record(ready_records++));
   }
  };
  json_rows(request, input, ranges, parsers, workspace, annotation_row, ready, true, [&](std::size_t i) { return retained.row(base + i).complete(); }, masks);
 };
 bool categories_complete = false, images_complete = false;
 std::vector<ByteRange> pending_annotations;
 discover_json_arrays(input, fields, true, [&](std::size_t field, std::span<const ByteRange> rows, bool complete) {
  if (field == 0) {
   json_rows(request, input, rows, parsers, workspace, category_row, {}, false, {}, masks);
   if (complete) {
    category_admission.complete();
    categories_complete = true;
   }
  } else if (field == 1) {
   json_rows(request, input, rows, parsers, workspace, image_row, {}, false, {}, masks);
   if (request.preparation_progress) request.preparation_progress(CoconutPreparationStage::Images, images.size());
   if (complete) {
    joined_images.assign(images.size(), false);
    images_complete = true;
   }
  } else if (categories_complete && images_complete)
   annotations(rows);
  else
   pending_annotations.insert(pending_annotations.end(), rows.begin(), rows.end());
  if (categories_complete && images_complete && !pending_annotations.empty()) {
   annotations(pending_annotations);
   std::vector<ByteRange>().swap(pending_annotations);
  }
 }, request.cancellation, (request.execution ? &request.execution->execution() : nullptr), [&](const std::function<void()>& work) {
  if (masks)
   masks->metadata(65536, work);
  else
   work();
 });
 for (std::size_t begin = 0; begin < images.size(); begin += 256) {
  const auto check_joins = [&](std::size_t) {
   for (auto i = begin; i < std::min(images.size(), begin + 256); ++i) {
    throw_if_benchmark_cancelled(request.cancellation);
    if (!joined_images[i]) invalid("offered JSON image has no annotation/mask join: " + images[i].file_name);
   }
  };
  if (request.execution)
   request.execution->execution().run(CompileTaskKind::Metadata, {}, check_joins);
  else
   check_joins(0);
 }
 if (discovered_rows != retained.size()) invalid("retained JSON row count mismatch");
 retained.document_complete = true;
}
void xlarge_records(const CoconutImportRequest& request, CoconutAnnotationRecords& retained, Importer* importer = nullptr) try {
 constexpr std::uint64_t workspace = 0;
 auto owned = retained.archive;
 if (owned)
  owned->resume(workspace, request.parent_allowance);
 else
  owned = std::make_shared<Archive>(request.mask_archive, (request.execution ? &request.execution->execution() : nullptr), workspace, request.parent_allowance, 1, true, CompileAllowance{}, 1024, 0);
 auto& archive = *owned;
 retained.archive = owned;
 std::unique_ptr<AnnotationMaskStream> masks;
 if (importer) masks = std::make_unique<AnnotationMaskStream>(request, retained, *importer);
 std::unordered_map<std::string, std::size_t> records;
 records.reserve(retained.size());
 std::unordered_set<std::string> encountered;
 for (std::size_t i = 0; i < retained.size(); ++i) records.emplace(retained.record(i).physical_stem, i);
 struct Parser {
  simdjson::ondemand::parser parser;
  JsonArray<JsonSegment> segments;
  std::atomic<std::uint64_t> retained{0};
 };
 std::vector<Parser> parsers(request.execution ? request.execution->execution().workers() : 1);
 std::optional<CompileExecution::Workspace> parser_workspace;
 if (request.execution)
  parser_workspace.emplace(request.execution->execution(), [&](std::size_t lane) noexcept {
   parsers[lane].parser = simdjson::ondemand::parser{};
   parsers[lane].segments = {};
   parsers[lane].retained.store(0, std::memory_order_relaxed);
  });
 bool suspended = false;
 const auto resume = [&] {
  if (masks) {
   masks->resume();
   return;
  }
  if (!suspended) return;
  while (!archive.try_resume(workspace, request.parent_allowance)) {
   const auto generation = request.execution->execution().admission_generation();
   if (archive.try_resume(workspace, request.parent_allowance)) break;
   // A parked controller must request reclamation of idle parser custody;
   // waiting for a generation alone cannot make that retained capacity free.
   auto pressure = request.execution->execution().defer_resources();
   request.execution->execution().wait_for_admission_change(generation);
  }
  suspended = false;
 };
 constexpr std::string_view prefix = "coconuts_xlarge/panseg_info/";
 while ((resume(), archive.next(request.cancellation))) {
  if (request.preparation_progress && (archive.position() == 0 || (archive.position() + 1) % 64 == 0)) request.preparation_progress(CoconutPreparationStage::Archive, archive.position() + 1);
  if (masks && archive.regular() && archive.member().starts_with("coconuts_xlarge/panseg/") && archive.member().ends_with(".png")) {
   const auto stem = std::filesystem::path(archive.member()).stem().string();
   if (const auto found = records.find(stem); found != records.end()) (void)masks->consume_known(found->second);
   continue;
  }
  if (!archive.regular() || !archive.member().starts_with(prefix) || !archive.member().ends_with(".json")) continue;
  const auto name = parse_coconut_objects_member(archive.member());
  if (name.source != CoconutImageNamespace::Objects365V2 || archive.member() != std::string(prefix) + name.stem + ".json") invalid("unsupported XL info member: " + archive.member());
  if (!encountered.insert(name.stem).second) invalid("duplicate XL info member: " + archive.member());
  if (const auto found = records.find(name.stem); found != records.end()) {
   if (masks)
    (void)masks->consume_known(found->second);
   else
    (void)request.physical_membership->identify(request.edition, retained.record(found->second));
   continue;
  }
  CoconutRecord record;
  record.image_id = name.id;
  record.physical_stem = name.stem;
  archive.require_regular(16U * 1024U * 1024U);
  const auto bytes = common::math::checked_cast<std::size_t>(archive.size(), "COCONut XL source range overflow");
  const auto member = archive.member();
  // The parser can exceed an ordinary task context. Its immutable source range
  // survives suspension of the forward archive and retains the opened inode.
  CoconutStaging::Request range(common::math::checked_add(bytes, std::size_t{simdjson::SIMDJSON_PADDING}, "COCONut XL source padding overflow"), alignof(char));
  retained.staging->prepare(range, request.execution ? &request.execution->execution() : nullptr, request.execution ? &request.execution->files() : nullptr, request.parent_allowance);
  const auto output = range.target();
  archive.read_into({reinterpret_cast<std::uint8_t*>(output.data()), bytes}, request.cancellation);
  std::memset(output.data() + bytes, 0, simdjson::SIMDJSON_PADDING);
  const auto input = range.bytes();
  std::uint64_t prior = 0;
  for (const auto& state : parsers) prior = std::max(prior, state.retained.load(std::memory_order_relaxed));
  const auto demand = common::math::checked_add(json_workspace_bytes(bytes, request.limits), prior, "COCONut XL parser workspace overflow");
  const auto parse = [&](std::size_t) {
   auto& state = parsers[request.execution ? request.execution->execution().current_lane() : 0];
   struct Account {
    Parser& state;
    ~Account() { state.retained.store(state.parser.capacity() * 32ULL + state.segments.retained_bytes(), std::memory_order_relaxed); }
   } accounting{state};
   auto& parser = state.parser;
   auto& segments = state.segments;
   auto document = parser.iterate(simdjson::padded_string_view(reinterpret_cast<const char*>(input.values.data()), bytes, input.values.size()));
   if (document.type().value() != simdjson::ondemand::json_type::array) invalid("XL info member requires a segments array: " + member);
   JsonConsumption admission{0, 0, false, request.limits.max_segments};
   segments.reset();
   consume_json_value(document.get_value().value(), admission, 0, nullptr, nullptr, nullptr, &segments);
   segments_from_json(segments, record, request.limits);
  };
  if (request.execution) {
   if (demand > request.execution->execution().workspace_capacity()) {
    if (masks)
     masks->suspend();
    else {
     archive.suspend(request.parent_allowance);
     suspended = true;
    }
    if (importer) importer->drain_normalization();
   }
   request.execution->execution().for_each(CompileTaskKind::Metadata, 1, [&](std::size_t) { return CompileResources{demand, 0}; }, parse, *parser_workspace);
  } else
   parse(0);
  const auto position = retained.size();
  record.source_ordinal = position;
  const auto [inserted, unique] = records.emplace(name.stem, position);
  if (!unique) invalid("duplicate XL info member: " + archive.member());
  retained.resize(position + 1);
  retained.admit_json(position, std::move(record));
  retained.stage_segments(position, request.execution ? &request.execution->execution() : nullptr, request.execution ? &request.execution->files() : nullptr, request.parent_allowance);
  if (request.preparation_progress && (position == 0 || (position + 1) % 64 == 0)) request.preparation_progress(CoconutPreparationStage::Annotations, position + 1);
  // Resolve real image work as each JSON record arrives. XL source ordering is
  // assigned below; its full physical stem is already a stable logical key.
  if (masks)
   (void)masks->consume_known(inserted->second);
  else
   (void)request.physical_membership->identify(request.edition, retained.record(inserted->second));
 }
 if (encountered.size() != records.size()) invalid("retained XL row count mismatch");
 throw_if_benchmark_cancelled(request.cancellation);
 if (masks) {
  for (std::size_t i = 0; i < retained.size(); ++i) (void)masks->consume_known(i);
  masks->finish();
  importer->drain_normalization();
  importer->canonical_ordinals(retained.seal_xlarge_order());
 } else
  (void)retained.seal_xlarge_order();
} catch (const simdjson::simdjson_error& error) { reject_json_document(error); }
}  // namespace
void CoconutAnnotationRecords::admit_metadata(std::size_t position, CoconutRecord record) {
 auto& row = rows_.at(position);
 if (row.metadata_ready()) return;
 row.record_ = std::move(record);
 row.state_ = Row::State::Metadata;
 metadata_rows_.fetch_add(1, std::memory_order_relaxed);
}
void CoconutAnnotationRecords::admit_segments(std::size_t position, std::vector<CoconutSegment> segments, std::uint64_t local_ordinal) {
 auto& row = rows_.at(position);
 if (row.complete()) return;
 if (!row.metadata_ready()) throw std::logic_error("COCONut: segments have no admitted image row");
 row.record_.segments = CoconutSegments(std::move(segments));
 row.record_.first_segment_ordinal = local_ordinal;
 row.segment_ordinal_ = local_ordinal;
 row.state_ = Row::State::Complete;
}
void CoconutAnnotationRecords::stage_segments(std::size_t position, detail::CompileExecution* execution, detail::FileWorkQueue* files, const CompileAllowance& parent) {
 if (!staging) staging = std::make_shared<CoconutStaging>(staging_directory, storage);
 rows_.at(position).record_.segments.stage(*staging, execution, files, parent);
}
void CoconutAnnotationRecords::adopt(std::vector<CoconutRecord> records) {
 std::deque<Row> rows(records.size());
 for (std::size_t i = 0; i < records.size(); ++i) {
  rows[i].segment_ordinal_ = records[i].first_segment_ordinal;
  rows[i].record_ = std::move(records[i]);
  rows[i].state_ = Row::State::Complete;
 }
 rows_ = std::move(rows);
 metadata_rows_.store(rows_.size(), std::memory_order_relaxed);
 prefixes_sealed_ = false;
 document_complete = true;
}
void CoconutAnnotationRecords::admit_json(std::size_t position, CoconutRecord record, std::optional<std::size_t> image_join) {
 auto& row = rows_.at(position);
 if (row.complete()) return;
 if (!row.metadata_ready()) metadata_rows_.fetch_add(1, std::memory_order_relaxed);
 row.record_ = std::move(record);
 row.image_join_ = image_join;
 row.segment_ordinal_ = row.record_.first_segment_ordinal;
 row.state_ = Row::State::Complete;
}
void CoconutAnnotationRecords::json_prefix(std::size_t position, std::uint64_t ordinal) {
 auto& row = rows_.at(position);
 if (row.native_ && row.record_.first_segment_ordinal != ordinal) throw std::logic_error("COCONut: cannot change a published JSON ordinal");
 row.record_.first_segment_ordinal = row.segment_ordinal_ = ordinal;
}
std::vector<std::size_t> CoconutAnnotationRecords::seal_xlarge_order() {
 // Validate before moving any owned rows. Sorting and ordinal assignment then
 // form one nonallocating commit; cancellation cannot leave a partial move.
 std::uint64_t ordinal = 0;
 std::vector<std::size_t> order(rows_.size(), SIZE_MAX);
 for (const auto& row : rows_) {
  if (!row.complete() || row.record_.source_ordinal >= order.size() || order[row.record_.source_ordinal] != SIZE_MAX) invalid("cannot order incomplete or duplicate annotation rows");
  order[row.record_.source_ordinal] = 0;
  ordinal = mmltk::common::math::checked_add(ordinal, std::uint64_t{row.record_.segments.size()}, "segment ordinal overflow");
 }
 std::ranges::sort(rows_, {}, [](const Row& row) -> const std::string& { return row.record_.physical_stem; });
 ordinal = 0;
 for (std::size_t i = 0; i < rows_.size(); ++i) {
  auto& row = rows_[i];
  order[row.record_.source_ordinal] = i;
  row.record_.source_ordinal = i;
  row.record_.first_segment_ordinal = row.segment_ordinal_ = ordinal;
  ordinal += row.record_.segments.size();
 }
 document_complete = true;
 return order;
}
void CoconutAnnotationRecords::native(std::size_t position, std::shared_ptr<const CoconutNativeImage> product) {
 const std::lock_guard lock(rows_mutex_);
 auto& row = rows_.at(position);
 if (product && !row.complete()) throw std::logic_error("COCONut: native image has no complete annotation row");
 row.native_ = std::move(product);
}
void CoconutAnnotationRecords::retire_native() noexcept {
 for (auto& row : rows_) row.native_.reset();
}
void CoconutAnnotationRecords::discard() noexcept {
 archive.reset();
 parquet.reset();
 groups.clear();
 inventories.clear();
 staging.reset();
 std::deque<Row>().swap(rows_);
 metadata_rows_.store(0, std::memory_order_relaxed);
 identity.clear();
 document_complete = false;
 prefixes_sealed_ = false;
}
std::shared_ptr<BenchmarkArchive> discover_coconut_masks(const CoconutImportRequest& request, std::stop_token stop) {
 auto archive = std::make_shared<Archive>(request.mask_archive, (request.execution ? &request.execution->execution() : nullptr), 0, request.parent_allowance, 1, true, CompileAllowance{}, 1024, 0);
 // Keep only the archive owner's compact locators. The eventual annotation
 // consumer borrows this same generation and its forward compressed context.
 try {
  while (!stop.stop_requested() && archive->next(request.cancellation)) {
   if (request.preparation_progress && (archive->position() == 0 || (archive->position() + 1) % 64 == 0)) request.preparation_progress(CoconutPreparationStage::Archive, archive->position() + 1);
  }
  archive->pause();
  return archive;
 } catch (...) {
  archive->pause();
  throw;
 }
}
std::vector<CoconutComponent> import_coconut_annotations(const CoconutImportRequest& supplied) {
 auto producer = supplied.execution ? supplied.execution->execution().open_class(detail::CompileWorkClass::Annotations) : CompileExecution::Producer{};
 auto retained = supplied.records ? supplied.records : std::make_shared<CoconutAnnotationRecords>();
 auto request = supplied;
 request.records = retained;
 retained->staging_directory = request.inventory_directory;
 retained->storage = request.execution ? &request.execution->storage() : nullptr;
 if (retained->identity != request.input_identity) retained->discard();
 if (!retained->staging) retained->staging = std::make_shared<CoconutStaging>(retained->staging_directory, retained->storage);
 retained->identity = request.input_identity;
 (void)coconut_release_component(request.edition);
 Importer importer(request);
 if (request.finish_retained) {
  if (!request.accept_partial) throw std::logic_error("retained annotation settlement requires partial admission");
  return importer.finish(true);
 }
 try {
  if (request.edition == CoconutEdition::Base || request.edition == CoconutEdition::RelabeledValidation) {
   if (request.parquet_shards.empty()) invalid("missing Parquet shards");
   read_coconut_parquet(request.parquet_shards, request.limits, request.cancellation,
    [&](std::size_t group, const CoconutRecord& record, const CoconutAnnotationInput& input) { importer.consume_group(group, record, input); }, request.metadata_only,
    request.execution ? &request.execution->execution() : nullptr, request.execution ? &request.execution->files() : nullptr, [&] { importer.settle_normalizations(); }, request.parent_allowance,
    retained.get(), [&](const CoconutRecord& record) { return importer.reusable_native(record); },
    [&](const CoconutRecord& record, std::span<const std::uint8_t> png) { return importer.stage_parquet_input(record, png); }, request.trace);
   importer.collect_groups();
  } else {
   if (request.metadata_only) {
    if (!retained->document_complete) {
     if (request.edition == CoconutEdition::XLarge)
      xlarge_records(request, *retained);
     else
      json_records(request, *retained);
    }
    for (std::size_t i = 0; i < retained->size(); ++i) importer.consume(retained->record(i), {}, retained->archive ? retained->archive->allowance() : CompileAllowance{}, nullptr, i);
    if (retained->archive) retained->archive->pause();
   } else if (request.mask_archive.empty() && request.accept_partial) {
    if (!retained->document_complete) json_records(request, *retained);
    for (std::size_t i = 0; i < retained->size(); ++i) importer.consume_group(0, retained->record(i), {}, i);
   } else if (request.edition == CoconutEdition::XLarge && !retained->document_complete) {
    xlarge_records(request, *retained, &importer);
   } else {
    AnnotationMaskStream masks(request, *retained, importer);
    if (!retained->document_complete)
     json_records(request, *retained, &masks);
    else
     for (std::size_t i = 0; i < retained->size(); ++i) masks.consume(i);
    masks.finish();
   }
  }
  if (retained->archive) retained->archive->pause();
  return importer.finish();
 } catch (const AnnotationDocumentRejected& error) {
  if (!request.accept_partial) throw CoconutInputError(error.what());
  if (request.rejected_input) request.rejected_input(error.what());
  if (retained->archive) retained->archive->pause();
  return importer.finish(true);
 } catch (const CoconutInputError& error) {
  if (!request.accept_partial) throw;
  if (request.rejected_input) request.rejected_input(error.what());
  if (retained->archive) retained->archive->pause();
  return importer.finish(true);
 } catch (const BenchmarkArchiveError& error) {
  if (!request.accept_partial) throw CoconutInputError(error.what());
  if (request.rejected_input) request.rejected_input(error.what());
  if (retained->archive) retained->archive->pause();
  return importer.finish(true);
 } catch (...) {
  if (retained->archive) retained->archive->pause();
  importer.stop_recovery();
  throw;
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
