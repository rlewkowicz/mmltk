#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include "src/backend/models/rfdetr/contract/class_layout.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/staging_directory.h"
#include "src/backend/models/rfdetr/core/detail/class_artifact_files.h"
namespace mmltk::backend::models::rfdetr {
// Owns the named artifact and automatic-companion replacement transaction.
// Producers serialize directly to staged_artifact(), then validate before Publish.
class ClassArtifactPublication final {
 public:
    explicit ClassArtifactPublication(const std::filesystem::path& destination, const std::filesystem::path& explicit_descriptor = {});
    [[nodiscard]] const std::filesystem::path& staged_artifact() const noexcept { return staged_; }
    [[nodiscard]] const std::optional<ModelClassDescriptor>& previous_descriptor() const noexcept { return previous_descriptor_; }
    // Stable prior source admission; release the lease before staged production.
    [[nodiscard]] mmltk::common::io::UniqueFd LockPreviousArtifact() const;
    void Publish(std::optional<ModelClassDescriptor> companion = {},
        std::function_ref<bool()> cancel_requested = [] { return false; });
 private:
    std::filesystem::path destination_, companion_, staged_;
    void RequirePreviousUnchanged() const;
    std::optional<mmltk::common::io::StagingDirectory> staging_;
    std::optional<mmltk::common::io::FileSnapshot> previous_artifact_, previous_companion_;
    std::optional<ModelClassDescriptor> previous_descriptor_;
    bool published_ = false;
};
}
