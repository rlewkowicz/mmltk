include(MmltkComponent)

add_library(mmltk_backend_ml_layers STATIC)
add_library(mmltk::backend::ml::layers ALIAS mmltk_backend_ml_layers)
mmltk_configure_component(mmltk_backend_ml_layers PRIVATE_DECLARATIONS)
mmltk_register_retained_module_importers(
    mmltk_backend_ml_layers ms_deform_attn_op.cpp)
mmltk_register_ordinary_sources(mmltk_backend_ml_layers
    ms_deform_attn_autograd.cpp
    ms_deform_attn_cuda_wrapper.cpp
    ms_deform_attn_cuda.cu)
mmltk_register_private_headers(mmltk_backend_ml_layers
    detail/ms_deform_attn_op_abi.h
    detail/ms_deform_attn_cuda.h
    detail/ms_deform_attn_cuda_launch.h
    detail/ms_deform_im2col_cuda.cuh
    detail/ms_deform_attn.h
)
target_include_directories(mmltk_backend_ml_layers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mmltk_backend_ml_layers
    PRIVATE
        mmltk::common::logging
        CUDA::cudart
)
mmltk_enable_torch(mmltk_backend_ml_layers PRIVATE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(ms_deform_attn_autograd.cpp PROPERTIES
        COMPILE_OPTIONS
            "-Wno-stringop-overflow;-Wno-null-dereference;-Wno-array-bounds"
    )
endif()
set_target_properties(mmltk_backend_ml_layers PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    CUDA_SEPARABLE_COMPILATION ON
)

function(mmltk_add_backend_ml_layers_tests)
    add_executable(mmltk_backend_ml_layers_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_ml_layers_tests PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_backend_ml_layers_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/ms_deform_attn.test.cpp)
    target_include_directories(mmltk_backend_ml_layers_tests PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/detail)
    target_link_libraries(mmltk_backend_ml_layers_tests PRIVATE
        mmltk_acceptance_support
        mmltk::backend::ml::layers
        CUDA::cudart
    )
    mmltk_enable_torch(mmltk_backend_ml_layers_tests PRIVATE)
    mmltk_enable_native_runtime_rpath(mmltk_backend_ml_layers_tests)
    catch_discover_tests(
        mmltk_backend_ml_layers_tests
        TEST_PREFIX "backend::ml::layers::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
