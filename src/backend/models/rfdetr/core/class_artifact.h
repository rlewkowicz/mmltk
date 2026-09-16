#pragma once
#include <compare>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/common/io/file_digest.h"
namespace mmltk::backend::models::rfdetr {
// Named identities, including absent descriptors, captured under the bundle read lock.
struct ClassArtifactSnapshot final {
    std::filesystem::path artifact_path, descriptor_path;
    mmltk::common::io::FileSnapshot artifact;
    std::optional<mmltk::common::io::FileSnapshot> companion{}, descriptor{};
    auto operator<=>(const ClassArtifactSnapshot&) const = default;
    [[nodiscard]] static ClassArtifactSnapshot Read(const std::filesystem::path&, const std::filesystem::path& = {});
};
// Immutable evidence for the exact input consumed by a format-specific owner.
// This is not a source-path lifetime lock. Rebind checks do no hashing or parsing.
class ClassArtifactAdmission final {
   public:
    explicit ClassArtifactAdmission(const std::filesystem::path& artifact, const std::filesystem::path& descriptor = {},
                                    std::shared_ptr<const mmltk::common::io::FileDigests> admitted_file = {}, std::stop_token stop = {},
                                    bool include_md5 = false);
    [[nodiscard]] const std::filesystem::path& artifact_path() const noexcept { return snapshot_.artifact_path; }
    [[nodiscard]] const std::filesystem::path& descriptor_path() const noexcept { return snapshot_.descriptor_path; }
    [[nodiscard]] const std::shared_ptr<const mmltk::common::io::FileDigests>& file() const noexcept { return file_; }
    [[nodiscard]] std::vector<RfdetrNamedOutputRole> output_roles() const;
    [[nodiscard]] ModelClassLayout Resolve(std::size_t output_width, const std::optional<ModelClassLayout>& embedded, std::stop_token stop = {}) const;
    void RequireUnchanged(std::stop_token stop = {}) const;
    [[nodiscard]] bool Matches(const std::filesystem::path& artifact, const std::filesystem::path& descriptor = {}) const;

   private:
    ClassArtifactSnapshot snapshot_;
    std::shared_ptr<const mmltk::common::io::FileDigests> file_;
    std::vector<ModelClassDescriptor> descriptors_;
};
}  // namespace mmltk::backend::models::rfdetr
