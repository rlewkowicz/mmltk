include_guard(GLOBAL)

set(MMLTK_CEF_DISTRIBUTION_PLATFORM "linux64")
set(MMLTK_CEF_DISTRIBUTION_FLAVOR "minimal_debug")
set(MMLTK_CEF_DISTRIBUTION_ARCHIVE_NAME
    "mmltk_cef_${MMLTK_CEF_DISTRIBUTION_PLATFORM}_${MMLTK_CEF_DISTRIBUTION_FLAVOR}.tar.bz2")
if(NOT DEFINED MMLTK_CEF_DISTRIBUTION_CACHE_DIR OR
   MMLTK_CEF_DISTRIBUTION_CACHE_DIR STREQUAL "")
    get_filename_component(MMLTK_CEF_DISTRIBUTION_CACHE_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../.docker-cache/cef"
        ABSOLUTE)
endif()
set(MMLTK_CEF_DISTRIBUTION_ARCHIVE_PATH
    "${MMLTK_CEF_DISTRIBUTION_CACHE_DIR}/${MMLTK_CEF_DISTRIBUTION_ARCHIVE_NAME}")
set(MMLTK_CEF_DISTRIBUTION_SHA256_PATH
    "${MMLTK_CEF_DISTRIBUTION_ARCHIVE_PATH}.sha256")
set(MMLTK_CEF_DISTRIBUTION_BINARY_SUBDIR "Debug")
set(MMLTK_CEF_DISTRIBUTION_RESOURCES_SUBDIR "Resources")
if(NOT DEFINED MMLTK_CEF_DISTRIBUTION_ROOT OR
   MMLTK_CEF_DISTRIBUTION_ROOT STREQUAL "")
    set(MMLTK_CEF_DISTRIBUTION_ROOT
        "/opt/mmltk-cef")
endif()
set(MMLTK_CEF_DISTRIBUTION_BINARY_ROOT
    "${MMLTK_CEF_DISTRIBUTION_ROOT}/${MMLTK_CEF_DISTRIBUTION_BINARY_SUBDIR}")
set(MMLTK_CEF_DISTRIBUTION_RESOURCES_ROOT
    "${MMLTK_CEF_DISTRIBUTION_ROOT}/${MMLTK_CEF_DISTRIBUTION_RESOURCES_SUBDIR}")

function(mmltk_classify_pinned_cef_distribution_root root out_binary_root out_resources_root)
    set(_mmltk_cef_binary_root "${root}/${MMLTK_CEF_DISTRIBUTION_BINARY_SUBDIR}")
    set(_mmltk_cef_resources_root "${root}/${MMLTK_CEF_DISTRIBUTION_RESOURCES_SUBDIR}")

    if(NOT IS_DIRECTORY "${_mmltk_cef_binary_root}")
        message(FATAL_ERROR
            "Pinned CEF distribution root ${root} is missing ${MMLTK_CEF_DISTRIBUTION_BINARY_SUBDIR}/.")
    endif()
    if(NOT IS_DIRECTORY "${_mmltk_cef_resources_root}")
        message(FATAL_ERROR
            "Pinned CEF distribution root ${root} is missing ${MMLTK_CEF_DISTRIBUTION_RESOURCES_SUBDIR}/.")
    endif()

    set(${out_binary_root} "${_mmltk_cef_binary_root}" PARENT_SCOPE)
    set(${out_resources_root} "${_mmltk_cef_resources_root}" PARENT_SCOPE)
endfunction()

function(mmltk_write_pinned_cef_distribution_env output_path)
    mmltk_require_repo_local_cef_distribution_archive(
        _mmltk_cef_archive_path
        _mmltk_cef_sha256_path)
    file(WRITE "${output_path}"
        "MMLTK_CEF_DISTRIBUTION_PLATFORM='${MMLTK_CEF_DISTRIBUTION_PLATFORM}'\n"
        "MMLTK_CEF_DISTRIBUTION_FLAVOR='${MMLTK_CEF_DISTRIBUTION_FLAVOR}'\n"
        "MMLTK_CEF_DISTRIBUTION_CACHE_DIR='${MMLTK_CEF_DISTRIBUTION_CACHE_DIR}'\n"
        "MMLTK_CEF_DISTRIBUTION_ARCHIVE_NAME='${MMLTK_CEF_DISTRIBUTION_ARCHIVE_NAME}'\n"
        "MMLTK_CEF_DISTRIBUTION_ARCHIVE_PATH='${_mmltk_cef_archive_path}'\n"
        "MMLTK_CEF_DISTRIBUTION_SHA256_PATH='${_mmltk_cef_sha256_path}'\n"
        "MMLTK_CEF_DISTRIBUTION_ROOT='${MMLTK_CEF_DISTRIBUTION_ROOT}'\n"
        "MMLTK_CEF_DISTRIBUTION_BINARY_ROOT='${MMLTK_CEF_DISTRIBUTION_BINARY_ROOT}'\n"
        "MMLTK_CEF_DISTRIBUTION_RESOURCES_ROOT='${MMLTK_CEF_DISTRIBUTION_RESOURCES_ROOT}'\n")
