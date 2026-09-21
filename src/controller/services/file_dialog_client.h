#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/controller/contracts/settings_commands.h"
#include "src/controller/contracts/workflows.h"
#include "src/controller/services/file_dialog_types.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::controller::services {
// A cancellation capability is owned by the caller of one dialog operation. Its
// eventfd lets the blocking subprocess wait remain entirely event driven.
struct FileDialogCancellationTag;
using FileDialogCancellationSignal = mmltk::common::concurrency::EventCancellationSignal<FileDialogCancellationTag>;
using FileDialogCancellationSource = mmltk::common::concurrency::EventCancellationSource<FileDialogCancellationTag, false>;
using FileDialogCancellationToken = mmltk::common::concurrency::EventCancellationToken<FileDialogCancellationTag>;
struct FileDialogRequest final {
 BoundedText<kFileDialogTextCapacity> title{};
 mmltk::controller::contracts::FileDialogMode mode = mmltk::controller::contracts::FileDialogMode::OpenFile;
 mmltk::controller::services::FileDialogFilter filter{};
 [[nodiscard]] bool valid() const noexcept { return title.valid() && filter.name.valid() && filter.pattern.valid(); }
};
enum class FileDialogDisposition : std::uint8_t {
 Selected,
 Cancelled,
 Failed,
 Reaped,
};
MMLTK_REFLECT_ENUM(FileDialogDisposition)
enum class FileDialogFailure : std::uint8_t {
 None,
 CapabilityUnavailable,
 InvalidRequest,
 PipeCreation,
 Fork,
 ProcessHandle,
 ChildSetup,
 Exec,
 ProcessExit,
 Unexpected,
};
struct FileDialogResult final {
 FileDialogDisposition disposition = FileDialogDisposition::Failed;
 FileDialogFailure failure = FileDialogFailure::Unexpected;
 BoundedText<kFileDialogPathStorageCapacity> path{};
 BoundedText<kFileDialogTextCapacity> error{};
};
class FileDialogClient final {
public:
 static constexpr std::size_t kOutputCapacity = 64U * 1024U;
 FileDialogClient() noexcept = default;
 [[nodiscard]] bool valid() const noexcept;
 [[nodiscard]] FileDialogResult run(const FileDialogRequest& request, FileDialogCancellationToken cancellation) const noexcept;

private:
 struct Configuration;
 explicit FileDialogClient(std::shared_ptr<const Configuration> configuration) noexcept : configuration_(std::move(configuration)) {}
 std::shared_ptr<const Configuration> configuration_;
 friend class FileDialogClientOwner;
};
// The shell owns one immutable helper configuration. Direct work
// receive a copyable ordinary handle that retains that configuration directly.
class FileDialogClientOwner final {
public:
 FileDialogClientOwner(std::string_view helper_program, std::string_view launch_directory);
 ~FileDialogClientOwner() noexcept = default;
 FileDialogClientOwner(const FileDialogClientOwner&) = delete;
 FileDialogClientOwner& operator=(const FileDialogClientOwner&) = delete;
 [[nodiscard]] FileDialogClient client() const noexcept;

private:
 FileDialogClient client_{};
};
}  // namespace mmltk::controller::services
