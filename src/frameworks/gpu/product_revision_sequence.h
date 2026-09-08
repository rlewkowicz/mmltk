#pragma once

#include <cstdint>
#include <mutex>

namespace mmltk::frameworks::gpu {

class ImageProductRevisionSequence final {
   public:
    explicit ImageProductRevisionSequence(std::uint64_t next = 1U);
    [[nodiscard]] std::uint64_t Take();

   private:
    std::mutex mutex_;
    std::uint64_t next_;
};

}  // namespace mmltk::frameworks::gpu
