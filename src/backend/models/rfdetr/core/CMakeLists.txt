include(MmltkComponent)

if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE
        "${CMAKE_SOURCE_DIR}/src/backend/models/rfdetr/training/rfdetr_checkpoint_bridge.py")
    if(NOT EXISTS "${MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE}")
        message(FATAL_ERROR
            "Missing RF-DETR Python checkpoint bridge: "
            "${MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE}")
    endif()
    set(MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE
        "${MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE}" CACHE INTERNAL
        "RF-DETR Python checkpoint bridge" FORCE)
endif()

function(mmltk_enable_python_checkpoint_loader target visibility)
    target_compile_definitions("${target}" "${visibility}"
        MMLTK_RFDETR_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
        MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE="${MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE}")
endfunction()

add_library(mmltk_backend_models_rfdetr_core STATIC)
add_library(
    mmltk::backend::models::rfdetr::core
    ALIAS mmltk_backend_models_rfdetr_core
)
mmltk_configure_component(
    mmltk_backend_models_rfdetr_core PUBLIC_DECLARATIONS)

mmltk_register_retained_module_providers(mmltk_backend_models_rfdetr_core dataset_limit_resolution.cppm)
mmltk_register_public_headers(
    mmltk_backend_models_rfdetr_core
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        artifact_publication.h
        class_artifact.h
        class_layout.h
        evaluation.h
        evaluator.h
        model_info.h
        model_state.h
        model.h
        model_state_load.h
        runtime.h
        detection_types.h
        detection_ops.h
        sample_output.h
        execution_precision.h
        postprocess.h)
mmltk_register_private_headers(mmltk_backend_models_rfdetr_core detail/class_artifact_files.h)
mmltk_register_header_isolation(mmltk_backend_models_rfdetr_core)
mmltk_register_retained_module_importers(mmltk_backend_models_rfdetr_core model.cpp runtime.cpp)
mmltk_register_ordinary_sources(
    mmltk_backend_models_rfdetr_core
    artifact_publication.cpp
    class_artifact.cpp
    class_layout.cpp
    python_checkpoint_bridge.cpp
    model_state.cpp)
mmltk_register_retained_module_importers(
    mmltk_backend_models_rfdetr_core
    evaluator.cpp
    detection_ops.cpp
    matcher_workspace.cpp
    draw.cpp
    mask_pack_cuda_wrapper.cpp
    modules.cpp
    postprocess.cpp
    training_supervision.cpp)
mmltk_register_ordinary_sources(
    mmltk_backend_models_rfdetr_core
    detection_geometry.cpp
    detection_types.cpp
    nested_tensor.cpp
    detr_matcher_cuda.cu
    scipy_rectangular_lsap.cpp
    training_mask_ops_cuda_wrapper.cpp
    training_mask_ops_cuda.cu)
mmltk_register_private_headers(
    mmltk_backend_models_rfdetr_core
    detail/lsap_scratch.h
    detail/matcher_workspace.h
    detail/runtime_workspace_fwd.h
    detail/traced_loss_cache.h
    detail/detr_matcher_cuda.h
    detail/detection_geometry.h
    detail/mask_pack_cuda.h
    detail/class_tensor_axes.h
    detail/modules_technical.h
    detail/scipy_rectangular_lsap.h
    detail/training_mask_ops_cuda.h
    detail/training_mask_ops_cuda_launch.h
    detail/training_supervision.h
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC 16 diagnoses a null dereference inside PyTorch's inlined IValue
    # dictionary hash even though the library guards the pointer invariant.
    # Keep the exception on this one Torch-heavy implementation unit.
    set_source_files_properties(model.cpp PROPERTIES
        COMPILE_OPTIONS "-Wno-null-dereference"
    )
endif()

target_include_directories(mmltk_backend_models_rfdetr_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}

    ${CMAKE_SOURCE_DIR}/third_party/spdmon/include
)
target_link_libraries(mmltk_backend_models_rfdetr_core
    PUBLIC
        mmltk::backend::data
        mmltk::backend::ml::cuda
        mmltk::backend::models::rfdetr::contract
        mmltk::common::concurrency
        mmltk::common::io
        mmltk::frameworks::gpu
    PRIVATE
        mmltk::backend::imaging::primitives
        mmltk::backend::imaging::raster
        mmltk::backend::models::rfdetr::augmentation
        mmltk::backend::ml::layers
        mmltk::backend::ml::ops
        mmltk::backend::ml::runtime
        mmltk::common::logging
        mmltk::common::math
        mmltk::common::system
        mmltk::common::types
        mmltk::frameworks::process
        mmltk::frameworks::reflection
        mmltk::frameworks::serialization
        CUDA::cudart
        nlohmann_json
        stb
)
mmltk_enable_torch(mmltk_backend_models_rfdetr_core PUBLIC)
if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
    mmltk_enable_python_checkpoint_loader(
        mmltk_backend_models_rfdetr_core PRIVATE)
