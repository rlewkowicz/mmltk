option(BUILD_MMLTK_FIREFOX_RUNTIME
    "Build and stage the owned Firefox runtime in the default graph" OFF)
set(MMLTK_BUILD_JOBS "" CACHE STRING
    "Parallel worker count assigned independently to coarse build branches")
set(MMLTK_FIREFOX_CACHE_ROOT
    "${CMAKE_SOURCE_DIR}/.cache/firefox" CACHE PATH
    "Canonical cache root for owned Firefox build state")
set(MMLTK_FIREFOX_RUNTIME_ROOT
    "${MMLTK_FIREFOX_CACHE_ROOT}/obj-minimal-opt/dist/firefox" CACHE PATH
    "Firefox Linux runtime directory containing the owned executable and libraries")
set(MMLTK_FIREFOX_BUILD_INPUT
    "${MMLTK_FIREFOX_CACHE_ROOT}/build-input.sha256" CACHE FILEPATH
    "Content fingerprint for the owned Firefox source and toolchain")

if(BUILD_MMLTK_FIREFOX_RUNTIME)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR
            "BUILD_MMLTK_FIREFOX_RUNTIME is supported only by the Release graph.")
    endif()
    if(MMLTK_BUILD_JOBS STREQUAL "")
        cmake_host_system_information(
            RESULT MMLTK_BUILD_JOBS QUERY NUMBER_OF_LOGICAL_CORES)
    endif()
    if(NOT MMLTK_BUILD_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "MMLTK_BUILD_JOBS must be a positive integer, got `${MMLTK_BUILD_JOBS}`.")
    endif()

    set(_mmltk_firefox_source_dir "${CMAKE_SOURCE_DIR}/third_party/firefox")
    set(_mmltk_firefox_mach "${_mmltk_firefox_source_dir}/mach")
    set(_mmltk_traced_command "${CMAKE_SOURCE_DIR}/utilities/run_traced_command.sh")
    set(_mmltk_firefox_build_command
        "${CMAKE_SOURCE_DIR}/utilities/build_firefox_runtime.sh")
    if(NOT EXISTS "${_mmltk_firefox_mach}")
        message(FATAL_ERROR "Missing owned Firefox Mach driver: ${_mmltk_firefox_mach}")
    endif()
    if(NOT EXISTS "${_mmltk_traced_command}")
        message(FATAL_ERROR "Missing gated command tracer: ${_mmltk_traced_command}")
    endif()
    if(NOT EXISTS "${_mmltk_firefox_build_command}")
        message(FATAL_ERROR
            "Missing Firefox build command: ${_mmltk_firefox_build_command}")
    endif()
    if(NOT EXISTS "${MMLTK_FIREFOX_BUILD_INPUT}")
        message(FATAL_ERROR
            "Missing Firefox build input fingerprint: ${MMLTK_FIREFOX_BUILD_INPUT}")
    endif()

    set(_mmltk_firefox_required_runtime_files
        firefox
        firefox-bin
        libxul.so
        libmozgtk.so
        libmozwayland.so
        application.ini
        platform.ini
        omni.ja
        browser/omni.ja
    )
    set(_mmltk_firefox_runtime_byproducts
        "${MMLTK_FIREFOX_CACHE_ROOT}/obj-minimal-opt/dist/.mmltk-runtime-stage.sha256")
    foreach(_mmltk_firefox_runtime_file IN LISTS
            _mmltk_firefox_required_runtime_files)
        list(APPEND _mmltk_firefox_runtime_byproducts
            "${MMLTK_FIREFOX_RUNTIME_ROOT}/${_mmltk_firefox_runtime_file}")
    endforeach()

    set(_mmltk_firefox_build_stamp
        "${MMLTK_FIREFOX_CACHE_ROOT}/obj-minimal-opt/.mmltk-build-input.sha256")
    add_custom_command(
        OUTPUT "${_mmltk_firefox_build_stamp}"
        COMMAND "${CMAKE_COMMAND}" -E env
            --unset=CC
            --unset=CXX
            --unset=HOST_CC
            --unset=HOST_CXX
            "MMLTK_FIREFOX_CACHE_ROOT=${MMLTK_FIREFOX_CACHE_ROOT}"
            "MOZBUILD_STATE_PATH=${MMLTK_FIREFOX_CACHE_ROOT}/mozbuild"
            "SCCACHE_DIR=${MMLTK_FIREFOX_CACHE_ROOT}/sccache"
            "MMLTK_JOBS=${MMLTK_BUILD_JOBS}"
            "MMLTK_FIREFOX_BUILD_INPUT=${MMLTK_FIREFOX_BUILD_INPUT}"
            "MMLTK_FIREFOX_BUILT_INPUT=${_mmltk_firefox_build_stamp}"
            "MMLTK_FIREFOX_MACH=${_mmltk_firefox_mach}"
            "MMLTK_TRACED_COMMAND=${_mmltk_traced_command}"
            bash "${_mmltk_firefox_build_command}"
        DEPENDS
            "${MMLTK_FIREFOX_BUILD_INPUT}"
            "${_mmltk_firefox_build_command}"
        BYPRODUCTS ${_mmltk_firefox_runtime_byproducts}
        WORKING_DIRECTORY "${_mmltk_firefox_source_dir}"
        COMMENT "Building and staging the owned Firefox runtime with ${MMLTK_BUILD_JOBS} workers"
        VERBATIM
    )
    add_custom_target(mmltk_firefox_runtime ALL
        DEPENDS "${_mmltk_firefox_build_stamp}")
endif()
