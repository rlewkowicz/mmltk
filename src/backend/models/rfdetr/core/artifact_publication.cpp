#include "detail/class_artifact_files.h"
#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include <stdexcept>
#include <utility>
namespace mmltk::backend::models::rfdetr {
namespace io = mmltk::common::io;
namespace {
bool same_path(const std::filesystem::path& first, const std::filesystem::path& second) {
    if (std::filesystem::weakly_canonical(first) == std::filesystem::weakly_canonical(second)) return true;
    return std::filesystem::exists(first) && std::filesystem::exists(second) && std::filesystem::equivalent(first, second);
}
}  // namespace
ClassArtifactPublication::ClassArtifactPublication(const std::filesystem::path& destination, const std::filesystem::path& explicit_descriptor)
    : destination_(std::filesystem::absolute(destination).lexically_normal()), companion_(destination_.string() + ".classes.json") {
    if (!explicit_descriptor.empty() && (same_path(explicit_descriptor, companion_) || same_path(explicit_descriptor, destination_)))
        throw std::invalid_argument("publication would modify the selected class descriptor");
    std::filesystem::create_directories(destination_.parent_path());
    {
        const auto admission = detail::lock_class_artifact(destination_, false, true);
        if (std::filesystem::exists(destination_)) previous_artifact_ = io::FileSnapshot::Read(destination_);
        if (std::filesystem::exists(companion_)) {
            if (!previous_artifact_) throw std::invalid_argument("class companion has no artifact");
            previous_companion_ = io::FileSnapshot::Read(companion_);
            previous_descriptor_ = detail::read_class_descriptor(companion_);
            if (previous_descriptor_->artifact_sha256 != io::sha256_hex(io::sha256_file(destination_)))
                throw std::invalid_argument("existing class companion does not match artifact");
        }
    }
    staging_.emplace(destination_, ".", ".classes-XXXXXX", "stage RF-DETR artifact");
    staged_ = staging_->path() / "artifact" / destination_.filename();
    std::filesystem::create_directory(staged_.parent_path());
}
io::ScopedFd ClassArtifactPublication::LockPreviousArtifact() const {
    auto lease = detail::lock_class_artifact(destination_, false);
    RequirePreviousUnchanged();
    return lease;
}
void ClassArtifactPublication::RequirePreviousUnchanged() const {
    if (previous_artifact_)
        previous_artifact_->RequireUnchanged(destination_);
    else if (std::filesystem::exists(destination_))
        throw std::runtime_error("artifact appeared during publication");
    if (previous_companion_)
        previous_companion_->RequireUnchanged(companion_);
    else if (std::filesystem::exists(companion_))
        throw std::runtime_error("companion appeared during publication");
}
void ClassArtifactPublication::Publish(std::optional<ModelClassDescriptor> descriptor, std::function_ref<bool()> cancelled) {
    if (published_) throw std::logic_error("RF-DETR artifact already published");
    const auto checkpoint = [&] {
        if (cancelled()) throw ArtifactPublicationCancelled{};
    };
    checkpoint();
    io::FileHandle::open_readonly(staged_.string()).sync_data();
    const auto staged_descriptor = staging_->path() / "classes.json";
    if (descriptor) {
        descriptor->artifact_sha256 = io::sha256_hex(io::sha256_file(staged_));
        const auto encoded = encode_class_descriptor(*descriptor);
        auto output = io::FileHandle::create_output(staged_descriptor.string(), encoded.size());
        output.pwrite_all(encoded.data(), encoded.size(), 0);
        output.sync_data();
    }
    const auto commit_lock = detail::lock_class_artifact(destination_, true, true);
    checkpoint();
    RequirePreviousUnchanged();
    const auto old_artifact = staging_->path() / "previous-artifact";
    const auto old_descriptor = staging_->path() / "previous-classes";
    bool backed_artifact = false, backed_descriptor = false, installed_artifact = false, installed_descriptor = false;
    try {
        if (previous_artifact_) {
            std::filesystem::rename(destination_, old_artifact);
            backed_artifact = true;
        }
        checkpoint();
        if (previous_companion_) {
            std::filesystem::rename(companion_, old_descriptor);
            backed_descriptor = true;
        }
        checkpoint();
        std::filesystem::rename(staged_, destination_);
        installed_artifact = true;
        checkpoint();
        if (descriptor) {
            std::filesystem::rename(staged_descriptor, companion_);
            installed_descriptor = true;
        }
        checkpoint();
        io::sync_parent_directory(destination_);
        checkpoint();
        published_ = true;
    } catch (...) {
        try {
            if (installed_descriptor) std::filesystem::remove(companion_);
            if (installed_artifact) std::filesystem::remove(destination_);
            if (backed_artifact) std::filesystem::rename(old_artifact, destination_);
            if (backed_descriptor) std::filesystem::rename(old_descriptor, companion_);
            io::sync_parent_directory(destination_);
        } catch (...) {
            staging_->published();
            throw std::runtime_error("RF-DETR publication rollback failed; previous bundle retained at " + staging_->path().string());
        }
        throw;
    }
}
}  // namespace mmltk::backend::models::rfdetr
