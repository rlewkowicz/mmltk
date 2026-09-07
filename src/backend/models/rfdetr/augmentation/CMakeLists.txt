include(MmltkComponent)

add_library(mmltk_backend_models_rfdetr_augmentation STATIC)
add_library(
    mmltk::backend::models::rfdetr::augmentation
    ALIAS mmltk_backend_models_rfdetr_augmentation
)
mmltk_configure_component(
    mmltk_backend_models_rfdetr_augmentation PUBLIC_DECLARATIONS)

mmltk_register_public_headers(
    mmltk_backend_models_rfdetr_augmentation
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        annotation_support.h
        support_sampling.h
        augmentation_plan.h
        gpu_augment.h
        gpu_augment_cuda.h
        gpu_augment_types.h
        spatial_erasure.h)
mmltk_register_retained_module_providers(
    mmltk_backend_models_rfdetr_augmentation
    augmentation_metadata.cppm)
mmltk_register_retained_module_implementation_units(
    mmltk_backend_models_rfdetr_augmentation
    augmentation_metadata.cpp)
mmltk_register_ordinary_sources(
    mmltk_backend_models_rfdetr_augmentation
    annotation_support.cpp
    gpu_augment.cpp
    gpu_augment_cuda.cu)
mmltk_register_private_headers(
    mmltk_backend_models_rfdetr_augmentation
    detail/gpu_augment_cuda_launch.h
    detail/gpu_augment_plan_math.h
    detail/gpu_augmentation_donor_index.h)
target_include_directories(
    mmltk_backend_models_rfdetr_augmentation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    mmltk_backend_models_rfdetr_augmentation
    PUBLIC
        mmltk::backend::data
        mmltk::backend::models::rfdetr::contract
        CUDA::cudart
    PRIVATE
        mmltk::frameworks::gpu)
set_target_properties(mmltk_backend_models_rfdetr_augmentation PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    CUDA_SEPARABLE_COMPILATION ON)

function(mmltk_add_backend_models_rfdetr_augmentation_tests)
    add_executable(mmltk_backend_models_rfdetr_augmentation_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_models_rfdetr_augmentation_tests PRIVATE_DECLARATIONS)
    mmltk_register_private_headers(
        mmltk_backend_models_rfdetr_augmentation_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/gpu_augment_test_support.h
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/copy_paste_fixture.h)
    mmltk_register_retained_module_importers(
        mmltk_backend_models_rfdetr_augmentation_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/gpu_augment.test.cpp)
    target_link_libraries(
        mmltk_backend_models_rfdetr_augmentation_tests PRIVATE
        mmltk_acceptance_support
        mmltk::backend::models::rfdetr::augmentation
        CUDA::cudart)
    mmltk_target_cuda_architectures(
        mmltk_backend_models_rfdetr_augmentation_tests)
    catch_discover_tests(
        mmltk_backend_models_rfdetr_augmentation_tests
        TEST_PREFIX "backend::models::rfdetr::augmentation::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS)
endfunction()
