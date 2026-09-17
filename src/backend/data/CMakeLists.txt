include(MmltkComponent)

find_package(simdjson 3.13.0 EXACT CONFIG REQUIRED
    PATHS /opt/simdjson/lib/cmake/simdjson
    NO_DEFAULT_PATH)
find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED)

find_path(MMLTK_TURBOJPEG_INCLUDE_DIR
    NAMES turbojpeg.h
    HINTS /usr/include /usr/local/include)
find_library(MMLTK_TURBOJPEG_LIBRARY
    NAMES turbojpeg
    HINTS
        /usr/lib/x86_64-linux-gnu
        /usr/local/lib
        /usr/local/lib64
        /usr/lib64
        /usr/lib)
if(NOT MMLTK_TURBOJPEG_INCLUDE_DIR OR NOT MMLTK_TURBOJPEG_LIBRARY)
    message(FATAL_ERROR
        "backend.data requires libjpeg-turbo headers and library")
endif()
include(CheckSymbolExists)
set(CMAKE_REQUIRED_INCLUDES "${MMLTK_TURBOJPEG_INCLUDE_DIR}")
set(CMAKE_REQUIRED_LIBRARIES "${MMLTK_TURBOJPEG_LIBRARY}")
check_symbol_exists(
    tjDecompressHeader3 "turbojpeg.h"
    MMLTK_HAVE_TURBOJPEG_DECOMPRESS_HEADER3)
check_symbol_exists(
    tjDecompress2 "turbojpeg.h"
    MMLTK_HAVE_TURBOJPEG_DECOMPRESS2)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT MMLTK_HAVE_TURBOJPEG_DECOMPRESS_HEADER3 OR
   NOT MMLTK_HAVE_TURBOJPEG_DECOMPRESS2)
    message(FATAL_ERROR
        "backend.data requires the TurboJPEG 2.x decompression API")
endif()

add_library(mmltk_backend_data STATIC)
add_library(mmltk::backend::data ALIAS mmltk_backend_data)
mmltk_configure_component(mmltk_backend_data PUBLIC_DECLARATIONS)

mmltk_register_public_headers(
    mmltk_backend_data
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        benchmark_dataset_compiler.h
        compiled_image_stream.h
        compiled_dataset.h
        compiled_format.h
        compiled_format_limits.h
        dataset_compile_phase.h
        dataset_compiler.h
        dataset_loader.h
        data_loading_options.h
        compiled_file_utils.h
        benchmark_hash.h
)
mmltk_register_ordinary_sources(mmltk_backend_data
    compiled_dataset.cpp
    benchmark_annotations.cpp
    benchmark_cache.cpp
    benchmark_catalog.cpp
    benchmark_compiler.cpp
    benchmark_progress.cpp
    benchmark_storage.cpp
    open_images_acquisition.cpp
    benchmark_curl.cpp
    benchmark_download.cpp
    benchmark_hash.cpp
    benchmark_images.cpp
    benchmark_jpeg.cpp
    benchmark_sampling.cpp
    dataset_compiler_progress.cpp
    mask_rle_utils.cpp
    staging_file_cleanup.cpp)
mmltk_register_retained_module_importers(mmltk_backend_data
    compiled_image_stream.cpp
    benchmark_writer.cpp
    dataset_compiler.cpp
    dataset_compiler_layout.cpp
    dataset_compiler_pixels.cpp
    dataset_compiler_scan_labels.cpp
    dataset_loader.cpp)
mmltk_register_ordinary_sources(mmltk_backend_data
    writable_pixel_range.cpp)
mmltk_register_private_headers(mmltk_backend_data
    detail/benchmark_annotations.h
    detail/benchmark_cache.h
    detail/benchmark_catalog.h
    detail/benchmark_compiler.h
    detail/benchmark_progress.h
    detail/benchmark_storage.h
    detail/open_images_acquisition.h
    detail/benchmark_curl.h
    detail/benchmark_download.h
    detail/benchmark_images.h
    detail/benchmark_jpeg.h
    detail/benchmark_sampling.h
    detail/benchmark_writer.h
    detail/worker_queue.h
    detail/dataset_compiler_internal.h
    detail/mask_rle_utils.h
    detail/staging_file_cleanup.h
    detail/writable_pixel_range.h)

