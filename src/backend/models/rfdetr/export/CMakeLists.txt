include(MmltkComponent)

set(MMLTK_ONNX_ROOT "" CACHE PATH
    "ONNX installation prefix containing lib/cmake/ONNX")
if(NOT MMLTK_ONNX_ROOT OR
   NOT EXISTS "${MMLTK_ONNX_ROOT}/lib/cmake/ONNX/ONNXConfig.cmake")
    message(FATAL_ERROR
        "backend.models.rfdetr.export requires MMLTK_ONNX_ROOT to contain "
        "lib/cmake/ONNX/ONNXConfig.cmake")
endif()

find_path(MMLTK_SYSTEM_PROTOBUF_INCLUDE_DIR
    NAMES google/protobuf/message.h
    HINTS /usr/include /usr/local/include
    NO_DEFAULT_PATH)
find_library(MMLTK_SYSTEM_PROTOBUF_LIBRARY
    NAMES protobuf libprotobuf
    HINTS
        /usr/lib/x86_64-linux-gnu
        /usr/local/lib
        /usr/local/lib64
        /usr/lib64
        /usr/lib
    NO_DEFAULT_PATH)
if(MMLTK_SYSTEM_PROTOBUF_INCLUDE_DIR)
    set(MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR
        "${CMAKE_BINARY_DIR}/mmltk_system_protobuf")
    file(MAKE_DIRECTORY "${MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR}")
    file(REMOVE_RECURSE "${MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR}/google")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E create_symlink
            "${MMLTK_SYSTEM_PROTOBUF_INCLUDE_DIR}/google"
            "${MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR}/google"
        RESULT_VARIABLE _mmltk_protobuf_symlink_result)
    if(NOT _mmltk_protobuf_symlink_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to create the system protobuf include redirect")
    endif()
    set(Protobuf_INCLUDE_DIR "${MMLTK_SYSTEM_PROTOBUF_INCLUDE_DIR}"
        CACHE PATH "Pinned system protobuf headers" FORCE)
endif()
if(MMLTK_SYSTEM_PROTOBUF_LIBRARY)
    set(Protobuf_LIBRARY "${MMLTK_SYSTEM_PROTOBUF_LIBRARY}"
        CACHE FILEPATH "Pinned system protobuf library" FORCE)
endif()
set(Protobuf_PROTOC_EXECUTABLE "/usr/bin/protoc" CACHE FILEPATH
    "Pinned protoc executable" FORCE)
find_package(Protobuf REQUIRED)
if(MMLTK_SYSTEM_PROTOBUF_LIBRARY)
    set(MMLTK_PROTOBUF_LIBRARY_TARGET
        "${MMLTK_SYSTEM_PROTOBUF_LIBRARY}")
elseif(Protobuf_LIBRARY)
    set(MMLTK_PROTOBUF_LIBRARY_TARGET "${Protobuf_LIBRARY}")
elseif(Protobuf_LIBRARIES)
    set(MMLTK_PROTOBUF_LIBRARY_TARGET "${Protobuf_LIBRARIES}")
elseif(TARGET protobuf::libprotobuf)
    set(MMLTK_PROTOBUF_LIBRARY_TARGET protobuf::libprotobuf)
else()
    message(FATAL_ERROR
        "backend.models.rfdetr.export could not resolve protobuf")
endif()

list(PREPEND CMAKE_PREFIX_PATH "${MMLTK_ONNX_ROOT}")
find_package(ONNX CONFIG REQUIRED)
foreach(_mmltk_onnx_location_property IN ITEMS
        IMPORTED_LOCATION_RELEASE
        IMPORTED_LOCATION_RELWITHDEBINFO
        IMPORTED_LOCATION_DEBUG
        IMPORTED_LOCATION)
    get_target_property(
        _mmltk_onnx_imported_location
        ONNX::onnx
        ${_mmltk_onnx_location_property})
    if(_mmltk_onnx_imported_location AND
       EXISTS "${_mmltk_onnx_imported_location}")
        get_filename_component(
            _mmltk_onnx_library_dir
            "${_mmltk_onnx_imported_location}" DIRECTORY)
        mmltk_register_runtime_library_directory(
            "${_mmltk_onnx_library_dir}")
        break()
    endif()
endforeach()

function(mmltk_prefer_system_protobuf_headers target visibility)
    if(MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR)
        target_include_directories("${target}" SYSTEM BEFORE "${visibility}"
            "${MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR}")
    elseif(Protobuf_INCLUDE_DIRS)
        target_include_directories("${target}" SYSTEM BEFORE "${visibility}"
            ${Protobuf_INCLUDE_DIRS})
    endif()
endfunction()

add_library(mmltk_backend_models_rfdetr_export STATIC)
add_library(
    mmltk::backend::models::rfdetr::export
    ALIAS mmltk_backend_models_rfdetr_export
)
mmltk_configure_component(
    mmltk_backend_models_rfdetr_export PUBLIC_DECLARATIONS)

mmltk_register_retained_module_providers(
    mmltk_backend_models_rfdetr_export export.cppm)
mmltk_register_retained_module_providers(
    mmltk_backend_models_rfdetr_export onnx_lowering.cppm)
mmltk_register_retained_module_providers(
    mmltk_backend_models_rfdetr_export onnx_model_info.cppm)
mmltk_register_retained_module_providers(
    mmltk_backend_models_rfdetr_export onnx_simplify.cppm)
mmltk_register_retained_module_implementation_units(
    mmltk_backend_models_rfdetr_export
    model_export.cpp
    onnx_lowering.cpp
    onnx_model_io.cpp
)
target_include_directories(mmltk_backend_models_rfdetr_export
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/backend/models/rfdetr/core/detail
)
target_include_directories(mmltk_backend_models_rfdetr_export
    SYSTEM PRIVATE
        $<TARGET_PROPERTY:ONNX::onnx,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(mmltk_backend_models_rfdetr_export
    PUBLIC
        mmltk::backend::ml::runtime
        mmltk::backend::models::rfdetr::contract
        mmltk::backend::models::rfdetr::core
    PRIVATE
        mmltk::backend::ml::cuda
        mmltk::common::io
        mmltk::common::logging
        mmltk::common::types
        mmltk::frameworks::serialization
        $<LINK_ONLY:ONNX::onnx>
        ${MMLTK_PROTOBUF_LIBRARY_TARGET}
)
mmltk_prefer_system_protobuf_headers(
    mmltk_backend_models_rfdetr_export PRIVATE)
mmltk_enable_torch(mmltk_backend_models_rfdetr_export PRIVATE)
target_compile_definitions(mmltk_backend_models_rfdetr_export PRIVATE
    ONNX_ML=1
)

function(mmltk_add_backend_models_rfdetr_export_tests)
    add_executable(mmltk_backend_models_rfdetr_export_tests
        EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_models_rfdetr_export_tests PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_backend_models_rfdetr_export_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/onnx_lowering.test.cpp)
    target_include_directories(
        mmltk_backend_models_rfdetr_export_tests PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
        ${CMAKE_SOURCE_DIR}/src/backend/ml/torch/detail)
    target_link_libraries(mmltk_backend_models_rfdetr_export_tests PRIVATE
        mmltk_acceptance_support
        mmltk::backend::models::rfdetr::export
        mmltk::backend::ml::torch_dependencies)
    mmltk_prefer_system_protobuf_headers(
        mmltk_backend_models_rfdetr_export_tests PRIVATE)
    mmltk_enable_native_runtime_rpath(
        mmltk_backend_models_rfdetr_export_tests)
    catch_discover_tests(mmltk_backend_models_rfdetr_export_tests
        TEST_PREFIX "backend::models::rfdetr::export::"
        DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)
endfunction()
