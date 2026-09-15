#pragma once
#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
namespace mmltk::common::io {
using Sha256Digest = std::array<std::uint8_t, 32>;
struct FileSnapshot final {
    std::uint64_t device{}, inode{}, bytes{};
    std::int64_t modified_seconds{}, modified_nanoseconds{}, changed_seconds{}, changed_nanoseconds{};
    auto operator<=>(const FileSnapshot&) const = default;
    [[nodiscard]] static FileSnapshot Read(const std::filesystem::path& path);
    [[nodiscard]] static FileSnapshot Read(int file_descriptor);
    void RequireUnchanged(const std::filesystem::path& path) const;
};
struct FileDigests final { FileSnapshot snapshot; Sha256Digest sha256; std::string md5; };
[[nodiscard]] std::optional<FileDigests> try_file_digests(const std::filesystem::path& path, bool include_md5,
    std::function_ref<bool()> cancelled = +[]() noexcept { return false; });
[[nodiscard]] Sha256Digest sha256_bytes(std::span<const std::uint8_t> bytes);
[[nodiscard]] Sha256Digest sha256_file(const std::filesystem::path& path,
    std::function_ref<bool()> cancelled = +[]() noexcept { return false; });
[[nodiscard]] std::optional<Sha256Digest> try_sha256_file(const std::filesystem::path& path,
    std::function_ref<bool()> cancelled = +[]() noexcept { return false; });
[[nodiscard]] std::string sha256_hex(const Sha256Digest& digest);
[[nodiscard]] Sha256Digest parse_sha256_hex(const std::string& value);
}  // namespace mmltk::common::io
