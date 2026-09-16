include(MmltkComponent)

add_library(mmltk_backend_ml_cuda STATIC)
add_library(mmltk::backend::ml::cuda ALIAS mmltk_backend_ml_cuda)
mmltk_configure_component(mmltk_backend_ml_cuda PUBLIC_DECLARATIONS)

mmltk_register_public_headers(
    mmltk_backend_ml_cuda
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        torch_autocast_scope.h
        numa_host_tensor.h
        tensor_readback.h)

mmltk_register_retained_module_providers(
    mmltk_backend_ml_cuda torch_cuda_scope.cppm)
foreach(_mmltk_cuda_partition IN ITEMS
        gpu_quiescence.cppm
        shared_cuda_event.cppm)
    mmltk_register_retained_module_providers(
        mmltk_backend_ml_cuda "${_mmltk_cuda_partition}")
endforeach()
mmltk_register_retained_module_implementation_units(mmltk_backend_ml_cuda
    gpu_quiescence.cpp
    shared_cuda_event.cpp
    detail/torch_cuda_scope.cpp
)
mmltk_register_ordinary_sources(mmltk_backend_ml_cuda
    numa_host_tensor.cpp
    tensor_readback.cpp
    detail/torch_cuda_utils.cpp)
mmltk_register_private_headers(mmltk_backend_ml_cuda
    detail/shared_cuda_event_runtime.inc
    detail/torch_cuda_utils.h)
target_include_directories(mmltk_backend_ml_cuda PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mmltk_backend_ml_cuda
    PUBLIC
        mmltk::frameworks::gpu
    PRIVATE
        CUDA::cudart
)
mmltk_enable_torch(mmltk_backend_ml_cuda PRIVATE)
mmltk_link_declarations(mmltk_backend_ml_cuda PUBLIC mmltk::backend::ml::torch_dependencies)
set_target_properties(mmltk_backend_ml_cuda PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    CUDA_SEPARABLE_COMPILATION ON
)

function(mmltk_add_backend_ml_cuda_tests)
    add_executable(mmltk_backend_ml_cuda_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_ml_cuda_tests PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(mmltk_backend_ml_cuda_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/cuda_utils.test.cpp)
    mmltk_register_ordinary_sources(mmltk_backend_ml_cuda_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/tensor_readback.test.cpp)
    target_link_options(mmltk_backend_ml_cuda_tests PRIVATE "LINKER:--wrap=cudaStreamSynchronize" "LINKER:--wrap=cudaMemcpyAsync")
    target_include_directories(mmltk_backend_ml_cuda_tests PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests
    )
    target_link_libraries(mmltk_backend_ml_cuda_tests PRIVATE
        mmltk_frameworks_gpu_test_support
        mmltk_test_support
        mmltk::backend::ml::cuda
        CUDA::cudart
    )
    mmltk_enable_native_runtime_rpath(mmltk_backend_ml_cuda_tests)
    catch_discover_tests(
        mmltk_backend_ml_cuda_tests
        TEST_PREFIX "backend::ml::cuda::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
