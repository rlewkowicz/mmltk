include(MmltkComponent)

add_library(mmltk_backend_media_capture STATIC)
add_library(
    mmltk::backend::media::capture
    ALIAS mmltk_backend_media_capture
)
mmltk_configure_component(
    mmltk_backend_media_capture PUBLIC_DECLARATIONS)

foreach(_mmltk_capture_partition IN ITEMS
        status.cppm capture_types.cppm live_video_source.cppm capture_session.cppm)
    mmltk_register_retained_module_providers(
        mmltk_backend_media_capture "${_mmltk_capture_partition}")
endforeach()
mmltk_register_retained_module_implementation_units(
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
target_link_libraries(mmltk_backend_media_capture PRIVATE
    CUDA::cudart
    Threads::Threads
    mmltk::backend::ml::cuda
    mmltk::common::concurrency
    mmltk::common::logging
    mmltk::common::system
    mmltk::common::types
    mmltk::frameworks::gpu
)
