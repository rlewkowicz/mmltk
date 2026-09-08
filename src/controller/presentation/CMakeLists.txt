include(MmltkComponent)

add_library(mmltk_controller_visual_contract STATIC)
add_library(mmltk::controller::visual_contract ALIAS
    mmltk_controller_visual_contract)
mmltk_configure_component(
    mmltk_controller_visual_contract PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_visual_contract
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES visual_system_types.h visual_document.h visual_source_projection.h)
mmltk_register_ordinary_sources(
    mmltk_controller_visual_contract
    visual_system_types.cpp
    visual_document.cpp)
target_link_libraries(mmltk_controller_visual_contract
    PUBLIC
        mmltk::controller::application_boundary
        mmltk::controller::contracts
        mmltk::frameworks::gpu
        mmltk::frameworks::reflection)
mmltk_register_header_isolation(mmltk_controller_visual_contract)

add_library(mmltk_controller_visual_runtime STATIC)
add_library(mmltk::controller::visual_runtime ALIAS
    mmltk_controller_visual_runtime)
mmltk_configure_component(mmltk_controller_visual_runtime PUBLIC_DECLARATIONS)
mmltk_register_public_headers(mmltk_controller_visual_runtime
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES visual_runtime.h visual_diagnostics.h)
mmltk_register_private_headers(mmltk_controller_visual_runtime
    detail/visual_runtime_owner.h)
mmltk_register_ordinary_sources(mmltk_controller_visual_runtime
    visual_runtime.cpp visual_diagnostics.cpp detail/visual_runtime_owner.cpp)
target_link_libraries(mmltk_controller_visual_runtime
    PUBLIC
        mmltk::controller::contracts
        mmltk::controller::visual_contract
        mmltk::frameworks::gpu
        mmltk::frameworks::reflection)
mmltk_link_declarations(mmltk_controller_visual_runtime PUBLIC
    mmltk::controller::direct_services)
mmltk_register_header_isolation(mmltk_controller_visual_runtime)

add_library(mmltk_controller_presentation_system STATIC)
add_library(mmltk::controller::presentation_system ALIAS
    mmltk_controller_presentation_system)
mmltk_configure_component(
    mmltk_controller_presentation_system PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_presentation_system
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES presentation_system.h)
mmltk_register_ordinary_sources(
    mmltk_controller_presentation_system presentation_system.cpp)
mmltk_register_retained_module_importers(
    mmltk_controller_presentation_system
    native_presentation_writer.cpp
    workspace_surface_import_channel.cpp)
mmltk_register_private_headers(
    mmltk_controller_presentation_system
    detail/workspace_frame_signal.h
    detail/workspace_surface_import_abi.h
    detail/workspace_surface_import_channel.h
    workspace_presentation_types.h)
target_link_libraries(mmltk_controller_presentation_system
    PUBLIC
        mmltk::controller::application_boundary
        mmltk::controller::visual_contract
        mmltk::frameworks::gpu
    PRIVATE
        mmltk::controller::visual_runtime
        mmltk::backend::imaging::raster
        mmltk::common::io
        mmltk::common::logging
        CUDA::cuda_driver
        CUDA::cudart)
mmltk_link_declarations(mmltk_controller_presentation_system PUBLIC
    mmltk::controller::visual_runtime)
mmltk_register_header_isolation(mmltk_controller_presentation_system)

function(mmltk_add_controller_presentation_tests)
    add_executable(
        mmltk_controller_visual_systems_tests EXCLUDE_FROM_ALL)
    mmltk_register_ordinary_sources(
        mmltk_controller_visual_systems_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/visual_systems.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/native_gallery.test.cpp)
    mmltk_configure_component(
        mmltk_controller_visual_systems_tests PRIVATE_DECLARATIONS)
    target_link_libraries(
        mmltk_controller_visual_systems_tests PRIVATE
        mmltk_acceptance_support
        mmltk::controller::browser::protocol_v13
        mmltk::controller::direct_services
        mmltk::controller::annotation_system
        mmltk::controller::explore_system
        mmltk::controller::live_system
        mmltk::controller::presentation_system
        mmltk::controller::upscale_system
        mmltk::controller::visual_runtime
        mmltk::frameworks::gpu
        mmltk_frameworks_gpu_test_support
        nlohmann_json)
    catch_discover_tests(
        mmltk_controller_visual_systems_tests
        TEST_PREFIX "controller::visual-systems::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS)

endfunction()
