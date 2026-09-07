include(MmltkComponent)

add_library(mmltk_common_math STATIC)
add_library(mmltk::common::math ALIAS mmltk_common_math)
mmltk_configure_component(mmltk_common_math PUBLIC_DECLARATIONS)

mmltk_register_public_headers(mmltk_common_math
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES checked_arithmetic.h
)
set_target_properties(mmltk_common_math PROPERTIES LINKER_LANGUAGE CXX)
