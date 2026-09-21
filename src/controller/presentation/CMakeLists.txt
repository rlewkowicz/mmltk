include(MmltkComponent)

add_library(mmltk_controller_visual_contract STATIC)
add_library(mmltk::controller::visual_contract ALIAS
    mmltk_controller_visual_contract)
mmltk_configure_component(
    mmltk_controller_visual_contract PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_visual_contract
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES annotation_palette.h visual_system_types.h visual_document.h visual_source_projection.h)
mmltk_register_ordinary_sources(
    mmltk_controller_visual_contract
    annotation_palette.cpp
    visual_system_types.cpp
    visual_document.cpp)
target_link_libraries(mmltk_controller_visual_contract
    PUBLIC
        mmltk::controller::application_boundary
        mmltk::controller::contracts
        mmltk::frameworks::gpu
        mmltk::frameworks::reflection)
target_link_libraries(mmltk_controller_visual_contract PRIVATE
    mmltk::backend::imaging::primitives)
mmltk_register_header_isolation(mmltk_controller_visual_contract)

add_library(mmltk_controller_visual_runtime STATIC)
add_library(mmltk::controller::visual_runtime ALIAS
    mmltk_controller_visual_runtime)
mmltk_configure_component(mmltk_controller_visual_runtime PUBLIC_DECLARATIONS)
mmltk_register_public_headers(mmltk_controller_visual_runtime
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES visual_runtime.h visual_runtime_owner.h visual_diagnostics.h workspace_input.h)
mmltk_register_ordinary_sources(mmltk_controller_visual_runtime
    visual_diagnostics.cpp workspace_input.cpp visual_runtime_owner.cpp)
mmltk_register_retained_module_importers(mmltk_controller_visual_runtime visual_runtime.cpp)
target_link_libraries(mmltk_controller_visual_runtime
    PUBLIC
        mmltk::controller::contracts
        mmltk::controller::visual_contract
        mmltk::frameworks::gpu
        mmltk::frameworks::reflection)
target_link_libraries(mmltk_controller_visual_runtime PRIVATE
    mmltk::backend::imaging::raster
    mmltk::common::types)
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
    detail/native_presentation_writer_test_access.h
    abi/workspace_frame_signal.h
    abi/workspace_surface_import_abi.h
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

add_library(mmltk_controller_presentation_test_support INTERFACE)
mmltk_register_public_headers(mmltk_controller_presentation_test_support
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES tests/support/workspace_surface_socket_test_utils.hpp detail/native_presentation_writer_test_access.h)
target_link_libraries(mmltk_controller_presentation_test_support INTERFACE mmltk::controller::presentation_system)

add_library(mmltk_controller_visual_fixture STATIC EXCLUDE_FROM_ALL)
mmltk_configure_component(mmltk_controller_visual_fixture PUBLIC_DECLARATIONS)
mmltk_register_public_headers(mmltk_controller_visual_fixture
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES tests/support/visual_system_fixture.h tests/support/visual_runtime_fixture.h)
mmltk_register_ordinary_sources(mmltk_controller_visual_fixture tests/support/visual_system_fixture.cpp)
target_link_libraries(mmltk_controller_visual_fixture PUBLIC
    mmltk_controller_presentation_test_support
    mmltk_frameworks_gpu_test_support
    mmltk_test_support
    mmltk::controller::visual_runtime
    mmltk::controller::direct_services
    nlohmann_json)

function(mmltk_add_controller_presentation_tests)
    add_executable(
        mmltk_controller_visual_systems_tests EXCLUDE_FROM_ALL)
    mmltk_register_ordinary_sources(
        mmltk_controller_visual_systems_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/visual_systems.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/presentation_contract.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/visual_runtime.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/native_gallery.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/workspace_acceptance_gate.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/support/allocation_tracking.cpp)
    mmltk_register_private_headers(mmltk_controller_visual_systems_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/support/allocation_tracking.h)
    mmltk_configure_component(
        mmltk_controller_visual_systems_tests PRIVATE_DECLARATIONS)
    target_link_libraries(
        mmltk_controller_visual_systems_tests PRIVATE
        mmltk_controller_annotation_test_support
        mmltk_controller_visual_fixture
        mmltk_controller_explore_test_support
        mmltk_controller_upscale_test_support
        mmltk_controller_live_test_support
        mmltk_controller_presentation_test_support
        mmltk_test_support
        mmltk::controller::browser::protocol_v17
        mmltk::controller::direct_services
        mmltk::controller::annotation_system
        mmltk::controller::explore_system
        mmltk::controller::live_system
        mmltk::controller::presentation_system
        mmltk::controller::upscale_system
        mmltk::controller::visual_runtime
        mmltk::frameworks::gpu
        mmltk_frameworks_gpu_test_support
        mmltk::backend::ml::runtime
        nlohmann_json)
    catch_discover_tests(
        mmltk_controller_visual_systems_tests
        TEST_PREFIX "controller::visual-systems::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS)

endfunction()

mmltk_register_header_isolation(mmltk_controller_visual_fixture)
