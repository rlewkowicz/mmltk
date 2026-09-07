include(MmltkComponent)

add_library(mmltk_backend_media_live STATIC)
add_library(mmltk::backend::media::live ALIAS mmltk_backend_media_live)
mmltk_configure_component(mmltk_backend_media_live PUBLIC_DECLARATIONS)

foreach(_mmltk_live_partition IN ITEMS
        live_capture_region.cppm
        live_frame_id.cppm
        live_session_controller.cppm
        live_types.cppm
        manual_overlay_document.cppm)
    mmltk_register_retained_module_providers(
        mmltk_backend_media_live "${_mmltk_live_partition}")
endforeach()
mmltk_register_retained_module_implementation_units(
    mmltk_backend_media_live
    live_types.cpp
    live_compositor.cpp
    live_runtime_support.cpp
    live_session_controller.cpp
    manual_overlay_document.cpp
    live_analyzer_worker.cpp
    live_frame_fanout.cpp
    live_manual_overlay_worker.cpp
    live_raw_frame_cache.cpp
    live_video_ingress.cpp
)
mmltk_register_private_headers(
    mmltk_backend_media_live
    detail/live_analyzer_worker.h
    detail/analysis_frame.h
    detail/live_compositor_status.h
    detail/live_compositor_owner.h
    detail/live_device_types.h
    detail/live_frame_fanout.h
    detail/live_manual_overlay_worker.h
    detail/live_module_dependencies.h
    detail/live_output_callback_lifetime.h
    detail/live_physical_retirement.h
    detail/live_state_signal.h
    detail/live_session_owner.h
    detail/live_slot_state.h
    detail/live_video_ingress.h
    detail/overlay_palette.h
    detail/workspace_frame_signal.h)
target_include_directories(mmltk_backend_media_live PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mmltk_backend_media_live
    PUBLIC
        mmltk::backend::imaging::annotation
        mmltk::backend::media::capture
        mmltk::backend::ml::runtime
        mmltk::frameworks::gpu
    PRIVATE
        CUDA::cudart
        Threads::Threads
        nlohmann_json
        mmltk::backend::imaging::raster
        mmltk::common::system
        mmltk::common::types
)

function(mmltk_add_backend_media_live_tests)
    add_executable(mmltk_backend_media_live_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_media_live_tests PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_backend_media_live_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/live_output_lease.test.cpp)
    target_link_libraries(
        mmltk_backend_media_live_tests PRIVATE
        mmltk_acceptance_support
        mmltk::backend::media::live)
    catch_discover_tests(
        mmltk_backend_media_live_tests
        TEST_PREFIX "backend::media::live::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS)
endfunction()
