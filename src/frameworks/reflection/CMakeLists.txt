include(MmltkComponent)

add_library(mmltk_frameworks_reflection STATIC)
add_library(mmltk::frameworks::reflection ALIAS mmltk_frameworks_reflection)
mmltk_configure_component(mmltk_frameworks_reflection
    PUBLIC_DECLARATIONS
)

mmltk_register_public_headers(mmltk_frameworks_reflection
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        field_policy.h
        reflected_descriptors.h
        reflected_field_policy.h
        record_projection.h
        reflection_metadata.h
        include/mmltk/frameworks/reflection/materializer.h
        include/mmltk/frameworks/reflection/member_path.h
        include/mmltk/frameworks/reflection/member_relation.h
)
target_include_directories(mmltk_frameworks_reflection PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
set_target_properties(mmltk_frameworks_reflection PROPERTIES
    LINKER_LANGUAGE CXX
)
target_link_libraries(mmltk_frameworks_reflection
    PUBLIC
        mmltk::common::types
)
