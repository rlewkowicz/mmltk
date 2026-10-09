module;
#include <openssl/evp.h>
export module mmltk.tools.cpp_digest;
import std;
import mmltk.tools.cpp_support;
export {
 namespace mmltk::tools {
 inline std::string hex_bytes(std::span<const unsigned char> bytes) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result(bytes.size() * 2, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
   result[i * 2] = digits[bytes[i] >> 4];
   result[i * 2 + 1] = digits[bytes[i] & 15];
  }
  return result;
 }
 class Sha256 {
 public:
  Sha256() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
   if (!context_) throw ToolError("cannot allocate SHA256 context");
   reset();
  }
  void reset() {
   if (EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) throw ToolError("cannot initialize SHA256");
  }
  void append(std::string_view bytes, std::string_view context = {}) {
   if (EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) != 1) throw ToolError("cannot update SHA256" + std::string(context));
  }
  void field(std::string_view bytes) {
   append(bytes);
   append(std::string_view("\0", 1));
  }
  std::string finish(std::string_view context = {}) {
   std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
   unsigned length = 0;
   if (EVP_DigestFinal_ex(context_.get(), bytes.data(), &length) != 1 || length != 32) throw ToolError("cannot finish SHA256" + std::string(context));
   return hex_bytes({bytes.data(), length});
  }

 private:
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
 };
 inline std::string sha256(std::span<const unsigned char> bytes, std::string_view failure) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned size = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 || size != 32) throw ToolError(std::string(failure));
  return hex_bytes({digest.data(), size});
 }
 // Reuse both the digest context and bounded read buffer across files. The
 // caller's checkpoint preserves signal handling in long oracle operations.
 class FileHasher {
 public:
  explicit FileHasher(std::size_t buffer_size = 65536) : buffer_(buffer_size) {}
  std::string operator()(const fs::path& path, const std::function<void()>& checkpoint = {}) {
   digest_.reset();
   std::ifstream stream(path, std::ios::binary);
   if (!stream) throw ToolError("cannot read " + path.string());
   const auto context = " for " + path.string();
   while (stream) {
    if (checkpoint) checkpoint();
    stream.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    if (stream.gcount()) digest_.append({buffer_.data(), static_cast<std::size_t>(stream.gcount())}, context);
   }
   if (stream.bad() || !stream.eof()) throw ToolError("read failed: " + path.string());
   return digest_.finish(context);
  }

 private:
  Sha256 digest_;
  std::vector<char> buffer_;
 };
 }  // namespace mmltk::tools
}
