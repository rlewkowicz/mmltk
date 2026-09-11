include(MmltkComponent)

option(BUILD_MMLTK_BROWSER_APP
    "Build and install the iced WebAssembly frontend" ON)
set(MMLTK_INSTALL_BROWSER_APPDIR
    "${CMAKE_INSTALL_DATADIR}/mmltk/browser-app" CACHE STRING
    "Install-relative directory for packaged browser app assets")
set(MMLTK_TRUNK_VERSION "0.21.14" CACHE STRING
    "Required Trunk version for reproducible iced WebAssembly builds")
set(MMLTK_ICED_TEST_ARGUMENTS "" CACHE STRING
    "Additional Cargo test arguments for focused Iced validation")

add_library(mmltk_frontend_iced INTERFACE)
add_library(mmltk::frontend::iced ALIAS mmltk_frontend_iced)
target_link_libraries(mmltk_frontend_iced INTERFACE
    mmltk::controller::browser::protocol_v16
)

set(MMLTK_FRONTEND_ICED_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    CACHE INTERNAL "Iced frontend source root")
set(MMLTK_FRONTEND_ICED_DIST_DIR
    "${CMAKE_BINARY_DIR}/frontend/iced/dist"
    CACHE INTERNAL "Packaged Iced frontend output")

if(BUILD_MMLTK_BROWSER_APP AND BUILD_RFDETR_NATIVE)
    set(_mmltk_iced_source_dir "${CMAKE_SOURCE_DIR}/third_party/iced")
    foreach(_mmltk_required_path IN ITEMS
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/Cargo.toml"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/Cargo.lock"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/.cargo/config.toml"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/Trunk.toml"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/index.html"
            "${_mmltk_iced_source_dir}/Cargo.toml"
            )
        if(NOT EXISTS "${_mmltk_required_path}")
            message(FATAL_ERROR "Missing Iced frontend input: ${_mmltk_required_path}")
        endif()
    endforeach()

    find_program(MMLTK_FRONTEND_NODE_EXECUTABLE NAMES node REQUIRED)
    find_program(MMLTK_CARGO_EXECUTABLE NAMES cargo REQUIRED)
    find_program(MMLTK_RUSTC_EXECUTABLE NAMES rustc REQUIRED)
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
       NOT _mmltk_trunk_version_output
           MATCHES "trunk ${MMLTK_TRUNK_VERSION}([^0-9]|$)")
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
       NOT "\n${_mmltk_rust_targets}"
           MATCHES "\nwasm32-unknown-unknown(\n|$)")
        message(FATAL_ERROR
            "Rust target wasm32-unknown-unknown is required (`rustup target add wasm32-unknown-unknown`): "
            "${_mmltk_rust_targets_error}")
    endif()

    add_executable(mmltk_frontend_iced_publisher EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_frontend_iced_publisher PRIVATE_DECLARATIONS)
    mmltk_register_ordinary_sources(
        mmltk_frontend_iced_publisher browser_dist_publish.cpp)
    set_target_properties(mmltk_frontend_iced_publisher PROPERTIES
        OUTPUT_NAME mmltk-browser-bundle-contract)
    mmltk_install_target(mmltk_frontend_iced_publisher)

    file(GLOB_RECURSE _mmltk_frontend_source_inputs
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/src/*"
        "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/assets/*"
    )
    file(GLOB_RECURSE _mmltk_iced_inputs
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${_mmltk_iced_source_dir}/*.rs"
        "${_mmltk_iced_source_dir}/*.ttf"
        "${_mmltk_iced_source_dir}/*.toml"
        "${_mmltk_iced_source_dir}/*.wgsl"
    )
    set(_mmltk_frontend_build_inputs
        "${MMLTK_APPLICATION_BINDINGS}"
        "${MMLTK_BROWSER_PROTOCOL_MARKER}"
        "${MMLTK_BROWSER_PROTOCOL_V16_CLIENT_FIXTURE}"
        "${MMLTK_BROWSER_PROTOCOL_V16_SERVER_FIXTURE}"
        "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/Cargo.toml"
        "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/Cargo.lock"
        "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/.cargo/config.toml"
        "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/Trunk.toml"
        "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/index.html"
        "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/wasm-opt-wrapper"
        ${_mmltk_frontend_source_inputs}
        ${_mmltk_iced_inputs}
    )
    set(_mmltk_protocol_v16_environment
        "MMLTK_APPLICATION_BINDINGS_PATH=${MMLTK_APPLICATION_BINDINGS}"
        "MMLTK_PROTOCOL_V16_CLIENT_FIXTURE_PATH=${MMLTK_BROWSER_PROTOCOL_V16_CLIENT_FIXTURE}"
        "MMLTK_PROTOCOL_V16_SERVER_FIXTURE_PATH=${MMLTK_BROWSER_PROTOCOL_V16_SERVER_FIXTURE}")
    set(_mmltk_frontend_environment ${_mmltk_protocol_v16_environment})

    add_custom_command(
        OUTPUT "${MMLTK_BROWSER_PROTOCOL_V16_CLIENT_FIXTURE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_BINARY_DIR}/generated/frontend/iced"
        COMMAND "${CMAKE_COMMAND}" -E env
            ${_mmltk_protocol_v16_environment}
            "${MMLTK_CARGO_EXECUTABLE}" run
            --manifest-path "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/Cargo.toml"
            --locked
            --frozen
            --bin protocol_v16_client_fixture
            --
            "${MMLTK_BROWSER_PROTOCOL_V16_CLIENT_FIXTURE}"
        DEPENDS
            "${MMLTK_APPLICATION_BINDINGS}"
            "${MMLTK_BROWSER_PROTOCOL_V16_SERVER_FIXTURE}"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/Cargo.toml"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/Cargo.lock"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/.cargo/config.toml"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/src/lib.rs"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/src/application_codec.rs"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/src/generated.rs"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/src/protocol/mod.rs"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/src/protocol/client_records.rs"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/src/protocol/cbor.rs"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/src/protocol/records.rs"
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/src/bin/protocol_v16_client_fixture.rs"
        WORKING_DIRECTORY "${MMLTK_FRONTEND_ICED_SOURCE_DIR}"
        VERBATIM)
    add_custom_target(mmltk_frontend_iced_protocol_v16_client_fixture
        DEPENDS "${MMLTK_BROWSER_PROTOCOL_V16_CLIENT_FIXTURE}")
    add_custom_target(mmltk_protocol_v16_generation
        DEPENDS
            mmltk_controller_browser_protocol_v16_boundary
            mmltk_frontend_iced_protocol_v16_client_fixture)

    add_custom_target(mmltk_frontend_iced_check
        COMMAND "${CMAKE_COMMAND}" -E env
            ${_mmltk_frontend_environment}
            "${MMLTK_CARGO_EXECUTABLE}" metadata
            --locked
            --frozen
            --format-version 1
            --no-deps
        COMMAND "${CMAKE_COMMAND}" -E env
            ${_mmltk_frontend_environment}
            "${MMLTK_CARGO_EXECUTABLE}" fmt
            --manifest-path "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/Cargo.toml"
            --
            --check
        COMMAND "${CMAKE_COMMAND}" -E env
            ${_mmltk_frontend_environment}
            "${MMLTK_CARGO_EXECUTABLE}" test
            --manifest-path "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/Cargo.toml"
            --locked
            --frozen
            --no-run
        DEPENDS ${_mmltk_frontend_build_inputs}
        WORKING_DIRECTORY "${MMLTK_FRONTEND_ICED_SOURCE_DIR}"
        VERBATIM
    )
    add_dependencies(mmltk_frontend_iced_check
        mmltk_controller_browser_protocol_v16_boundary
    )

    add_custom_target(mmltk_frontend_iced_tests
        COMMAND "${MMLTK_FRONTEND_NODE_EXECUTABLE}" --test
            "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/tests/integration_control.test.mjs"
        COMMAND "${CMAKE_COMMAND}" -E env
            ${_mmltk_frontend_environment}
            "${MMLTK_CARGO_EXECUTABLE}" test
            --manifest-path "${MMLTK_FRONTEND_ICED_SOURCE_DIR}/Cargo.toml"
            --locked
            --frozen
            ${MMLTK_ICED_TEST_ARGUMENTS}
        DEPENDS
            ${_mmltk_frontend_build_inputs}
        WORKING_DIRECTORY "${MMLTK_FRONTEND_ICED_SOURCE_DIR}"
        VERBATIM
    )
    add_dependencies(mmltk_frontend_iced_tests
        mmltk_controller_browser_protocol_v16_boundary
    )

    set(_mmltk_frontend_dist_next_dir
        "${CMAKE_BINARY_DIR}/frontend/iced/dist.next")
    set(_mmltk_frontend_build_stamp
        "${CMAKE_BINARY_DIR}/frontend/iced/build.stamp")
    add_custom_command(
        OUTPUT "${_mmltk_frontend_build_stamp}"
        BYPRODUCTS "${MMLTK_FRONTEND_ICED_DIST_DIR}/index.html"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf
            "${_mmltk_frontend_dist_next_dir}"
        COMMAND "${CMAKE_COMMAND}" -E env
            "PATH=/usr/local/bin:$ENV{PATH}"
            ${_mmltk_frontend_environment}
            "${MMLTK_TRUNK_EXECUTABLE}" build
            --release
            --locked
            --frozen
            --dist "${_mmltk_frontend_dist_next_dir}"
        COMMAND "$<TARGET_FILE:mmltk_frontend_iced_publisher>"
            publish
            "${_mmltk_frontend_dist_next_dir}"
            "${MMLTK_FRONTEND_ICED_DIST_DIR}"
            "${MMLTK_BROWSER_PROTOCOL_MARKER}"
        COMMAND "${CMAKE_COMMAND}" -E touch
            "${_mmltk_frontend_build_stamp}"
        DEPENDS
            mmltk_frontend_iced_publisher
            ${_mmltk_frontend_build_inputs}
        WORKING_DIRECTORY "${MMLTK_FRONTEND_ICED_SOURCE_DIR}"
        VERBATIM
    )
    add_custom_target(mmltk_frontend_iced_bundle ALL
        DEPENDS "${_mmltk_frontend_build_stamp}"
    )
    add_dependencies(mmltk_frontend_iced_bundle
        mmltk_controller_browser_protocol_v16_boundary
    )
    add_dependencies(mmltk_frontend_iced mmltk_frontend_iced_bundle)

    string(CONCAT _mmltk_frontend_install_code
        "foreach(_mmltk_frontend_output IN ITEMS\n"
        "    \"${_mmltk_frontend_build_stamp}\"\n"
        "    \"${MMLTK_FRONTEND_ICED_DIST_DIR}/index.html\")\n"
        "  if(NOT EXISTS \"\${_mmltk_frontend_output}\")\n"
        "    message(FATAL_ERROR\n"
        "      \"Iced frontend graph output is missing: \${_mmltk_frontend_output}\")\n"
        "  endif()\n"
        "endforeach()\n"
        "file(GLOB _mmltk_frontend_wasm\n"
        "  \"${MMLTK_FRONTEND_ICED_DIST_DIR}/*.wasm\")\n"
        "file(GLOB _mmltk_frontend_javascript\n"
        "  \"${MMLTK_FRONTEND_ICED_DIST_DIR}/*.js\")\n"
        "if(NOT _mmltk_frontend_wasm OR NOT _mmltk_frontend_javascript)\n"
        "  message(FATAL_ERROR\n"
        "    \"Iced frontend graph did not produce a usable JavaScript/Wasm bundle\")\n"
        "endif()\n"
    )
    install(CODE "${_mmltk_frontend_install_code}")
    install(DIRECTORY "${MMLTK_FRONTEND_ICED_DIST_DIR}/"
        DESTINATION "${MMLTK_INSTALL_BROWSER_APPDIR}"
    )
    install(FILES "${MMLTK_BROWSER_PROTOCOL_MARKER}"
        DESTINATION "${MMLTK_INSTALL_BROWSER_APPDIR}"
    )
else()
    add_custom_target(mmltk_frontend_iced_check)
    add_custom_target(mmltk_frontend_iced_tests)
endif()
