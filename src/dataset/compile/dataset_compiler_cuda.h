#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mmltk::compiler_internal {

class CudaBinaryMaskResizer {
   public:
    explicit CudaBinaryMaskResizer(int device_id);
    ~CudaBinaryMaskResizer();

    CudaBinaryMaskResizer(const CudaBinaryMaskResizer&) = delete;
    CudaBinaryMaskResizer& operator=(const CudaBinaryMaskResizer&) = delete;

    CudaBinaryMaskResizer(CudaBinaryMaskResizer&& other) noexcept;
    CudaBinaryMaskResizer& operator=(CudaBinaryMaskResizer&& other) noexcept;

    static bool available() noexcept;

    void resize_batch(const std::vector<uint8_t>& source_masks, const std::vector<uint32_t>& source_widths,
                      const std::vector<uint32_t>& source_heights, const std::vector<size_t>& source_offsets,
                      uint32_t target_width, uint32_t target_height, std::vector<uint8_t>& output_masks);

   private:
    void reset() noexcept;

    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace mmltk::compiler_internal
