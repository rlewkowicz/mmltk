include(MmltkComponent)

add_library(mmltk_controller_data_compute_systems STATIC)
add_library(mmltk::controller::data_compute_systems ALIAS mmltk_controller_data_compute_systems)
mmltk_configure_component(mmltk_controller_data_compute_systems PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_data_compute_systems
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        compute_intent_materializer.h
        compute_runtime.h
        ../validate/validation_system.h
        ../validate/validation_types.h
        ../validate/validation_runtime.h
        ../export/export_system.h
        predict_system.h
        dataset_system.h
        local_run.h
        model_system.h
        system_events.h
        ../../services/file_dialog_system.h
        ../../services/settings_system.h
        ../train/training_system.h)
mmltk_register_ordinary_sources(
    mmltk_controller_data_compute_systems
    compute_intent_materializer.cpp
    compute_runtime.cpp
    detail/cuda_runtime_resources.cpp
    dataset_system.cpp
    local_run.cpp
    ../../services/file_dialog_system.cpp
    ../../services/settings_system.cpp
    ../validate/validation_system.cpp
    ../validate/detail/validation_samples.cpp
    ../train/training_system.cpp)
mmltk_register_retained_module_importers(
    mmltk_controller_data_compute_systems
    ../export/export_system.cpp
    model_system.cpp
    predict_system.cpp
    detail/prediction_preview.cpp)
mmltk_register_private_headers(
    mmltk_controller_data_compute_systems detail/cuda_runtime_resources.h
        detail/predict_revision.h
        detail/prediction_playback.h
        detail/prediction_preview.h
        ../validate/detail/validation_samples.h)
target_include_directories(mmltk_controller_data_compute_systems PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(mmltk_controller_data_compute_systems
    PUBLIC
        mmltk::backend::models::rfdetr::contract
        mmltk::common::types
        mmltk::common::system
        mmltk::controller::application_boundary
        mmltk::controller::contracts
        mmltk::controller::direct_services
        mmltk::controller::visual_contract
        mmltk::frameworks::gpu
        mmltk::frameworks::reflection
    PRIVATE
        mmltk::backend::models::rfdetr::inference
        mmltk::backend::models::rfdetr::training
        mmltk::controller::visual_runtime
        mmltk::backend::imaging::raster
        mmltk::backend::imaging::explore
        mmltk::backend::ml::runtime
        mmltk::backend::models::rfdetr::export)
mmltk_link_declarations(mmltk_controller_data_compute_systems PUBLIC
    mmltk::controller::visual_runtime
    mmltk::backend::models::rfdetr::inference)
mmltk_register_header_isolation(mmltk_controller_data_compute_systems)

function(mmltk_add_controller_subsystems_system_tests)
    find_package(Vulkan REQUIRED)
    add_executable(mmltk_controller_data_compute_systems_tests EXCLUDE_FROM_ALL)
    mmltk_register_ordinary_sources(
        mmltk_controller_data_compute_systems_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/application_data_compute_systems.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../validate/tests/validation_system.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/application_compute_services.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/prediction_playback.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/prediction_test_support.cpp)
    mmltk_register_private_headers(mmltk_controller_data_compute_systems_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/prediction_test_support.h
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/application_data_test_support.h)
    mmltk_configure_component(mmltk_controller_data_compute_systems_tests PRIVATE_DECLARATIONS)
    target_include_directories(mmltk_controller_data_compute_systems_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(mmltk_controller_data_compute_systems_tests PRIVATE
        mmltk_common_system_test_support
        mmltk_frameworks_gpu_test_support
        mmltk_test_support
        mmltk::backend::data
        mmltk::backend::models::rfdetr::contract
        mmltk::common::io
        mmltk::controller::data_compute_systems
        mmltk::controller::direct_services
        mmltk::controller::presentation_system
        mmltk_backend_data_test_support
        CUDA::cuda_driver
        mmltk_frameworks_gpu_hardware_test_support)
    catch_discover_tests(
        mmltk_controller_data_compute_systems_tests
        TEST_PREFIX "controller::data_compute_systems::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS)
endfunction()
