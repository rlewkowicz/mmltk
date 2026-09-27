#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/common/math/checked_arithmetic.h"
#include <unistd.h>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
namespace common_io = mmltk::common::io;
BenchmarkStagedArtifact BenchmarkStagedArtifact::create(StorageReservationPool& destination, const std::filesystem::path& path,
 std::uint64_t promised, std::string_view description, std::string_view suffix, mode_t mode) {
 BenchmarkStagedArtifact result;
 result.temporary_ = path.string() + std::string(suffix);
 result.allocation_ = destination.reserve(promised, description);
 result.file_ = common_io::FileHandle::create_unique_output(result.temporary_, 0, mode);
 // No allocating operation may intervene before cleanup owns the native name.
 result.cleanup_ = true;
 result.path_ = result.temporary_;
 return result;
}
BenchmarkStagedArtifact::BenchmarkStagedArtifact(BenchmarkStagedArtifact&& other) noexcept
 : allocation_(std::move(other.allocation_)), path_(std::move(other.path_)), temporary_(std::move(other.temporary_)),
   cleanup_(std::exchange(other.cleanup_, false)), file_(std::move(other.file_)) {}
BenchmarkStagedArtifact& BenchmarkStagedArtifact::operator=(BenchmarkStagedArtifact&& other) noexcept {
 if (this != &other) {
  discard();
  allocation_ = std::move(other.allocation_);
  path_ = std::move(other.path_);
  temporary_ = std::move(other.temporary_);
  cleanup_ = std::exchange(other.cleanup_, false);
  file_ = std::move(other.file_);
 }
 return *this;
}
BenchmarkStagedArtifact::~BenchmarkStagedArtifact() { discard(); }
void BenchmarkStagedArtifact::discard() noexcept {
 file_ = {};
 if (cleanup_) { (void)::unlink(temporary_.c_str()); cleanup_ = false; }
 allocation_.release();
}
void BenchmarkStagedArtifact::preallocate(std::size_t bytes) {
 allocation_.grow(bytes, "benchmark staged allocation");
 // FileHandle's fallback may truncate, so restore the promise before either path.
 allocation_.withdraw_allocation();
 file_.preallocate(bytes);
 reconcile();
}
void BenchmarkStagedArtifact::resize(std::size_t bytes, std::string_view description) {
 reconcile();
 allocation_.resize(bytes, description);
 allocation_.withdraw_allocation();
 if (::ftruncate(file_.get(), mmltk::common::math::checked_cast<off_t>(bytes, "benchmark staged size overflow")) != 0)
  throw common_io::errno_error("cannot size benchmark staged artifact");
 file_.preallocate(bytes);
 reconcile();
}
void BenchmarkStagedArtifact::reconcile() { allocation_.reconcile(file_.get()); }
void BenchmarkStagedArtifact::close() {
 if (file_.get() >= 0) { reconcile(); file_ = {}; }
}
void BenchmarkStagedArtifact::publish(const std::filesystem::path& destination, mmltk::common::concurrency::CancellationObservation cancellation, Publication publication, bool overwrite) {
 // Publication retires this backing immediately. Keep its remaining promise
 // until rename succeeds instead of inspecting an allocation about to retire.
 file_ = {};
 throw_if_benchmark_cancelled(cancellation);
 if (publication == Publication::DurableReplace) common_io::publish_staged_path_atomically(path_, destination, overwrite);
 else {
  std::filesystem::rename(path_, destination);
  if (publication == Publication::RenameAndSync) common_io::sync_parent_directory(destination);
 }
 cleanup_ = false;
 allocation_.release();
}
}  // namespace mmltk::backend::data::benchmark_internal
