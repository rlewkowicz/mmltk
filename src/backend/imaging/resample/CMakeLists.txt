include(MmltkComponent)
add_library(mmltk_backend_imaging_resample STATIC)
add_library(mmltk::backend::imaging::resample ALIAS mmltk_backend_imaging_resample)
mmltk_configure_component(mmltk_backend_imaging_resample PUBLIC_DECLARATIONS)
mmltk_register_public_headers(mmltk_backend_imaging_resample
    BASE_DIRS ${CMAKE_SOURCE_DIR} FILES image_resize.h image_resize_cuda.h)
mmltk_register_private_headers(mmltk_backend_imaging_resample
    detail/perceptual_downscale.h detail/perceptual_downscale_views.h
    detail/perceptual_downscale_math.h detail/perceptual_downscale_completion.h
    detail/perceptual_downscale_accumulation.cuh)
mmltk_register_ordinary_sources(mmltk_backend_imaging_resample
    image_resize.cpp detail/perceptual_downscale.cpp detail/perceptual_downscale_views.cpp)
target_link_libraries(mmltk_backend_imaging_resample
    PUBLIC CUDA::cudart
    PRIVATE mmltk::common::math avir mmltk::frameworks::gpu CUDA::cuda_driver)
mmltk_link_declarations(mmltk_backend_imaging_resample PUBLIC
    mmltk::frameworks::reflection)
if(BUILD_RFDETR_NATIVE)
    mmltk_register_ordinary_sources(mmltk_backend_imaging_resample image_resize_cuda.cu)
    set_target_properties(mmltk_backend_imaging_resample PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        CUDA_SEPARABLE_COMPILATION ON CUDA_RESOLVE_DEVICE_SYMBOLS ON)
endif()
add_library(mmltk_backend_imaging_resample_test_support INTERFACE)
mmltk_register_public_headers(mmltk_backend_imaging_resample_test_support
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES tests/perceptual_downscale_reference.h)
mmltk_link_declarations(mmltk_backend_imaging_resample_test_support INTERFACE
    mmltk::backend::imaging::resample)

function(mmltk_add_backend_imaging_resample_tests)
    add_executable(mmltk_backend_imaging_resample_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(mmltk_backend_imaging_resample_tests PRIVATE_DECLARATIONS)
    mmltk_register_ordinary_sources(mmltk_backend_imaging_resample_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/image_resize.test.cpp)
    target_link_libraries(mmltk_backend_imaging_resample_tests PRIVATE
        mmltk_backend_imaging_resample_test_support
        mmltk_test_support mmltk::backend::imaging::resample mmltk::frameworks::gpu)
    if(BUILD_RFDETR_NATIVE)
        mmltk_register_ordinary_sources(mmltk_backend_imaging_resample_tests
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/image_resize_cuda.test.cpp
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/perceptual_downscale_traversal.cu)
        mmltk_register_private_headers(mmltk_backend_imaging_resample_tests
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/perceptual_downscale_traversal.h)
        set_target_properties(mmltk_backend_imaging_resample_tests PROPERTIES
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
            CUDA_SEPARABLE_COMPILATION ON CUDA_RESOLVE_DEVICE_SYMBOLS ON)
        target_link_libraries(mmltk_backend_imaging_resample_tests PRIVATE
            mmltk_frameworks_gpu_hardware_test_support)
    endif()
    catch_discover_tests(mmltk_backend_imaging_resample_tests
        TEST_PREFIX "backend::imaging::resample::" DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)
endfunction()
