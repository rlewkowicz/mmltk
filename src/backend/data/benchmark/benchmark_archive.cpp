#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include <archive.h>
#include <archive_entry.h>
#include <filereader/Standard.hpp>
#include <rapidgzip/ParallelGzipReader.hpp>
#include <rapidgzip/gzip/isal.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>
#include <utility>
#include <sys/stat.h>
namespace mmltk::backend::data::benchmark_internal {
namespace {
constexpr std::size_t kBlock = 128U << 10;
constexpr std::uint64_t kGzipIndexBytes = 64ULL << 20;
// The library's soft chunk limit is checked at deflate block boundaries. A
// valid single block can expand past it. Stop before retaining that backing;
// this internal control exception must bypass speculative decoder catches for
// malformed candidates (which catch std::exception).
struct ArchiveChunkCapacity final {};
struct ArchiveChunk final : rapidgzip::ChunkData {
 using rapidgzip::ChunkData::ChunkData;
 static constexpr std::size_t decoded_limit = 32U << 20, control_limit = 4096;
 std::size_t appended = 0;
 void check(std::size_t count) const {
  if (count > decoded_limit - appended) throw ArchiveChunkCapacity{};
 }
 void append(rapidgzip::deflate::DecodedVector&& decoded) {
  const auto count = decoded.size();
  check(count);
  rapidgzip::ChunkData::append(std::move(decoded));
  appended += count;
 }
 void append(const rapidgzip::deflate::DecodedDataView& decoded) {
  const auto count = decoded.size();
  check(count);
  rapidgzip::ChunkData::append(decoded);
  appended += count;
 }
 bool appendDeflateBlockBoundary(std::size_t encoded, std::size_t decoded) {
  if (blockBoundaries.size() >= control_limit) throw ArchiveChunkCapacity{};
  return rapidgzip::ChunkData::appendDeflateBlockBoundary(encoded, decoded);
 }
 void appendFooter(const rapidgzip::Footer& footer) {
  if (footers.size() >= control_limit) throw ArchiveChunkCapacity{};
  rapidgzip::ChunkData::appendFooter(footer);
 }
};
using Cancellation = mmltk::common::concurrency::CancellationObservation;
using mmltk::common::math::checked_add;
using mmltk::common::math::checked_cast;
std::uint64_t archive_workspace(bool compressed, std::size_t workers, std::uint64_t workspace) {
 // Each parallel chunk retains at most 32 MiB of symbols (64 MiB while
 // markers are present), bounded control vectors, compressed input and decode
 // scratch. At most 1024 old 32-KiB windows (under 40 MiB with compression
 // and node backing) plus bounded control tables fit the 64-MiB index grant.
 // Each <=32-MiB chunk has <=64 subchunks at the reader's minimum 512-KiB
 // spacing; new/in-flight windows and container slack fit its 84-MiB slot.
 // C=max(16,w) cache slots, 2w prefetch slots and <=w decoder futures are live.
 // Opt-in bounded fetchers keep no detached speculative marker futures.
 // Control-table exhaustion retires all slots before serial replay.
 const auto backing = compressed ? checked_add((std::max<std::size_t>(16, workers) + 3 * workers) * (84ULL << 20), kGzipIndexBytes, "archive seek workspace overflow") : (4ULL << 20);
 return checked_add(backing, workspace, "archive workspace overflow");
}
bool gzip_magic(const mmltk::common::io::FileHandle& file, std::uint64_t bytes) {
 std::array<unsigned char, 2> magic{};
 if (bytes >= magic.size()) file.pread_all(magic.data(), magic.size(), 0);
 return magic[0] == 0x1f && magic[1] == 0x8b;
}
[[noreturn]] void failure(archive* reader, std::string_view operation) {
 const int error = archive_errno(reader);
 if (error == ENOMEM) throw std::bad_alloc{};
 const char* detail = archive_error_string(reader);
 const auto message = std::string(operation) + ": " + (detail ? detail : "archive failure") + " (errno=" + std::to_string(error) + ")";
 if (error > 0 && error != EINVAL && error != EILSEQ) throw std::runtime_error(message);
 throw BenchmarkArchiveError(message);
}
}  // namespace
std::string canonical_benchmark_archive_member(std::string_view raw) {
 while (raw.starts_with("./")) raw.remove_prefix(2);
 if (raw.empty() || raw.front() == '/' || raw.find('\\') != std::string_view::npos || raw.find('\0') != std::string_view::npos)
  throw BenchmarkArchiveError("unsafe archive member: " + std::string(raw));
 const std::filesystem::path path(raw);
 for (const auto& part : path)
  if (part == "..") throw BenchmarkArchiveError("traversing archive member: " + std::string(raw));
 return path.lexically_normal().generic_string();
}
std::optional<std::uint64_t> benchmark_archive_image_candidate(std::string_view path) {
 const std::size_t slash = path.find_last_of('/');
 std::string_view filename = path.substr(slash == std::string_view::npos ? 0U : slash + 1U);
 const std::size_t dot = filename.find_last_of('.');
 if (dot != std::string_view::npos) filename = filename.substr(0U, dot);
 const std::size_t underscore = filename.find_last_of('_');
 const std::string_view digits = filename.substr(underscore == std::string_view::npos ? 0U : underscore + 1U);
 if (digits.empty()) return std::nullopt;
 std::uint64_t image_id = 0U;
 const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), image_id);
 if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) return std::nullopt;
 return image_id;
}
struct BenchmarkArchive::Impl {
 struct Position {
  std::uint64_t header = 0, bytes = 0, ordinal = 0;
  std::optional<std::uint64_t> extent;
  bool raw = false, conflict = false;
 };
 BenchmarkCompilePipeline* execution;
 BenchmarkAllowance parent_allowance, file_allowance, decoder_allowance, credits, supplied_workspace;
 mmltk::common::io::FileHandle file;
 std::unique_ptr<rapidgzip::ParallelGzipReader<ArchiveChunk>> gzip;
 std::unique_ptr<rapidgzip::IsalInflateWrapper> streaming;
 rapidgzip::CRC32Calculator stream_crc;
 std::uint64_t stream_offset = 0;
 bool streaming_mode = false, stream_boundary = false, rolling_windows = false, control_capacity_reached = false;
 Cancellation cancellation;
 std::vector<char> block;
 std::vector<std::uint8_t> bytes;
 std::unique_ptr<archive, decltype(&archive_read_free)> reader{nullptr, archive_read_free};
 archive_entry* entry = nullptr;
 std::unordered_map<std::string, Position> positions;
 std::string name;
 std::uint64_t offset = 0, block_offset = 0, origin = 0, file_size = 0, next_ordinal = 0;
 std::array<std::uint64_t, 7> generation{};
 std::size_t block_size = 0;
 bool started = false, direct = false, is_regular = false, safe_name = true, raw_tar = false, compressed = false, verify_crc = true;
 std::size_t decoders = 1, requested_decoders = 1, retained_windows = 1024, index_entries = 32768;
 bool external_cpus = false, owns_workspace = false;
 std::size_t consumer_descriptors = 0;
 std::uint64_t workspace_bytes = 0;
 Position current;
 Impl(const std::filesystem::path& path, BenchmarkCompilePipeline* owner, std::uint64_t workspace, const BenchmarkAllowance& parent, std::size_t workers, bool verify_gzip_crc,
  BenchmarkAllowance workspace_allowance, std::size_t retained_gzip_windows, std::size_t dependent_descriptors, std::size_t gzip_index_entries)
     : execution(owner),
       parent_allowance(parent),
       file_allowance(owner ? owner->reserve(BenchmarkResources::handles(1), parent) : BenchmarkAllowance{}),
       supplied_workspace(std::move(workspace_allowance)),
       file(mmltk::common::io::FileHandle::open_readonly(path.string())) {
  struct stat status{};
  if (::fstat(file.get(), &status) != 0) throw mmltk::common::io::errno_error("cannot inspect opened archive generation", path.string());
  if (status.st_size < 0) throw BenchmarkArchiveError("negative archive size");
  file_size = static_cast<std::uint64_t>(status.st_size);
  generation = {
   static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino), file_size, static_cast<std::uint64_t>(status.st_mtim.tv_sec),
   static_cast<std::uint64_t>(status.st_mtim.tv_nsec), static_cast<std::uint64_t>(status.st_ctim.tv_sec), static_cast<std::uint64_t>(status.st_ctim.tv_nsec)
  };
  consumer_descriptors = dependent_descriptors;
  compressed = gzip_magic(file, file_size);
  verify_crc = verify_gzip_crc;
  retained_windows = std::min<std::size_t>(1024, retained_gzip_windows);
  index_entries = std::clamp<std::size_t>(gzip_index_entries, 1, 32768);
  requested_decoders = std::max<std::size_t>(1, workers);
  decoders = execution ? std::size_t{1} : requested_decoders;
  resume(workspace);
 }
 ~Impl() {
  reader.reset();
  gzip.reset();
  streaming.reset();
  decoder_allowance = {};
  file = {};
  file_allowance.retire_descriptors();
  file_allowance.retire_workspace();
  std::vector<std::uint8_t>().swap(bytes);
  std::vector<char>().swap(block);
  if (owns_workspace) credits.retire_workspace();
 }
 void resume(std::uint64_t workspace) {
  if (reader) return;
  workspace_bytes = workspace;
  external_cpus = false;
  owns_workspace = false;
  if (execution) {
   const auto workspace_for = [&](std::size_t workers) { return archive_workspace(compressed, workers, workspace); };
   if (supplied_workspace) {
    if (supplied_workspace.bytes() < workspace_for(1)) throw BenchmarkArchiveError("archive format exceeds its admitted consumer envelope");
    credits = supplied_workspace;
   }
   // The fixed grant includes the calling CPU and decoder workers, leaving a
   // CPU for consumers. Otherwise use a cooperatively admitted serial reader.
   if (!credits && compressed && requested_decoders > 1 && execution->workers() > 3) {
    for (auto wanted = std::min(execution->workers() - 1, requested_decoders + 1); wanted > 2; --wanted) {
     auto available = execution->try_reserve({workspace_for(wanted - 1), 0, consumer_descriptors == 0, wanted, false, consumer_descriptors}, parent_allowance);
     if (available) {
      credits = std::move(*available);
      decoders = wanted - 1;
      external_cpus = true;
      owns_workspace = true;
      break;
     }
    }
   }
   // Metadata sources already committed their input descriptor pair before
   // transfer. The consumer continuation is admitted against physical capacity
   // before allocating a decoder or retaining any archive input bytes.
   if (!credits) {
    decoders = 1;
    credits = execution->reserve({workspace_for(decoders), 0, consumer_descriptors == 0, 0, false, consumer_descriptors}, parent_allowance);
    owns_workspace = true;
   }
  }
  block.resize(kBlock);
  if (compressed) reset_decoder();
  cpu([&] { open(0); });
 }
 void reset_decoder() {
  // Retire worker, cache, dictionary and compressed-reader backing before
  // constructing a replacement under the same allowance.
  gzip.reset();
  streaming.reset();
  if (execution && !decoder_allowance) decoder_allowance = execution->reserve(BenchmarkResources::handles(1), parent_allowance);
  if (streaming_mode) {
   streaming = std::make_unique<rapidgzip::IsalInflateWrapper>(rapidgzip::gzip::BitReader(std::make_unique<rapidgzip::StandardFileReader>(file.get())));
   streaming->setFileType(rapidgzip::FileType::GZIP);
   streaming->setStartWithHeader(true);
   streaming->setStoppingPoints(rapidgzip::StoppingPoint::END_OF_STREAM);
   stream_offset = 0;
   stream_boundary = false;
   stream_crc.reset();
   stream_crc.setEnabled(verify_crc);
  } else {
   gzip = std::make_unique<rapidgzip::ParallelGzipReader<ArchiveChunk>>(std::make_unique<rapidgzip::StandardFileReader>(file.get()), decoders);
   gzip->setCRC32Enabled(verify_crc);
   gzip->setIndexEntryLimit(index_entries);
   rolling_windows = false;
  }
 }
 void roll_windows() {
  if (gzip && !rolling_windows && gzip->availableWindowCount() >= retained_windows) {
   gzip->setKeepIndex(false);
   rolling_windows = true;
  }
 }
 void use_streaming() {
  streaming_mode = true;
  reset_decoder();
 }
 std::size_t stream_read(char* output, std::size_t count) {
  for (;;) {
   throw_if_benchmark_cancelled(cancellation);
   const auto [read, footer] = streaming->readStream(reinterpret_cast<std::uint8_t*>(output), count);
   stream_crc.update(output, read);
   stream_offset = checked_add(stream_offset, read, "gzip stream offset overflow");
   if (footer) {
    (void)stream_crc.verify(footer->gzipFooter.crc32);
    stream_crc.reset();
    stream_boundary = true;
   } else if (read)
    stream_boundary = false;
   else if (!stream_boundary)
    throw BenchmarkArchiveError("truncated consumed gzip stream");
   if (read || !footer) return read;
  }
 }
 void position_decoder(std::uint64_t start) {
  if (gzip) {
   try {
    if (start < gzip->tell()) {
     if (rolling_windows)
      use_streaming();
     else
      gzip->seek(checked_cast<long long>(start, "archive seek overflow"));
    }
    while (gzip && gzip->tell() < start) {
     throw_if_benchmark_cancelled(cancellation);
     roll_windows();
     if (!gzip->read(-1, nullptr, std::min<std::uint64_t>(kBlock, start - gzip->tell()))) throw BenchmarkArchiveError("gzip seek exceeds consumed source");
     roll_windows();
    }
   } catch (const ArchiveChunkCapacity&) { use_streaming(); } catch (const rapidgzip::IndexCapacityExceeded&) {
    control_capacity_reached = true;
    use_streaming();
   }
  }
  if (streaming) {
   if (start < stream_offset) reset_decoder();
   while (stream_offset < start) {
    if (!stream_read(block.data(), std::min<std::uint64_t>(block.size(), start - stream_offset))) throw BenchmarkArchiveError("gzip replay exceeds consumed source");
   }
  }
 }
 std::size_t read_source() {
  if (!compressed) {
   const auto count = checked_cast<std::size_t>(std::min<std::uint64_t>(block.size(), file_size - offset), "archive read size overflow");
   if (count) file.pread_all(block.data(), count, offset);
   return count;
  }
  roll_windows();
  if (gzip) {
   try {
    const auto count = gzip->read(block.data(), block.size());
    roll_windows();
    return count;
   } catch (const ArchiveChunkCapacity&) { use_streaming(); } catch (const rapidgzip::IndexCapacityExceeded&) {
    control_capacity_reached = true;
    use_streaming();
   }
  }
  // A failed chunk has delivered none of this callback's bytes to libarchive.
  // Replay only to that callback's logical cursor, leaving the format reader
  // and all already delivered member spans intact. No tail is drained.
  position_decoder(offset);
  return stream_read(block.data(), block.size());
 }
 static la_ssize_t read_callback(archive* reader, void* context, const void** out) noexcept {
  auto& self = *static_cast<Impl*>(context);
  try {
   self.block_offset = self.offset;
   self.block_size = self.read_source();
   self.offset += self.block_size;
   *out = self.block.data();
   return checked_cast<la_ssize_t>(self.block_size, "archive read size overflow");
  } catch (const std::bad_alloc&) { archive_set_error(reader, ENOMEM, "%s", "archive allocation failed"); } catch (const std::exception& error) {
   archive_set_error(reader, self.compressed ? EINVAL : EIO, "%s", error.what());
  }
  return ARCHIVE_FATAL;
 }
 static la_int64_t skip_callback(archive* reader, void* context, la_int64_t requested) noexcept {
  auto& self = *static_cast<Impl*>(context);
  // libarchive asks only after its format/filter has accounted for buffered
  // data and format records. Never skip decoder state or sparse logical data.
  if (requested <= 0 || self.compressed || !self.raw_tar || archive_format(reader) != ARCHIVE_FORMAT_TAR_USTAR || archive_filter_count(reader) != 1 ||
      archive_filter_code(reader, 0) != ARCHIVE_FILTER_NONE || !self.entry || archive_entry_sparse_count(self.entry) != 0)
   return 0;
  const auto count = static_cast<std::uint64_t>(requested);
  if (self.offset > self.file_size || count > self.file_size - self.offset) {
   archive_set_error(reader, EINVAL, "%s", "raw archive skip exceeds opened file extent");
   return ARCHIVE_FATAL;
  }
  self.offset += count;
  self.block_size = 0;
  return requested;
 }
 void open(std::uint64_t start, Cancellation requested_cancellation = {}, std::uint64_t ordinal = 0) {
  cancellation = requested_cancellation;
  reader.reset();
  entry = nullptr;
  started = false;
  raw_tar = false;
  direct = false;
  origin = offset = start;
  next_ordinal = ordinal;
  try {
   position_decoder(start);
  } catch (const std::bad_alloc&) { throw; } catch (const std::exception& error) {
   throw_if_benchmark_cancelled(cancellation);
   throw BenchmarkArchiveError(error.what());
  }
  reader.reset(archive_read_new());
  if (!reader) throw std::bad_alloc{};
  if (archive_read_support_filter_all(reader.get()) < ARCHIVE_WARN || archive_read_support_format_tar(reader.get()) < ARCHIVE_WARN || archive_read_support_format_zip(reader.get()) < ARCHIVE_WARN)
   failure(reader.get(), "cannot configure archive");
  const auto status = archive_read_open2(reader.get(), this, nullptr, read_callback, skip_callback, nullptr);
  throw_if_benchmark_cancelled(cancellation);
  if (status < ARCHIVE_WARN) failure(reader.get(), "cannot open archive");
 }
 void cpu(const std::function<void()>& work) const {
  if (execution && !external_cpus)
   execution->run(BenchmarkStage::Archive, {}, [&](std::size_t) { work(); }, credits);
  else
   work();
 }
};
BenchmarkArchive::InputRequirement BenchmarkArchive::input_requirement(const std::filesystem::path& path, std::uint64_t workspace, std::size_t workers) {
 const auto file = mmltk::common::io::FileHandle::open_readonly(path.string());
 const bool compressed = gzip_magic(file, file.size());
 return {archive_workspace(compressed, std::max<std::size_t>(1, workers), workspace), compressed ? 2U : 1U};
}
BenchmarkArchive::BenchmarkArchive(const std::filesystem::path& path, BenchmarkCompilePipeline* execution, std::uint64_t workspace, const BenchmarkAllowance& parent, std::size_t workers,
 bool verify_gzip_crc, BenchmarkAllowance workspace_allowance, std::size_t retained_gzip_windows, std::size_t consumer_descriptors, std::size_t gzip_index_entries)
    : impl_(std::make_unique<Impl>(path, execution, workspace, parent, workers, verify_gzip_crc, std::move(workspace_allowance), retained_gzip_windows, consumer_descriptors, gzip_index_entries)) {}
