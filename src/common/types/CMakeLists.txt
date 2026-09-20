include(MmltkComponent)

add_library(mmltk_common_types STATIC)
add_library(mmltk::common::types ALIAS mmltk_common_types)
mmltk_configure_component(mmltk_common_types PUBLIC_DECLARATIONS)

mmltk_register_public_headers(mmltk_common_types
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        byte_encoding.h
        generation.h
        stable_identity.h
        string_utils.h
        strong_id.h
        utf8.h
)
set_target_properties(mmltk_common_types PROPERTIES LINKER_LANGUAGE CXX)
mmltk_register_header_isolation(mmltk_common_types)
