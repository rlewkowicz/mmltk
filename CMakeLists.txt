cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0138)
    cmake_policy(SET CMP0138 NEW)
endif()
project(mmltk LANGUAGES C CXX)

include(cmake/mmltk_project_configuration.cmake)
include(cmake/mmltk_helpers.cmake)
include(cmake/mmltk_pch.cmake)
include(cmake/mmltk_native_dependencies.cmake)

add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/third_party/stb)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/third_party/json/include)

add_library(cli11 INTERFACE)
target_include_directories(cli11 SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

add_library(mmltk_browser_api_contract INTERFACE)
target_include_directories(mmltk_browser_api_contract INTERFACE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_browser_api_contract INTERFACE
    nlohmann_json
)

set(MMLTK_AVIR_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/avir")
if(NOT EXISTS "${MMLTK_AVIR_SOURCE_DIR}/avir.h")
    message(FATAL_ERROR
        "Missing vendored AVIR checkout at ${MMLTK_AVIR_SOURCE_DIR}. "
        "CPU image resize is implemented via AVIR only; there is no fallback.")
endif()

add_library(avir INTERFACE)
target_include_directories(avir SYSTEM INTERFACE ${MMLTK_AVIR_SOURCE_DIR})

add_library(mmltk_build_options INTERFACE)
target_compile_definitions(mmltk_build_options INTERFACE
    MMLTK_ENABLE_PROFILING=$<IF:$<CONFIG:Dev>,1,0>
    MMLTK_BUILD_CONFIG=\"$<CONFIG>\"
)
foreach(MMLTK_PROJECT_CXX_WARNING_FLAG IN LISTS MMLTK_PROJECT_CXX_WARNING_FLAGS)
    target_compile_options(mmltk_build_options INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:${MMLTK_PROJECT_CXX_WARNING_FLAG}>
    )
endforeach()
foreach(MMLTK_PROJECT_CUDA_WARNING_FLAG IN LISTS MMLTK_PROJECT_CUDA_WARNING_FLAGS)
    target_compile_options(mmltk_build_options INTERFACE
        $<$<COMPILE_LANGUAGE:CUDA>:${MMLTK_PROJECT_CUDA_WARNING_FLAG}>
    )
endforeach()

add_library(mmltk_torch_warning_suppression_options INTERFACE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(mmltk_torch_warning_suppression_options INTERFACE
        $<$<AND:$<CONFIG:Dev>,$<COMPILE_LANGUAGE:CXX>>:-Wno-null-dereference>
    )
endif()

add_library(spdlog STATIC
    third_party/spdlog/src/async.cpp
    third_party/spdlog/src/cfg.cpp
    third_party/spdlog/src/color_sinks.cpp
    third_party/spdlog/src/file_sinks.cpp
    third_party/spdlog/src/spdlog.cpp
    third_party/spdlog/src/stdout_sinks.cpp
)
target_include_directories(spdlog PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
)
target_link_libraries(spdlog PUBLIC Threads::Threads)
target_compile_definitions(spdlog PUBLIC
    SPDLOG_COMPILED_LIB
    SPDLOG_USE_STD_FORMAT
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
)
mmltk_configure_target_pch(spdlog HEADER third_party/spdlog/src/pch.hpp)
add_library(spdlog::spdlog ALIAS spdlog)

add_library(mmltk_logging SHARED
    src/mmltk_logging.cpp
)
target_include_directories(mmltk_logging PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_logging PUBLIC
    mmltk_build_options
    spdlog
)

add_library(mmltk_core SHARED
    src/dataset/compile/dataset_compiler.cpp
    src/dataset/compile/dataset_compiler_scan_labels.cpp
    src/dataset/compile/dataset_compiler_pixels.cpp
    src/dataset/compile/dataset_compiler_layout.cpp
    src/image_resize.cpp
    src/dataset/loader/dataset_loader.cpp
    src/dataset/loader/dataset_loader_runtime.cpp
    src/dataset/loader/dataset_loader_pipeline.cpp
    src/cpu_affinity.cpp
    src/execution_policy.cpp
    src/cuda_stream_manager.cpp
    src/path_cleanup.cpp
    src/profile_utils.cpp
    src/worker_pool.cpp
    src/stb_image_write.cpp
)
target_include_directories(mmltk_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_core PUBLIC
    CUDA::cudart
    Threads::Threads
    stb
    nlohmann_json
    mmltk_build_options
    mmltk_logging
PRIVATE
    avir
)
mmltk_configure_target_pch(mmltk_core HEADER src/pch/core.hpp)

add_library(mmltk_model_api STATIC
    src/model/module.cpp
)
target_include_directories(mmltk_model_api PUBLIC
    ${CMAKE_SOURCE_DIR}/include
PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_model_api PUBLIC
    mmltk_core
    mmltk_build_options
)

add_library(mmltk_runtime STATIC
    src/runtime/async_runtime.cpp
)
target_include_directories(mmltk_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/include
PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_runtime PUBLIC
    mmltk_core
    mmltk_build_options
    mmltk_model_api
)

add_executable(mmltk_cli
    src/cli.cpp
)
target_link_libraries(mmltk_cli PRIVATE mmltk_core mmltk_build_options cli11 mmltk_logging)
target_include_directories(mmltk_cli PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/spdmon/include
)

include(cmake/mmltk_rfdetr_native.cmake)

if(BUILD_RFDETR_NATIVE)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES OR CMAKE_CUDA_ARCHITECTURES STREQUAL "")
        set(_mmltk_cuda_architectures "${MMLTK_CUDA_ARCHITECTURES}")
        if(_mmltk_cuda_architectures STREQUAL "")
            set(_mmltk_cuda_architectures "${MMLTK_DEFAULT_CUDA_ARCHITECTURES}")
        endif()
        set(CMAKE_CUDA_ARCHITECTURES "${_mmltk_cuda_architectures}" CACHE STRING
            "CUDA architectures" FORCE)
    endif()
    enable_language(CUDA)
endif()

target_compile_definitions(mmltk_core PRIVATE
    MMLTK_DATASET_COMPILER_CUDA=$<IF:$<BOOL:${BUILD_RFDETR_NATIVE}>,1,0>
)

add_library(mmltk_rfdetr_lsap STATIC
    src/rfdetr/utils/scipy_rectangular_lsap.cpp
)
target_include_directories(mmltk_rfdetr_lsap
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_rfdetr_lsap
    PUBLIC
        mmltk_build_options
)

add_library(mmltk_rfdetr_core STATIC
    src/rfdetr/core/module.cpp
    src/rfdetr/layers/model_config.cpp
    src/rfdetr/weight_catalog.cpp
)
target_include_directories(mmltk_rfdetr_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_rfdetr_core
    PUBLIC
        mmltk_build_options
        mmltk_model_api
)

mmltk_enable_native_runtime_rpath(mmltk_core)
mmltk_enable_native_runtime_rpath(mmltk_logging)

if(BUILD_RFDETR_NATIVE)
    mmltk_configure_rfdetr_native()
else()
    target_compile_definitions(mmltk_cli PRIVATE MMLTK_BUILD_RFDETR_NATIVE=0)
endif()

mmltk_enable_native_runtime_rpath(mmltk_cli)

include(cmake/mmltk_browser_app.cmake)
include(cmake/mmltk_firefox.cmake)
include(cmake/mmltk_gui.cmake)

set_target_properties(mmltk_core PROPERTIES OUTPUT_NAME "mmltk_core")
set_target_properties(mmltk_cli PROPERTIES OUTPUT_NAME "mmltk")

install(TARGETS mmltk_logging mmltk_core mmltk_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(BUILD_RFDETR_NATIVE)
    install(TARGETS mmltk_rfdetr_native
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(TARGETS mmltk_rfdetr_onnx_simplify_tool
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(TARGETS mmltk_rfdetr_onnx_info_tool
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER)
    install(FILES "${MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE}"
        DESTINATION "${MMLTK_INSTALL_PYTHONDIR}"
        RENAME "rfdetr_checkpoint_bridge.py"
    )
endif()

if(BUILD_RFDETR_NATIVE)
    mmltk_install_native_runtime_libraries("${MMLTK_ONNX_ROOT}")
endif()

include(cmake/mmltk_tests.cmake)
