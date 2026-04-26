option(BUILD_MMLTK_BROWSER_APP "Build and install the packaged browser app bundle" ON)
set(MMLTK_INSTALL_BROWSER_APPDIR "${CMAKE_INSTALL_DATADIR}/mmltk/browser-app" CACHE STRING
    "Install-relative directory for packaged browser app assets")

if(BUILD_MMLTK_BROWSER_APP)
    set(MMLTK_BROWSER_APP_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/browser/app")
    if(NOT EXISTS "${MMLTK_BROWSER_APP_SOURCE_DIR}/package.json")
        message(FATAL_ERROR
            "Missing browser app package.json at ${MMLTK_BROWSER_APP_SOURCE_DIR}.")
    endif()

    find_program(MMLTK_NODE_EXECUTABLE NAMES node REQUIRED)
    find_program(MMLTK_NPM_EXECUTABLE NAMES npm REQUIRED)

    set(MMLTK_BROWSER_APP_STAGE_DIR "${CMAKE_BINARY_DIR}/browser_app")
    set(MMLTK_BROWSER_APP_DIST_DIR "${MMLTK_BROWSER_APP_STAGE_DIR}/dist")
    set(MMLTK_BROWSER_APP_BROWSER_DIST_DIR "${MMLTK_BROWSER_APP_DIST_DIR}/browser")
    set(MMLTK_BROWSER_APP_DEPS_STAMP "${MMLTK_BROWSER_APP_STAGE_DIR}/.deps.stamp")
    set(MMLTK_BROWSER_APP_SYNC_STAMP "${MMLTK_BROWSER_APP_STAGE_DIR}/.sync.stamp")
    set(MMLTK_BROWSER_APP_CHECK_STAMP "${MMLTK_BROWSER_APP_STAGE_DIR}/.check.stamp")
    set(MMLTK_BROWSER_APP_BUILD_STAMP "${MMLTK_BROWSER_APP_STAGE_DIR}/.build.stamp")

    set(MMLTK_BROWSER_APP_CONFIG_INPUTS
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/angular.json"
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/index.html"
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/package-lock.json"
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/package.json"
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/tsconfig.app.json"
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/tsconfig.json"
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/tsconfig.tests.json"
    )
    file(GLOB_RECURSE MMLTK_BROWSER_APP_SOURCE_INPUTS
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/src/*"
    )
    file(GLOB_RECURSE MMLTK_BROWSER_APP_TEST_INPUTS
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/tests/*"
    )
    file(GLOB_RECURSE MMLTK_BROWSER_APP_SCRIPT_INPUTS
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${MMLTK_BROWSER_APP_SOURCE_DIR}/scripts/*"
    )
    set(MMLTK_BROWSER_APP_BUILD_INPUTS
        ${MMLTK_BROWSER_APP_CONFIG_INPUTS}
        ${MMLTK_BROWSER_APP_SOURCE_INPUTS}
        ${MMLTK_BROWSER_APP_TEST_INPUTS}
        ${MMLTK_BROWSER_APP_SCRIPT_INPUTS}
    )
    set(MMLTK_BROWSER_APP_DIST_FILES
        "${MMLTK_BROWSER_APP_BROWSER_DIST_DIR}/index.html"
        "${MMLTK_BROWSER_APP_DIST_DIR}/3rdpartylicenses.txt"
        "${MMLTK_BROWSER_APP_DIST_DIR}/prerendered-routes.json"
    )

    add_custom_command(
        OUTPUT "${MMLTK_BROWSER_APP_DEPS_STAMP}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${MMLTK_BROWSER_APP_STAGE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/package-lock.json"
            "${MMLTK_BROWSER_APP_STAGE_DIR}/package-lock.json"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/package.json"
            "${MMLTK_BROWSER_APP_STAGE_DIR}/package.json"
        COMMAND "${MMLTK_NPM_EXECUTABLE}" ci
            --include=dev
            --no-audit
            --no-fund
        COMMAND "${CMAKE_COMMAND}" -E touch "${MMLTK_BROWSER_APP_DEPS_STAMP}"
        DEPENDS
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/package-lock.json"
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/package.json"
        WORKING_DIRECTORY "${MMLTK_BROWSER_APP_STAGE_DIR}"
        USES_TERMINAL
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${MMLTK_BROWSER_APP_SYNC_STAMP}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${MMLTK_BROWSER_APP_STAGE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/angular.json"
            "${MMLTK_BROWSER_APP_STAGE_DIR}/angular.json"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/index.html"
            "${MMLTK_BROWSER_APP_STAGE_DIR}/index.html"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/tsconfig.app.json"
            "${MMLTK_BROWSER_APP_STAGE_DIR}/tsconfig.app.json"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/tsconfig.json"
            "${MMLTK_BROWSER_APP_STAGE_DIR}/tsconfig.json"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/tsconfig.tests.json"
            "${MMLTK_BROWSER_APP_STAGE_DIR}/tsconfig.tests.json"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MMLTK_BROWSER_APP_STAGE_DIR}/src"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/src"
            "${MMLTK_BROWSER_APP_STAGE_DIR}/src"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MMLTK_BROWSER_APP_STAGE_DIR}/tests"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/tests"
            "${MMLTK_BROWSER_APP_STAGE_DIR}/tests"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MMLTK_BROWSER_APP_STAGE_DIR}/scripts"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${MMLTK_BROWSER_APP_SOURCE_DIR}/scripts"
            "${MMLTK_BROWSER_APP_STAGE_DIR}/scripts"
        COMMAND "${CMAKE_COMMAND}" -E touch "${MMLTK_BROWSER_APP_SYNC_STAMP}"
        DEPENDS
            "${MMLTK_BROWSER_APP_DEPS_STAMP}"
            ${MMLTK_BROWSER_APP_BUILD_INPUTS}
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${MMLTK_BROWSER_APP_CHECK_STAMP}"
        COMMAND "${MMLTK_NPM_EXECUTABLE}" run check
        COMMAND "${MMLTK_NPM_EXECUTABLE}" run test
        COMMAND "${CMAKE_COMMAND}" -E touch "${MMLTK_BROWSER_APP_CHECK_STAMP}"
        DEPENDS "${MMLTK_BROWSER_APP_SYNC_STAMP}"
        WORKING_DIRECTORY "${MMLTK_BROWSER_APP_STAGE_DIR}"
        USES_TERMINAL
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${MMLTK_BROWSER_APP_BUILD_STAMP}"
        BYPRODUCTS ${MMLTK_BROWSER_APP_DIST_FILES}
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MMLTK_BROWSER_APP_DIST_DIR}"
        COMMAND "${MMLTK_NPM_EXECUTABLE}" run build
        COMMAND "${CMAKE_COMMAND}" -E touch "${MMLTK_BROWSER_APP_BUILD_STAMP}"
        DEPENDS "${MMLTK_BROWSER_APP_CHECK_STAMP}"
        WORKING_DIRECTORY "${MMLTK_BROWSER_APP_STAGE_DIR}"
        USES_TERMINAL
        VERBATIM
    )

    add_custom_target(mmltk_browser_app_check
        DEPENDS "${MMLTK_BROWSER_APP_CHECK_STAMP}"
    )
    add_custom_target(mmltk_browser_app ALL
        DEPENDS "${MMLTK_BROWSER_APP_BUILD_STAMP}"
    )

    string(CONCAT _mmltk_browser_app_install_code
        "if(NOT EXISTS \"${MMLTK_BROWSER_APP_BROWSER_DIST_DIR}/index.html\")\n"
        "  message(STATUS \"Browser app bundle missing from ${MMLTK_BROWSER_APP_DIST_DIR}. Rebuilding before install.\")\n"
        "  execute_process(\n"
        "    COMMAND \"${CMAKE_COMMAND}\" --build \"${CMAKE_BINARY_DIR}\" --target mmltk_browser_app\n"
        "    RESULT_VARIABLE _mmltk_browser_app_build_result\n"
        "  )\n"
        "  if(NOT _mmltk_browser_app_build_result EQUAL 0)\n"
        "    message(FATAL_ERROR \"Failed to build mmltk_browser_app during install.\")\n"
        "  endif()\n"
        "endif()\n"
    )
    install(CODE "${_mmltk_browser_app_install_code}")
    install(DIRECTORY "${MMLTK_BROWSER_APP_DIST_DIR}/"
        DESTINATION "${MMLTK_INSTALL_BROWSER_APPDIR}"
    )
endif()
