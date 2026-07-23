option(BUILD_MMLTK_BROWSER_APP "Build and install the iced WebAssembly browser app" ON)
set(MMLTK_INSTALL_BROWSER_APPDIR "${CMAKE_INSTALL_DATADIR}/mmltk/browser-app" CACHE STRING
    "Install-relative directory for packaged browser app assets")
set(MMLTK_TRUNK_VERSION "0.21.14" CACHE STRING
    "Required Trunk version for reproducible iced WebAssembly builds")

if(BUILD_MMLTK_BROWSER_APP)
    set(MMLTK_BROWSER_APP_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/browser/app")
    set(MMLTK_BROWSER_APP_ICED_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/iced")
    set(MMLTK_BROWSER_APP_ICED_AW_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/iced_aw")
    foreach(_mmltk_required_path IN ITEMS
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/Cargo.toml"
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/Cargo.lock"
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/.cargo/config.toml"
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/Trunk.toml"
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/index.html"
            "${MMLTK_BROWSER_APP_ICED_SOURCE_DIR}/Cargo.toml"
            "${MMLTK_BROWSER_APP_ICED_AW_SOURCE_DIR}/Cargo.toml")
        if(NOT EXISTS "${_mmltk_required_path}")
            message(FATAL_ERROR "Missing iced browser app input: ${_mmltk_required_path}")
        endif()
    endforeach()

    find_program(MMLTK_CARGO_EXECUTABLE NAMES cargo REQUIRED)
    find_program(MMLTK_RUSTFMT_EXECUTABLE NAMES rustfmt REQUIRED)
    find_program(MMLTK_RUSTUP_EXECUTABLE NAMES rustup REQUIRED)
    find_program(MMLTK_TRUNK_EXECUTABLE NAMES trunk REQUIRED)
    find_program(MMLTK_WASM_BINDGEN_EXECUTABLE NAMES wasm-bindgen REQUIRED)
    find_program(MMLTK_WASM_OPT_EXECUTABLE NAMES wasm-opt REQUIRED)

    execute_process(
        COMMAND "${MMLTK_TRUNK_EXECUTABLE}" --version
        OUTPUT_VARIABLE _mmltk_trunk_version_output
        ERROR_VARIABLE _mmltk_trunk_version_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _mmltk_trunk_version_result
    )
    if(NOT _mmltk_trunk_version_result EQUAL 0 OR
       NOT _mmltk_trunk_version_output MATCHES "trunk ${MMLTK_TRUNK_VERSION}([^0-9]|$)")
        message(FATAL_ERROR
            "Trunk ${MMLTK_TRUNK_VERSION} is required; got `${_mmltk_trunk_version_output}` "
            "(`${_mmltk_trunk_version_error}`).")
    endif()

    execute_process(
        COMMAND "${MMLTK_RUSTUP_EXECUTABLE}" target list --installed
        OUTPUT_VARIABLE _mmltk_rust_targets
        ERROR_VARIABLE _mmltk_rust_targets_error
        RESULT_VARIABLE _mmltk_rust_targets_result
    )
    if(NOT _mmltk_rust_targets_result EQUAL 0 OR
       NOT "\n${_mmltk_rust_targets}" MATCHES "\nwasm32-unknown-unknown(\n|$)")
        message(FATAL_ERROR
            "Rust target wasm32-unknown-unknown is required (`rustup target add wasm32-unknown-unknown`): "
            "${_mmltk_rust_targets_error}")
    endif()

    add_executable(mmltk_browser_host_contract_codegen
        src/browser/host_api_contract_codegen.cpp
    )
    mmltk_target_project_dirs(mmltk_browser_host_contract_codegen)
    target_link_libraries(mmltk_browser_host_contract_codegen PRIVATE
        mmltk_build_options
        mmltk_browser_api_contract
    )
    add_executable(mmltk_browser_dist_publish
        src/browser/browser_dist_publish.cpp
    )
    mmltk_target_project_dirs(mmltk_browser_dist_publish)
    target_link_libraries(mmltk_browser_dist_publish PRIVATE
        mmltk_build_options
    )

    set(MMLTK_BROWSER_APP_STAGE_DIR "${CMAKE_SOURCE_DIR}/build/browser-app" CACHE PATH
        "Directory for publishable browser assets")
    set(MMLTK_BROWSER_APP_STATE_DIR "${CMAKE_SOURCE_DIR}/.cache/browser-app" CACHE PATH
        "Shared Cargo/Trunk fingerprint and publication state")
    set(MMLTK_BROWSER_APP_DIST_DIR "${MMLTK_BROWSER_APP_STAGE_DIR}/dist")
    set(MMLTK_BROWSER_APP_DIST_NEXT_DIR "${MMLTK_BROWSER_APP_STAGE_DIR}/dist.next")
    set(MMLTK_BROWSER_CONTRACT_OPENAPI_OUT "${MMLTK_BROWSER_APP_STATE_DIR}/host_api.openapi.json")
    set(MMLTK_BROWSER_CONTRACT_RUST_OUT
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/src/generated/host_api_contract_generated.rs")
    set(MMLTK_BROWSER_CONTRACT_RUST_STAGE_OUT
        "${MMLTK_BROWSER_APP_STATE_DIR}/host_api_contract_generated.rs")
    set(MMLTK_BROWSER_CONTRACT_CPP_OUT
        "${CMAKE_SOURCE_DIR}/src/browser/workflow_contract_generated.h")
    set(MMLTK_BROWSER_APP_CONTRACT_STAMP "${MMLTK_BROWSER_APP_STATE_DIR}/contract.stamp")
    set(MMLTK_BROWSER_APP_BUILD_STAMP "${MMLTK_BROWSER_APP_STATE_DIR}/build.stamp")

    file(GLOB_RECURSE MMLTK_BROWSER_APP_SOURCE_INPUTS
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/src/*"
    )
    file(GLOB_RECURSE MMLTK_BROWSER_APP_ICED_INPUTS
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${MMLTK_BROWSER_APP_ICED_SOURCE_DIR}/*.rs"
        "${MMLTK_BROWSER_APP_ICED_SOURCE_DIR}/*.ttf"
        "${MMLTK_BROWSER_APP_ICED_SOURCE_DIR}/*.toml"
        "${MMLTK_BROWSER_APP_ICED_SOURCE_DIR}/*.wgsl"
    )
    file(GLOB_RECURSE MMLTK_BROWSER_APP_ICED_AW_INPUTS
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${MMLTK_BROWSER_APP_ICED_AW_SOURCE_DIR}/*.rs"
        "${MMLTK_BROWSER_APP_ICED_AW_SOURCE_DIR}/*.ttf"
        "${MMLTK_BROWSER_APP_ICED_AW_SOURCE_DIR}/*.toml"
    )
    set(MMLTK_BROWSER_APP_BUILD_INPUTS
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/Cargo.toml"
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/Cargo.lock"
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/.cargo/config.toml"
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/Trunk.toml"
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/index.html"
        ${MMLTK_BROWSER_APP_SOURCE_INPUTS}
        ${MMLTK_BROWSER_APP_ICED_INPUTS}
        ${MMLTK_BROWSER_APP_ICED_AW_INPUTS}
    )
    set(MMLTK_BROWSER_NATIVE_CONTRACT_INPUTS
        "${CMAKE_SOURCE_DIR}/src/browser/browser_contract_metadata.h"
        "${CMAKE_SOURCE_DIR}/src/browser/api_contract_dsl.h"
        "${CMAKE_SOURCE_DIR}/src/browser/host_api_protocol.h"
        "${CMAKE_SOURCE_DIR}/src/browser/host_api_intents.h"
        "${CMAKE_SOURCE_DIR}/src/gui/browser_file_dialog_contract.h"
        "${CMAKE_SOURCE_DIR}/src/gui/browser_settings_contract.h"
        "${CMAKE_SOURCE_DIR}/src/gui/browser_host_adapters.h"
    )

    add_custom_command(
        OUTPUT "${MMLTK_BROWSER_APP_CONTRACT_STAMP}"
        BYPRODUCTS
            "${MMLTK_BROWSER_CONTRACT_OPENAPI_OUT}"
            "${MMLTK_BROWSER_CONTRACT_RUST_OUT}"
            "${MMLTK_BROWSER_CONTRACT_RUST_STAGE_OUT}"
            "${MMLTK_BROWSER_CONTRACT_CPP_OUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${MMLTK_BROWSER_APP_STATE_DIR}"
        COMMAND "$<TARGET_FILE:mmltk_browser_host_contract_codegen>"
            --write
            --openapi-out "${MMLTK_BROWSER_CONTRACT_OPENAPI_OUT}"
            --rust-out "${MMLTK_BROWSER_CONTRACT_RUST_STAGE_OUT}"
            --cpp-out "${MMLTK_BROWSER_CONTRACT_CPP_OUT}"
        COMMAND "${MMLTK_RUSTFMT_EXECUTABLE}"
            --edition 2024
            "${MMLTK_BROWSER_CONTRACT_RUST_STAGE_OUT}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MMLTK_BROWSER_CONTRACT_RUST_STAGE_OUT}"
            "${MMLTK_BROWSER_CONTRACT_RUST_OUT}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${MMLTK_BROWSER_APP_CONTRACT_STAMP}"
        DEPENDS
            mmltk_browser_host_contract_codegen
            ${MMLTK_BROWSER_NATIVE_CONTRACT_INPUTS}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        VERBATIM
    )

    add_custom_target(mmltk_browser_app_check
        COMMAND "$<TARGET_FILE:mmltk_browser_host_contract_codegen>"
            --check
            --openapi-out "${MMLTK_BROWSER_CONTRACT_OPENAPI_OUT}"
            --cpp-out "${MMLTK_BROWSER_CONTRACT_CPP_OUT}"
        COMMAND "$<TARGET_FILE:mmltk_browser_host_contract_codegen>"
            --write
            --rust-out "${MMLTK_BROWSER_CONTRACT_RUST_STAGE_OUT}"
        COMMAND "${MMLTK_RUSTFMT_EXECUTABLE}"
            --edition 2024
            "${MMLTK_BROWSER_CONTRACT_RUST_STAGE_OUT}"
        COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${MMLTK_BROWSER_CONTRACT_RUST_STAGE_OUT}"
            "${MMLTK_BROWSER_CONTRACT_RUST_OUT}"
        COMMAND "${MMLTK_CARGO_EXECUTABLE}" metadata
            --locked
            --frozen
            --format-version 1
            --no-deps
        COMMAND "${MMLTK_CARGO_EXECUTABLE}" fmt
            --manifest-path "${MMLTK_BROWSER_APP_ICED_AW_SOURCE_DIR}/Cargo.toml"
            --
            --check
        COMMAND "${MMLTK_CARGO_EXECUTABLE}" fmt
            --manifest-path "${MMLTK_BROWSER_APP_SOURCE_DIR}/Cargo.toml"
            --
            --check
        COMMAND /bin/bash "${CMAKE_SOURCE_DIR}/utilities/check_cargo_dupes.sh"
            "${MMLTK_BROWSER_APP_SOURCE_DIR}"
        DEPENDS
            "${MMLTK_BROWSER_APP_CONTRACT_STAMP}"
            "${CMAKE_SOURCE_DIR}/utilities/check_cargo_dupes.sh"
            ${MMLTK_BROWSER_APP_BUILD_INPUTS}
        WORKING_DIRECTORY "${MMLTK_BROWSER_APP_SOURCE_DIR}"
        VERBATIM
    )

    add_custom_target(mmltk_browser_app_contract
        DEPENDS "${MMLTK_BROWSER_APP_CONTRACT_STAMP}"
    )

    add_custom_command(
        OUTPUT "${MMLTK_BROWSER_APP_BUILD_STAMP}"
        BYPRODUCTS "${MMLTK_BROWSER_APP_DIST_DIR}/index.html"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MMLTK_BROWSER_APP_DIST_NEXT_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E env
            "PATH=/usr/local/bin:$ENV{PATH}"
            "${MMLTK_TRUNK_EXECUTABLE}" build
            --release
            --locked
            --frozen
            --dist "${MMLTK_BROWSER_APP_DIST_NEXT_DIR}"
        COMMAND "$<TARGET_FILE:mmltk_browser_dist_publish>"
            "${MMLTK_BROWSER_APP_DIST_NEXT_DIR}"
            "${MMLTK_BROWSER_APP_DIST_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${MMLTK_BROWSER_APP_BUILD_STAMP}"
        DEPENDS
            "${MMLTK_BROWSER_APP_CONTRACT_STAMP}"
            mmltk_browser_dist_publish
            ${MMLTK_BROWSER_APP_BUILD_INPUTS}
        WORKING_DIRECTORY "${MMLTK_BROWSER_APP_SOURCE_DIR}"
        VERBATIM
    )

    add_custom_target(mmltk_browser_app ALL
        DEPENDS "${MMLTK_BROWSER_APP_BUILD_STAMP}"
    )

    string(CONCAT _mmltk_browser_app_install_code
        "foreach(_mmltk_browser_output IN ITEMS\n"
        "    \"${MMLTK_BROWSER_APP_BUILD_STAMP}\"\n"
        "    \"${MMLTK_BROWSER_APP_DIST_DIR}/index.html\")\n"
        "  if(NOT EXISTS \"\${_mmltk_browser_output}\")\n"
        "    message(FATAL_ERROR\n"
        "      \"Browser graph output is missing: \${_mmltk_browser_output}\")\n"
        "  endif()\n"
        "endforeach()\n"
    )
    install(CODE "${_mmltk_browser_app_install_code}")
    install(DIRECTORY "${MMLTK_BROWSER_APP_DIST_DIR}/"
        DESTINATION "${MMLTK_INSTALL_BROWSER_APPDIR}"
    )
endif()
