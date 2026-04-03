#pragma once

#include "cpu_affinity.h"
#include "execution_policy.h"
#include "profile_utils.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace mmltk {

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) : fd_(fd) {}
    ~UniqueFd() { if (fd_ >= 0) ::close(fd_); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
        return *this;
    }
    [[nodiscard]] int get() const { return fd_; }

private:
    int fd_;
};

template <typename T, typename U>
inline T checked_cast(U value, const char* ctx) {
    static_assert(std::is_integral_v<T>, "checked_cast target must be an integer type");
    static_assert(std::is_integral_v<U>, "checked_cast source must be an integer type");

    if constexpr (std::is_signed_v<U>) {
        const auto signed_value = static_cast<std::intmax_t>(value);
        if constexpr (std::is_signed_v<T>) {
            if (signed_value < static_cast<std::intmax_t>(std::numeric_limits<T>::min()) ||
                signed_value > static_cast<std::intmax_t>(std::numeric_limits<T>::max())) {
                throw std::overflow_error(ctx);
            }
        } else if (signed_value < 0 ||
                   static_cast<std::uintmax_t>(signed_value) >
                       static_cast<std::uintmax_t>(std::numeric_limits<T>::max())) {
            throw std::overflow_error(ctx);
        }
    } else {
        const auto unsigned_value = static_cast<std::uintmax_t>(value);
        if (unsigned_value > static_cast<std::uintmax_t>(std::numeric_limits<T>::max())) {
            throw std::overflow_error(ctx);
        }
    }

    return static_cast<T>(value);
}

inline std::runtime_error errno_error(const char* action, const std::string& path = {}) {
    const int err = errno;
    std::string message(action);
    if (!path.empty()) {
        message += ": ";
        message += path;
    }
    message += ": ";
    message += std::strerror(err);
    return std::runtime_error(message);
}

class FileHandle {
public:
    FileHandle() = default;
    explicit FileHandle(int fd) : fd_(fd) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&&) noexcept = default;
    FileHandle& operator=(FileHandle&&) noexcept = default;

    static FileHandle open_readonly(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw errno_error("open failed", path);
        }
        return FileHandle(fd);
    }

    static FileHandle create_output(const std::string& path, size_t bytes) {
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            throw errno_error("open failed", path);
        }
        FileHandle file(fd);
        file.preallocate(bytes);
        return file;
    }

    [[nodiscard]] int get() const { return fd_.get(); }

    [[nodiscard]] size_t size() const {
        struct stat st;
        if (fstat(fd_.get(), &st) != 0) {
            throw errno_error("fstat failed");
        }
        return checked_cast<size_t>(st.st_size, "file size overflow");
    }

    void preallocate(size_t bytes) const {
        MMLTK_PROFILE_SCOPE("io.preallocate");
        MMLTK_PROFILE_ADD("io.preallocate.bytes", bytes);
        const auto length = checked_cast<off_t>(bytes, "file size overflow");
        if (fallocate(fd_.get(), 0, 0, length) == 0) {
            return;
        }
        if (ftruncate(fd_.get(), length) != 0) {
            throw errno_error("ftruncate failed");
        }
    }

    // POSIX read/write syscalls conventionally take byte-count plus file offset.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void pwrite_all(const void* src, size_t bytes, size_t offset) const {
        MMLTK_PROFILE_SCOPE("io.pwrite_all");
        MMLTK_PROFILE_ADD("io.pwrite_all.bytes", bytes);
        const auto* ptr = static_cast<const uint8_t*>(src);
        size_t written = 0;
        while (written < bytes) {
            const auto write_offset =
                checked_cast<off_t>(offset + written, "pwrite offset overflow");
            ssize_t ret = pwrite(fd_.get(), ptr + written, bytes - written, write_offset);
            if (ret < 0) {
                throw errno_error("pwrite failed");
            }
            written += static_cast<size_t>(ret);
        }
    }

    // POSIX read/write syscalls conventionally take byte-count plus file offset.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void pread_all(void* dst, size_t bytes, size_t offset) const {
        MMLTK_PROFILE_SCOPE("io.pread_all");
        MMLTK_PROFILE_ADD("io.pread_all.bytes", bytes);
        auto* ptr = static_cast<uint8_t*>(dst);
        size_t read_bytes = 0;
        while (read_bytes < bytes) {
            const auto read_offset =
                checked_cast<off_t>(offset + read_bytes, "pread offset overflow");
            ssize_t ret = pread(fd_.get(), ptr + read_bytes, bytes - read_bytes, read_offset);
            if (ret < 0) {
                throw errno_error("pread failed");
            }
            if (ret == 0) {
                throw std::runtime_error("unexpected EOF during pread");
            }
            read_bytes += static_cast<size_t>(ret);
        }
    }

    // posix_fadvise uses byte offset and length as its native API contract.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void advise(size_t offset, size_t bytes, int advice) const {
        const auto adv_offset = checked_cast<off_t>(offset, "posix_fadvise offset overflow");
        const auto adv_len = checked_cast<off_t>(bytes, "posix_fadvise length overflow");
        const int ret = posix_fadvise(fd_.get(), adv_offset, adv_len, advice);
        if (ret != 0) {
            errno = ret;
            throw errno_error("posix_fadvise failed");
        }
    }

    void sync_data() const {
        MMLTK_PROFILE_SCOPE("io.sync_data");
        if (fdatasync(fd_.get()) != 0) {
            throw errno_error("fdatasync failed");
        }
    }

