#pragma once
#include <filesystem>
#include <stdexcept>
#include "src/backend/models/rfdetr/contract/class_layout.h"
#include "src/common/io/file_memory.h"
namespace mmltk::backend::models::rfdetr {
class ArtifactPublicationCancelled final : public std::runtime_error {
 public:
    ArtifactPublicationCancelled() : std::runtime_error("RF-DETR artifact publication cancelled") {}
};
namespace detail {
// Shared file primitives: neither admission nor publication owns the other.
[[nodiscard]] mmltk::common::io::UniqueFd lock_class_artifact(const std::filesystem::path& artifact, bool write, bool create = false);
[[nodiscard]] ModelClassDescriptor read_class_descriptor(const std::filesystem::path& path);
}
}
