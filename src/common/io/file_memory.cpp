#include "src/common/io/file_memory.h"
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
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
void publish_staged_path_atomically(const std::filesystem::path& staging, const std::filesystem::path& destination, const bool overwrite) {
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(destination, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory) {
        throw std::filesystem::filesystem_error("failed to inspect publication destination", destination, status_error);
    }
    const bool exists = !status_error && status.type() != std::filesystem::file_type::not_found;
    if (!overwrite) {
        if (exists) throw std::runtime_error("publication destination already exists: " + destination.string());
        if (::syscall(SYS_renameat2, AT_FDCWD, staging.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) != 0) {
            throw errno_error("atomic no-replace publication failed", destination.string());
        }
        try {
            sync_parent_directory(destination);
        } catch (...) {
            std::filesystem::rename(destination, staging);
            throw;
        }
        return;
    }
    if (!exists) {
        std::filesystem::rename(staging, destination);
        try {
            sync_parent_directory(destination);
        } catch (...) {
            std::filesystem::rename(destination, staging);
            throw;
        }
        return;
    }
    if (::syscall(SYS_renameat2, AT_FDCWD, staging.c_str(), AT_FDCWD, destination.c_str(), RENAME_EXCHANGE) != 0) {
        throw errno_error("atomic path exchange failed", destination.string());
    }
    try {
        sync_parent_directory(destination);
    } catch (...) {
        if (::syscall(SYS_renameat2, AT_FDCWD, staging.c_str(), AT_FDCWD, destination.c_str(), RENAME_EXCHANGE) != 0) std::terminate();
        throw;
    }
    std::error_code ignored;
    (void)remove_tree_no_follow(staging, ignored);
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
FileHandle FileHandle::create_unique_output(std::string& path_template, const std::size_t bytes) {
    std::vector<char> writable(path_template.begin(), path_template.end());
    writable.push_back('\0');
    const int fd = ::mkostemp(writable.data(), O_CLOEXEC);
    if (fd < 0) throw errno_error("mkostemp failed", path_template);
    path_template.assign(writable.data());
    FileHandle file(fd);
    try {
        if (::fchmod(fd, 0644) != 0) throw errno_error("fchmod failed", path_template);
        file.preallocate(bytes);
    } catch (...) {
        (void)::unlink(path_template.c_str());
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
    if (::fallocate(get(), 0, 0, length) != 0 && ::ftruncate(get(), length) != 0) { throw errno_error("ftruncate failed"); }
}
void FileHandle::pwrite_all(const void* source, const std::size_t bytes, const std::size_t offset) const {
    const auto* data = static_cast<const std::uint8_t*>(source);
    for (std::size_t written = 0; written < bytes;) {
        const auto position = mmltk::common::math::checked_cast<off_t>(offset + written, "pwrite offset overflow");
        const ssize_t result = ::pwrite(get(), data + written, bytes - written, position);
        if (result < 0) throw errno_error("pwrite failed");
        written += static_cast<std::size_t>(result);
    }
}
void FileHandle::pread_all(void* destination, const std::size_t bytes, const std::size_t offset) const {
    auto* data = static_cast<std::uint8_t*>(destination);
    for (std::size_t read = 0; read < bytes;) {
        const auto position = mmltk::common::math::checked_cast<off_t>(offset + read, "pread offset overflow");
        const ssize_t result = ::pread(get(), data + read, bytes - read, position);
        if (result < 0) throw errno_error("pread failed");
        if (result == 0) throw std::runtime_error("unexpected EOF during pread");
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
MappedByteRegion::MappedByteRegion(MappedByteRegion&& other) noexcept
    : address_(std::exchange(other.address_, nullptr)), bytes_(std::exchange(other.bytes_, 0)) {}
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
