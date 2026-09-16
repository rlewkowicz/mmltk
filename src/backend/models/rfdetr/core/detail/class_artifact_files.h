#pragma once
#include <filesystem>
#include <stdexcept>
#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include "src/backend/models/rfdetr/contract/class_layout.h"
#include "src/common/io/file_memory.h"
namespace mmltk::backend::models::rfdetr {
namespace detail {
// Shared file primitives: neither admission nor publication owns the other.
[[nodiscard]] mmltk::common::io::ScopedFd lock_class_artifact(const std::filesystem::path& artifact, bool write, bool create = false);
[[nodiscard]] ModelClassDescriptor read_class_descriptor(const std::filesystem::path& path);
}  // namespace detail
}  // namespace mmltk::backend::models::rfdetr
