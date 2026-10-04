#include "src/common/io/file_memory.h"
#include <fcntl.h>
#include <linux/falloc.h>
#include <sys/uio.h>
#include <array>
#include <algorithm>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::common::io {
std::runtime_error errno_error(const char* action, const std::string& path) {
 const int error = errno;
 std::string message(action);
 if (!path.empty()) message += ": " + path;
 message += ": ";
 message += std::strerror(error);
 return std::runtime_error(message);
}
std::filesystem::path ensure_parent_directory(const std::filesystem::path& path) {
 const auto parent = path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path();
 std::filesystem::create_directories(parent);
 return parent;
}
void sync_parent_directory(const std::filesystem::path& path) {
 const auto parent = path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path();
 const ScopedFd directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
 if (directory.get() < 0) throw errno_error("open parent directory failed", parent.string());
 if (::fsync(directory.get()) != 0) throw errno_error("fsync parent directory failed", parent.string());
}
FileHandle::FileHandle(const int fd) noexcept : fd_(fd) {}
FileHandle FileHandle::open_readonly(const std::string& path) {
 const int fd = ::open(path.c_str(), O_RDONLY);
 if (fd < 0) throw errno_error("open failed", path);
 return FileHandle(fd);
}
FileHandle FileHandle::create_output(const std::string& path, const std::size_t bytes) {
 const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
 if (fd < 0) throw errno_error("open failed", path);
 FileHandle file(fd);
 file.preallocate(bytes);
 return file;
}
FileHandle FileHandle::create_unique_output(std::string& path_template, const std::size_t bytes, const mode_t mode) {
 std::vector<char> writable(path_template.begin(), path_template.end());
 writable.push_back('\0');
 const int fd = ::mkostemp(writable.data(), O_CLOEXEC);
 if (fd < 0) throw errno_error("mkostemp failed", path_template);
 FileHandle file(fd);
 try {
  path_template.assign(writable.data());
  if (mode != 0600 && ::fchmod(fd, mode) != 0) throw errno_error("fchmod failed", path_template);
  if (bytes != 0) file.preallocate(bytes);
 } catch (...) {
  (void)::unlink(writable.data());
  throw;
 }
 return file;
}
int FileHandle::get() const noexcept { return fd_.get(); }
std::size_t FileHandle::size() const {
 struct stat status{};
 if (::fstat(get(), &status) != 0) throw errno_error("fstat failed");
 return mmltk::common::math::checked_cast<std::size_t>(status.st_size, "file size overflow");
}
void FileHandle::preallocate(const std::size_t bytes) const {
 const auto length = mmltk::common::math::checked_cast<off_t>(bytes, "file size overflow");
 int result;
 do { result = ::fallocate(get(), 0, 0, length); } while (result < 0 && errno == EINTR);
 if (result != 0) resize_sparse(bytes);
}
std::size_t FileHandle::Range::end() const {
 using namespace mmltk::common::math;
 const auto last = checked_add(offset, bytes, "file extent overflow");
 (void)checked_cast<off_t>(last, "file extent overflow");
 return last;
}
std::vector<FileHandle::Range> FileHandle::normalize_ranges(std::span<const Range> input, RangeCoverage coverage) {
 std::vector<Range> ranges;
 ranges.reserve(input.size());
 for (auto range : input) {
  auto end = range.end();
  if (!range.bytes) continue;
  if (coverage == RangeCoverage::CoverPages) {
   constexpr std::size_t page = 4096;
   range.offset = range.offset / page * page;
   end = mmltk::common::math::checked_add(end, page - 1, "file page extent overflow") / page * page;
   range.bytes = end - range.offset;
   (void)range.end();
  }
  ranges.push_back(range);
 }
 std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.offset < b.offset; });
 std::size_t used = 0;
 for (const auto range : ranges) {
  if (used && range.offset <= ranges[used - 1].end()) {
   auto& previous = ranges[used - 1];
   previous.bytes = std::max(previous.end(), range.end()) - previous.offset;
  } else
   ranges[used++] = range;
 }
 ranges.resize(used);
 return ranges;
}
void FileHandle::resize_sparse(std::size_t bytes, std::uint64_t* syscalls, concurrency::CancellationObservation cancellation) const {
 const auto length = mmltk::common::math::checked_cast<off_t>(bytes, "sparse file size overflow");
 int result;
 do {
  if (cancellation.requested()) throw std::runtime_error("file resize cancelled");
  if (syscalls) ++*syscalls;
  result = ::ftruncate(get(), length);
 } while (result < 0 && errno == EINTR);
 if (result != 0) throw errno_error("sparse file resize failed");
}
void FileHandle::allocate_range(std::size_t offset, std::size_t bytes) const {
 const Range range{offset, bytes};
 allocate_ranges({&range, 1});
}
void FileHandle::allocate_ranges(std::span<const Range> ranges, concurrency::CancellationObservation cancellation, std::uint64_t* syscalls) const {
 for (const auto& range : ranges) (void)range.end();
 for (const auto& range : ranges) {
  if (!range.bytes) continue;
  int result;
  do {
   if (cancellation.requested()) throw std::runtime_error("file allocation cancelled");
   if (syscalls) ++*syscalls;
   result = ::fallocate(get(), 0, static_cast<off_t>(range.offset), static_cast<off_t>(range.bytes));
  } while (result < 0 && errno == EINTR);
  if (result != 0) throw errno_error("file extent allocation failed");
 }
}
bool FileHandle::punch_hole(std::size_t offset, std::size_t bytes) const {
 const Range range{offset, bytes};
 return punch_holes({&range, 1});
}
bool FileHandle::punch_holes(std::span<const Range> ranges, concurrency::CancellationObservation cancellation, std::uint64_t* syscalls, std::size_t* completed) const {
 if (completed) *completed = 0;
 for (const auto& range : ranges) (void)range.end();
 for (const auto& range : ranges) {
  if (!range.bytes) {
   if (completed) ++*completed;
   continue;
  }
  int result;
  do {
   if (cancellation.requested()) throw std::runtime_error("file reclamation cancelled");
   if (syscalls) ++*syscalls;
   result = ::fallocate(get(), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(range.offset), static_cast<off_t>(range.bytes));
  } while (result < 0 && errno == EINTR);
  if (!result) {
   if (completed) ++*completed;
   continue;
  }
  if (errno == EOPNOTSUPP || errno == ENOSYS) return false;
  throw errno_error("file extent reclaim failed");
 }
 return true;
}
void FileHandle::pwritev_all(std::span<const WriteSpan> sources, std::size_t offset, std::uint64_t* syscalls, concurrency::CancellationObservation cancellation) const {
 using namespace mmltk::common::math;
 std::size_t total = 0;
 for (const auto& source : sources) total = checked_add(total, source.bytes, "pwritev extent overflow");
 (void)checked_cast<off_t>(checked_add(offset, total, "pwritev extent overflow"), "pwritev extent overflow");
 std::size_t index = 0, consumed = 0;
 while (index < sources.size()) {
  std::array<iovec, 16> vectors{};
  std::size_t count = 0, bytes = 0;
  for (auto i = index; i < sources.size() && count < vectors.size(); ++i) {
   const auto skip = i == index ? consumed : 0;
   if (skip == sources[i].bytes) continue;
   const auto amount = std::min(sources[i].bytes - skip, std::size_t{0x7ffff000} - bytes);
   if (!amount) break;
   vectors[count++] = {const_cast<std::byte*>(static_cast<const std::byte*>(sources[i].data) + skip), amount};
   bytes += amount;
  }
  if (!count) break;
  if (cancellation.requested()) throw std::runtime_error("file write cancelled");
  if (syscalls) ++*syscalls;
  const auto result = ::pwritev(get(), vectors.data(), static_cast<int>(count), checked_cast<off_t>(offset, "pwritev offset overflow"));
  if (result < 0) {
   if (errno == EINTR) continue;
   throw errno_error("pwritev failed");
  }
  if (!result) throw std::runtime_error("zero progress during pwritev");
  auto completed = static_cast<std::size_t>(result);
  offset += completed;
  while (index < sources.size() && completed >= sources[index].bytes - consumed) {
   completed -= sources[index].bytes - consumed;
   ++index;
   consumed = 0;
  }
  consumed += completed;
 }
}
void FileHandle::pwrite_all(const void* source, const std::size_t bytes, const std::size_t offset, std::uint64_t* syscalls, concurrency::CancellationObservation cancellation) const {
 const WriteSpan span{source, bytes};
 pwritev_all({&span, 1}, offset, syscalls, cancellation);
}
void FileHandle::pread_all(void* destination, const std::size_t bytes, const std::size_t offset, std::uint64_t* syscalls, concurrency::CancellationObservation cancellation) const {
 using namespace mmltk::common::math;
 (void)checked_cast<off_t>(checked_add(offset, bytes, "pread extent overflow"), "pread extent overflow");
 auto* data = static_cast<std::uint8_t*>(destination);
 for (std::size_t read = 0; read < bytes;) {
  if (cancellation.requested()) throw std::runtime_error("file read cancelled");
  if (syscalls) ++*syscalls;
  const auto result = ::pread(get(), data + read, std::min(bytes - read, std::size_t{0x7ffff000}), checked_cast<off_t>(offset + read, "pread offset overflow"));
  if (result < 0) {
   if (errno == EINTR) continue;
   throw errno_error("pread failed");
  }
  if (!result) throw std::runtime_error("unexpected EOF during pread");
  read += static_cast<std::size_t>(result);
 }
}
void FileHandle::advise(const std::size_t offset, const std::size_t bytes, const int advice) const {
 const auto begin = mmltk::common::math::checked_cast<off_t>(offset, "posix_fadvise offset overflow");
 const auto length = mmltk::common::math::checked_cast<off_t>(bytes, "posix_fadvise length overflow");
 const int result = ::posix_fadvise(get(), begin, length, advice);
 if (result != 0) {
  errno = result;
  throw errno_error("posix_fadvise failed");
 }
}
void FileHandle::sync_data() const {
 if (::fdatasync(get()) != 0) throw errno_error("fdatasync failed");
}
MappedByteRegion::~MappedByteRegion() { unmap(); }
MappedByteRegion::MappedByteRegion(MappedByteRegion&& other) noexcept : address_(std::exchange(other.address_, nullptr)), bytes_(std::exchange(other.bytes_, 0)) {}
MappedByteRegion& MappedByteRegion::operator=(MappedByteRegion&& other) noexcept {
 if (this != &other) {
  unmap();
  address_ = std::exchange(other.address_, nullptr);
  bytes_ = std::exchange(other.bytes_, 0);
 }
 return *this;
}
MappedByteRegion MappedByteRegion::allocate_anonymous(const std::size_t bytes) {
 MappedByteRegion region;
 if (bytes == 0) return region;
 void* address = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
 if (address == MAP_FAILED) throw errno_error("anonymous mmap failed");
 region.adopt(address, bytes);
 return region;
}
void MappedByteRegion::adopt(void* address, const std::size_t bytes) noexcept {
 unmap();
 address_ = address;
 bytes_ = bytes;
}
void MappedByteRegion::unmap() noexcept {
 if (address_) {
  (void)::munmap(address_, bytes_);
  address_ = nullptr;
  bytes_ = 0;
 }
}
void MappedByteRegion::advise(const int advice) const {
 if (bytes_ != 0 && ::madvise(address_, bytes_, advice) != 0) throw errno_error("madvise failed");
}
void MappedByteRegion::advise_hugepage() const {
 if (bytes_ != 0 && ::madvise(address_, bytes_, MADV_HUGEPAGE) != 0) throw errno_error("madvise failed");
}
void* MappedByteRegion::address() const noexcept { return address_; }
std::size_t MappedByteRegion::mapped_bytes() const noexcept { return bytes_; }
std::expected<MappedFile, std::error_code> MappedFile::try_open_readonly(const std::string& path) {
 const int fd = ::open(path.c_str(), O_RDONLY);
 if (fd < 0) return std::unexpected{std::error_code{errno, std::generic_category()}};
 MappedFile mapped;
 mapped.file_ = FileHandle(fd);
 struct stat status{};
 if (::fstat(fd, &status) != 0) return std::unexpected{std::error_code{errno, std::generic_category()}};
 const auto bytes = mmltk::common::math::checked_cast<std::size_t>(status.st_size, "file size overflow");
 if (bytes == 0) return mapped;
 void* address = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
 if (address == MAP_FAILED) return std::unexpected{std::error_code{errno, std::generic_category()}};
 mapped.region_.adopt(address, bytes);
 return mapped;
}
MappedFile MappedFile::open_readonly(const std::string& path) {
 auto mapped = try_open_readonly(path);
 if (!mapped) throw std::system_error(mapped.error(), "mapped file open failed: " + path);
 return std::move(*mapped);
}
MappedFile MappedFile::open_readonly_range(const std::string& path, const std::size_t offset, const std::size_t bytes) {
 MappedFile mapped;
 mapped.file_ = FileHandle::open_readonly(path);
 const auto file_bytes = mapped.file_.size();
 if (offset > file_bytes || bytes > file_bytes - offset) throw std::out_of_range("mmap range out of bounds");
 if (bytes == 0) return mapped;
 void* address = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, mapped.file_.get(), mmltk::common::math::checked_cast<off_t>(offset, "mmap offset overflow"));
 if (address == MAP_FAILED) throw errno_error("mmap failed", path);
 mapped.region_.adopt(address, bytes);
 return mapped;
}
const std::uint8_t* MappedFile::data() const noexcept { return static_cast<const std::uint8_t*>(region_.address()); }
std::size_t MappedFile::size() const noexcept { return region_.mapped_bytes(); }
void MappedFile::advise(const int advice) const { advise_range(0, size(), advice); }
void MappedFile::advise_range(const std::size_t offset, const std::size_t bytes, const int advice) const {
 if (offset > size() || bytes > size() - offset) throw std::out_of_range("madvise range out of bounds");
 if (bytes != 0 && ::madvise(static_cast<std::uint8_t*>(region_.address()) + offset, bytes, advice) != 0) { throw errno_error("madvise failed"); }
}
void MappedFile::advise_aligned_range(const std::size_t offset, const std::size_t bytes, const int advice) const {
 if (offset > size() || bytes > size() - offset) throw std::out_of_range("aligned madvise range out of bounds");
 if (bytes == 0) return;
 static const auto page = [] {
  const auto value = ::sysconf(_SC_PAGESIZE);
  if (value <= 0) throw std::runtime_error("cannot determine mapping page size");
  return static_cast<std::size_t>(value);
 }();
 const auto begin = offset - offset % page;
 const auto end = offset + bytes;
 const auto tail = end % page;
 const auto rounded = tail == 0 || page - tail > size() - end ? end : end + page - tail;
 advise_range(begin, rounded - begin, advice);
}
}  // namespace mmltk::common::io
