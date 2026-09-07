include_guard(GLOBAL)

set(MMLTK_SUPPORTED_BUILD_TYPES Dev Release)
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES
        "${MMLTK_SUPPORTED_BUILD_TYPES}" CACHE STRING
        "Supported mmltk build types" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    set_property(
        CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        ${MMLTK_SUPPORTED_BUILD_TYPES})
    list(FIND MMLTK_SUPPORTED_BUILD_TYPES
        "${CMAKE_BUILD_TYPE}" MMLTK_BUILD_TYPE_INDEX)
    if(MMLTK_BUILD_TYPE_INDEX EQUAL -1)
        string(JOIN ", " MMLTK_SUPPORTED_BUILD_TYPES_TEXT
            ${MMLTK_SUPPORTED_BUILD_TYPES})
        message(FATAL_ERROR
            "mmltk supports only these build types: "
            "${MMLTK_SUPPORTED_BUILD_TYPES_TEXT}")
    endif()
endif()

option(MMLTK_DEV_STRICT_WARNINGS
    "Enable aggressive host-side warnings for the Dev build" ON)
option(MMLTK_WARNINGS_AS_ERRORS
    "Treat compiler warnings as build errors for every project target" ON)
option(MMLTK_DEV_ENABLE_ASAN
    "Enable AddressSanitizer for host-side Dev C++ builds" OFF)
option(MMLTK_DEV_ENABLE_TSAN
    "Enable ThreadSanitizer for host-side Dev C/C++ builds" OFF)
option(MMLTK_DEV_ENABLE_UBSAN
    "Enable UndefinedBehaviorSanitizer for host-side Dev C++ builds" OFF)
option(MMLTK_ENABLE_TIME_TRACE
    "Emit host-side Clang -ftime-trace JSON artifacts during compilation" OFF)
option(MMLTK_STATIC_ANALYSIS_BUILD
    "Configure the repository static-analysis compiler graph" OFF)

option(BUILD_RFDETR_NATIVE
    "Build native RF-DETR validation backends" ON)
option(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER
    "Enable embedded Python parsing of upstream RF-DETR .pth checkpoints"
    ${BUILD_RFDETR_NATIVE})
if(BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER AND NOT BUILD_RFDETR_NATIVE)
    message(FATAL_ERROR
        "BUILD_RFDETR_PYTHON_CHECKPOINT_LOADER requires BUILD_RFDETR_NATIVE=ON")
endif()

function(mmltk_apply_component_options target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot apply options to unknown target `${target}`")
    endif()

    target_compile_definitions("${target}" PRIVATE
        MMLTK_ENABLE_PROFILING=$<IF:$<CONFIG:Dev>,1,0>
        MMLTK_BUILD_CONFIG=\"$<CONFIG>\"
    )
    foreach(_mmltk_option IN LISTS MMLTK_RELEASE_HOST_COMPILE_OPTIONS)
        target_compile_options("${target}" PRIVATE
            "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:${_mmltk_option}>")
    endforeach()
    foreach(_mmltk_option IN LISTS MMLTK_DEV_HOST_COMPILE_OPTIONS)
        target_compile_options("${target}" PRIVATE
            "$<$<AND:$<CONFIG:Dev>,$<COMPILE_LANGUAGE:C,CXX>>:${_mmltk_option}>")
    endforeach()
    foreach(_mmltk_option IN LISTS MMLTK_RELEASE_CUDA_COMPILE_OPTIONS)
        target_compile_options("${target}" PRIVATE
            "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CUDA>>:${_mmltk_option}>")
    endforeach()
    foreach(_mmltk_option IN LISTS MMLTK_DEV_CUDA_COMPILE_OPTIONS)
        target_compile_options("${target}" PRIVATE
            "$<$<AND:$<CONFIG:Dev>,$<COMPILE_LANGUAGE:CUDA>>:${_mmltk_option}>")
    endforeach()
    foreach(_mmltk_warning IN LISTS MMLTK_PROJECT_CXX_WARNING_FLAGS)
        target_compile_options("${target}" PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:${_mmltk_warning}>
        )
    endforeach()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options("${target}" PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wframe-larger-than=1048576>
        )
    endif()
    foreach(_mmltk_warning IN LISTS MMLTK_PROJECT_CUDA_WARNING_FLAGS)
        target_compile_options("${target}" PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:${_mmltk_warning}>
        )
    endforeach()
endfunction()