BenchmarkArchive::~BenchmarkArchive() = default;
bool BenchmarkArchive::next(Cancellation cancellation) {
 auto& s = *impl_;
 s.cancellation = cancellation;
 throw_if_benchmark_cancelled(cancellation);
 if (!s.reader) s.resume(s.workspace_bytes);
 if (s.direct) {
  s.cpu([&] { s.open(s.current.header, cancellation, s.current.ordinal); });
  s.direct = false;
  if (!next(cancellation)) return false;
 }
 int status = ARCHIVE_RETRY;
 s.cpu([&] {
  for (int retry = 0; retry < 8 && status == ARCHIVE_RETRY; ++retry) status = archive_read_next_header(s.reader.get(), &s.entry);
 });
 throw_if_benchmark_cancelled(cancellation);
 s.started = true;
 if (status == ARCHIVE_EOF) return false;
 if (status != ARCHIVE_OK && status != ARCHIVE_WARN) failure(s.reader.get(), "cannot read archive header");
 const char* path = archive_entry_pathname(s.entry);
 if (!path) throw BenchmarkArchiveError("archive member has no name");
 s.safe_name = true;
 try {
  s.name = (std::string_view(path) == "." || std::string_view(path) == "./") ? "." : canonical_benchmark_archive_member(path);
 } catch (const BenchmarkArchiveError&) {
  s.name = path;
  s.safe_name = false;
 }
 s.is_regular = archive_entry_filetype(s.entry) == AE_IFREG && !archive_entry_symlink(s.entry) && !archive_entry_hardlink(s.entry);
 const auto size = archive_entry_size(s.entry);
 if (size < 0) throw BenchmarkArchiveError("negative archive member size");
 s.current = {
  checked_add(s.origin, checked_cast<std::uint64_t>(archive_read_header_position(s.reader.get()), "archive header position overflow"), "archive position overflow"), static_cast<std::uint64_t>(size),
  s.next_ordinal++, {}
 };
 s.raw_tar = !s.compressed && archive_filter_count(s.reader.get()) == 1 && archive_filter_code(s.reader.get(), 0) == ARCHIVE_FILTER_NONE &&
             archive_format(s.reader.get()) == ARCHIVE_FORMAT_TAR_USTAR && archive_entry_sparse_count(s.entry) == 0;
 // Member locations are canonical metadata; conflicting required identities are
 // checked by consumers when encountered, never by draining an unused tail.
 s.current.raw = archive_format(s.reader.get()) == ARCHIVE_FORMAT_TAR_USTAR && archive_entry_sparse_count(s.entry) == 0 && archive_filter_count(s.reader.get()) == 1 &&
                 archive_filter_code(s.reader.get(), 0) == ARCHIVE_FILTER_NONE;
 const auto [stored, inserted] = s.positions.try_emplace(s.name, s.current);
 if (!inserted && stored->second.ordinal != s.current.ordinal) stored->second.conflict = true;
 return true;
}
const std::string& BenchmarkArchive::member() const { return impl_->name; }
bool BenchmarkArchive::regular() const { return impl_->is_regular; }
std::uint64_t BenchmarkArchive::size() const { return impl_->current.bytes; }
std::uint64_t BenchmarkArchive::position() const { return impl_->current.ordinal; }
BenchmarkArchive::MemberPosition BenchmarkArchive::member_position() const {
 const auto& s = *impl_;
 MemberPosition result;
 result.generation_ = s.generation;
 result.member_ = s.name;
 result.header_ = s.current.header;
 result.ordinal_ = s.current.ordinal;
 result.bytes_ = s.current.bytes;
 result.extent_ = s.current.extent;
 result.raw_ = s.current.raw;
 return result;
}
bool BenchmarkArchive::seek(const MemberPosition& position, Cancellation cancellation) {
 auto& s = *impl_;
 if (position.generation_ != s.generation) throw BenchmarkArchiveError("archive member position belongs to a replaced source generation");
 if (position.extent_ && (*position.extent_ > s.file_size || position.bytes_ > s.file_size - *position.extent_)) throw BenchmarkArchiveError("archive member position exceeds opened file bounds");
 const auto [stored, inserted] = s.positions.try_emplace(position.member_, Impl::Position{position.header_, position.bytes_, position.ordinal_, position.extent_, position.raw_});
 if (!inserted && (stored->second.ordinal != position.ordinal_ || stored->second.bytes != position.bytes_)) throw BenchmarkArchiveError("archive member position conflicts with consumed identity");
 return seek(position.member_, cancellation);
}
void BenchmarkArchive::require_regular(std::uint64_t limit) const {
 if (!impl_->safe_name) throw BenchmarkArchiveError("unsafe consumed archive member: " + member());
 if (!regular() || size() > limit || size() > std::numeric_limits<std::size_t>::max()) throw BenchmarkArchiveError("archive member type or size exceeds admission: " + member());
 if (impl_->positions.at(member()).conflict) throw BenchmarkArchiveError("conflicting consumed archive member: " + member());
}
void BenchmarkArchive::consume(std::uint64_t limit, const std::function<void(std::span<const std::uint8_t>, std::uint64_t)>& consumer, Cancellation cancellation) {
 auto& s = *impl_;
 s.cancellation = cancellation;
 require_regular(limit);
 if (s.direct) {
  const auto buffer = std::span(reinterpret_cast<std::uint8_t*>(s.block.data()), s.block.size());
  for (std::uint64_t offset = 0; offset < size();) {
   throw_if_benchmark_cancelled(cancellation);
   const auto count = std::min<std::uint64_t>(buffer.size(), size() - offset);
   s.file.pread_all(buffer.data(), count, *s.current.extent + offset);
   consumer(buffer.first(count), offset);
   offset += count;
  }
  return;
 }
 std::uint64_t written = 0;
 std::optional<std::uint64_t> extent;
 bool contiguous = s.raw_tar;
 const auto hole = [&](std::uint64_t end) {
  const std::array<std::uint8_t, 4096> zeros{};
  while (written < end) {
   const auto count = std::min<std::uint64_t>(end - written, zeros.size());
   consumer(std::span(zeros).first(count), written);
   written += count;
  }
 };
 for (;;) {
  throw_if_benchmark_cancelled(cancellation);
  const void* data = nullptr;
  std::size_t count = 0;
  la_int64_t offset = 0;
  int status = ARCHIVE_RETRY;
  s.cpu([&] {
   for (int retry = 0; retry < 8 && status == ARCHIVE_RETRY; ++retry) status = archive_read_data_block(s.reader.get(), &data, &count, &offset);
  });
  throw_if_benchmark_cancelled(cancellation);
  if (status == ARCHIVE_EOF) break;
  if (status != ARCHIVE_OK) failure(s.reader.get(), "cannot read archive member");
  if (offset < 0 || static_cast<std::uint64_t>(offset) < written || static_cast<std::uint64_t>(offset) > size() || count > size() - static_cast<std::uint64_t>(offset))
   throw BenchmarkArchiveError("archive data block exceeds member extent");
  const auto address = reinterpret_cast<std::uintptr_t>(data), base = reinterpret_cast<std::uintptr_t>(s.block.data());
  const bool borrowed = address >= base && address - base <= s.block_size && count <= s.block_size - (address - base);
  if (contiguous && borrowed && static_cast<std::uint64_t>(offset) == written) {
   const auto physical = checked_add(s.block_offset, address - base, "archive extent overflow");
   if (!extent)
    extent = physical;
   else
    contiguous = physical == *extent + written;
  } else
   contiguous = false;
  hole(static_cast<std::uint64_t>(offset));
  consumer({static_cast<const std::uint8_t*>(data), count}, written);
  written += count;
 }
 if (written != size()) {
  if (!s.entry || archive_entry_sparse_count(s.entry) == 0) throw BenchmarkArchiveError("truncated archive member: " + s.name);
  hole(size());
 }
 if (contiguous && extent && *extent <= s.file_size && size() <= s.file_size - *extent) {
  s.current.extent = extent;
  auto& position = s.positions.at(s.name);
  if (position.header == s.current.header) position.extent = extent;
 }
}
void BenchmarkArchive::read_into(std::span<std::uint8_t> output, Cancellation cancellation) {
 if (size() != output.size()) throw BenchmarkArchiveError("archive member size disagrees with destination");
 if (impl_->direct) {
  require_regular(output.size());
  throw_if_benchmark_cancelled(cancellation);
  impl_->file.pread_all(output.data(), output.size(), *impl_->current.extent);
  return;
 }
 consume(output.size(), [&](std::span<const std::uint8_t> bytes, std::uint64_t offset) { std::memcpy(output.data() + offset, bytes.data(), bytes.size()); }, cancellation);
}
std::span<const std::uint8_t> BenchmarkArchive::read(std::uint64_t limit, Cancellation cancellation) {
 require_regular(limit);
 impl_->bytes.resize(checked_cast<std::size_t>(size(), "archive member size overflow"));
 read_into(impl_->bytes, cancellation);
 return impl_->bytes;
}
bool BenchmarkArchive::seek(std::string_view member, Cancellation cancellation) {
 auto& s = *impl_;
 const auto found = s.positions.find(std::string(member));
 if (found == s.positions.end()) return false;
 const auto position = found->second;
 if (position.conflict) throw BenchmarkArchiveError("conflicting requested archive member: " + std::string(member));
 if (!s.reader) s.resume(s.workspace_bytes);
 if (position.extent) {
  s.name = found->first;
  s.current = position;
  s.is_regular = true;
  s.safe_name = true;
  s.direct = true;
  return true;
 }
 if (position.raw) {
  s.cpu([&] { s.open(position.header, cancellation, position.ordinal); });
  if (!next(cancellation) || s.name != member) throw BenchmarkArchiveError("archive seek identity mismatch");
  return true;
 }
 // ZIP and extended tar keep the library's full format context. A requested
 // reread is necessary; this is never a census or separate validation pass.
 s.cpu([&] { s.open(0, cancellation); });
 while (next(cancellation))
  if (s.name == member) return true;
 throw BenchmarkArchiveError("archive member disappeared from opened generation");
}
void BenchmarkArchive::visit_known(std::span<const std::string> members, const std::function<void(std::size_t)>& consume, Cancellation cancellation) {
 auto& s = *impl_;
 struct Required {
  std::size_t index;
  Impl::Position position;
 };
 std::vector<Required> ordered;
 ordered.reserve(members.size());
 for (std::size_t i = 0; i < members.size(); ++i) {
  throw_if_benchmark_cancelled(cancellation);
  const auto found = s.positions.find(members[i]);
  if (found == s.positions.end()) throw BenchmarkArchiveError("missing requested archive member: " + members[i]);
  ordered.push_back({i, found->second});
 }
 std::ranges::sort(ordered, {}, [](const Required& row) { return row.position.ordinal; });
 if (ordered.empty()) return;
 for (std::size_t i = 1; i < ordered.size(); ++i)
  if (ordered[i - 1].position.ordinal == ordered[i].position.ordinal) throw BenchmarkArchiveError("duplicate required archive member: " + members[ordered[i].index]);
 if (!s.reader) s.resume(s.workspace_bytes);
 const bool direct = std::ranges::all_of(ordered, [](const Required& row) { return row.position.extent.has_value(); });
 // A fresh resumed reader is already at zero. Otherwise restart exactly once
 // for the batch; all context-dependent members share this forward traversal.
 if (!direct && (s.started || s.origin != 0 || s.direct)) s.cpu([&] { s.open(0, cancellation); });
 for (const auto& required : ordered) {
  throw_if_benchmark_cancelled(cancellation);
  const auto& name = members[required.index];
  if (required.position.conflict) throw BenchmarkArchiveError("conflicting requested archive member: " + name);
  if (direct) {
   if (!seek(name, cancellation)) throw BenchmarkArchiveError("archive member disappeared from opened generation");
  } else {
   bool found = false;
   while (next(cancellation)) {
    // ZIP can consume a data descriptor during either read or skip. Its byte
    // header position therefore depends on the earlier body-consumption path.
    // Entry order is stable across both traversals of this opened generation.
    if (position() < required.position.ordinal) continue;
    if (position() != required.position.ordinal || member() != name || size() != required.position.bytes)
     throw BenchmarkArchiveError("archive visit identity mismatch: expected " + name + " at entry " + std::to_string(required.position.ordinal) + ", found " + member() + " at entry " + std::to_string(position()));
    found = true;
    break;
   }
   if (!found) throw BenchmarkArchiveError("archive member disappeared from opened generation");
  }
  consume(required.index);
 }
}
void BenchmarkArchive::pause() {
 auto& s = *impl_;
 s.reader.reset();
 s.entry = nullptr;
 s.gzip.reset();
 s.streaming.reset();
 s.decoder_allowance = {};
 std::vector<std::uint8_t>().swap(s.bytes);
 std::vector<char>().swap(s.block);
 if (s.owns_workspace) s.credits.retire_workspace();
 s.credits = {};
 s.supplied_workspace = {};
}
void BenchmarkArchive::resume(std::uint64_t workspace, std::size_t consumer_descriptors) {
 impl_->consumer_descriptors = consumer_descriptors;
 impl_->resume(workspace);
}
BenchmarkArchive::GzipSeekState BenchmarkArchive::gzip_seek_state() const {
 return {
  impl_->gzip ? impl_->gzip->availableWindowCount() : 0, static_cast<bool>(impl_->gzip), impl_->streaming_mode, impl_->rolling_windows, impl_->control_capacity_reached,
  impl_->gzip ? impl_->gzip->indexEntryCount() : 0, impl_->gzip ? impl_->gzip->indexStorageBytes() : 0
 };
}
std::uint64_t BenchmarkArchive::retained_workspace_bytes() const {
 const auto& state = *impl_;
 return checked_add(
  state.reader ? archive_workspace(state.compressed, state.decoders, 0) : std::uint64_t{0}, static_cast<std::uint64_t>(state.bytes.capacity()), "archive retained workspace overflow");
}
BenchmarkAllowance BenchmarkArchive::allowance() const { return impl_->credits; }
void BenchmarkArchive::cpu(const std::function<void()>& work) const { impl_->cpu(work); }
}  // namespace mmltk::backend::data::benchmark_internal
