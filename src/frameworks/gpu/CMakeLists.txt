include(MmltkComponent)

add_library(mmltk_frameworks_gpu STATIC)
add_library(mmltk::frameworks::gpu ALIAS mmltk_frameworks_gpu)
mmltk_configure_component(mmltk_frameworks_gpu PUBLIC_DECLARATIONS)

mmltk_register_public_headers(mmltk_frameworks_gpu
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        cuda/cuda_context_scope.h
        cuda/cuda_device_scope.h
        cuda/cuda_error.h
        cuda/cuda_high_water_allocation.h
        cuda/cuda_launch.cuh
        cuda/cuda_priority.h
        cuda/device_execution.h
        cuda/device_inventory.h
        cuda/resource_owner_command_authority.h
        cuda/terminal_cuda_retirement_authority.h
        cuda/terminal_cuda_retirement_owner.h
        image/external_graphics_timeline.h
        image/image_buffer.h
        image/image_failure.h
        image/image_geometry.h
        image/image_product_pool.h
        image/image_product_retirement.h
        image/image_types.h
        image/image_workspace.h
        image/imported_image_buffer.h
        memory/gdr_mapped_buffer.h
        memory/pinned_host_buffer.h
        runtime/product_revision_sequence.h
        runtime/system_image_model.h
        runtime/system_image_runtime.h
        runtime/system_image_worker.h
)
mmltk_register_ordinary_sources(
    mmltk_frameworks_gpu
    cuda/cuda_runtime.cpp
    cuda/device_execution.cpp
    cuda/terminal_cuda_retirement_owner.cpp
    image/external_graphics_timeline.cpp
    image/image_buffer.cpp
    image/image_product_pool.cpp
    image/image_workspace.cpp
    image/imported_image_buffer.cpp
    memory/detail/gdr_buffer_backend.cpp
    memory/gdr_mapped_buffer.cpp
    memory/pinned_host_buffer.cpp
    runtime/system_image_runtime.cpp
    runtime/system_image_worker.cpp)
mmltk_register_private_headers(
    mmltk_frameworks_gpu
    memory/detail/gdr_buffer_backend.h
)

target_link_libraries(mmltk_frameworks_gpu
    PUBLIC
        mmltk::common::io
        mmltk::common::logging
        mmltk::common::system
        mmltk::common::types
        mmltk::frameworks::reflection
        CUDA::cudart
    PRIVATE
        CUDA::cuda_driver
        mmltk_gdrcopy
)
mmltk_register_header_isolation(mmltk_frameworks_gpu)

add_library(mmltk_frameworks_gpu_test_support INTERFACE)
mmltk_register_public_headers(mmltk_frameworks_gpu_test_support
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES tests/fake_image_backend.h tests/device_execution_fixture.h tests/pinned_host_fault.h)
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
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/product_transaction.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/receiver_copy.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/workspace_interop.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/image_execution_retirement.test.cpp
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
