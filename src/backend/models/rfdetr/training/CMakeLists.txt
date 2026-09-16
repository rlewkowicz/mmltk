include(MmltkComponent)

set(MMLTK_INSTALL_PYTHONDIR
    "${CMAKE_INSTALL_DATADIR}/mmltk/python" CACHE STRING
    "Install-relative directory for Python bridge scripts")
if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
    install(FILES "${MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE}"
        DESTINATION "${MMLTK_INSTALL_PYTHONDIR}"
        RENAME "rfdetr_checkpoint_bridge.py")
endif()

add_library(mmltk_backend_models_rfdetr_training STATIC)
add_library(
    mmltk::backend::models::rfdetr::training
    ALIAS mmltk_backend_models_rfdetr_training
)
mmltk_configure_component(
    mmltk_backend_models_rfdetr_training PUBLIC_DECLARATIONS)

mmltk_register_public_headers(
    mmltk_backend_models_rfdetr_training
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        distributed_train_launcher.h
        train.h
        telemetry_writer.h
        checkpoint.h
        train_recipe.h)
mmltk_register_private_headers(mmltk_backend_models_rfdetr_training detail/evaluation_runtime.h)
foreach(_mmltk_training_partition IN ITEMS
        checkpoint.cppm)
    mmltk_register_retained_module_providers(
        mmltk_backend_models_rfdetr_training "${_mmltk_training_partition}")
endforeach()
mmltk_register_retained_module_implementation_units(
    mmltk_backend_models_rfdetr_training
    checkpoint.cpp
)
mmltk_register_retained_module_importers(
    mmltk_backend_models_rfdetr_training
    checkpoint_io.cpp
    evaluation_run_owner.cpp
    evaluation_runtime.cpp
    gpu_augment.cpp
    native_optimizer.cpp
    target_builder.cpp
    train.cpp)
mmltk_register_ordinary_sources(
    mmltk_backend_models_rfdetr_training
    distributed_train_launcher.cpp
    training_ops.cpp
    training_continuation.cpp
    model_ema.cpp
    telemetry_writer.cpp)
mmltk_register_private_headers(
    mmltk_backend_models_rfdetr_training
    detail/target_builder_private.h
    detail/gpu_augment_private.h
    detail/native_optimizer_private.h
    detail/training_scalar_packet.h
    detail/training_ops_private.h
    detail/training_continuation.h
    detail/model_ema.h
    detail/checkpoint_private.h)
mmltk_register_header_isolation(mmltk_backend_models_rfdetr_training)
target_include_directories(mmltk_backend_models_rfdetr_training PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/backend/ml/cuda/detail
    ${CMAKE_SOURCE_DIR}/src/backend/models/rfdetr/core/detail
    ${CMAKE_SOURCE_DIR}/third_party/spdmon/include
)
target_link_libraries(mmltk_backend_models_rfdetr_training
    PUBLIC
        mmltk::backend::data
        mmltk::backend::models::rfdetr::contract
        mmltk::backend::models::rfdetr::core
    PRIVATE
        mmltk::backend::models::rfdetr::augmentation
        mmltk::backend::ml::cuda
        mmltk::backend::ml::layers
        mmltk::backend::ml::runtime
        mmltk::common::concurrency
        mmltk::common::io
        mmltk::common::logging
        mmltk::common::math
        mmltk::common::types
        mmltk::frameworks::gpu
        mmltk::frameworks::serialization
        CUDA::cudart
        nlohmann_json
)
mmltk_enable_torch(mmltk_backend_models_rfdetr_training PRIVATE)

function(mmltk_add_backend_models_rfdetr_training_tests)
    set(sources
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/distributed_placement.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/training_supervision.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/weight_artifact_resolution.test.cpp)
    if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
        list(APPEND sources
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/checkpoint.test.cpp
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/checkpoint_parity.test.cpp)
    endif()
    add_executable(mmltk_backend_models_rfdetr_training_tests
        EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_models_rfdetr_training_tests PRIVATE_DECLARATIONS)
    mmltk_register_private_headers(
        mmltk_backend_models_rfdetr_training_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/model_state_fixture.h
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/training_continuation_fixture.h)
    mmltk_register_retained_module_importers(
        mmltk_backend_models_rfdetr_training_tests ${sources})
    mmltk_register_ordinary_sources(
        mmltk_backend_models_rfdetr_training_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/evaluation_runtime.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/telemetry_writer.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/training_continuation.test.cpp)
    target_include_directories(
        mmltk_backend_models_rfdetr_training_tests PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src/backend/ml/cuda/detail
        ${CMAKE_SOURCE_DIR}/src/backend/ml/torch/detail
        ${CMAKE_SOURCE_DIR}/src/backend/models/rfdetr/core/detail
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests)
    target_link_libraries(
        mmltk_backend_models_rfdetr_training_tests PRIVATE
        mmltk_backend_models_rfdetr_augmentation_test_support
        mmltk::backend::models::rfdetr::export
        mmltk_test_support
        mmltk::backend::models::rfdetr::augmentation
        mmltk::backend::models::rfdetr::contract
        mmltk::backend::models::rfdetr::training
        mmltk::backend::ml::torch_dependencies
        mmltk::common::system
        mmltk_backend_data_test_support
        CUDA::cudart)
    if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
        target_link_libraries(
            mmltk_backend_models_rfdetr_training_tests PRIVATE
            mmltk_backend_models_rfdetr_checkpoint_test_support)
    endif()
    target_compile_definitions(
        mmltk_backend_models_rfdetr_training_tests PRIVATE
        MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER=$<IF:$<BOOL:${BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER}>,1,0>)
    mmltk_target_cuda_architectures(
        mmltk_backend_models_rfdetr_training_tests)
    mmltk_enable_native_runtime_rpath(
        mmltk_backend_models_rfdetr_training_tests)
    catch_discover_tests(mmltk_backend_models_rfdetr_training_tests
        TEST_PREFIX "backend::models::rfdetr::training::"
        DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)

    add_executable(mmltk_rfdetr_train_profile_runner EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_rfdetr_train_profile_runner PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_rfdetr_train_profile_runner
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/rfdetr_train_profile_runner.cpp)
    target_include_directories(mmltk_rfdetr_train_profile_runner PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src/backend/ml/torch/detail)
    target_link_libraries(mmltk_rfdetr_train_profile_runner PRIVATE
        mmltk::backend::data
        mmltk::backend::models::rfdetr::training
        mmltk::common::logging
        mmltk::common::system
        mmltk::backend::ml::torch_dependencies
        mmltk_backend_data_test_support
        mmltk_test_support)
    mmltk_target_cuda_architectures(mmltk_rfdetr_train_profile_runner)
    mmltk_enable_native_runtime_rpath(mmltk_rfdetr_train_profile_runner)
endfunction()
