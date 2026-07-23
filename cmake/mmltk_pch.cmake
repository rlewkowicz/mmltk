function(mmltk_configure_target_pch target)
    set(options)
    set(oneValueArgs HEADER REUSE_FROM)
    cmake_parse_arguments(MMLTK_PCH "${options}" "${oneValueArgs}" "" ${ARGN})

    if(NOT TARGET ${target})
        message(FATAL_ERROR "PCH requested for unknown target `${target}`")
    endif()

    if(MMLTK_PCH_HEADER AND MMLTK_PCH_REUSE_FROM)
        message(FATAL_ERROR
            "mmltk_configure_target_pch(${target}) accepts either HEADER or REUSE_FROM, not both.")
    endif()

    if(NOT MMLTK_PCH_HEADER AND NOT MMLTK_PCH_REUSE_FROM)
        message(FATAL_ERROR
            "mmltk_configure_target_pch(${target}) requires HEADER or REUSE_FROM.")
    endif()

    if(MMLTK_PCH_REUSE_FROM)
        if(NOT TARGET ${MMLTK_PCH_REUSE_FROM})
            message(FATAL_ERROR
                "PCH reuse target `${MMLTK_PCH_REUSE_FROM}` does not exist for `${target}`.")
        endif()
        # Only reuse when the producer and consumer already share the same compile surface.
        target_precompile_headers(${target} REUSE_FROM ${MMLTK_PCH_REUSE_FROM})
        return()
    endif()

    get_filename_component(_mmltk_pch_header
        "${MMLTK_PCH_HEADER}"
        ABSOLUTE
        BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT EXISTS "${_mmltk_pch_header}")
        message(FATAL_ERROR
            "Configured PCH header for `${target}` does not exist: ${_mmltk_pch_header}")
    endif()

    # Keep the first pass C++-only so mixed-language targets do not try to force CUDA PCH generation.
    target_precompile_headers(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${_mmltk_pch_header}>")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC emits an ordinary preprocessed stream for ccache and fails loudly if
        # a target silently stops consuming its configured PCH.
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-fpch-preprocess>"
            "$<$<COMPILE_LANGUAGE:CXX>:-Winvalid-pch>"
        )
    endif()
endfunction()
