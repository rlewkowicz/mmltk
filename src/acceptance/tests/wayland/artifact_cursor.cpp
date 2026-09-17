#include "artifact_cursor.h"
#include <cstdint>
#include <optional>
#include <stdexcept>
namespace mmltk::acceptance::wayland {
auto JsonLineCursor::line() const noexcept -> std::optional<std::uint64_t> { return line_; }
auto JsonLineCursor::finish() const -> void {
    if (format_ == Format::NativeJson && !pending_.empty()) throw std::runtime_error("native evidence ended in an incomplete record: " + path_.string());
}
}  // namespace mmltk::acceptance::wayland
