#pragma once
#include <cuda_runtime_api.h>
#include <utility>
namespace mmltk::backend::media::live {
template <class Handle, class Release>
[[nodiscard]] cudaError_t retire_live_local_handle(Handle& handle, const Handle empty, Release&& release) noexcept {
    const Handle owned = std::exchange(handle, empty);
    if (owned == empty) return cudaSuccess;
    return std::forward<Release>(release)(owned);
}
}  // namespace mmltk::backend::media::live
