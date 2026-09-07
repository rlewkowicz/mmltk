include_guard(GLOBAL)

# Upstream owns its C dialect and architecture-specific copy compilation.
# All mutable Make outputs live in this graph's private staging directory.
find_program(MMLTK_GDR_MAKE NAMES gmake make REQUIRED)
set(_mmltk_gdr_source "${CMAKE_SOURCE_DIR}/third_party/gdrcopy")
set(_mmltk_gdr_stage "${CMAKE_BINARY_DIR}/third_party/gdrcopy")
file(GLOB_RECURSE _mmltk_gdr_inputs CONFIGURE_DEPENDS
    "${_mmltk_gdr_source}/*")
add_custom_command(
    OUTPUT "${_mmltk_gdr_stage}/src/libgdrapi.a"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_mmltk_gdr_stage}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${_mmltk_gdr_source}" "${_mmltk_gdr_stage}"
    COMMAND "${MMLTK_GDR_MAKE}" -C "${_mmltk_gdr_stage}/src"
        "CC=${CMAKE_C_COMPILER}" "AR=${CMAKE_AR}"
        "CFLAGS=-O2 -fPIC -fvisibility=hidden" static
    DEPENDS ${_mmltk_gdr_inputs}
    VERBATIM
    COMMENT "Building private GDRCopy C archive")
add_custom_target(mmltk_gdrcopy_build
    DEPENDS "${_mmltk_gdr_stage}/src/libgdrapi.a")
add_library(mmltk_gdrcopy STATIC IMPORTED GLOBAL)
set_target_properties(mmltk_gdrcopy PROPERTIES
    IMPORTED_LOCATION "${_mmltk_gdr_stage}/src/libgdrapi.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_mmltk_gdr_source}/include"
    INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS}"
    INTERFACE_LINK_OPTIONS "LINKER:--exclude-libs,libgdrapi.a")
add_dependencies(mmltk_gdrcopy mmltk_gdrcopy_build)
install(FILES "${_mmltk_gdr_source}/LICENSE" "${_mmltk_gdr_source}/UPSTREAM.md"
    DESTINATION share/mmltk/licenses/gdrcopy)