target_include_directories(mmltk_backend_data
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/detail
        ${MMLTK_TURBOJPEG_INCLUDE_DIR}
)
target_include_directories(mmltk_backend_data SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/json/include
)
target_link_libraries(mmltk_backend_data
    PUBLIC
        mmltk::backend::imaging::resample
        mmltk::backend::data::catalog
        mmltk::common::io
        mmltk::common::math
        mmltk::frameworks::reflection
        Threads::Threads
        CUDA::cudart
    PRIVATE
        mmltk::common::concurrency
        mmltk::common::logging
        mmltk::common::system
        mmltk::common::types
        mmltk::frameworks::gpu
        mmltk::frameworks::process
        CURL::libcurl
        LibArchive::LibArchive
        rapidgzip::rapidgzip
        simdjson::simdjson
        stb
        CUDA::cuda_driver
        ${MMLTK_TURBOJPEG_LIBRARY}
)

mmltk_target_precompiled_headers(mmltk_backend_data std json)

function(mmltk_add_backend_data_tests)
    add_library(mmltk_backend_data_test_support STATIC EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_data_test_support PUBLIC_DECLARATIONS)
    mmltk_register_ordinary_sources(mmltk_backend_data_test_support
        ${CMAKE_SOURCE_DIR}/src/backend/data/tests/test_fixture.cpp)
    mmltk_register_public_headers(mmltk_backend_data_test_support
        BASE_DIRS ${CMAKE_SOURCE_DIR}
        FILES ${CMAKE_SOURCE_DIR}/src/backend/data/tests/test_fixture.h)
    target_link_libraries(mmltk_backend_data_test_support PUBLIC
        mmltk_test_support
        mmltk::backend::data
        stb
    )

    add_executable(mmltk_backend_data_tests EXCLUDE_FROM_ALL)
    mmltk_register_retained_module_importers(mmltk_backend_data_tests
        ${CMAKE_SOURCE_DIR}/src/backend/data/tests/compile_progress.test.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/data/tests/roundtrip.test.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/data/tests/benchmark_dataset.test.cpp)
    mmltk_register_ordinary_sources(mmltk_backend_data_tests
        ${CMAKE_SOURCE_DIR}/src/backend/data/catalog/tests/class_catalog.test.cpp
        ${CMAKE_SOURCE_DIR}/src/common/io/tests/file_digest.test.cpp)
    mmltk_configure_component(
        mmltk_backend_data_tests PRIVATE_DECLARATIONS)
    target_include_directories(mmltk_backend_data_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src/backend/data
        ${CMAKE_SOURCE_DIR}/src/backend/data/tests
    )
    target_link_libraries(mmltk_backend_data_tests PRIVATE
        mmltk_backend_imaging_resample_test_support
        mmltk::backend::imaging::resample
        mmltk_test_support
        mmltk::backend::data
        mmltk::common::concurrency
        mmltk::common::io
        mmltk::common::logging
        mmltk::common::system
        mmltk::frameworks::gpu
        mmltk_backend_data_test_support
        CUDA::cudart
        CUDA::cuda_driver
        nlohmann_json
        stb
    )
    catch_discover_tests(
        mmltk_backend_data_tests
        TEST_PREFIX "backend::data::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )

    add_executable(mmltk_backend_data_profile_runner EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_data_profile_runner PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_backend_data_profile_runner
        ${CMAKE_SOURCE_DIR}/src/backend/data/tests/profile_runner.cpp)
    target_include_directories(mmltk_backend_data_profile_runner PRIVATE
        ${CMAKE_SOURCE_DIR}/src/backend/data/tests
    )
    target_link_libraries(mmltk_backend_data_profile_runner PRIVATE
        mmltk::backend::data
        mmltk::common::logging
        mmltk::common::system
        mmltk::frameworks::gpu
        mmltk_backend_data_test_support
        CUDA::cudart
    )
endfunction()
