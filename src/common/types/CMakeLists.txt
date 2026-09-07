include(MmltkComponent)

add_library(mmltk_common_types STATIC)
add_library(mmltk::common::types ALIAS mmltk_common_types)
mmltk_configure_component(mmltk_common_types PUBLIC_DECLARATIONS)

mmltk_register_public_headers(mmltk_common_types
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        atomic_mirror.h
        atomic_snapshot.h
        byte_encoding.h
        config_cast.h
        enum.h
        generation.h
        stable_identity.h
        string_utils.h
        strong_id.h
)
mmltk_register_private_headers(
    mmltk_common_types detail/fixed_buffer_format.h)
set_target_properties(mmltk_common_types PROPERTIES LINKER_LANGUAGE CXX)
