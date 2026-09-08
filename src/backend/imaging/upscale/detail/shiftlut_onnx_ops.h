#pragma once

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Ort { struct SessionOptions; }

namespace mmltk::backend::imaging::upscale::shiftlut {

inline constexpr const char* kDomain = "mmltk.upscale";
inline constexpr const char* kOperator = "ShiftLutS7";
inline constexpr int kVersion = 1;
inline constexpr std::size_t kChannels = 16;
inline constexpr std::size_t kStages = 8;
inline constexpr std::size_t kDepthwiseElements = kChannels * 9 * 64;
inline constexpr std::size_t kPointwiseElements = kChannels * kChannels * 64;
inline constexpr std::size_t kLowElements = kChannels * 9 * 4;
inline constexpr std::size_t kUpOffset = kLowElements + kStages * (kDepthwiseElements + kPointwiseElements);
inline constexpr std::size_t kShiftOffset = kUpOffset + kPointwiseElements;
inline constexpr std::size_t kTableElements = kShiftOffset + kStages * 2 * kChannels;
inline constexpr std::size_t kScratchElements = 4 * 3 * kChannels * 256 * 256;

// The runtime owns physical storage across ORT graph fallback and releases it
// explicitly after provider settlement, before destroying the session.
class Operators final {
   public:
    explicit Operators(std::size_t decision_capacity_pixels = 0);
    ~Operators();
    Operators(const Operators&) = delete;
    Operators& operator=(const Operators&) = delete;
    void Register(Ort::SessionOptions&);
    cudaError_t Release() noexcept;
    // Verification-only opt-in: copy captured integer lookup inputs after the
    // caller settles inference. Ordinary runtimes allocate no decision storage.
    cudaError_t ReadDecisions(std::span<std::int8_t>) noexcept;
   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Effect-only cold allocation evidence; never used to order or select work.
std::uint64_t allocation_count() noexcept;

// All storage belongs to the session kernel and remains fixed across capture,
// replay and requests. This function only submits kernels to ORT's stream.
cudaError_t enqueue(const float* input, const float* tables, std::int8_t* first, std::int8_t* second,
             float* output, std::uint32_t height, std::uint32_t width, cudaStream_t stream,
             std::int8_t* decisions = nullptr);

}  // namespace mmltk::backend::imaging::upscale::shiftlut