private:
    UniqueFd fd_;
};

class MappedFile {
public:
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept
        : file_(std::move(other.file_)), data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    MappedFile& operator=(MappedFile&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        unmap();
        file_ = std::move(other.file_);
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
        return *this;
    }

    ~MappedFile() { unmap(); }

    static MappedFile open_readonly(const std::string& path) {
        MMLTK_PROFILE_SCOPE("mapped_file.open_readonly");
        MappedFile mapped;
        mapped.file_ = FileHandle::open_readonly(path);
        mapped.size_ = mapped.file_.size();
        MMLTK_PROFILE_ADD("mapped_file.bytes", mapped.size_);
        void* ptr = mmap(nullptr, mapped.size_, PROT_READ, MAP_SHARED,
                         mapped.file_.get(), 0);
        if (ptr == MAP_FAILED) {
            throw errno_error("mmap failed", path);
        }
        mapped.data_ = static_cast<const uint8_t*>(ptr);
        return mapped;
    }

    static MappedFile open_readonly_range(const std::string& path, size_t offset, size_t bytes) {
        MMLTK_PROFILE_SCOPE("mapped_file.open_readonly_range");
        MappedFile mapped;
        mapped.file_ = FileHandle::open_readonly(path);
        const size_t file_size = mapped.file_.size();
        if (offset > file_size || bytes > file_size - offset) {
            throw std::out_of_range("mmap range out of bounds");
        }
        mapped.size_ = bytes;
        MMLTK_PROFILE_ADD("mapped_file.bytes", bytes);
        void* ptr = mmap(nullptr, mapped.size_, PROT_READ, MAP_SHARED,
                         mapped.file_.get(), checked_cast<off_t>(offset, "mmap offset overflow"));
        if (ptr == MAP_FAILED) {
            throw errno_error("mmap failed", path);
        }
        mapped.data_ = static_cast<const uint8_t*>(ptr);
        return mapped;
    }

    [[nodiscard]] const uint8_t* data() const { return data_; }
    [[nodiscard]] size_t size() const { return size_; }

    void advise(int advice) const {
        advise_range(0, size_, advice);
    }

    void advise_range(size_t offset, size_t bytes, int advice) const {
        if (bytes == 0) {
            return;
        }
        if (offset > size_ || bytes > size_ - offset) {
            throw std::out_of_range("madvise range out of bounds");
        }
        if (madvise(const_cast<uint8_t*>(data_ + offset), bytes, advice) != 0) {
            throw errno_error("madvise failed");
        }
    }

private:
    void unmap() noexcept {
        if (data_) {
            munmap(const_cast<uint8_t*>(data_), size_);
            data_ = nullptr;
            size_ = 0;
        }
    }

    FileHandle file_;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

template <typename T>
class OwnedBuffer {
public:
    OwnedBuffer() = default;
    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(const OwnedBuffer&) = delete;

    OwnedBuffer(OwnedBuffer&& other) noexcept : data_(other.data_), count_(other.count_) {
        other.data_ = nullptr;
        other.count_ = 0;
    }

    OwnedBuffer& operator=(OwnedBuffer&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        data_ = other.data_;
        count_ = other.count_;
        other.data_ = nullptr;
        other.count_ = 0;
        return *this;
    }

    ~OwnedBuffer() { reset(); }

