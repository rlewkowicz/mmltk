include(MmltkComponent)

add_library(mmltk_backend_imaging_annotation STATIC)
add_library(
    mmltk::backend::imaging::annotation
    ALIAS mmltk_backend_imaging_annotation
)
mmltk_configure_component(
    mmltk_backend_imaging_annotation PUBLIC_DECLARATIONS)

foreach(_mmltk_annotation_partition IN ITEMS
        semantic_scene_descriptor.cppm
        semantic_scene.cppm
        manual_mask_mapping.cppm
        common.cppm
        core.cppm
        renderer.cppm
        canvas_layers.cppm
        preview_rect_drag.cppm)
    mmltk_register_retained_module_providers(
        mmltk_backend_imaging_annotation "${_mmltk_annotation_partition}")
endforeach()
mmltk_register_retained_module_implementation_units(
    mmltk_backend_imaging_annotation
    core.cpp
    core_persistence.cpp
    core_preview.cpp
    types.cpp
    manual_mask_mapping.cpp
    renderer.cpp
    canvas_layers.cpp
    preview_rect_drag.cpp
)
mmltk_register_ordinary_sources(
    mmltk_backend_imaging_annotation manual_mask_mapping_cuda.cu)
mmltk_register_public_headers(
    mmltk_backend_imaging_annotation
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    FILES detail/manual_mask_mapping_abi.h)
mmltk_register_private_headers(
    mmltk_backend_imaging_annotation
    detail/manual_mask_mapping_cuda_abi.h)
target_include_directories(mmltk_backend_imaging_annotation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mmltk_backend_imaging_annotation
    PRIVATE
        CUDA::cudart
        mmltk::common::io
        nlohmann_json
        stb
)
set_target_properties(mmltk_backend_imaging_annotation PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
)

function(mmltk_add_backend_imaging_annotation_tests)
    add_executable(mmltk_backend_imaging_annotation_tests EXCLUDE_FROM_ALL)
    mmltk_register_retained_module_importers(
        mmltk_backend_imaging_annotation_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/annotation_core.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/canvas_layers.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/preview_rect_drag.test.cpp
    )
    mmltk_configure_component(
        mmltk_backend_imaging_annotation_tests PRIVATE_DECLARATIONS)
    target_link_libraries(mmltk_backend_imaging_annotation_tests PRIVATE
        mmltk_acceptance_support
        mmltk::backend::imaging::annotation
    )
    catch_discover_tests(
        mmltk_backend_imaging_annotation_tests
        TEST_PREFIX "backend::imaging::annotation::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
