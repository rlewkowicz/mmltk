#pragma once

#include <exception>

namespace mmltk::frameworks::gpu {

class SystemImageModel {
   public:
    struct Release final {
        bool all_released = true;
        std::exception_ptr failure{};
    };
    virtual ~SystemImageModel() = default;
    virtual void StopIngress() noexcept {}
    [[nodiscard]] virtual Release ReleaseResources() noexcept { return {}; }
};

}  // namespace mmltk::frameworks::gpu
