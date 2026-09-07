include_guard(GLOBAL)

function(_mmltk_absolute_source output source)
    if(IS_ABSOLUTE "${source}")
        cmake_path(NORMAL_PATH source OUTPUT_VARIABLE _mmltk_source)
    else()
        cmake_path(ABSOLUTE_PATH source
            BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            NORMALIZE OUTPUT_VARIABLE _mmltk_source)
    endif()
    set("${output}" "${_mmltk_source}" PARENT_SCOPE)
endfunction()

function(mmltk_register_public_headers target)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs BASE_DIRS FILES)
    cmake_parse_arguments(
        MMLTK_HEADERS "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(NOT MMLTK_HEADERS_BASE_DIRS OR NOT MMLTK_HEADERS_FILES)
        message(FATAL_ERROR
            "Public header registration requires BASE_DIRS and FILES")
    endif()

    set(_mmltk_base_directories)
    foreach(_mmltk_base IN LISTS MMLTK_HEADERS_BASE_DIRS)
        _mmltk_absolute_source(_mmltk_absolute_base "${_mmltk_base}")
        list(APPEND _mmltk_base_directories "${_mmltk_absolute_base}")
    endforeach()
    set(_mmltk_headers)
    foreach(_mmltk_header IN LISTS MMLTK_HEADERS_FILES)
        _mmltk_absolute_source(_mmltk_absolute_header "${_mmltk_header}")
        list(APPEND _mmltk_headers "${_mmltk_absolute_header}")
    endforeach()

    get_target_property(_mmltk_target_type "${target}" TYPE)
    if(_mmltk_target_type STREQUAL "INTERFACE_LIBRARY")
        set(_mmltk_visibility INTERFACE)
    else()
        set(_mmltk_visibility PUBLIC)
    endif()
    target_sources("${target}" "${_mmltk_visibility}"
        FILE_SET mmltk_public_headers
        TYPE HEADERS
        BASE_DIRS ${_mmltk_base_directories}
        FILES ${_mmltk_headers})
endfunction()

function(mmltk_register_private_headers target)
    set(_mmltk_headers)
    foreach(_mmltk_header IN LISTS ARGN)
        _mmltk_absolute_source(_mmltk_source "${_mmltk_header}")
        list(APPEND _mmltk_headers "${_mmltk_source}")
    endforeach()
    target_sources("${target}" PRIVATE
        FILE_SET mmltk_private_headers TYPE HEADERS
        BASE_DIRS "${CMAKE_SOURCE_DIR}"
        FILES ${_mmltk_headers})
endfunction()

# A projection consumes declarations with the owner's complete transitive usage
# requirements, without linking or constructing that owner's implementation.
function(mmltk_link_declarations target visibility)
    foreach(_mmltk_owner IN LISTS ARGN)
        target_include_directories("${target}" "${visibility}"
            "$<TARGET_PROPERTY:${_mmltk_owner},INTERFACE_INCLUDE_DIRECTORIES>")
        target_include_directories("${target}" SYSTEM "${visibility}"
            "$<TARGET_PROPERTY:${_mmltk_owner},INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>")
        target_compile_definitions("${target}" "${visibility}"
            "$<TARGET_PROPERTY:${_mmltk_owner},INTERFACE_COMPILE_DEFINITIONS>")
        target_compile_options("${target}" "${visibility}"
            "$<TARGET_PROPERTY:${_mmltk_owner},INTERFACE_COMPILE_OPTIONS>")
        target_compile_features("${target}" "${visibility}"
            "$<TARGET_PROPERTY:${_mmltk_owner},INTERFACE_COMPILE_FEATURES>")
    endforeach()
endfunction()

function(mmltk_register_header_isolation target)
    get_target_property(_mmltk_public "${target}" HEADER_SET_mmltk_public_headers)
    get_target_property(_mmltk_private "${target}" HEADER_SET_mmltk_private_headers)
    set(_mmltk_headers)
    foreach(_mmltk_set IN ITEMS _mmltk_public _mmltk_private)
        if(${_mmltk_set})
            list(APPEND _mmltk_headers ${${_mmltk_set}})
        endif()
    endforeach()
    if(NOT _mmltk_headers)
        message(FATAL_ERROR "Header isolation requires registered headers for `${target}`")
    endif()
    list(REMOVE_DUPLICATES _mmltk_headers)
    set(_mmltk_sources)
    foreach(_mmltk_header IN LISTS _mmltk_headers)
        file(RELATIVE_PATH _mmltk_relative "${CMAKE_SOURCE_DIR}" "${_mmltk_header}")
        set(_mmltk_source
            "${CMAKE_CURRENT_BINARY_DIR}/header-isolation/${target}/${_mmltk_relative}.cpp")
        file(GENERATE OUTPUT "${_mmltk_source}"
            CONTENT "#include \"${_mmltk_relative}\"\n")
        list(APPEND _mmltk_sources "${_mmltk_source}")
    endforeach()
    set(_mmltk_check "${target}_header_isolation")
    add_library("${_mmltk_check}" OBJECT EXCLUDE_FROM_ALL)
    mmltk_configure_header_isolation("${_mmltk_check}" "${target}")
    mmltk_register_ordinary_sources("${_mmltk_check}" ${_mmltk_sources})
    add_dependencies("${target}" "${_mmltk_check}")
endfunction()

function(mmltk_register_header_include_orders target first second assertion)
    foreach(_mmltk_order IN ITEMS forward reverse)
        if(_mmltk_order STREQUAL "forward")
            set(_mmltk_content "#include \"${first}\"\n#include \"${second}\"\n")
        else()
            set(_mmltk_content "#include \"${second}\"\n#include \"${first}\"\n")
        endif()
        set(_mmltk_source
            "${CMAKE_CURRENT_BINARY_DIR}/header-isolation/${target}/include-order-${_mmltk_order}.cpp")
        file(GENERATE OUTPUT "${_mmltk_source}" CONTENT "${_mmltk_content}${assertion}\n")
        mmltk_register_ordinary_sources("${target}_header_isolation" "${_mmltk_source}")
    endforeach()
endfunction()

function(mmltk_register_ordinary_sources target)
    foreach(_mmltk_input IN LISTS ARGN)
        _mmltk_absolute_source(_mmltk_source "${_mmltk_input}")
        set_source_files_properties(
            "${_mmltk_source}" PROPERTIES CXX_SCAN_FOR_MODULES OFF)
        target_sources("${target}" PRIVATE "${_mmltk_source}")
    endforeach()
endfunction()

function(mmltk_register_retained_module_providers target)
    set(_mmltk_visibility PUBLIC)
    set(_mmltk_sources ${ARGN})
    list(LENGTH _mmltk_sources _mmltk_source_count)
    if(_mmltk_source_count GREATER 0 AND
       ("${ARGV1}" STREQUAL "PUBLIC" OR "${ARGV1}" STREQUAL "PRIVATE"))
        list(POP_FRONT _mmltk_sources _mmltk_visibility)
    endif()
    if(NOT _mmltk_sources)
        message(FATAL_ERROR
            "Retained module provider registration requires at least one source")
    endif()

    set(_mmltk_modules)
    foreach(_mmltk_input IN LISTS _mmltk_sources)
        _mmltk_absolute_source(_mmltk_source "${_mmltk_input}")
        set_source_files_properties(
            "${_mmltk_source}" PROPERTIES CXX_SCAN_FOR_MODULES ON)
        list(APPEND _mmltk_modules "${_mmltk_source}")
    endforeach()
    string(TOLOWER "${_mmltk_visibility}" _mmltk_file_set_visibility)
    target_sources("${target}" "${_mmltk_visibility}"
        FILE_SET "mmltk_${_mmltk_file_set_visibility}_modules"
        TYPE CXX_MODULES
        FILES ${_mmltk_modules})
endfunction()

function(mmltk_register_retained_module_implementation_units target)
    foreach(_mmltk_input IN LISTS ARGN)
        _mmltk_absolute_source(_mmltk_source "${_mmltk_input}")
        set_source_files_properties(
            "${_mmltk_source}" PROPERTIES CXX_SCAN_FOR_MODULES ON)
        target_sources("${target}" PRIVATE "${_mmltk_source}")
    endforeach()
endfunction()

function(mmltk_register_retained_module_importers target)
    foreach(_mmltk_input IN LISTS ARGN)
        _mmltk_absolute_source(_mmltk_source "${_mmltk_input}")
        set_source_files_properties(
            "${_mmltk_source}" PROPERTIES CXX_SCAN_FOR_MODULES ON)
        target_sources("${target}" PRIVATE "${_mmltk_source}")
    endforeach()
endfunction()
