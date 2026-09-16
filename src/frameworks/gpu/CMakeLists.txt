include(MmltkComponent)

add_library(mmltk_frameworks_gpu STATIC)
add_library(mmltk::frameworks::gpu ALIAS mmltk_frameworks_gpu)
mmltk_configure_component(mmltk_frameworks_gpu PUBLIC_DECLARATIONS)

mmltk_register_public_headers(mmltk_frameworks_gpu
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        cuda_error.h
        device_execution.h
        pinned_host_buffer.h
        gdr_mapped_buffer.h
        image_geometry.h
        image_buffer.h
        image_failure.h
        image_product_pool.h
        image_product_retirement.h
        image_types.h
        image_workspace.h
        imported_image_buffer.h
        external_graphics_timeline.h
        system_image_worker.h
        system_image_runtime.h
        product_revision_sequence.h
        system_image_model.h
        cuda_high_water_allocation.h
        cuda_priority.h
        cuda_device_scope.h
        cuda_context_scope.h
        resource_owner_command_authority.h
        terminal_cuda_retirement_authority.h
        terminal_cuda_retirement_owner.h
)
mmltk_register_ordinary_sources(
    mmltk_frameworks_gpu
    terminal_cuda_retirement_owner.cpp
    cuda_runtime.cpp
    device_execution.cpp
    pinned_host_buffer.cpp
    gdr_mapped_buffer.cpp
    detail/gdr_buffer_backend.cpp
    imported_image_buffer.cpp
    external_graphics_timeline.cpp
    image_buffer.cpp
    image_workspace.cpp
    image_product_pool.cpp
    system_image_runtime.cpp
    system_image_worker.cpp)
mmltk_register_private_headers(
    mmltk_frameworks_gpu
    detail/gdr_buffer_backend.h
)

target_link_libraries(mmltk_frameworks_gpu
    PUBLIC
        mmltk::common::io
        mmltk::common::logging
        mmltk::common::system
        mmltk::common::types
        CUDA::cudart
    PRIVATE
        CUDA::cuda_driver
        mmltk_gdrcopy
)
mmltk_register_header_isolation(mmltk_frameworks_gpu)

add_library(mmltk_frameworks_gpu_test_support INTERFACE)
mmltk_register_public_headers(mmltk_frameworks_gpu_test_support
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES tests/fake_image_backend.h tests/device_execution_fixture.h)
target_link_libraries(mmltk_frameworks_gpu_test_support INTERFACE
    mmltk::frameworks::gpu mmltk_test_support)

function(mmltk_add_frameworks_gpu_tests)
    find_package(Vulkan REQUIRED)
    add_library(mmltk_frameworks_gpu_hardware_test_support STATIC EXCLUDE_FROM_ALL)
    mmltk_configure_component(mmltk_frameworks_gpu_hardware_test_support PUBLIC_DECLARATIONS)
    mmltk_register_public_headers(mmltk_frameworks_gpu_hardware_test_support
        BASE_DIRS ${CMAKE_SOURCE_DIR}
        FILES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/vulkan_workspace_fixture.h)
    mmltk_register_ordinary_sources(mmltk_frameworks_gpu_hardware_test_support
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/vulkan_workspace_fixture.cpp)
    target_link_libraries(mmltk_frameworks_gpu_hardware_test_support PUBLIC
        mmltk_frameworks_gpu_test_support CUDA::cuda_driver Vulkan::Vulkan)
    add_executable(mmltk_frameworks_gpu_tests EXCLUDE_FROM_ALL)
    mmltk_register_ordinary_sources(
        mmltk_frameworks_gpu_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/image_buffer.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/device_execution.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/pinned_host_buffer.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/gdr_mapped_buffer.test.cpp)
    mmltk_configure_component(
        mmltk_frameworks_gpu_tests PRIVATE_DECLARATIONS)
    target_link_libraries(
        mmltk_frameworks_gpu_tests PRIVATE
        mmltk_test_support
        mmltk::frameworks::gpu
        mmltk_frameworks_gpu_test_support
        CUDA::cuda_driver
        mmltk_frameworks_gpu_hardware_test_support)
    catch_discover_tests(
        mmltk_frameworks_gpu_tests
        TEST_PREFIX "frameworks::gpu::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS)
endfunction()
