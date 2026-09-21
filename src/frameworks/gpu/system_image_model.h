#pragma once
#include <exception>
#include <memory>
namespace mmltk::frameworks::gpu {
class DeviceContext;
class ImageStream;
class SystemImageModel {
public:
 struct Release final {
  bool all_released = true;
  std::exception_ptr failure{};
 };
 virtual ~SystemImageModel() = default;
 // Supplies the exact runtime context to algorithm owners without creating another context.
 virtual void BindExecutionContext(const DeviceContext&, std::shared_ptr<ImageStream>) {}
 virtual void StopIngress() noexcept {}
 [[nodiscard]] virtual Release ReleaseResources() noexcept { return {}; }
};
}  // namespace mmltk::frameworks::gpu
