#pragma once
#include <cstddef>
#include <cstdint>
#include "src/common/io/file_memory.h"
namespace mmltk::backend::data::detail {
class WritablePixelRange final {
public:
 WritablePixelRange(int file_descriptor, std::size_t offset, std::size_t bytes);
 ~WritablePixelRange();
 WritablePixelRange(const WritablePixelRange&) = delete;
 WritablePixelRange& operator=(const WritablePixelRange&) = delete;
 WritablePixelRange(WritablePixelRange&&) noexcept;
 WritablePixelRange& operator=(WritablePixelRange&&) noexcept;
 [[nodiscard]] float* image(std::uint32_t image_index, std::size_t image_stride) const noexcept;

private:
 mmltk::common::io::MappedByteRegion region_;
};
}  // namespace mmltk::backend::data::detail
