#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/common/types/byte_encoding.h"
#include <openssl/evp.h>
#include <sys/stat.h>
#include <algorithm>
#include <charconv>
#include <memory>
#include <stdexcept>
#include <vector>
namespace mmltk::common::io {
namespace {
constexpr std::size_t kHashReadBytes = std::size_t{16U} * 1024U * 1024U;
struct DigestContextDestroy {
    void operator()(EVP_MD_CTX* context) const noexcept { EVP_MD_CTX_free(context); }
};
using DigestContext = std::unique_ptr<EVP_MD_CTX, DigestContextDestroy>;
[[nodiscard]] DigestContext make_digest_context(const EVP_MD* algorithm) {
    DigestContext context(EVP_MD_CTX_new());
    if (!context || EVP_DigestInit_ex(context.get(), algorithm, nullptr) != 1) { throw std::runtime_error("cannot initialize SHA-256 digest"); }
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
FileSnapshot snapshot(const struct stat& value) {
    return {static_cast<std::uint64_t>(value.st_dev),
            static_cast<std::uint64_t>(value.st_ino),
            static_cast<std::uint64_t>(value.st_size),
            value.st_mtim.tv_sec,
            value.st_mtim.tv_nsec,
            value.st_ctim.tv_sec,
            value.st_ctim.tv_nsec};
}
FileSnapshot snapshot(int fd) {
    struct stat value{};
    if (::fstat(fd, &value) != 0 || !S_ISREG(value.st_mode) || value.st_size < 0) throw std::runtime_error("cannot inspect digest input file");
    return snapshot(value);
}
}  // namespace
struct Sha256Hasher::Impl {
    DigestContext context = make_digest_context(EVP_sha256());
};
Sha256Hasher::Sha256Hasher() : impl_(std::make_unique<Impl>()) {}
Sha256Hasher::~Sha256Hasher() = default;
void Sha256Hasher::Update(std::span<const std::uint8_t> bytes) {
    if (!impl_->context) throw std::logic_error("SHA-256 digest already finalized");
    update_digest(impl_->context.get(), bytes.data(), bytes.size());
}
Sha256Digest Sha256Hasher::Finish() {
    if (!impl_->context) throw std::logic_error("SHA-256 digest already finalized");
    auto result = finish_digest(impl_->context.get());
    impl_->context.reset();
    return result;
}
FileSnapshot FileSnapshot::Read(int file_descriptor) { return snapshot(file_descriptor); }
FileSnapshot FileSnapshot::Read(const std::filesystem::path& path) {
    const auto file = FileHandle::open_readonly(path.string());
    return snapshot(file.get());
}
void FileSnapshot::RequireUnchanged(const std::filesystem::path& path) const {
    if (*this != Read(path)) throw std::runtime_error("artifact changed during admission: " + path.string());
}
std::optional<FileDigests> try_file_digests(const std::filesystem::path& path, bool include_md5, std::function_ref<bool()> cancelled) {
    if (cancelled()) return std::nullopt;
    const auto file = FileHandle::open_readonly(path.string());
    FileDigests result;
    result.snapshot = snapshot(file.get());
    Sha256Hasher sha;
    auto md5 = include_md5 ? make_digest_context(EVP_md5()) : DigestContext{};
    std::vector<std::uint8_t> buffer(std::min<std::uint64_t>(kHashReadBytes, std::max<std::uint64_t>(result.snapshot.bytes, 1U)));
    for (std::uint64_t offset = 0; offset < result.snapshot.bytes;) {
        if (cancelled()) return std::nullopt;
        const auto count = std::min<std::uint64_t>(buffer.size(), result.snapshot.bytes - offset);
        file.pread_all(buffer.data(), count, offset);
        sha.Update(std::span(buffer.data(), count));
        if (md5) update_digest(md5.get(), buffer.data(), count);
        offset += count;
    }
    if (cancelled()) return std::nullopt;
    if (snapshot(file.get()) != result.snapshot) throw std::runtime_error("artifact changed while computing digest");
    result.snapshot.RequireUnchanged(path);
    result.sha256 = sha.Finish();
    if (md5) {
        std::array<std::uint8_t, 16> bytes{};
        unsigned int size = 0;
        if (EVP_DigestFinal_ex(md5.get(), bytes.data(), &size) != 1 || size != bytes.size()) throw std::runtime_error("cannot finalize MD5 digest");
        result.md5 = mmltk::common::types::hex_encode(bytes);
    }
    return result;
}
std::optional<Sha256Digest> try_sha256_file(const std::filesystem::path& path, std::function_ref<bool()> cancelled) {
    const auto digest = try_file_digests(path, false, cancelled);
    if (!digest) return std::nullopt;
    return digest->sha256;
}
Sha256Digest sha256_file(const std::filesystem::path& path, std::function_ref<bool()> cancelled) {
    const auto digest = try_sha256_file(path, cancelled);
    if (!digest) throw std::runtime_error("file digest cancelled");
    return *digest;
}
Sha256Digest sha256_bytes(const std::span<const std::uint8_t> bytes) {
    Sha256Hasher hash;
    hash.Update(bytes);
    return hash.Finish();
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
}  // namespace mmltk::common::io
