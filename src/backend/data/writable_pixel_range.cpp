#include "detail/writable_pixel_range.h"

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"

namespace mmltk::backend::data::detail {

class WritablePixelRange::Owner final {
   public:
    Owner(const int file_descriptor, const std::size_t offset, const std::size_t bytes) {
        if (bytes == 0U) return;
        void* const mapping = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor,
                                     mmltk::common::math::checked_cast<off_t>(offset, "writable pixel mmap offset overflow"));
        if (mapping == MAP_FAILED) throw mmltk::common::io::errno_error("writable pixel mmap failed");
        region_.adopt(mapping, bytes);
        (void)::madvise(mapping, bytes, MADV_HUGEPAGE);
    }

    [[nodiscard]] float* image(const std::uint32_t image_index, const std::size_t image_stride) const noexcept {
        return static_cast<float*>(region_.address()) + static_cast<std::size_t>(image_index) * (image_stride / sizeof(float));
    }

   private:
    mmltk::common::io::MappedByteRegion region_;
};

WritablePixelRange::WritablePixelRange(const int file_descriptor, const std::size_t offset, const std::size_t bytes)
    : owner_(std::make_unique<Owner>(file_descriptor, offset, bytes)) {}

WritablePixelRange::~WritablePixelRange() = default;
WritablePixelRange::WritablePixelRange(WritablePixelRange&&) noexcept = default;
WritablePixelRange& WritablePixelRange::operator=(WritablePixelRange&&) noexcept = default;

float* WritablePixelRange::image(const std::uint32_t image_index, const std::size_t image_stride) const noexcept {
    return owner_->image(image_index, image_stride);
}

}  // namespace mmltk::backend::data::detail
