#include <filesystem>
#include "src/controller/subsystems/annotation/detail/annotation_document.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>
#include <charconv>
#include <system_error>
#include "src/common/io/scoped_fd.h"
#include "src/common/io/noexcept_io.h"
namespace mmltk::controller::subsystems::annotation {
namespace {
class PosixAtomicSaveBackend final {
public:
 [[nodiscard]] bool open_exclusive(const std::string_view path) noexcept {
  return with_path(path, [this](const char* value) noexcept {
   descriptor_.reset(::open(value, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR));
   return descriptor_.get() >= 0;
  });
 }
 [[nodiscard]] bool write_all(const std::span<const std::byte> bytes) noexcept {
  return mmltk::common::io::try_write_all_noexcept(descriptor_.get(), {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
 }
 [[nodiscard]] bool sync_file() noexcept { return ::fsync(descriptor_.get()) == 0; }
 [[nodiscard]] bool close_file() noexcept {
  const bool closed = ::close(descriptor_.release()) == 0;
  return closed;
 }
 [[nodiscard]] bool rename_file(const std::string_view from, const std::string_view to) noexcept {
  return with_two_paths(from, to, [](const char* source, const char* destination) noexcept { return ::rename(source, destination) == 0; });
 }
 void remove_file(const std::string_view path) noexcept {
  static_cast<void>(with_path(path, [](const char* value) noexcept { return ::unlink(value) == 0; }));
 }
 [[nodiscard]] bool sync_parent(const std::string_view path) noexcept {
  const auto separator = path.rfind('/');
  if (separator == std::string_view::npos) return false;
  const auto parent = path.substr(0U, separator == 0U ? 1U : separator);
  return with_path(parent, [](const char* value) noexcept {
   const int descriptor = ::open(value, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   if (descriptor < 0) return false;
   const bool synced = ::fsync(descriptor) == 0;
   const bool closed = ::close(descriptor) == 0;
   return synced && closed;
  });
 }

private:
 template <class Operation>
 [[nodiscard]] static bool with_path(const std::string_view path, Operation operation) noexcept {
  std::array<char, domain::kWorkspacePathCapacity + 128U> storage{};
  if (path.empty() || path.size() >= storage.size()) return false;
  std::copy(path.begin(), path.end(), storage.begin());
  return operation(storage.data());
 }
 template <class Operation>
 [[nodiscard]] static bool with_two_paths(const std::string_view first, const std::string_view second, Operation operation) noexcept {
  std::array<char, domain::kWorkspacePathCapacity + 128U> first_storage{};
  std::array<char, domain::kWorkspacePathCapacity + 128U> second_storage{};
  if (first.empty() || second.empty() || first.size() >= first_storage.size() || second.size() >= second_storage.size()) return false;
  std::copy(first.begin(), first.end(), first_storage.begin());
  std::copy(second.begin(), second.end(), second_storage.begin());
  return operation(first_storage.data(), second_storage.data());
 }
 mmltk::common::io::ScopedFd descriptor_;
};
[[nodiscard]] DocumentSaveEffect atomic_save(
 const std::span<const std::byte> bytes, const std::string_view destination, const std::uint64_t document_revision, const std::uint64_t generation) noexcept {
 PosixAtomicSaveBackend backend;
 if (bytes.empty() || destination.empty() || generation == 0U) return DocumentSaveEffect::NotApplied;
 constexpr std::string_view marker{".mmltk-annotation-"};
 constexpr std::string_view suffix{".tmp"};
 std::array<char, domain::kWorkspacePathCapacity + marker.size() + 2U * 24U + suffix.size() + 2U> temporary{};
 if (destination.size() >= temporary.size()) return DocumentSaveEffect::NotApplied;
 auto* cursor = std::copy(destination.begin(), destination.end(), temporary.begin());
 cursor = std::copy(marker.begin(), marker.end(), cursor);
 const auto [revision_end, revision_error] = std::to_chars(cursor, temporary.data() + temporary.size() - suffix.size() - 1U, document_revision);
 if (revision_error != std::errc{}) return DocumentSaveEffect::NotApplied;
 *revision_end = '-';
 const auto [generation_end, generation_error] = std::to_chars(revision_end + 1, temporary.data() + temporary.size() - suffix.size() - 1U, generation);
 if (generation_error != std::errc{}) return DocumentSaveEffect::NotApplied;
 cursor = std::copy(suffix.begin(), suffix.end(), generation_end);
 const std::string_view temporary_path{temporary.data(), static_cast<std::size_t>(cursor - temporary.data())};
 if (!backend.open_exclusive(temporary_path)) return DocumentSaveEffect::NotApplied;
 const bool written = backend.write_all(bytes);
 const bool synced = written && backend.sync_file();
 const bool closed = backend.close_file();
 const bool prepared = written && synced && closed;
 if (!prepared || !backend.rename_file(temporary_path, destination)) {
  backend.remove_file(temporary_path);
  return DocumentSaveEffect::NotApplied;
 }
 return backend.sync_parent(destination) ? DocumentSaveEffect::Committed : DocumentSaveEffect::Uncertain;
}
}  // namespace
DocumentSaveEffect save_annotation_document(const domain::AnnotationUiState& state, const std::string_view destination, const std::uint64_t generation) noexcept {
 if (!state.valid() || destination.empty() || generation == 0U) return DocumentSaveEffect::NotApplied;
 std::vector<std::byte> bytes;
 if (!domain::encode_annotation_persistence(state, bytes)) return DocumentSaveEffect::NotApplied;
 try {
  std::filesystem::path path{destination};
  std::error_code error;
  const bool directory = std::filesystem::is_directory(path, error);
  const bool missing_directory = !std::filesystem::exists(path, error) && path.extension() != ".cbor";
  if (directory || missing_directory) {
   if (missing_directory && !std::filesystem::create_directories(path, error)) return DocumentSaveEffect::NotApplied;
   // Stable FNV identity gives each imported document its own destination.
   std::uint64_t identity = 14695981039346656037ULL;
   for (auto byte : state.scene.document.view()) {
    identity ^= static_cast<unsigned char>(byte);
    identity *= 1099511628211ULL;
   }
   path /= "annotation-" + std::to_string(identity) + ".cbor";
  }
  return atomic_save(bytes, path.string(), state.document_revision, generation);
 } catch (...) { return DocumentSaveEffect::NotApplied; }
}
}  // namespace mmltk::controller::subsystems::annotation
