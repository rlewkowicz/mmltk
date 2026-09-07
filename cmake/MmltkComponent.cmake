include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/MmltkOptions.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/MmltkToolchain.cmake")

function(_mmltk_configure_concrete_target_settings target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot configure unknown concrete target `${target}`")
    endif()
    get_target_property(_mmltk_concrete_configured
        "${target}" MMLTK_CONCRETE_TARGET_CONFIGURED)
    if(_mmltk_concrete_configured)
        message(FATAL_ERROR
            "Concrete target `${target}` was configured more than once")
    endif()
    _mmltk_apply_linux_cxx_target_properties("${target}")
    mmltk_apply_component_options("${target}")
    set_property(TARGET "${target}" PROPERTY
        MMLTK_CONCRETE_TARGET_CONFIGURED ON)
endfunction()

function(mmltk_configure_concrete_target target)
    _mmltk_configure_concrete_target_settings("${target}")
    set_property(GLOBAL APPEND PROPERTY
        MMLTK_CONFIGURED_CONCRETE_TARGETS "${target}")
endfunction()

function(mmltk_configure_foreign_target target)
    _mmltk_configure_concrete_target_settings("${target}")
endfunction()

function(mmltk_configure_component target)
    set(options PUBLIC_DECLARATIONS PRIVATE_DECLARATIONS)
    cmake_parse_arguments(MMLTK_COMPONENT
        "${options}" "" "" ${ARGN})
    if(MMLTK_COMPONENT_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown mmltk_configure_component arguments for `${target}`: "
            "${MMLTK_COMPONENT_UNPARSED_ARGUMENTS}")
    endif()
    if(MMLTK_COMPONENT_PUBLIC_DECLARATIONS
       AND MMLTK_COMPONENT_PRIVATE_DECLARATIONS)
        message(FATAL_ERROR
            "Component `${target}` selected both declaration visibilities")
    endif()
    if(NOT MMLTK_COMPONENT_PUBLIC_DECLARATIONS
       AND NOT MMLTK_COMPONENT_PRIVATE_DECLARATIONS)
        message(FATAL_ERROR
            "Component `${target}` must select PUBLIC_DECLARATIONS or "
            "PRIVATE_DECLARATIONS")
    endif()

    mmltk_configure_concrete_target("${target}")
    set_property(TARGET "${target}" PROPERTY MMLTK_COMPONENT_CONFIGURED ON)
    if(MMLTK_COMPONENT_PUBLIC_DECLARATIONS)
        set(_mmltk_declaration_visibility PUBLIC)
    else()
        set(_mmltk_declaration_visibility PRIVATE)
    endif()
    set_property(TARGET "${target}" PROPERTY
        MMLTK_DECLARATION_VISIBILITY "${_mmltk_declaration_visibility}")
    target_link_libraries("${target}"
        "${_mmltk_declaration_visibility}" mmltk_cxx26_dialect)
endfunction()

function(mmltk_target_cuda_architectures target)
    if(DEFINED CMAKE_CUDA_ARCHITECTURES AND
       NOT CMAKE_CUDA_ARCHITECTURES STREQUAL "")
        set_target_properties("${target}" PROPERTIES
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
    endif()
endfunction()

function(mmltk_register_runtime_library_directory build_directory)
    set(options)
    set(oneValueArgs INSTALL_DIRECTORY)
    cmake_parse_arguments(
        MMLTK_RUNTIME "${options}" "${oneValueArgs}" "" ${ARGN})
    set_property(GLOBAL APPEND PROPERTY
        MMLTK_BUILD_RUNTIME_LIBRARY_DIRECTORIES "${build_directory}")
    if(MMLTK_RUNTIME_INSTALL_DIRECTORY)
        set_property(GLOBAL APPEND PROPERTY
            MMLTK_INSTALL_RUNTIME_LIBRARY_DIRECTORIES
            "${MMLTK_RUNTIME_INSTALL_DIRECTORY}")
    endif()
endfunction()

function(mmltk_enable_native_runtime_rpath target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR
            "Cannot configure runtime RPATH for unknown target `${target}`")
    endif()
    get_target_property(_mmltk_target_type "${target}" TYPE)
    if(NOT _mmltk_target_type STREQUAL "EXECUTABLE" AND
       NOT _mmltk_target_type STREQUAL "SHARED_LIBRARY" AND
       NOT _mmltk_target_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR
            "Native runtime RPATH requires a linkable target: `${target}`")
    endif()

    get_property(_mmltk_build_runtime_directories GLOBAL PROPERTY
        MMLTK_BUILD_RUNTIME_LIBRARY_DIRECTORIES)
    set(_mmltk_build_rpath
        "${MMLTK_GCC16_RUNTIME_LIBDIR}"
        ${MMLTK_NVIDIA_LIBRARY_DIRECTORIES}
        ${_mmltk_build_runtime_directories})
    foreach(_mmltk_system_libdir IN ITEMS
            /lib64
            /usr/lib64
            /lib/x86_64-linux-gnu
            /usr/lib/x86_64-linux-gnu)
        if(EXISTS "${_mmltk_system_libdir}")
            list(APPEND _mmltk_build_rpath "${_mmltk_system_libdir}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _mmltk_build_rpath)
    set_target_properties("${target}" PROPERTIES
        BUILD_RPATH "${_mmltk_build_rpath}")

    get_property(_mmltk_install_runtime_directories GLOBAL PROPERTY
        MMLTK_INSTALL_RUNTIME_LIBRARY_DIRECTORIES)
    set(_mmltk_install_rpath
        "${MMLTK_GCC16_RUNTIME_LIBDIR}"
        "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
        ${MMLTK_NVIDIA_LIBRARY_DIRECTORIES}
        ${_mmltk_install_runtime_directories})
    list(REMOVE_DUPLICATES _mmltk_install_rpath)
    set_target_properties("${target}" PROPERTIES
        INSTALL_RPATH "${_mmltk_install_rpath}")
endfunction()

function(mmltk_install_target target)
    set(options SHARED)
    set(oneValueArgs COMPONENT)
    cmake_parse_arguments(
        MMLTK_INSTALL_TARGET "${options}" "${oneValueArgs}" "" ${ARGN})
    set(_mmltk_install_component_args "")
    if(MMLTK_INSTALL_TARGET_COMPONENT)
        set(_mmltk_install_component_args
            COMPONENT "${MMLTK_INSTALL_TARGET_COMPONENT}")
    endif()
    if(MMLTK_INSTALL_TARGET_SHARED)
        install(TARGETS "${target}"
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            ${_mmltk_install_component_args})
    else()
        install(TARGETS "${target}"
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
            ${_mmltk_install_component_args})
    endif()
endfunction()

function(mmltk_install_file source destination)
    set(options)
    set(oneValueArgs RENAME)
    cmake_parse_arguments(
        MMLTK_INSTALL_FILE "${options}" "${oneValueArgs}" "" ${ARGN})
    if(MMLTK_INSTALL_FILE_RENAME)
        install(FILES "${source}" DESTINATION "${destination}"
            RENAME "${MMLTK_INSTALL_FILE_RENAME}")
    else()
        install(FILES "${source}" DESTINATION "${destination}")
    endif()
endfunction()
