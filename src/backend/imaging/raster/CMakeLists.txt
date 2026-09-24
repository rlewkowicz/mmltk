include(MmltkComponent)

add_library(mmltk_backend_imaging_raster STATIC)
add_library(
    mmltk::backend::imaging::raster
    ALIAS mmltk_backend_imaging_raster
)
mmltk_configure_component(
    mmltk_backend_imaging_raster PUBLIC_DECLARATIONS)

mmltk_register_retained_module_providers(
    mmltk_backend_imaging_raster raster.cppm)
mmltk_register_retained_module_implementation_units(
    mmltk_backend_imaging_raster raster.cpp)
mmltk_register_ordinary_sources(
    mmltk_backend_imaging_raster
    caption_raster.cpp
    rendered_image_writer.cpp
    caption_raster.cu
    image_operations.cpp
    image_operations.cu
    chw_image.cu
    raster_cuda.cu
    mask_pack_cuda.cu)
mmltk_register_public_headers(mmltk_backend_imaging_raster BASE_DIRS ${CMAKE_SOURCE_DIR} FILES native_caption_metrics.h rendered_image_writer.h caption_raster.h chw_image.h image_operations.h image_containment.h)
mmltk_register_private_headers(
    mmltk_backend_imaging_raster
    detail/checked_png.h
    detail/caption_raster_cuda.h
    detail/mask_pack_cuda_abi.h
    detail/raster_math.cuh
    detail/image_operations_cuda.h
    detail/raster_cuda_abi.h
)
target_include_directories(mmltk_backend_imaging_raster PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(mmltk_backend_imaging_raster
    PUBLIC
        mmltk::frameworks::gpu
        CUDA::cudart
    PRIVATE
        stb
        mmltk::common::concurrency
        mmltk::backend::imaging::primitives
)
set_target_properties(mmltk_backend_imaging_raster PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
)

mmltk_register_header_isolation(mmltk_backend_imaging_raster)

function(mmltk_add_backend_imaging_raster_tests)
    add_executable(mmltk_backend_imaging_raster_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(mmltk_backend_imaging_raster_tests PRIVATE_DECLARATIONS)
    mmltk_register_ordinary_sources(mmltk_backend_imaging_raster_tests ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/caption_raster.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/rendered_image_writer.test.cpp)
    mmltk_register_retained_module_importers(mmltk_backend_imaging_raster_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/image_operations.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/validation_overlay.test.cpp)
    target_link_libraries(mmltk_backend_imaging_raster_tests PRIVATE
        stb mmltk_test_support mmltk_frameworks_gpu_test_support mmltk::backend::imaging::raster)
    mmltk_enable_native_runtime_rpath(mmltk_backend_imaging_raster_tests)
    catch_discover_tests(mmltk_backend_imaging_raster_tests
        TEST_PREFIX "backend::imaging::raster::" DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)
endfunction()

# Only the existing bundled font is admitted; runtime has no font-path lookup.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/frontend/iced/assets/SourceSansPro-Regular.otf")
file(READ "${CMAKE_SOURCE_DIR}/src/frontend/iced/assets/SourceSansPro-Regular.otf" MMLTK_CAPTION_FONT_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," MMLTK_CAPTION_FONT_BYTES "${MMLTK_CAPTION_FONT_HEX}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/caption_font_data.h" CONTENT "static constexpr unsigned char kCaptionFont[] = {${MMLTK_CAPTION_FONT_BYTES}};\n" @ONLY)

# The face is embedded in native code; its notices must accompany native installs.
install(FILES "${CMAKE_SOURCE_DIR}/src/frontend/iced/assets/OFL.txt"
    DESTINATION share/licenses/mmltk/caption-font)
install(FILES "${CMAKE_SOURCE_DIR}/third_party/stb/stb_truetype.h"
    DESTINATION share/licenses/mmltk/stb-truetype)
