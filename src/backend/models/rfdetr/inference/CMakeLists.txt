include(MmltkComponent)

add_library(mmltk_backend_models_rfdetr_inference STATIC)
add_library(
    mmltk::backend::models::rfdetr::inference
    ALIAS mmltk_backend_models_rfdetr_inference
)
mmltk_configure_component(
    mmltk_backend_models_rfdetr_inference PUBLIC_DECLARATIONS)

foreach(_mmltk_inference_module IN ITEMS
        rfdetr_analysis_provider.cppm
        rfdetr_runtime_backend.cppm
        predict.cppm
        validation_support.cppm)
    mmltk_register_retained_module_providers(
        mmltk_backend_models_rfdetr_inference "${_mmltk_inference_module}")
endforeach()
mmltk_register_retained_module_implementation_units(
    mmltk_backend_models_rfdetr_inference
    rfdetr_analysis_provider.cpp
    rfdetr_runtime_backend.cpp
    predict.cpp
    validation_support.cpp
)
mmltk_register_public_headers(
    mmltk_backend_models_rfdetr_inference
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        prediction_delivery.h
        validate.h
        evaluation.h)
mmltk_register_retained_module_importers(
    mmltk_backend_models_rfdetr_inference
    validate.cpp
    evaluation.cpp)
mmltk_register_private_headers(mmltk_backend_models_rfdetr_inference inference_preprocessor.h prediction_capacity.h prediction_raw_preparation.h dataset_batch_lease.h)
target_include_directories(mmltk_backend_models_rfdetr_inference PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/backend/ml/cuda/detail
    ${CMAKE_SOURCE_DIR}/src/backend/models/rfdetr/core/detail
)
target_link_libraries(mmltk_backend_models_rfdetr_inference
    PUBLIC
        mmltk::backend::ml::runtime
        mmltk::backend::models::rfdetr::contract
        mmltk::backend::models::rfdetr::core
    PRIVATE
        mmltk::backend::models::rfdetr::export
        mmltk::common::io
        mmltk::backend::media::video
        mmltk::backend::data
        mmltk::backend::ml::cuda
        mmltk::common::concurrency
        mmltk::common::logging
        mmltk::common::math
        mmltk::common::types
        mmltk::frameworks::gpu
        mmltk::frameworks::reflection
        CUDA::cudart
        nlohmann_json
        stb
)
mmltk_enable_torch(mmltk_backend_models_rfdetr_inference PRIVATE)

function(mmltk_add_backend_models_rfdetr_inference_tests)
    add_executable(mmltk_backend_models_rfdetr_inference_tests
        EXCLUDE_FROM_ALL)
    mmltk_register_retained_module_importers(
        mmltk_backend_models_rfdetr_inference_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/eval_sample_writer.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/model_artifact_resolution.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/prediction_json.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/prediction_session.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/class_layout_roundtrip.test.cpp)
    mmltk_configure_component(
        mmltk_backend_models_rfdetr_inference_tests PRIVATE_DECLARATIONS)
    target_include_directories(
        mmltk_backend_models_rfdetr_inference_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src/backend/ml/cuda/detail
        ${CMAKE_SOURCE_DIR}/src/backend/ml/torch/detail)
    target_include_directories(
        mmltk_backend_models_rfdetr_inference_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src/backend/models/rfdetr/core/detail)
    target_link_libraries(
        mmltk_backend_models_rfdetr_inference_tests PRIVATE
        mmltk_test_support
        mmltk_backend_data_test_support
        mmltk::backend::ml::cuda
        mmltk::backend::ml::runtime
        mmltk::backend::models::rfdetr::inference
        mmltk::backend::models::rfdetr::export
        mmltk::common::io
        mmltk::backend::ml::torch_dependencies
        CUDA::cudart
        ONNX::onnx "${Protobuf_LIBRARY}"
        nlohmann_json)
    mmltk_target_cuda_architectures(
        mmltk_backend_models_rfdetr_inference_tests)
    mmltk_enable_native_runtime_rpath(
        mmltk_backend_models_rfdetr_inference_tests)
    catch_discover_tests(mmltk_backend_models_rfdetr_inference_tests
        TEST_PREFIX "backend::models::rfdetr::inference::"
        DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)

    add_executable(mmltk_rfdetr_profile_runner EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_rfdetr_profile_runner PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_rfdetr_profile_runner
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/rfdetr_profile_runner.cpp)
    target_link_libraries(mmltk_rfdetr_profile_runner PRIVATE
        mmltk::backend::models::rfdetr::inference
        mmltk::common::logging
        mmltk::common::system
        mmltk_test_support)
    mmltk_target_cuda_architectures(mmltk_rfdetr_profile_runner)
    mmltk_enable_native_runtime_rpath(mmltk_rfdetr_profile_runner)
endfunction()
