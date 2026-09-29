#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
#include "src/common/io/file_memory.h"
#include "src/backend/data/compiled/compiled_file_layout.h"
namespace mmltk::backend::data::detail {
// Independently mapped blocks never relocate when later slots arrive. The owner
// drains all borrowers before destruction; vector growth moves only map handles.
class WritablePixelRange final {
public:
 WritablePixelRange(int file_descriptor, const FileLayout&);
 void grow(const FileLayout&);
 [[nodiscard]] float* image(std::uint32_t slot) const;

private:
 int descriptor_;
 FileLayout layout_;
 mutable std::mutex mutex_;
 std::vector<mmltk::common::io::MappedByteRegion> blocks_;
};
}  // namespace mmltk::backend::data::detail
