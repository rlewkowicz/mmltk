#pragma once
#include "src/backend/data/benchmark/coconut/detail/coconut_annotations.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_staging.h"
#include "src/backend/data/detail/compile_execution.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkArchive;
class CoconutParquetMetadata;
struct CoconutRecordGroup {
 std::uint64_t first_row = 0, rows = 0, first_segment = 0, segments = 0;
};
class CoconutAnnotationRecords final {
public:
 class Row final {
 public:
  [[nodiscard]] const CoconutRecord& record() const noexcept { return record_; }
  [[nodiscard]] bool metadata_ready() const noexcept { return state_ != State::Unparsed; }
  [[nodiscard]] std::optional<std::size_t> image_join() const noexcept { return image_join_; }
  [[nodiscard]] bool complete() const noexcept { return state_ == State::Complete; }
  [[nodiscard]] std::uint64_t segment_ordinal() const noexcept { return segment_ordinal_; }
  [[nodiscard]] bool segment_ordinal_complete() const noexcept { return segment_ordinal_complete_; }
  [[nodiscard]] const std::shared_ptr<const CoconutNativeImage>& native() const noexcept { return native_; }

 private:
  friend class CoconutAnnotationRecords;
  enum class State { Unparsed, Metadata, Complete };
  CoconutRecord record_;
  State state_ = State::Unparsed;
  std::uint64_t segment_ordinal_ = 0;
  bool segment_ordinal_complete_ = true;
  std::optional<std::size_t> image_join_;
  std::shared_ptr<const CoconutNativeImage> native_;
 };
 void discard() noexcept;
 void resize(std::size_t count) {
  const std::lock_guard lock(rows_mutex_);
  if (count < rows_.size()) throw std::logic_error("cannot shrink admitted annotation rows");
  if (count != rows_.size()) {
   rows_.resize(count);
   prefixes_sealed_ = false;
  }
 }
 [[nodiscard]] std::size_t size() const noexcept { return rows_.size(); }
 [[nodiscard]] bool metadata_complete() const noexcept { return metadata_rows_.load(std::memory_order_relaxed) == rows_.size(); }
 [[nodiscard]] const Row& row(std::size_t i) const { const std::lock_guard lock(rows_mutex_); return rows_.at(i); }
 [[nodiscard]] const CoconutRecord& record(std::size_t i) const { return row(i).record(); }
 [[nodiscard]] const std::deque<Row>& rows() const noexcept { return rows_; }
 void admit_metadata(std::size_t, CoconutRecord);
 void admit_segments(std::size_t, std::vector<CoconutSegment>, std::uint64_t local_ordinal);
 void adopt(std::vector<CoconutRecord>);
 void stage_segments(std::size_t);
 void admit_json(std::size_t, CoconutRecord, std::optional<std::size_t> = {});
 void json_prefix(std::size_t, std::uint64_t);
 [[nodiscard]] std::vector<std::size_t> seal_xlarge_order();
 void native(std::size_t, std::shared_ptr<const CoconutNativeImage>);
 void retire_native() noexcept;
 // Immutable local offsets are never incremented again on retries. This only
 // seals the separate canonical placement after all group producers settle.
 void rebase_parquet_segments();
 std::shared_ptr<BenchmarkArchive> archive;
 std::shared_ptr<CoconutParquetMetadata> parquet;
 std::vector<CoconutRecordGroup> groups;
 std::vector<CoconutComponent> inventories;
 std::string identity;
 std::filesystem::path staging_directory;
 detail::StorageReservationPool* storage = nullptr;
 std::shared_ptr<CoconutStaging> staging;
 bool document_complete = false;

private:
 mutable std::mutex rows_mutex_;
 std::deque<Row> rows_;
 std::atomic<std::size_t> metadata_rows_{0};
 bool prefixes_sealed_ = false;
};
struct CoconutAnnotationInput {
 std::span<const std::uint8_t> png;
 // Keeps the Arrow batch and its complete pool alive, including after the
 // forward reader closes. Production bounds outstanding batch borrowers.
 std::shared_ptr<const void> backing;
 std::uint64_t live_bytes = 0;
 bool membership = false;
 // Archive inputs suspend their forward decoder before exceptional semantic
 // work. Parquet transfers exceptional batches before exposing this input.
 std::function<void()> retire_projection{};
 std::function<void(CompileExecution::Completion)> admitted{};
};
// A consumer retains immutable backing through its own completion. Projection
// reacquires a complete allocation window before the next library call.
using CoconutRecordConsumer = std::function<void(std::size_t group, const CoconutRecord&, const CoconutAnnotationInput&)>;
void read_coconut_parquet(std::span<const std::filesystem::path> shards, const CoconutImportLimits& limits, mmltk::common::concurrency::CancellationObservation cancellation,
 const CoconutRecordConsumer& consumer, bool metadata_only = false, detail::CompileExecution* execution = nullptr, const std::function<void()>& settle_consumers = {},
 const CompileAllowance& parent = {}, CoconutAnnotationRecords* retained = nullptr,
 const std::function<bool(const CoconutRecord&)>& reusable_native = {},
 const std::function<bool(const CoconutRecord&,std::span<const std::uint8_t>)>& exceptional = {}, const BenchmarkTraceSink& trace = {});