    static OwnedBuffer allocate(size_t count) {
        OwnedBuffer buffer;
        if (count == 0) {
            return buffer;
        }
        if (count > std::numeric_limits<size_t>::max() / sizeof(T)) {
            throw std::overflow_error("owned buffer allocation overflow");
        }
        const size_t bytes = count * sizeof(T);
        void* ptr = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) {
            throw errno_error("anonymous mmap failed");
        }
        buffer.data_ = static_cast<T*>(ptr);
        buffer.count_ = count;
        return buffer;
    }

    [[nodiscard]] T* data() { return data_; }
    [[nodiscard]] const T* data() const { return data_; }
    [[nodiscard]] size_t size() const { return count_; }
    [[nodiscard]] size_t bytes() const { return count_ * sizeof(T); }
    [[nodiscard]] bool empty() const { return count_ == 0; }

    void advise(int advice) const {
        if (empty()) {
            return;
        }
        if (madvise(data_, bytes(), advice) != 0) {
            throw errno_error("madvise failed");
        }
    }

private:
    void reset() noexcept {
        if (data_) {
            munmap(data_, bytes());
            data_ = nullptr;
            count_ = 0;
        }
    }

    T* data_ = nullptr;
    size_t count_ = 0;
};

template <typename T>
inline OwnedBuffer<T> allocate_hugepage_buffer(size_t count) {
    OwnedBuffer<T> buffer = OwnedBuffer<T>::allocate(count);
    if (!buffer.empty()) {
        buffer.advise(MADV_HUGEPAGE);
    }
    return buffer;
}

template <typename T>
// Count and byte offset are distinct dataset layout coordinates here.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
inline OwnedBuffer<T> load_hugepage_block(const FileHandle& file, size_t count, size_t offset) {
    OwnedBuffer<T> block = allocate_hugepage_buffer<T>(count);
    if (!block.empty()) {
        file.pread_all(block.data(), block.bytes(), offset);
    }
    return block;
}

template <typename Index, typename Func>
void parallel_for_range_indexed(Index begin, Index end, int num_workers, Func&& func) {
    static_assert(std::is_integral_v<Index>, "parallel_for_range index must be integral");
    MMLTK_PROFILE_SCOPE("parallel_for_range.total");

    if (begin >= end) {
        return;
    }

    const Index total = end - begin;
    const int requested_workers = std::max(
        1,
        std::min(num_workers, checked_cast<int>(total, "parallel range too large for worker count")));
    const std::vector<int> worker_cpus = allowed_cpu_set();
    const int worker_count =
        clamp_worker_count_to_cpus(requested_workers, worker_cpus.size(), 0, 1);
    log_worker_budget_clamp("parallel_for_range", requested_workers, worker_count, worker_cpus);
    MMLTK_PROFILE_ADD("parallel_for_range.calls", 1);
    MMLTK_PROFILE_ADD("parallel_for_range.total_items",
                           checked_cast<std::uint64_t>(total, "parallel range size overflow"));
    MMLTK_PROFILE_ADD("parallel_for_range.worker_count_sum",
                           static_cast<std::uint64_t>(worker_count));
    if (worker_count == 1) {
        func(0, begin, end);
        return;
    }

    const Index chunk = (total + static_cast<Index>(worker_count) - 1) /
                        static_cast<Index>(worker_count);

    std::exception_ptr error;
    std::mutex error_mtx;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(worker_count));
    for (int worker = 0; worker < worker_count; ++worker) {
        const Index chunk_begin = begin + static_cast<Index>(worker) * chunk;
        if (chunk_begin >= end) {
            break;
        }
        const Index chunk_end = std::min(end, chunk_begin + chunk);
        threads.emplace_back([&, worker, chunk_begin, chunk_end] {
            try {
                apply_worker_execution_policy(ExecutionPolicyRequest{
                    worker_cpus,
                    "fl_par" + std::to_string(worker),
                    static_cast<size_t>(worker),
                    true,
                    true,
                });
                func(worker, chunk_begin, chunk_end);
            } catch (...) {
                std::lock_guard<std::mutex> lock(error_mtx);
                if (!error) {
                    error = std::current_exception();
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

template <typename Index, typename Func>
void parallel_for_range(Index begin, Index end, int num_workers, Func&& func) {
    parallel_for_range_indexed<Index>(begin,
                                      end,
                                      num_workers,
                                      [&](int, Index chunk_begin, Index chunk_end) {
                                          func(chunk_begin, chunk_end);
                                      });
}

} // namespace mmltk