endfunction()

function(mmltk_require_repo_local_cef_distribution_archive out_archive_path out_sha256_path)
    if(NOT IS_DIRECTORY "${MMLTK_CEF_DISTRIBUTION_CACHE_DIR}")
        message(FATAL_ERROR
            "Repo-local CEF cache directory is missing: ${MMLTK_CEF_DISTRIBUTION_CACHE_DIR}. "
            "Run ./buildcef.sh to populate .docker-cache/cef before configuring this cutover.")
    endif()
    if(NOT EXISTS "${MMLTK_CEF_DISTRIBUTION_ARCHIVE_PATH}")
        message(FATAL_ERROR
            "Repo-local custom CEF archive is missing: ${MMLTK_CEF_DISTRIBUTION_ARCHIVE_PATH}. "
            "Run ./buildcef.sh before configuring this cutover.")
    endif()
    if(NOT EXISTS "${MMLTK_CEF_DISTRIBUTION_SHA256_PATH}")
        message(FATAL_ERROR
            "Repo-local custom CEF archive checksum is missing: ${MMLTK_CEF_DISTRIBUTION_SHA256_PATH}. "
            "Run ./buildcef.sh before configuring this cutover.")
    endif()

    set(${out_archive_path} "${MMLTK_CEF_DISTRIBUTION_ARCHIVE_PATH}" PARENT_SCOPE)
    set(${out_sha256_path} "${MMLTK_CEF_DISTRIBUTION_SHA256_PATH}" PARENT_SCOPE)
endfunction()

function(mmltk_require_pinned_cef_distribution_root out_var)
    if(NOT IS_DIRECTORY "${MMLTK_CEF_DISTRIBUTION_ROOT}")
        message(FATAL_ERROR
            "Pinned CEF distribution root is missing: ${MMLTK_CEF_DISTRIBUTION_ROOT}. "
            "Expected ${MMLTK_CEF_DISTRIBUTION_ARCHIVE_NAME} from ${MMLTK_CEF_DISTRIBUTION_CACHE_DIR} "
            "to be unpacked there by the repo Dockerfile.")
    endif()

    get_filename_component(_mmltk_cef_root "${MMLTK_CEF_DISTRIBUTION_ROOT}" REALPATH)
    mmltk_classify_pinned_cef_distribution_root(
        "${_mmltk_cef_root}"
        _mmltk_cef_binary_root
        _mmltk_cef_resources_root)

    set(_mmltk_cef_required_paths
        "${_mmltk_cef_root}/include/cef_app.h"
        "${_mmltk_cef_binary_root}/libcef.so"
        "${_mmltk_cef_binary_root}/snapshot_blob.bin"
        "${_mmltk_cef_binary_root}/v8_context_snapshot.bin"
        "${_mmltk_cef_binary_root}/libEGL.so"
        "${_mmltk_cef_binary_root}/libGLESv2.so"
        "${_mmltk_cef_resources_root}/icudtl.dat"
        "${_mmltk_cef_resources_root}/resources.pak"
        "${_mmltk_cef_resources_root}/chrome_100_percent.pak"
        "${_mmltk_cef_resources_root}/chrome_200_percent.pak"
        "${_mmltk_cef_resources_root}/locales"
        "${_mmltk_cef_root}/libcef_dll")
    foreach(_mmltk_cef_required_path IN LISTS _mmltk_cef_required_paths)
        if(NOT EXISTS "${_mmltk_cef_required_path}")
            file(RELATIVE_PATH _mmltk_cef_missing_path
                "${_mmltk_cef_root}"
                "${_mmltk_cef_required_path}")
            message(FATAL_ERROR
                "Pinned CEF distribution is incomplete at ${_mmltk_cef_root}. "
                "Missing ${_mmltk_cef_missing_path}.")
        endif()
    endforeach()

    set(${out_var} "${_mmltk_cef_root}" PARENT_SCOPE)
endfunction()

function(mmltk_install_pinned_cef_runtime cef_root destination)
    mmltk_classify_pinned_cef_distribution_root(
        "${cef_root}"
        _mmltk_cef_binary_root
        _mmltk_cef_resources_root)

    install(DIRECTORY "${_mmltk_cef_binary_root}"
        DESTINATION "${destination}")
    install(DIRECTORY "${_mmltk_cef_resources_root}"
        DESTINATION "${destination}")
endfunction()
