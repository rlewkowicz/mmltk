include(MmltkComponent)

add_library(mmltk_backend_imaging_explore STATIC)
add_library(
    mmltk::backend::imaging::explore
    ALIAS mmltk_backend_imaging_explore
)
mmltk_configure_component(
    mmltk_backend_imaging_explore PUBLIC_DECLARATIONS)

foreach(_mmltk_explore_partition IN ITEMS
        explore_render_core.cppm
        compiled_explore_store.cppm)
    mmltk_register_retained_module_providers(
        mmltk_backend_imaging_explore "${_mmltk_explore_partition}")
endforeach()
mmltk_register_retained_module_implementation_units(
    mmltk_backend_imaging_explore
    compiled_explore_store.cpp
    explore_render_core.cpp
)
mmltk_register_ordinary_sources(mmltk_backend_imaging_explore
    explore_render_core.cu explore_render_storage.cpp)
mmltk_register_public_headers(mmltk_backend_imaging_explore
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    FILES detail/explore_render_cuda_abi.h mask_sample.h
        explore_render_storage.h)
mmltk_register_private_headers(mmltk_backend_imaging_explore
    detail/explore_render_cuda_launch.h)
target_link_libraries(mmltk_backend_imaging_explore
    PUBLIC
        mmltk::backend::data
        mmltk::backend::imaging::primitives
        mmltk::backend::imaging::raster
        mmltk::backend::models::rfdetr::augmentation
        CUDA::cudart
    PRIVATE
        mmltk::common::math
        mmltk::backend::imaging::resample
        mmltk::common::concurrency
        mmltk::common::io
        mmltk::frameworks::gpu
)
set_target_properties(mmltk_backend_imaging_explore PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    CUDA_SEPARABLE_COMPILATION ON
)
mmltk_register_header_isolation(mmltk_backend_imaging_explore)

function(mmltk_add_backend_imaging_explore_tests)
    add_library(mmltk_backend_imaging_explore_test_support STATIC
        EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_imaging_explore_test_support PUBLIC_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_backend_imaging_explore_test_support
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/explore_dataset_fixture.cpp)
    mmltk_register_public_headers(mmltk_backend_imaging_explore_test_support
        BASE_DIRS ${CMAKE_SOURCE_DIR}
        FILES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/explore_dataset_fixture.h)
    target_link_libraries(
        mmltk_backend_imaging_explore_test_support PUBLIC
        mmltk_backend_models_rfdetr_augmentation_test_support
        mmltk::backend::data
        mmltk::backend::imaging::resample
        mmltk::backend::imaging::primitives
        mmltk_backend_data_test_support
        PRIVATE
        mmltk::backend::imaging::explore
    )

    add_executable(mmltk_backend_imaging_explore_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_imaging_explore_tests PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_backend_imaging_explore_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/explore_priority.test.cpp)
    target_link_libraries(mmltk_backend_imaging_explore_tests PRIVATE
        mmltk::backend::imaging::primitives
        mmltk_backend_models_rfdetr_augmentation_test_support
        mmltk_test_support
        mmltk::backend::imaging::explore
        mmltk::backend::imaging::raster
        mmltk::controller::explore_system
    )
    catch_discover_tests(
        mmltk_backend_imaging_explore_tests
        TEST_PREFIX "backend::imaging::explore::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
