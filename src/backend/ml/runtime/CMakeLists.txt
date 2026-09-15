include(MmltkComponent)

set(MMLTK_ONNXRUNTIME_ROOT "/opt/onnxruntime" CACHE PATH
    "Owned ONNX Runtime installation prefix containing include/ and lib/" FORCE)

if(BUILD_RFDETR_NATIVE)
    find_path(_mmltk_onnxruntime_include
        NAMES onnxruntime_cxx_api.h
        PATHS "${MMLTK_ONNXRUNTIME_ROOT}/include"
        NO_DEFAULT_PATH)
    find_library(_mmltk_onnxruntime_library
        NAMES onnxruntime
        PATHS "${MMLTK_ONNXRUNTIME_ROOT}/lib"
        NO_DEFAULT_PATH)
    if(NOT _mmltk_onnxruntime_include OR NOT _mmltk_onnxruntime_library)
        message(FATAL_ERROR
            "backend.ml.runtime requires ONNX Runtime GPU headers and library")
    endif()

    find_path(_mmltk_tensorrt_include
        NAMES NvInfer.h
        PATHS ${MMLTK_NVIDIA_tensorrt_ROOT}/include
        NO_DEFAULT_PATH)
    find_library(_mmltk_tensorrt_nvinfer
        NAMES nvinfer
        PATHS ${MMLTK_NVIDIA_tensorrt_ROOT}/lib
        NO_DEFAULT_PATH)
    find_library(_mmltk_tensorrt_parser
        NAMES nvonnxparser
        PATHS ${MMLTK_NVIDIA_tensorrt_ROOT}/lib
        NO_DEFAULT_PATH)
    if(NOT _mmltk_tensorrt_include OR NOT _mmltk_tensorrt_nvinfer OR
       NOT _mmltk_tensorrt_parser)
        message(FATAL_ERROR
            "backend.ml.runtime requires TensorRT headers, nvinfer, and nvonnxparser")
    endif()

    add_library(mmltk_vendor_onnxruntime SHARED IMPORTED GLOBAL)
    set_target_properties(mmltk_vendor_onnxruntime PROPERTIES
        IMPORTED_LOCATION "${_mmltk_onnxruntime_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${_mmltk_onnxruntime_include}")
    add_library(mmltk::vendor::onnxruntime
        ALIAS mmltk_vendor_onnxruntime)

    add_library(mmltk_vendor_tensorrt_nvinfer SHARED IMPORTED GLOBAL)
    set_target_properties(mmltk_vendor_tensorrt_nvinfer PROPERTIES
        IMPORTED_LOCATION "${_mmltk_tensorrt_nvinfer}"
        INTERFACE_INCLUDE_DIRECTORIES "${_mmltk_tensorrt_include}")
    add_library(mmltk::vendor::tensorrt_nvinfer
        ALIAS mmltk_vendor_tensorrt_nvinfer)

    add_library(mmltk_vendor_tensorrt_parser SHARED IMPORTED GLOBAL)
    set_target_properties(mmltk_vendor_tensorrt_parser PROPERTIES
        IMPORTED_LOCATION "${_mmltk_tensorrt_parser}"
        INTERFACE_INCLUDE_DIRECTORIES "${_mmltk_tensorrt_include}")
    add_library(mmltk::vendor::tensorrt_parser
        ALIAS mmltk_vendor_tensorrt_parser)

    get_filename_component(
        _mmltk_onnxruntime_library_dir
        "${_mmltk_onnxruntime_library}" DIRECTORY)
    mmltk_register_runtime_library_directory(
        "${_mmltk_onnxruntime_library_dir}"
        INSTALL_DIRECTORY "${MMLTK_ONNXRUNTIME_ROOT}/lib")
    mmltk_register_runtime_library_directory(
        ${MMLTK_NVIDIA_tensorrt_ROOT}/lib INSTALL_DIRECTORY ${MMLTK_NVIDIA_tensorrt_ROOT}/lib)
endif()

add_library(mmltk_backend_ml_runtime STATIC)
add_library(mmltk::backend::ml::runtime ALIAS mmltk_backend_ml_runtime)
mmltk_configure_component(mmltk_backend_ml_runtime PUBLIC_DECLARATIONS)
target_link_libraries(mmltk_backend_ml_runtime PUBLIC mmltk::backend::data::catalog)

mmltk_register_public_headers(
    mmltk_backend_ml_runtime
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        analysis_provider.h
        onnx_environment.h
        tensorrt_runtime.h
        backend_factory.h)
target_include_directories(mmltk_backend_ml_runtime
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

if(BUILD_RFDETR_NATIVE)
    mmltk_register_ordinary_sources(
        mmltk_backend_ml_runtime
        backend_factory.cpp
        backend_onnx.cpp
        backend_tensorrt.cpp
        onnx_environment.cpp
        runtime_validation.cpp
        tensorrt_runtime.cpp)
    mmltk_register_private_headers(
        mmltk_backend_ml_runtime detail/tensorrt_engine_access.h)
    target_link_libraries(mmltk_backend_ml_runtime
        PRIVATE
            CUDA::cudart
            mmltk::frameworks::gpu
            mmltk::vendor::onnxruntime
            mmltk::vendor::tensorrt_nvinfer
            mmltk::vendor::tensorrt_parser
    )
endif()