endif()
target_compile_definitions(mmltk_backend_models_rfdetr_core PRIVATE
    MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER=$<IF:$<BOOL:${BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER}>,1,0>
)
set_target_properties(mmltk_backend_models_rfdetr_core PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    CUDA_SEPARABLE_COMPILATION ON
)

function(mmltk_add_backend_models_rfdetr_core_tests)
    if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
        add_library(
            mmltk_backend_models_rfdetr_checkpoint_test_support
            STATIC EXCLUDE_FROM_ALL)
        mmltk_configure_component(
            mmltk_backend_models_rfdetr_checkpoint_test_support
            PRIVATE_DECLARATIONS)
        mmltk_register_retained_module_importers(
            mmltk_backend_models_rfdetr_checkpoint_test_support
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/checkpoint_fixture_support.cpp
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/parity_fixture_support.cpp)
        mmltk_register_private_headers(
            mmltk_backend_models_rfdetr_checkpoint_test_support
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/asset_cache_support.h
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/checkpoint_fixture_support/checkpoint_fixture_support.h
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/checkpoint_fixture_support/parity_fixture_support.h)
        target_include_directories(
            mmltk_backend_models_rfdetr_checkpoint_test_support
            PUBLIC
                ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/checkpoint_fixture_support
            PRIVATE
                ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
)
        target_link_libraries(
            mmltk_backend_models_rfdetr_checkpoint_test_support
            PUBLIC
                mmltk::backend::models::rfdetr::core
            PRIVATE
                mmltk::backend::ml::torch_dependencies)
    endif()

    set(sources
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/evaluator.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/matcher_workspace.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/native_ops.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/class_layout.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/training_supervision.test.cpp)
    if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
        list(APPEND sources
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/native_integration.test.cpp)
    endif()
    add_executable(mmltk_backend_models_rfdetr_core_tests
        EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_models_rfdetr_core_tests PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_backend_models_rfdetr_core_tests ${sources})
    target_include_directories(mmltk_backend_models_rfdetr_core_tests PRIVATE
        ${CMAKE_SOURCE_DIR}

        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests)
    target_link_libraries(mmltk_backend_models_rfdetr_core_tests PRIVATE
        mmltk::backend::models::rfdetr::augmentation
        mmltk_frameworks_gpu_test_support
        mmltk_test_support
        mmltk::frameworks::serialization
        mmltk_backend_data_test_support
        nlohmann_json
        mmltk::backend::models::rfdetr::contract
        mmltk::backend::models::rfdetr::core
        mmltk::common::io
        mmltk::common::system
        mmltk::backend::ml::torch_dependencies
        CUDA::cudart)
    target_compile_definitions(mmltk_backend_models_rfdetr_core_tests PRIVATE
        MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER=$<IF:$<BOOL:${BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER}>,1,0>)
    if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
        add_dependencies(
            mmltk_backend_models_rfdetr_core_tests
            mmltk_entrypoints_tools_onnx_info)
        target_link_libraries(
            mmltk_backend_models_rfdetr_core_tests PRIVATE
            mmltk_entrypoints_cli_test_support
            mmltk_backend_models_rfdetr_checkpoint_test_support
            mmltk::backend::models::rfdetr::export)
    endif()
    mmltk_target_cuda_architectures(
        mmltk_backend_models_rfdetr_core_tests)
    mmltk_enable_native_runtime_rpath(
        mmltk_backend_models_rfdetr_core_tests)
    catch_discover_tests(mmltk_backend_models_rfdetr_core_tests
        TEST_PREFIX "backend::models::rfdetr::core::"
        DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)
endfunction()
