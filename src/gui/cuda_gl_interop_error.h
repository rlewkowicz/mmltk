#pragma once

#include <cuda_runtime_api.h>

#include <string>
#include <string_view>

namespace mmltk::gui {

inline std::string cuda_gl_interop_error_message(const cudaError_t error, const char* label) {
    std::string message(label);
    message += " failed: ";
    message += cudaGetErrorString(error);
    if (error == cudaErrorInvalidGraphicsContext) {
        message +=
            ". CUDA-OpenGL interop requires a valid hardware OpenGL or EGL context. "
            "Ensure the browser host can reach the active Wayland display runtime and that GPU "
            "acceleration is available in the container session.";
    }
    return message;
}

inline bool is_invalid_graphics_context_error(const std::string_view error_message) {
    return error_message.find("invalid OpenGL or DirectX context") != std::string_view::npos;
}

}  // namespace mmltk::gui
