#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace mmltk::common::io {

class UniqueFd {
   public:
    explicit UniqueFd(int fd = -1) noexcept;
    ~UniqueFd();
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;
    [[nodiscard]] int get() const noexcept;

   private:
    int fd_ = -1;
};

[[nodiscard]] std::runtime_error errno_error(const char* action, const std::string& path = {});
void sync_parent_directory(const std::filesystem::path& path);
[[nodiscard]] bool remove_tree_no_follow(const std::filesystem::path& path, std::error_code& error) noexcept;
void publish_staged_path_atomically(const std::filesystem::path& staging, const std::filesystem::path& destination, bool overwrite = true);

class FileHandle {
   public:
    FileHandle() = default;
    explicit FileHandle(int fd) noexcept;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&&) noexcept = default;
    FileHandle& operator=(FileHandle&&) noexcept = default;
    [[nodiscard]] static FileHandle open_readonly(const std::string& path);
    [[nodiscard]] static FileHandle create_output(const std::string& path, std::size_t bytes);
    [[nodiscard]] static FileHandle create_unique_output(std::string& path_template, std::size_t bytes);
    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] std::size_t size() const;
    void preallocate(std::size_t bytes) const;
    void pwrite_all(const void* source, std::size_t bytes, std::size_t offset) const;
    void pread_all(void* destination, std::size_t bytes, std::size_t offset) const;
    void advise(std::size_t offset, std::size_t bytes, int advice) const;
    void sync_data() const;

   private:
    UniqueFd fd_;
};

class MappedByteRegion {
   public:
    MappedByteRegion() = default;
    ~MappedByteRegion();
    MappedByteRegion(const MappedByteRegion&) = delete;
    MappedByteRegion& operator=(const MappedByteRegion&) = delete;
    MappedByteRegion(MappedByteRegion&& other) noexcept;
    MappedByteRegion& operator=(MappedByteRegion&& other) noexcept;
    [[nodiscard]] static MappedByteRegion allocate_anonymous(std::size_t bytes);
    void adopt(void* address, std::size_t bytes) noexcept;
    void unmap() noexcept;
    void advise(int advice) const;
    void advise_hugepage() const;
    [[nodiscard]] void* address() const noexcept;
    [[nodiscard]] std::size_t mapped_bytes() const noexcept;

   private:
    void* address_ = nullptr;
    std::size_t bytes_ = 0;
};

class MappedFile {
   public:
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&&) noexcept = default;
    MappedFile& operator=(MappedFile&&) noexcept = default;
    [[nodiscard]] static std::expected<MappedFile, std::error_code> try_open_readonly(const std::string& path);
    [[nodiscard]] static MappedFile open_readonly(const std::string& path);
    [[nodiscard]] static MappedFile open_readonly_range(const std::string& path, std::size_t offset, std::size_t bytes);
    [[nodiscard]] const std::uint8_t* data() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    void advise(int advice) const;
    void advise_range(std::size_t offset, std::size_t bytes, int advice) const;
    void advise_aligned_range(std::size_t offset, std::size_t bytes, int advice) const;

   private:
    FileHandle file_;
    MappedByteRegion region_;
};

template <typename T>
class OwnedBuffer {
   public:
    OwnedBuffer() = default;
    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(const OwnedBuffer&) = delete;
    OwnedBuffer(OwnedBuffer&&) noexcept = default;
    OwnedBuffer& operator=(OwnedBuffer&&) noexcept = default;
    [[nodiscard]] static OwnedBuffer allocate(const std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) { throw std::overflow_error("owned buffer allocation overflow"); }
        OwnedBuffer buffer;
        buffer.region_ = MappedByteRegion::allocate_anonymous(count * sizeof(T));
        return buffer;
    }
    [[nodiscard]] T* data() noexcept { return static_cast<T*>(region_.address()); }
    [[nodiscard]] const T* data() const noexcept { return static_cast<const T*>(region_.address()); }
    [[nodiscard]] std::size_t size() const noexcept { return region_.mapped_bytes() / sizeof(T); }
    [[nodiscard]] std::size_t bytes() const noexcept { return region_.mapped_bytes(); }
    [[nodiscard]] bool empty() const noexcept { return region_.mapped_bytes() == 0; }
    void advise(const int advice) const { region_.advise(advice); }
    void advise_hugepage() const { region_.advise_hugepage(); }

   private:
    MappedByteRegion region_;
};

template <typename T>
[[nodiscard]] OwnedBuffer<T> allocate_hugepage_buffer(const std::size_t count) {
    OwnedBuffer<T> buffer = OwnedBuffer<T>::allocate(count);
    buffer.advise_hugepage();
    return buffer;
}

template <typename T>
[[nodiscard]] OwnedBuffer<T> load_hugepage_block(const FileHandle& file, const std::size_t count, const std::size_t offset) {
    OwnedBuffer<T> block = allocate_hugepage_buffer<T>(count);
    if (!block.empty()) { file.pread_all(block.data(), block.bytes(), offset); }
    return block;
}

}  // namespace mmltk::common::io
