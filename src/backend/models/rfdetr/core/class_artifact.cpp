#include "src/backend/models/rfdetr/core/class_artifact.h"
#include "src/backend/models/rfdetr/core/detail/class_artifact_files.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <utility>
namespace mmltk::backend::models::rfdetr {
namespace io = mmltk::common::io;
namespace detail {
io::ScopedFd lock_class_artifact(const std::filesystem::path& artifact, bool write, bool create) {
    const auto path = artifact.string() + ".classes.lock";
    io::ScopedFd lock(::open(path.c_str(), (create ? O_CREAT | O_RDWR : O_RDONLY) | O_CLOEXEC, 0600));
    if (lock.get() < 0) {
        if (!create && !write && errno == ENOENT) return lock;
        throw io::errno_error("open RF-DETR bundle lock", path);
    }
    if (::flock(lock.get(), write ? LOCK_EX : LOCK_SH) != 0) throw io::errno_error("lock RF-DETR bundle", path);
    return lock;
}
ModelClassDescriptor read_class_descriptor(const std::filesystem::path& path) {
    const auto file = mmltk::common::io::FileHandle::open_readonly(path.string());
    const auto snapshot = io::FileSnapshot::Read(file.get());
    if (snapshot.bytes > kClassLayoutByteBudget) throw std::invalid_argument("oversized class descriptor");
    std::string text(snapshot.bytes, '\0');
    file.pread_all(text.data(), text.size(), 0);
    auto descriptor = decode_class_descriptor(text);
    if (snapshot != io::FileSnapshot::Read(file.get())) throw std::runtime_error("class descriptor changed while reading");
    snapshot.RequireUnchanged(path);
    return descriptor;
}
}  // namespace detail
namespace {
std::filesystem::path normalized(const std::filesystem::path& path) { return path.empty() ? path : std::filesystem::absolute(path).lexically_normal(); }
ClassArtifactSnapshot capture(const std::filesystem::path& artifact, const std::filesystem::path& descriptor) {
    ClassArtifactSnapshot result{
        .artifact_path = normalized(artifact), .descriptor_path = normalized(descriptor), .artifact = io::FileSnapshot::Read(artifact)};
    const auto companion = std::filesystem::path(artifact.string() + ".classes.json");
    if (std::filesystem::exists(companion)) result.companion = io::FileSnapshot::Read(companion);
    if (!descriptor.empty() && std::filesystem::exists(descriptor)) result.descriptor = io::FileSnapshot::Read(descriptor);
    return result;
}
void check_stop(std::stop_token stop) {
    if (stop.stop_requested()) throw ArtifactPublicationCancelled{};
}
}  // namespace
ClassArtifactSnapshot ClassArtifactSnapshot::Read(const std::filesystem::path& artifact, const std::filesystem::path& descriptor) {
    const auto lock = detail::lock_class_artifact(artifact, false);
    return capture(artifact, descriptor);
}
ClassArtifactAdmission::ClassArtifactAdmission(const std::filesystem::path& artifact, const std::filesystem::path& descriptor,
                                               std::shared_ptr<const io::FileDigests> admitted_file, std::stop_token stop, bool include_md5) {
    check_stop(stop);
    // Hash outside the bundle lock; snapshot equality rejects a concurrent publication.
    if (!admitted_file) {
        auto digest = io::try_file_digests(artifact, include_md5, [&] { return stop.stop_requested(); });
        if (!digest) throw ArtifactPublicationCancelled{};
        admitted_file = std::make_shared<const io::FileDigests>(std::move(*digest));
    }
    const auto lock = detail::lock_class_artifact(artifact, false);
    check_stop(stop);
    snapshot_ = capture(artifact, descriptor);
    if (admitted_file->snapshot != snapshot_.artifact) throw std::runtime_error("artifact digest proof is stale");
    file_ = std::move(admitted_file);
    if (!descriptor.empty() && !snapshot_.descriptor) throw std::invalid_argument("missing selected class descriptor");
    const auto companion = std::filesystem::path(snapshot_.artifact_path.string() + ".classes.json");
    const auto digest = io::sha256_hex(file_->sha256);
    descriptors_.reserve(2);
    for (const auto& path : {companion, snapshot_.descriptor_path}) {
        if (path.empty() || (path == snapshot_.descriptor_path && path == companion && !descriptors_.empty())) continue;
        if (!std::filesystem::exists(path)) continue;
        auto parsed = detail::read_class_descriptor(path);
        if (parsed.artifact_sha256 != digest) throw std::invalid_argument("class descriptor does not match artifact digest");
        descriptors_.push_back(std::move(parsed));
    }
    if (snapshot_ != capture(snapshot_.artifact_path, snapshot_.descriptor_path))
        throw std::runtime_error("class artifact changed during descriptor admission");
    check_stop(stop);
}
std::vector<RfdetrNamedOutputRole> ClassArtifactAdmission::output_roles() const { return class_descriptor_output_roles(descriptors_); }
void ClassArtifactAdmission::RequireUnchanged(std::stop_token stop) const {
    check_stop(stop);
    if (snapshot_ != ClassArtifactSnapshot::Read(snapshot_.artifact_path, snapshot_.descriptor_path))
        throw std::runtime_error("class artifact changed during model admission");
    check_stop(stop);
}
bool ClassArtifactAdmission::Matches(const std::filesystem::path& artifact, const std::filesystem::path& descriptor) const {
    const auto current = ClassArtifactSnapshot::Read(artifact, descriptor);
    if (!current.descriptor_path.empty() && !current.descriptor) throw std::invalid_argument("missing selected class descriptor");
    return snapshot_ == current;
}
ModelClassLayout ClassArtifactAdmission::Resolve(std::size_t width, const std::optional<ModelClassLayout>& embedded, std::stop_token stop) const {
    check_stop(stop);
    auto result = admit_artifact_class_layout(width, embedded, descriptors_);
    RequireUnchanged(stop);
    return result;
}
}  // namespace mmltk::backend::models::rfdetr
