include(MmltkComponent)

add_library(mmltk_backend_media_capture STATIC)
add_library(
    mmltk::backend::media::capture
    ALIAS mmltk_backend_media_capture
)
mmltk_configure_component(
    mmltk_backend_media_capture PUBLIC_DECLARATIONS)

mmltk_register_public_headers(mmltk_backend_media_capture
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES capture_types.h status.h live_video_source.h capture_session.h)
mmltk_register_ordinary_sources(
    mmltk_backend_media_capture
    capture_types.cpp
    capture_session_core.cpp
    capture_session_device.cpp
    capture_session_internal.cpp
    capture_session_preview.cpp
)
mmltk_register_private_headers(mmltk_backend_media_capture
    detail/capture_session_impl.hpp)
target_include_directories(mmltk_backend_media_capture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mmltk_backend_media_capture
    PUBLIC mmltk::frameworks::gpu
    PRIVATE
    CUDA::cudart
    Threads::Threads
    mmltk::backend::ml::cuda
    mmltk::common::concurrency
    mmltk::common::logging
    mmltk::common::system
    mmltk::common::types
)

mmltk_register_header_isolation(mmltk_backend_media_capture)
