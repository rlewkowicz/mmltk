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
    foreach(_mmltk_header IN LISTS ARGN)
        _mmltk_absolute_source(_mmltk_source "${_mmltk_header}")
        target_sources("${target}" PRIVATE "${_mmltk_source}")
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