// Import subscriptions extend the component's original-generation value without
// making component validation depend on the execution-side notification.
struct CoconutOriginalReadiness : CoconutOriginalInput {
 detail::CompileExecution::Completion changed;
 CoconutOriginalReadiness(std::shared_ptr<const CoconutRecoveryOriginals> value={},bool complete=false,std::uint64_t version=0,detail::CompileExecution::Completion next={})
  : CoconutOriginalInput{std::move(value),complete,version},changed(std::move(next)) {}
};
using CoconutOriginalProvider = std::function<CoconutOriginalReadiness(CoconutImageNamespace, bool wait, std::stop_token)>;
enum class CoconutPreparationStage : std::uint8_t { Archive, Images, Annotations, Membership };
struct CoconutImportRequest {
 BenchmarkCompilePipeline* execution = nullptr;
 CompileAllowance parent_allowance;
 CoconutEdition edition = CoconutEdition::Base;
 std::string input_identity;
 bool metadata_only = false;
 bool accept_partial = false;
 // Production identity omissions are local while required payloads still undergo repair.
 bool reject_invalid_identities = false;
 // Terminal artifact failure: seal already completed native rows without reopening input.
 bool finish_retained = false;
 std::shared_ptr<CoconutAnnotationRecords> records;
 std::span<const CoconutImageNamespace> retained_sources;
 std::span<const CoconutComponent> reusable_images;
 std::vector<std::filesystem::path> parquet_shards;
 std::filesystem::path annotation_json;
 std::filesystem::path mask_archive;
 std::filesystem::path inventory_directory;
 const CoconutPhysicalMembership* physical_membership = nullptr;
 // Synchronous borrow; the owner and its original indexes outlive import.
 CoconutMaskRecovery* recovery = nullptr;
 CoconutOriginalProvider originals;
 std::uint64_t expected_rows = 0;
 CoconutImportLimits limits;
 mmltk::common::concurrency::CancellationObservation cancellation;
 std::function<void(std::uint64_t)> progress;
 // Immutable native products now own the consumed input. Release source I/O
 // custody before waiting for independent pixels or original-label settlement.
 std::function<void()> input_finished;
 // Bounded preparation observations are separate from normalized row completion.
 std::function<void(CoconutPreparationStage, std::uint64_t)> preparation_progress;
 // Publish canonical image membership independently of label/recovery settlement.
 std::function<void(std::vector<CoconutComponent>)> membership_ready;
 // Settle reports for freshly normalized discarded objects at image completion;
 // metadata, reused images and recovery-policy omissions produce no immediate report.
 std::function<void()> image_terminal;
 std::function<void(std::string_view)> rejected_input;
 // Synchronous observation of discarded objects; report failures never reject an image.
 std::function<void(const CoconutPhysicalImage&, const CoconutRecord&, const CoconutSegment&, std::string_view, std::uint32_t recovery_policy)> rejected_object;
 BenchmarkTraceSink trace;
};
// Every offered record is required. Unknown expected_rows means derive, never sample.
// Discover generation-bound mask locators while its external JSON is still pending.
[[nodiscard]] std::shared_ptr<BenchmarkArchive> discover_coconut_masks(const CoconutImportRequest&, std::stop_token);
[[nodiscard]] std::vector<CoconutComponent> import_coconut_annotations(const CoconutImportRequest& request);
}
