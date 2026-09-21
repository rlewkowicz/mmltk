#pragma once
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include "src/frameworks/gpu/image_types.h"
namespace mmltk::frameworks::gpu {
// Counts product states and display allocations whose cleanup responsibility
// they accepted. It owns settlement bookkeeping, never the physical resources.
class ImageProductRetirement final {
public:
 [[nodiscard]] ImageStreamSettlement Retire() noexcept;
 [[nodiscard]] std::exception_ptr failure() const noexcept;
 [[nodiscard]] bool unsafe() const noexcept;
 void SetRetirementSink(std::shared_ptr<const std::function<void()>>) noexcept;

private:
 void Acquire() noexcept;
 void Released(ImageStreamSettlement) noexcept;
 void Notify() const noexcept;
 mutable std::mutex mutex_;
 std::size_t live_ = 0U;
 bool retired_ = false;
 bool unsafe_ = false;
 std::exception_ptr failure_;
 std::shared_ptr<const std::function<void()>> sink_;
 friend class ImageProductBuffer;
 friend class ImageWorkspace;
};
}  // namespace mmltk::frameworks::gpu
