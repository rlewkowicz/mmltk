#include "src/backend/data/tests/profile_support.h"
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_writer.h"
#include "src/backend/data/compiled/compiled_dataset.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_parquet.h"
#include "src/backend/data/detail/compile_workspace.h"
#include "src/backend/data/detail/compile_pixel_workspace.h"
#include "src/common/io/staging_directory.h"
#include "src/common/io/file_digest.h"
#include "src/common/math/checked_arithmetic.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
namespace mmltk::backend::data::testsupport::profile {
namespace {
using namespace benchmark_internal;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
struct AnnotationObservation {
 std::uint64_t unchanged = 0, recovered = 0, staged_bytes = 0, copied_bytes = 0;
 std::uint64_t gather_tasks = 0, gather_images = 0, gather_boxes = 0, gather_runs = 0;
 std::uint64_t label_attempts = 0, label_writes = 0, settlements = 0;
};
MMLTK_REFLECT_FIELDS(AnnotationObservation)
class AnnotationEvidence final {
 bool enabled_;
 std::mutex mutex_;
 AnnotationObservation observation_;

public:
 explicit AnnotationEvidence(bool enabled) : enabled_(enabled) {}
 void receive(std::string_view event, const nlohmann::json& fields) {
  if (!enabled_ || !fields.is_object()) return;
  const std::lock_guard lock(mutex_);
  if (event == "benchmark.coconut.native_finalized") {
   observation_.unchanged += fields.at("unchanged").get<bool>();
   observation_.recovered += fields.at("recovered").get<std::uint64_t>();
  } else if (event == "benchmark.coconut.native_staged") {
   observation_.staged_bytes += fields.at("staged_bytes").get<std::uint64_t>();
   observation_.copied_bytes += fields.at("copied_bytes").get<std::uint64_t>();
  } else if (event == "benchmark.annotations.gathered") {
   observation_.gather_tasks += fields.at("tasks").get<std::uint64_t>();
   observation_.gather_images += fields.at("images").get<std::uint64_t>();
   observation_.gather_boxes += fields.at("boxes").get<std::uint64_t>();
   observation_.gather_runs += fields.at("runs").get<std::uint64_t>();
  }
 }
 void projected() {
  if (enabled_) {
   const std::lock_guard lock(mutex_);
   ++observation_.label_attempts;
  }
 }
 void written() {
  if (enabled_) {
   const std::lock_guard lock(mutex_);
   ++observation_.label_writes;
  }
 }
 void settled(std::size_t count) {
  if (enabled_) {
   const std::lock_guard lock(mutex_);
   observation_.settlements += count;
  }
 }
 nlohmann::json report() {
  const std::lock_guard lock(mutex_);
  return benchmark_observation_fields(observation_);
 }
};
// Optional bounded spacing evidence. No sample allocation or clock read in an
// ordinary throughput iteration. Completion callbacks never authorize products.
class CompletionGaps final {
 bool enabled_;
 std::mutex mutex_;
 Clock::time_point previous_{};
 std::vector<std::uint64_t> samples_;

public:
 CompletionGaps(bool enabled, std::size_t count) : enabled_(enabled) {
  if (enabled_) samples_.reserve(count);
 }
 void completed() {
  if (!enabled_) return;
  const std::lock_guard lock(mutex_);
  const auto now = Clock::now();
  if (previous_ != Clock::time_point{}) samples_.push_back(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous_).count()));
  previous_ = now;
 }
 nlohmann::json report() {
  const std::lock_guard lock(mutex_);
  return enabled_ ? nlohmann::json(samples_) : nlohmann::json{};
 }
};
}  // namespace
// Focused archive input variant uses the same production transport/admission
// owners and bounded fixture inventory; it never downloads a source.
static void run_archive_input_profile(const Options& opts, std::span<const ProfileImageFixture> inputs, const BenchmarkTraceSink& trace) {
 using namespace detail;
 using namespace benchmark_internal;
 const auto setup = Clock::now();
 const auto order = profile_order(opts, inputs.size());
 fs::create_directories(opts.test_dir);
 mmltk::common::io::StagingDirectory fixture(fs::path(opts.test_dir) / "archive", ".", ".XXXXXX", "cannot create bounded archive fixture");
 const auto path = fixture.path() / "source.tar.gz";
 const auto names = profile_archive_fixture(path, inputs, order, 64U << 20);
 const auto source_hash = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(path));
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(1, {}, 0, 0, 8U << 20));
 CompileExecution execution(profile_workers(opts), {}, {.transient_bytes = 32U << 20, .workspace_plan = plan}, {}, observe_compile_execution(trace));
 std::uint64_t copied = 0, reclaimed = 0, pressure_checks = 0;
 const auto started = Clock::now();
 {
  BenchmarkArchive reader(path, &execution, 0, {}, 1, true, {}, 1024, 0, 32768, trace);
  std::vector<std::uint8_t> output;
  for (std::size_t i = 0; i < order.size(); ++i) {
   if (!reader.next() || reader.member() != names[i]) throw std::runtime_error("archive profile lost source ordering");
   if (i && i % 16 == 0 && opts.archive_pressure != ProfileArchivePressure::None) {
    CompileResources demand;
    CompileAllowance held;
    switch (opts.archive_pressure) {
     case ProfileArchivePressure::Arena:
      demand = {.bytes = execution.workspace_capacity(), .supplied_workspace = true};
      held = execution.reserve(demand);
      break;
     case ProfileArchivePressure::Bytes:
      held = execution.reserve({.bytes = execution.transient_target() - reader.allowance().bytes()});
      demand = {.bytes = 1};
      break;
     case ProfileArchivePressure::Exclusive: demand = {.bytes = execution.workspace_capacity() + 1, .supplied_workspace = true}; break;
     case ProfileArchivePressure::None: break;
    }
    if (execution.try_reserve(demand)) throw std::runtime_error("archive profile pressure was not blocked");
    auto wait = execution.defer_resources(demand);
    ++pressure_checks;
    if (reader.reclamation_useful()) {
     reader.suspend();
     ++reclaimed;
    }
    held = {};
    auto accepted = execution.try_reserve(demand);
    if (!accepted) throw std::runtime_error("archive profile competing demand did not progress");
    execution.run(CompileTaskKind::Pixels, {}, [](std::size_t) {}, *accepted);
   }
   output.resize(inputs[order[i]].encoded.size());
   reader.read_into(output);
   if (!std::ranges::equal(output, inputs[order[i]].encoded)) throw std::runtime_error("archive profile changed member bytes");
   copied += output.size();
  }
  reader.pause();
 }
 execution.join();
 const auto remaining = execution.resource_usage();
 if (remaining.bytes || remaining.descriptors || remaining.continuation_descriptors) throw std::runtime_error("archive profile retained resource custody");
 const auto ended = Clock::now();
 nlohmann::json result{
  {"members", order.size()}, {"compressed_bytes", fs::file_size(path)}, {"source_sha256", source_hash}, {"copied_bytes", copied}, {"pressure_checks", pressure_checks}, {"reclaimed", reclaimed},
  {"pressure", std::string(mmltk::frameworks::reflection::enum_name(opts.archive_pressure))}, {"setup_sec", seconds_since(setup, started)}, {"read_sec", seconds_since(started, ended)},
  {"input_order", order}
 };
 std::printf("archive_profile %s\n", result.dump().c_str());
 trace_benchmark_event(trace, "profile.archive.completed", [&] { return result; });
 record_duration_metric("archive", "setup", setup, started);
 record_duration_metric("archive", "read", started, ended);
}
void run_pipeline_profile(const Options& opts, std::span<const ProfileImageFixture> inputs, const BenchmarkTraceSink& trace) {
 if (opts.input_storage == ProfileInputStorage::Archive) {
  run_archive_input_profile(opts, inputs, trace);
  return;
 }
 using namespace benchmark_internal;
 using namespace detail;
 using namespace mmltk::backend::imaging::resample;
 namespace math = mmltk::common::math;
 namespace io = mmltk::common::io;
 const auto setup_start = Clock::now();
 const auto workers = profile_workers(opts);
 const auto order = profile_order(opts, inputs.size());
 const auto cap = static_cast<std::uint64_t>(opts.output_cap_mib) << 20;
 fs::create_directories(opts.test_dir);
 io::StagingDirectory fixture(fs::path(opts.test_dir) / "pipeline", ".", ".XXXXXX", "cannot create bounded pipeline fixture");
 std::unique_ptr<PipelineNativeFixture> native;
 if (opts.annotations == PipelineAnnotations::DeferredClean || opts.annotations == PipelineAnnotations::DeferredRecovery)
  native = std::make_unique<PipelineNativeFixture>(fixture.path(), inputs, order, opts.annotations == PipelineAnnotations::DeferredRecovery);
 const auto annotation_sha256 = native ? native->input_identity() : std::string{};
 std::uint64_t input_bytes = 0, logical_input_bytes = 0;
 std::size_t largest_input = 0, largest_workspace = 0, label_workspace = 0;
 const CompilePixelPolicy policy{static_cast<std::uint32_t>(opts.width), static_cast<std::uint32_t>(opts.height), ImageResizeMode::Stretch, opts.perceptual};
 for (const auto& input : inputs) {
  if (input.encoded.size() > (64ULL << 20) - input_bytes) throw std::length_error("pipeline unique encoded inputs exceed 64 MiB");
  input_bytes += input.encoded.size();
  largest_input = std::max(largest_input, input.encoded.size());
  CompileImageDecoder decoder;
  const auto header = decoder.read_header(input.encoded, CompileImagePolicy::Benchmark);
  const auto plan = compile_pixel_workspace_plan(header, policy);
  largest_workspace = std::max(largest_workspace, math::checked_add<std::size_t>(plan.bytes(), input.encoded.size(), "pipeline workspace overflow"));
  label_workspace = std::max<std::size_t>(label_workspace, BenchmarkLabelWorkspace::required_bytes(input.width));
 }
 for (auto i : order) {
  if (inputs[i].encoded.size() > (1ULL << 30) - logical_input_bytes) throw std::length_error("pipeline logical input exceeds 1 GiB");
  logical_input_bytes += inputs[i].encoded.size();
 }
 std::array<CompileWorkspaceProfile, static_cast<std::size_t>(CompileWorkClass::Count)> profiles{};
 profiles[static_cast<std::size_t>(CompileWorkClass::Extraction)].normal_bytes = largest_input;
 profiles[static_cast<std::size_t>(CompileWorkClass::Pixels)].normal_bytes = largest_workspace;
 const auto annotation_workspace = std::max<std::uint64_t>(label_workspace, native ? native->workspace_bytes() : 0);
 const auto source_workspace = std::max<std::uint64_t>(largest_input, native ? kParquetPoolBytes : 0);
 profiles[static_cast<std::size_t>(CompileWorkClass::Annotations)] = {annotation_workspace, annotation_workspace};
 const auto protected_bytes = math::checked_add<std::uint64_t>(source_workspace, annotation_workspace + COMPILED_COMPACTION_SCRATCH_BYTES, "pipeline protected bytes overflow");
 const auto useful = math::checked_add<std::uint64_t>(protected_bytes, math::checked_multiply<std::uint64_t>(workers, largest_workspace, "pipeline arena overflow"), "pipeline arena overflow");
 auto workspace = std::make_shared<CompileWorkspacePlan>(
  CompileWorkspacePlan::prepare(workers, profiles, source_workspace, COMPILED_COMPACTION_SCRATCH_BYTES, std::min<std::uint64_t>(512ULL << 20, std::max<std::uint64_t>(64ULL << 20, useful))));
 const auto retained_output = fs::path(opts.test_dir) / "pipeline-last.bin";
 // This explicit profile owns just this last-result name, never a cache tree.
 fs::remove(retained_output);
 const auto images = fixture.path() / "images";
 fs::create_directories(images);
 std::vector<BenchmarkEncodedImage::Ptr> mapped;
 mapped.reserve(inputs.size());
 for (std::size_t i = 0; i < inputs.size(); ++i) mapped.push_back(BenchmarkEncodedImage::publish(fixture.path() / ("input-" + std::to_string(i)), inputs[i].encoded, {}));
 auto annotations = pipeline_annotation_fixture(inputs, order);
 PreparedBenchmarkSplit split{.name = "train", .class_names = {"object"}, .sources = {{images}}, .images = {}};
 split.images.reserve(order.size());
 for (std::size_t i = 0; i < order.size(); ++i) {
  const auto& input = inputs[order[i]];
  const auto path = cached_image_path(images, i + 1);
  fs::create_directories(path.parent_path());
  fs::create_hard_link(fixture.path() / ("input-" + std::to_string(order[i])), path);
  split.images.push_back({i + 1, input.width, input.height, static_cast<std::uint16_t>(annotations.image(i).box_count), 0});
 }
 split.labels = annotations.box_count();
 CompletionGaps gaps(opts.completion_gaps && trace.accepts("profile.pipeline.completed"), order.size());
 std::optional<std::uint64_t> sync_ns;
 const bool observe_annotations = trace.accepts("profile.pipeline.completed");
 AnnotationEvidence evidence(observe_annotations);
 auto prefixes = opts.trace_prefix;
 if (observe_annotations && !prefixes.empty()) {
  for (const auto event : {"benchmark.coconut.native_finalized", "benchmark.coconut.native_staged", "benchmark.annotations.gathered"}) prefixes.emplace_back(event);
 }
 BenchmarkTraceSink measured;
 if (trace)
  measured = {CompileTraceSelection(std::move(prefixes)), [&](std::string_view event, const nlohmann::json& fields) {
   if (event == "benchmark.split_sync.completed" && fields.is_object()) sync_ns = fields.at("duration_ns").get<std::uint64_t>();
   evidence.receive(event, fields);
   trace(event, fields);
  }};
 std::uint64_t output_bytes = 0, produced_images = 0, selected_images = 0;
 std::string output_sha256;
 Clock::time_point production_start, production_end, final_end;
 {
  BenchmarkCompilePipeline pipeline(workers, {}, {.task_records = static_cast<std::size_t>(opts.task_records), .workspace_plan = workspace}, {}, measured);
  BenchmarkWriteRequest request{
   .split = split,
   .output_path = fixture.path() / "train.bin",
   .resolution = static_cast<std::uint32_t>(opts.width),
   .num_workers = static_cast<int>(workers),
   .perceptual_downscale = opts.perceptual,
   .execution = &pipeline,
   .trace = measured
  };
  BenchmarkSplitWriter writer(request);
  BenchmarkCompilePipeline::Attempt attempt(pipeline);
  // Accepted pooled input calls can precede image admission. Join their actual
  // callbacks before Attempt withdraws sources or the borrowed writer retires.
  struct JoinInputs {
   FileWorkQueue& files;
   ~JoinInputs() { files.join(); }
  } join_inputs{pipeline.files()};
  bind_benchmark_slots(pipeline, writer, split);
  pipeline.bind_source(images, writer, AnnotationSource::Generic);
  std::function<void(const fs::path&, std::uint64_t, const BenchmarkLabelWorkspace&)> projected;
  std::function<void(const fs::path&, std::uint64_t)> written;
  if (observe_annotations) {
   projected = [&](const auto&, auto, const auto&) { evidence.projected(); };
   written = [&](const auto&, auto) { evidence.written(); };
  }
  pipeline.label_configuration(static_cast<std::uint32_t>(opts.width), ImageResizeMode::Stretch, std::move(projected), std::move(written));
  pipeline.membership_ready();
  const auto publication = pipeline.source_publication(images, {}, {}, true);
  BenchmarkCompletionWindow pending(std::min<std::size_t>(8, std::max<std::size_t>(2, workers * 2)), &pipeline.execution());
  production_start = Clock::now();
  if (native) annotations = native->run(pipeline, measured);
  if (opts.annotations == PipelineAnnotations::Gather) {
   std::vector<NormalizedAnnotationAssembler::Slice> slices;
   slices.reserve(annotations.image_count());
   for (std::size_t i = 0; i < annotations.image_count(); ++i) slices.push_back(NormalizedAnnotationAssembler::Slice::admitted(annotations.storage(), i));
   NormalizedAnnotationAssembler assembled(annotations, {}, fixture.path() / "gather", &pipeline.storage());
   assembled.gather(slices, &pipeline.execution(), &pipeline.files(), {}, measured);
   annotations = NormalizedAnnotationReadView(assembled.finish());
  }
  if (annotations.image_count() != order.size() || annotations.box_count() != split.labels) throw std::runtime_error("pipeline annotation fixture changed expected membership");
  for (std::size_t i = 0; i < order.size(); ++i) {
   pending.make_room();
   CompileExecution::CompletionSource completed(&pipeline.execution());
   const auto publish = [&, i, completed, publication, annotations](BenchmarkEncodedImage::Ptr payload) {
    publication({i + 1, {}, true, std::move(payload)});
    if (!pipeline.labels_ready(publication, i + 1, BenchmarkLabelInput(annotations, i), "profile-labels")) throw std::runtime_error("pipeline profile label publication withdrew");
    if (!publication.subscribe(i + 1, [&, completed](std::exception_ptr error) {
     if (!error) gaps.completed();
     completed.settle(error);
    }))
     throw std::runtime_error("pipeline profile image publication withdrew");
   };
   if (opts.input_storage == ProfileInputStorage::Pooled) {
    auto allowance = pipeline.execution().reserve({.bytes = inputs[order[i]].encoded.size(), .supplied_workspace = true, .workspace_class = CompileWorkClass::Extraction});
    auto payload = std::make_shared<BenchmarkEncodedImage::Ptr>();
    (void)pipeline.files().submit({.completed = [publish, payload, completed](std::exception_ptr error) {
     if (!error) try {
       publish(std::move(*payload));
      } catch (...) { error = std::current_exception(); }
     if (error) completed.settle(error);
    }, .work = [source = mapped[order[i]], payload, allowance] {
     *payload = source->preload(allowance, {});
    }, .service_class = CompileWorkClass::Extraction, .bytes = inputs[order[i]].encoded.size()});
   } else {
    publish(mapped[order[i]]);
   }
   pending.push(completed.completion(), [](std::exception_ptr error) {
    if (error) std::rethrow_exception(error);
   });
  }
  pending.drain();
  pipeline.drain();
  produced_images = writer.completed();
  if (produced_images != order.size()) throw std::runtime_error("pipeline profile lost produced images");
  production_end = Clock::now();
  if (opts.annotations != PipelineAnnotations::Ready) {
   std::array<BenchmarkSplitWriter::LabelSettlement, 64> settlements;
   for (std::size_t first = 0; first < order.size();) {
    const auto count = std::min(settlements.size(), order.size() - first);
    for (std::size_t i = 0; i < count; ++i) settlements[i] = {.slot = static_cast<std::uint32_t>(first + i), .ordinal_base = 1000};
    writer.settle_labels(std::span(settlements).first(count));
    evidence.settled(count);
    first += count;
   }
  }
  writer.finish(request);
  pipeline.files().join();
  final_end = Clock::now();
  output_bytes = fs::file_size(request.output_path);
  if (output_bytes > cap) throw std::length_error("pipeline final output exceeds configured cap");
  const auto output = CompiledDataset::open(request.output_path);
  selected_images = output.header().num_images;
  if (selected_images != order.size() || output.label_count() != split.labels) throw std::runtime_error("pipeline profile output counts differ");
  output_sha256 = io::sha256_hex(io::sha256_file(request.output_path));
  if (opts.keep_artifacts) fs::rename(request.output_path, retained_output);
 }
 const auto production_seconds = seconds_since(production_start, production_end);
 nlohmann::json result{
  {"images", order.size()}, {"produced_images", produced_images}, {"selected_images", selected_images}, {"labels", split.labels}, {"workers", workers}, {"input_bytes", input_bytes},
  {"logical_input_bytes", logical_input_bytes}, {"output_bytes", output_bytes}, {"output_cap_bytes", cap}, {"normal_arena_bytes", workspace->budget},
  {"input_storage", std::string(mmltk::frameworks::reflection::enum_name(opts.input_storage))}, {"setup_sec", seconds_since(setup_start, production_start)}, {"production_sec", production_seconds},
  {"finalization_including_sync_sec", seconds_since(production_end, final_end)}, {"images_per_sec", production_seconds > 0 ? static_cast<double>(order.size()) / production_seconds : 0},
  {"sync_ns", sync_ns ? nlohmann::json(*sync_ns) : nlohmann::json{}}, {"input_order", order}, {"annotations", std::string(mmltk::frameworks::reflection::enum_name(opts.annotations))},
  {"annotation_sha256", annotation_sha256}, {"output_sha256", output_sha256}
 };
 if (sync_ns) result["finalization_excluding_sync_sec"] = seconds_since(production_end, final_end) - static_cast<double>(*sync_ns) / 1e9;
 std::printf("pipeline_profile %s\n", result.dump().c_str());
 trace_benchmark_event(trace, "profile.pipeline.completed", [&] {
  result["completion_gaps_ns"] = gaps.report();
  result["annotation_work"] = evidence.report();
  return result;
 });
 record_duration_metric("pipeline", "setup", setup_start, production_start);
 record_duration_metric("pipeline", "production", production_start, production_end);
 record_duration_metric("pipeline", "finalization", production_end, final_end);
}
}  // namespace mmltk::backend::data::testsupport::profile
