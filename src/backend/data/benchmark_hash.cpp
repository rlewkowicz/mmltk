#include "src/backend/data/benchmark_hash.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "src/common/io/file_memory.h"
#include "src/common/types/byte_encoding.h"

#if defined(__x86_64__) && defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

#include "detail/benchmark_cache.h"

namespace mmltk::backend::data {

using mmltk::common::io::FileHandle;

namespace {

constexpr std::size_t kHashReadBytes = std::size_t{16U} * 1024U * 1024U;

struct DigestContextDestroy {
    void operator()(EVP_MD_CTX* context) const noexcept { EVP_MD_CTX_free(context); }
};

using DigestContext = std::unique_ptr<EVP_MD_CTX, DigestContextDestroy>;

[[nodiscard]] DigestContext make_sha256_context() {
    DigestContext context(EVP_MD_CTX_new());
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("cannot initialize SHA-256 digest");
    }
    return context;
}

void update_digest(EVP_MD_CTX* context, const void* data, const std::size_t size) {
    if (size != 0U && EVP_DigestUpdate(context, data, size) != 1) { throw std::runtime_error("cannot update SHA-256 digest"); }
}

[[nodiscard]] Sha256Digest finish_digest(EVP_MD_CTX* context) {
    Sha256Digest digest{};
    unsigned int digest_size = 0U;
    if (EVP_DigestFinal_ex(context, digest.data(), &digest_size) != 1 || digest_size != digest.size()) {
        throw std::runtime_error("cannot finalize SHA-256 digest");
    }
    return digest;
}

#if !defined(__x86_64__) || !defined(__SSE4_2__)
[[nodiscard]] const std::array<std::uint32_t, 256>& crc32c_table() noexcept {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0U; index < values.size(); ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value >> 1U) ^ ((value & 1U) != 0U ? 0x82F63B78U : 0U);
            }
            values[index] = value;
        }
        return values;
    }();
    return table;
}
#endif

}  // namespace

Sha256Digest sha256_bytes(const std::span<const std::uint8_t> bytes) {
    DigestContext context = make_sha256_context();
    update_digest(context.get(), bytes.data(), bytes.size());
    return finish_digest(context.get());
}

std::optional<Sha256Digest> try_sha256_file(const std::filesystem::path& path,
                                            mmltk::common::concurrency::CancellationObservation cancel_requested) {
    if (cancel_requested.requested()) return std::nullopt;
    const FileHandle file = FileHandle::open_readonly(path.string());
    const std::size_t file_size = file.size();
    DigestContext context = make_sha256_context();
    std::vector<std::uint8_t> buffer(std::min(kHashReadBytes, std::max<std::size_t>(file_size, 1U)));
    std::size_t offset = 0U;
    while (offset < file_size) {
        if (cancel_requested.requested()) return std::nullopt;
        const std::size_t count = std::min(buffer.size(), file_size - offset);
        file.pread_all(buffer.data(), count, offset);
        update_digest(context.get(), buffer.data(), count);
        offset += count;
    }
    if (cancel_requested.requested()) return std::nullopt;
    return finish_digest(context.get());
}

Sha256Digest sha256_file(const std::filesystem::path& path, mmltk::common::concurrency::CancellationObservation cancel_requested) {
    const auto digest = try_sha256_file(path, cancel_requested);
    if (!digest) throw std::runtime_error("benchmark dataset compilation cancelled");
    return *digest;
}

std::string sha256_hex(const Sha256Digest& digest) { return mmltk::common::types::hex_encode(digest); }

Sha256Digest parse_sha256_hex(const std::string& value) {
    Sha256Digest digest{};
    if (value.size() != digest.size() * 2U) { throw std::runtime_error("SHA-256 digest must contain 64 hexadecimal characters"); }
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        const char* begin = value.data() + index * 2U;
        unsigned int byte = 0U;
        const auto parsed = std::from_chars(begin, begin + 2, byte, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != begin + 2 || byte > 0xFFU) {
            throw std::runtime_error("SHA-256 digest contains invalid hexadecimal data");
        }
        digest[index] = static_cast<std::uint8_t>(byte);
    }
    return digest;
}

std::uint32_t crc32c(const std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t checksum = 0xFFFFFFFFU;
#if defined(__x86_64__) && defined(__SSE4_2__)
    const std::uint8_t* cursor = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining >= sizeof(std::uint64_t)) {
        std::uint64_t word = 0U;
        std::memcpy(&word, cursor, sizeof(word));
        checksum = static_cast<std::uint32_t>(_mm_crc32_u64(checksum, word));
        cursor += sizeof(word);
        remaining -= sizeof(word);
    }
    while (remaining != 0U) {
        checksum = _mm_crc32_u8(checksum, *cursor);
        ++cursor;
        --remaining;
    }
#else
    const auto& table = crc32c_table();
    for (const std::uint8_t byte : bytes) {
        checksum = table[(checksum ^ byte) & 0xFFU] ^ (checksum >> 8U);
    }
#endif
    return ~checksum;
}

}  // namespace mmltk::backend::data
