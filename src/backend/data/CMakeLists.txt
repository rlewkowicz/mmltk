include(MmltkComponent)

find_package(simdjson 3.13.0 EXACT CONFIG REQUIRED
    PATHS /opt/simdjson/lib/cmake/simdjson
    NO_DEFAULT_PATH)
find_package(Arrow 25.0.1 EXACT CONFIG REQUIRED GLOBAL
    PATHS /opt/arrow/lib/cmake/Arrow NO_DEFAULT_PATH)
find_package(Parquet 25.0.1 EXACT CONFIG REQUIRED GLOBAL
    PATHS /opt/arrow/lib/cmake/Parquet NO_DEFAULT_PATH)
install(DIRECTORY /opt/arrow/share/licenses/arrow/
    DESTINATION share/mmltk/licenses/arrow)
find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED GLOBAL)

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
        benchmark/benchmark_dataset_compiler.h
        benchmark/benchmark_dataset_options.h
        compiled/compiled_image_stream.h
        compiled/compiled_dataset.h
        compiled/compiled_file_layout.h
        compiled_file.h
        compiled/compiled_format.h
        compiled/compiled_format_limits.h
        compiler/dataset_compile_phase.h
        compiler/dataset_compile_progress.h
        compiler/dataset_compiler.h
        loading/dataset_loader.h
        loading/data_loading_options.h
        compiled/compiled_file_utils.h
        benchmark/benchmark_hash.h
)
mmltk_register_ordinary_sources(mmltk_backend_data
    compiled/compiled_file_layout.cpp
    compiled/compiled_dataset.cpp
    benchmark/benchmark_annotations.cpp
    benchmark/coconut/coconut_annotations.cpp
    benchmark/coconut/coconut_physical.cpp
    benchmark/coconut/coconut_mask_recovery.cpp
    benchmark/coconut/coconut_parquet.cpp
    benchmark/coconut/coconut_catalog.cpp
    benchmark/benchmark_annotation_cache.cpp
    benchmark/benchmark_cache.cpp
    benchmark/benchmark_catalog.cpp
    benchmark/benchmark_compiler.cpp
    benchmark/benchmark_custom_recipe.cpp
    benchmark/coconut/coconut_recipe.cpp
    benchmark/benchmark_pipeline.cpp
    benchmark/benchmark_progress.cpp
    benchmark/benchmark_staging.cpp
    benchmark/benchmark_storage.cpp
    benchmark/open_images_acquisition.cpp
    benchmark/benchmark_curl.cpp
    benchmark/benchmark_download.cpp
    benchmark/benchmark_hash.cpp
    benchmark/benchmark_archive.cpp
    benchmark/benchmark_images.cpp
    benchmark/benchmark_image_input.cpp
    benchmark/benchmark_image_decoder.cpp
    benchmark/benchmark_sampling.cpp
    compiler/dataset_compiler_progress.cpp
    mask_rle_utils.cpp
    staging_file_cleanup.cpp)
mmltk_register_retained_module_importers(mmltk_backend_data
    compiled/compiled_image_stream.cpp
    benchmark/benchmark_writer.cpp
    compiler/dataset_compiler.cpp
    compiler/dataset_compiler_layout.cpp
    compiler/dataset_compiler_pixels.cpp
    compiler/dataset_compiler_scan_labels.cpp
    loading/dataset_loader.cpp)
mmltk_register_ordinary_sources(mmltk_backend_data
    writable_pixel_range.cpp)
mmltk_register_private_headers(mmltk_backend_data
    benchmark/detail/benchmark_annotation_cache.h
    benchmark/detail/benchmark_annotations.h
    benchmark/detail/benchmark_archive.h
    benchmark/coconut/detail/coconut_annotations.h
    benchmark/coconut/detail/coconut_physical.h
    benchmark/coconut/detail/coconut_mask_recovery.h
    benchmark/coconut/detail/coconut_catalog.h
    benchmark/coconut/detail/coconut_inventory.h
    benchmark/detail/benchmark_cache.h
    benchmark/detail/benchmark_catalog.h
    benchmark/detail/benchmark_compiler.h
    benchmark/detail/benchmark_recipe.h
    benchmark/detail/benchmark_progress.h
    benchmark/detail/benchmark_resources.h
    benchmark/detail/benchmark_staging.h
    benchmark/detail/benchmark_storage.h
    benchmark/detail/open_images_acquisition.h
    benchmark/detail/benchmark_curl.h
    benchmark/detail/benchmark_download.h
    benchmark/detail/benchmark_image_facts.h
    benchmark/detail/benchmark_images.h
    benchmark/detail/benchmark_image_input.h
    benchmark/detail/benchmark_image_decoder.h
    benchmark/detail/benchmark_sampling.h
    benchmark/detail/benchmark_pipeline.h
    benchmark/detail/benchmark_writer.h
    detail/worker_queue.h
    compiler/detail/dataset_compiler_internal.h
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
        mmltk::frameworks::serialization
        CURL::libcurl
        LibArchive::LibArchive
        rapidgzip::rapidgzip
        simdjson::simdjson
        Arrow::arrow_static
        Parquet::parquet_static
        stb
        CUDA::cuda_driver
        ${MMLTK_TURBOJPEG_LIBRARY}
)

mmltk_register_header_isolation(mmltk_backend_data)

mmltk_target_precompiled_headers(mmltk_backend_data std linux json)

function(mmltk_add_backend_data_tests)
    find_package(ZLIB REQUIRED)
    add_library(mmltk_backend_data_test_support STATIC EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_data_test_support PUBLIC_DECLARATIONS)
    mmltk_register_ordinary_sources(mmltk_backend_data_test_support
        ${CMAKE_SOURCE_DIR}/src/backend/data/tests/test_fixture.cpp)
    mmltk_register_public_headers(mmltk_backend_data_test_support
        BASE_DIRS ${CMAKE_SOURCE_DIR}
        FILES ${CMAKE_SOURCE_DIR}/src/backend/data/tests/test_fixture.h
              ${CMAKE_SOURCE_DIR}/src/backend/data/tests/benchmark_http_fixture.h)
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
        ${CMAKE_SOURCE_DIR}/src/backend/data/tests/coconut_dataset.test.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/data/catalog/tests/class_catalog.test.cpp
        ${CMAKE_SOURCE_DIR}/src/common/io/tests/file_digest.test.cpp)
    mmltk_configure_component(
        mmltk_backend_data_tests PRIVATE_DECLARATIONS)
    target_include_directories(mmltk_backend_data_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src/backend/data
        ${CMAKE_SOURCE_DIR}/src/backend/data/tests
    )
    target_compile_definitions(mmltk_backend_data_tests PRIVATE
        MMLTK_TEST_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
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
        Arrow::arrow_static
        Parquet::parquet_static
        LibArchive::LibArchive
        ZLIB::ZLIB
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
