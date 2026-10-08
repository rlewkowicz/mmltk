include_guard(GLOBAL)

# This graph never uses the product compiler launcher: the launcher is one of
# the tools being built, including on a cold checkout.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_CXX_COMPILER "/opt/gcc-16.2/bin/g++" CACHE FILEPATH "Pinned tools compiler")
    set(CMAKE_CXX_COMPILER_LAUNCHER "" CACHE STRING "Tools bootstrap has no launcher" FORCE)
endif()

function(mmltk_tools_configure_target target)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "Native tools require Linux")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
       NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "16.2.0")
        message(FATAL_ERROR "Native tools require GCC 16.2.0")
    endif()
    get_filename_component(_compiler "${CMAKE_CXX_COMPILER}" REALPATH)
    get_filename_component(_pinned "/opt/gcc-16.2/bin/g++" REALPATH)
    if(NOT _compiler STREQUAL _pinned)
        message(FATAL_ERROR "Native tools require /opt/gcc-16.2/bin/g++")
    endif()
    target_compile_features(${target} PUBLIC cxx_std_26)
    target_compile_options(${target} PRIVATE -freflection -Wall -Wextra -Wpedantic
        "$<$<CONFIG:Debug,RelWithDebInfo,Dev>:-g1>")
    find_program(MMLTK_TOOLS_MOLD_PROGRAM mold REQUIRED)
    target_link_options(${target} PRIVATE -fuse-ld=mold -Wl,--as-needed)
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF CXX_SCAN_FOR_MODULES ON POSITION_INDEPENDENT_CODE ON)
endfunction()

# Owners supply their module and entrypoint once. The generated translation
# unit is only the process/C ABI adapter; it contains no implementation policy.
function(mmltk_tools_add_entry_adapter target)
    cmake_parse_arguments(ENTRY "WESTON" "MODULE;FUNCTION" "" ${ARGN})
    if(NOT ENTRY_MODULE OR NOT ENTRY_FUNCTION)
        message(FATAL_ERROR "Entry adapter requires MODULE and FUNCTION")
    endif()
    set(_adapter "${CMAKE_CURRENT_BINARY_DIR}/generated/tools/${target}_entry.cpp")
    if(ENTRY_WESTON)
        set(_source "#include <libweston/libweston.h>\nimport ${ENTRY_MODULE};\nextern \"C\" WL_EXPORT int wet_module_init(weston_compositor* compositor, int* argc, char* argv[]) { return ${ENTRY_FUNCTION}(compositor, argc, argv); }\n")
    else()
        set(_source "import ${ENTRY_MODULE};\nint main(int argc, char** argv) { return ${ENTRY_FUNCTION}(argc, argv); }\n")
    endif()
    file(GENERATE OUTPUT "${_adapter}" CONTENT "${_source}")
    target_sources(${target} PRIVATE "${_adapter}")
    set_source_files_properties("${_adapter}" PROPERTIES LANGUAGE CXX CXX_SCAN_FOR_MODULES ON)
endfunction()

# Product owners retain their dependencies and executable identities while
# using the same module/adapter registration as the standalone tools graph.
function(mmltk_tools_register_process target source module entry)
    mmltk_tools_configure_target(${target})
    target_sources(${target} PRIVATE FILE_SET tool_module TYPE CXX_MODULES FILES "${source}")
    set_source_files_properties("${source}" PROPERTIES LANGUAGE CXX)
    mmltk_tools_add_entry_adapter(${target} MODULE "${module}" FUNCTION "${entry}")
endfunction()
