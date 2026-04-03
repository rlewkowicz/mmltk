#pragma once

#include "mmltk_logging.h"

#include <array>
#include <cstdint>
#include <cuda_runtime_api.h>
#include <imgui.h>

#include <string>
#include <string_view>
#include <type_traits>

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <GL/gl.h>

namespace mmltk::gui {

template <typename T>
    requires(std::is_pointer_v<T>)
inline T texture_id_from_gl_name(const GLuint texture_name) {
    return reinterpret_cast<T>(static_cast<std::uintptr_t>(texture_name));
}

template <typename T>
    requires(!std::is_pointer_v<T>)
inline T texture_id_from_gl_name(const GLuint texture_name) {
    return static_cast<T>(texture_name);
}

inline ImTextureID imgui_texture_id_from_gl_name(const GLuint texture_name) {
    return texture_id_from_gl_name<ImTextureID>(texture_name);
}

inline std::string cuda_gl_interop_error_message(const cudaError_t error, const char* label) {
    std::string message(label);
    message += " failed: ";
    message += cudaGetErrorString(error);
    if (error == cudaErrorInvalidGraphicsContext) {
        message +=
            ". CUDA-OpenGL interop requires a valid hardware OpenGL context. "
            "On Linux/NVIDIA, prefer an X11/GLX context when DISPLAY is available "
            "(for example MMLTK_GUI_PLATFORM=x11).";
    }
    return message;
}

inline bool is_invalid_graphics_context_error(const std::string_view error_message) {
    return error_message.find("invalid OpenGL or DirectX context") != std::string_view::npos;
}

inline void log_gui_surface_error(const char* surface_name, const std::string_view message) {
    if (message.empty()) {
        return;
    }
    mmltk::logging::logger("gui")->error("mmltk {} error: {}", surface_name, message);
}

inline void initialize_gui_texture_pair(std::array<GLuint, 2>& textures) {
    glGenTextures(2, textures.data());
    for (const GLuint texture : textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

inline void destroy_gui_texture_pair(std::array<GLuint, 2>& textures) {
    if (textures[0] != 0U || textures[1] != 0U) {
        glDeleteTextures(2, textures.data());
        textures = {0U, 0U};
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

inline bool ensure_gui_texture_storage(std::array<GLuint, 2>& textures,
                                       const std::uint32_t width,
                                       const std::uint32_t height,
                                       const GLint internal_format,
                                       const GLenum format,
                                       const char* zero_dimension_message,
                                       std::uint32_t& cached_width,
                                       std::uint32_t& cached_height,
                                       std::string* error_message) {
    if (width == 0U || height == 0U) {
        if (error_message != nullptr) {
            *error_message = zero_dimension_message;
        }
        return false;
    }
    if (cached_width == width && cached_height == height) {
        return true;
    }

    for (const GLuint texture : textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     internal_format,
                     static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height),
                     0,
                     format,
                     GL_UNSIGNED_BYTE,
                     nullptr);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    cached_width = width;
    cached_height = height;
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

} // namespace mmltk::gui
