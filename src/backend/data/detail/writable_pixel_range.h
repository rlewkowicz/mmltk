#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

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
    class Owner;
    std::unique_ptr<Owner> owner_;
};

}  // namespace mmltk::backend::data::detail
