include(MmltkComponent)

add_library(mmltk_backend_media_live STATIC)
add_library(mmltk::backend::media::live ALIAS mmltk_backend_media_live)
mmltk_configure_component(mmltk_backend_media_live PUBLIC_DECLARATIONS)

mmltk_register_public_headers(mmltk_backend_media_live
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES live_frame_id.h live_types.h manual_overlay_document.h live_session_controller.h)
mmltk_register_ordinary_sources(
    mmltk_backend_media_live
    live_types.cpp
    live_device_types.cpp
    live_state_signal.cpp
    workspace_frame_signal.cpp
    live_session_controller.cpp
    manual_overlay_document.cpp
    live_analyzer_worker.cpp
    live_frame_fanout.cpp
    live_raw_frame_cache.cpp
    live_video_ingress.cpp
)
mmltk_register_retained_module_importers(mmltk_backend_media_live
    live_compositor.cpp live_manual_overlay_worker.cpp)
mmltk_register_private_headers(
    mmltk_backend_media_live
    detail/live_analyzer_worker.h
    detail/analysis_frame.h
    detail/live_compositor_owner.h
    detail/live_device_types.h
    detail/live_frame_fanout.h
    detail/live_manual_overlay_worker.h
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
        mmltk::backend::imaging::raster
        mmltk::common::system
        mmltk::common::types
)

function(mmltk_add_backend_media_live_tests)
    add_executable(mmltk_backend_media_live_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_media_live_tests PRIVATE_DECLARATIONS)
    mmltk_register_ordinary_sources(
        mmltk_backend_media_live_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/live_output_lease.test.cpp)
    target_link_libraries(
        mmltk_backend_media_live_tests PRIVATE
        mmltk_test_support
        mmltk::backend::media::live)
    catch_discover_tests(
        mmltk_backend_media_live_tests
        TEST_PREFIX "backend::media::live::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS)
endfunction()

mmltk_register_header_isolation(mmltk_backend_media_live)
