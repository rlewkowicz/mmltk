# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.


function(catch_discover_tests TARGET)

  cmake_parse_arguments(
    ""
    "SKIP_IS_FAILURE;ADD_TAGS_AS_LABELS"
    "TEST_PREFIX;TEST_SUFFIX;WORKING_DIRECTORY;TEST_LIST;REPORTER;OUTPUT_DIR;OUTPUT_PREFIX;OUTPUT_SUFFIX;DISCOVERY_MODE"
    "TEST_SPEC;EXTRA_ARGS;PROPERTIES;DL_PATHS;DL_FRAMEWORK_PATHS"
    ${ARGN}
  )

  if(${CMAKE_VERSION} VERSION_LESS "3.19")
    message(FATAL_ERROR "This script requires JSON support from CMake version 3.19 or greater.")
  endif()

  if(NOT _WORKING_DIRECTORY)
    set(_WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  if(NOT _TEST_LIST)
    set(_TEST_LIST ${TARGET}_TESTS)
  endif()
  if(_DL_PATHS AND ${CMAKE_VERSION} VERSION_LESS "3.22.0")
    message(FATAL_ERROR "The DL_PATHS option requires at least cmake 3.22")
  endif()
  if(_DL_FRAMEWORK_PATHS AND ${CMAKE_VERSION} VERSION_LESS "3.22.0")
    message(FATAL_ERROR "The DL_FRAMEWORK_PATHS option requires at least cmake 3.22")
  endif()
  if(NOT _DISCOVERY_MODE)
    if(NOT CMAKE_CATCH_DISCOVER_TESTS_DISCOVERY_MODE)
      set(CMAKE_CATCH_DISCOVER_TESTS_DISCOVERY_MODE "POST_BUILD")
    endif()
    set(_DISCOVERY_MODE ${CMAKE_CATCH_DISCOVER_TESTS_DISCOVERY_MODE})
  endif()
  if(NOT _DISCOVERY_MODE MATCHES "^(POST_BUILD|PRE_TEST)$")
    message(FATAL_ERROR "Unknown DISCOVERY_MODE: ${_DISCOVERY_MODE}")
  endif()

  string(SHA1 args_hash "${_TEST_SPEC} ${_EXTRA_ARGS} ${_REPORTER} ${_OUTPUT_DIR} ${_OUTPUT_PREFIX} ${_OUTPUT_SUFFIX}")
  string(SUBSTRING ${args_hash} 0 7 args_hash)

  set(ctest_file_base "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-${args_hash}")
  set(ctest_include_file "${ctest_file_base}_include.cmake")
  set(ctest_tests_file "${ctest_file_base}_tests.cmake")

  get_property(crosscompiling_emulator
    TARGET ${TARGET}
    PROPERTY CROSSCOMPILING_EMULATOR
  )
  if(NOT _SKIP_IS_FAILURE)
    set(_PROPERTIES ${_PROPERTIES} SKIP_RETURN_CODE 4)
  endif()

  if(_DISCOVERY_MODE STREQUAL "POST_BUILD")
    add_custom_command(
      TARGET ${TARGET} POST_BUILD
      BYPRODUCTS "${ctest_tests_file}"
      COMMAND "${CMAKE_COMMAND}"
              -D "TEST_TARGET=${TARGET}"
              -D "TEST_EXECUTABLE=$<TARGET_FILE:${TARGET}>"
              -D "TEST_EXECUTOR=${crosscompiling_emulator}"
              -D "TEST_WORKING_DIR=${_WORKING_DIRECTORY}"
              -D "TEST_SPEC=${_TEST_SPEC}"
              -D "TEST_EXTRA_ARGS=${_EXTRA_ARGS}"
              -D "TEST_PROPERTIES=${_PROPERTIES}"
              -D "TEST_PREFIX=${_TEST_PREFIX}"
              -D "TEST_SUFFIX=${_TEST_SUFFIX}"
              -D "TEST_LIST=${_TEST_LIST}"
              -D "TEST_REPORTER=${_REPORTER}"
              -D "TEST_OUTPUT_DIR=${_OUTPUT_DIR}"
              -D "TEST_OUTPUT_PREFIX=${_OUTPUT_PREFIX}"
              -D "TEST_OUTPUT_SUFFIX=${_OUTPUT_SUFFIX}"
              -D "TEST_DL_PATHS=${_DL_PATHS}"
              -D "TEST_DL_FRAMEWORK_PATHS=${_DL_FRAMEWORK_PATHS}"
              -D "CTEST_FILE=${ctest_tests_file}"
              -D "ADD_TAGS_AS_LABELS=${_ADD_TAGS_AS_LABELS}"
              -P "${_CATCH_DISCOVER_TESTS_SCRIPT}"
      VERBATIM
    )

    file(WRITE "${ctest_include_file}"
      "if(EXISTS \"${ctest_tests_file}\")\n"
      "  include(\"${ctest_tests_file}\")\n"
      "else()\n"
      "  add_test(${TARGET}_NOT_BUILT-${args_hash} ${TARGET}_NOT_BUILT-${args_hash})\n"
      "endif()\n"
    )

  elseif(_DISCOVERY_MODE STREQUAL "PRE_TEST")

    get_property(GENERATOR_IS_MULTI_CONFIG GLOBAL
        PROPERTY GENERATOR_IS_MULTI_CONFIG
    )

    if(GENERATOR_IS_MULTI_CONFIG)
      set(ctest_tests_file "${ctest_file_base}_tests-$<CONFIG>.cmake")
    endif()

    string(CONCAT ctest_include_content
      "if(EXISTS \"$<TARGET_FILE:${TARGET}>\")"                                    "\n"
      "  if(NOT EXISTS \"${ctest_tests_file}\" OR"                                 "\n"
      "     NOT \"${ctest_tests_file}\" IS_NEWER_THAN \"$<TARGET_FILE:${TARGET}>\" OR\n"
      "     NOT \"${ctest_tests_file}\" IS_NEWER_THAN \"\${CMAKE_CURRENT_LIST_FILE}\")\n"
      "    include(\"${_CATCH_DISCOVER_TESTS_SCRIPT}\")"                           "\n"
      "    catch_discover_tests_impl("                                             "\n"
      "      TEST_EXECUTABLE"        " [==[" "$<TARGET_FILE:${TARGET}>"   "]==]"   "\n"
      "      TEST_EXECUTOR"          " [==[" "${crosscompiling_emulator}" "]==]"   "\n"
      "      TEST_WORKING_DIR"       " [==[" "${_WORKING_DIRECTORY}"      "]==]"   "\n"
      "      TEST_SPEC"              " [==[" "${_TEST_SPEC}"              "]==]"   "\n"
      "      TEST_EXTRA_ARGS"        " [==[" "${_EXTRA_ARGS}"             "]==]"   "\n"
      "      TEST_PROPERTIES"        " [==[" "${_PROPERTIES}"             "]==]"   "\n"
      "      TEST_PREFIX"            " [==[" "${_TEST_PREFIX}"            "]==]"   "\n"
      "      TEST_SUFFIX"            " [==[" "${_TEST_SUFFIX}"            "]==]"   "\n"
      "      TEST_LIST"              " [==[" "${_TEST_LIST}"              "]==]"   "\n"
      "      TEST_REPORTER"          " [==[" "${_REPORTER}"               "]==]"   "\n"
      "      TEST_OUTPUT_DIR"        " [==[" "${_OUTPUT_DIR}"             "]==]"   "\n"
      "      TEST_OUTPUT_PREFIX"     " [==[" "${_OUTPUT_PREFIX}"          "]==]"   "\n"
      "      TEST_OUTPUT_SUFFIX"     " [==[" "${_OUTPUT_SUFFIX}"          "]==]"   "\n"
      "      CTEST_FILE"             " [==[" "${ctest_tests_file}"        "]==]"   "\n"
      "      TEST_DL_PATHS"          " [==[" "${_DL_PATHS}"               "]==]"   "\n"
      "      TEST_DL_FRAMEWORK_PATHS" " [==[" "${_DL_FRAMEWORK_PATHS}"     "]==]"   "\n"
      "      ADD_TAGS_AS_LABELS"     " [==[" "${_ADD_TAGS_AS_LABELS}"     "]==]"   "\n"
      "    )"                                                                      "\n"
      "  endif()"                                                                  "\n"
      "  include(\"${ctest_tests_file}\")"                                         "\n"
      "else()"                                                                     "\n"
      "  add_test(${TARGET}_NOT_BUILT ${TARGET}_NOT_BUILT)"                        "\n"
      "endif()"                                                                    "\n"
    )

    if(GENERATOR_IS_MULTI_CONFIG)
      foreach(_config ${CMAKE_CONFIGURATION_TYPES})
        file(GENERATE OUTPUT "${ctest_file_base}_include-${_config}.cmake" CONTENT "${ctest_include_content}" CONDITION $<CONFIG:${_config}>)
      endforeach()
      string(CONCAT ctest_include_multi_content
        "if(NOT CTEST_CONFIGURATION_TYPE)"                                              "\n"
        "  message(\"No configuration for testing specified, use '-C <cfg>'.\")"        "\n"
        "else()"                                                                        "\n"
        "  include(\"${ctest_file_base}_include-\${CTEST_CONFIGURATION_TYPE}.cmake\")"  "\n"
        "endif()"                                                                       "\n"
      )
      file(GENERATE OUTPUT "${ctest_include_file}" CONTENT "${ctest_include_multi_content}")
    else()
      file(GENERATE OUTPUT "${ctest_file_base}_include.cmake" CONTENT "${ctest_include_content}")
      file(WRITE "${ctest_include_file}" "include(\"${ctest_file_base}_include.cmake\")")
    endif()
  endif()

  set_property(DIRECTORY
    APPEND PROPERTY TEST_INCLUDE_FILES "${ctest_include_file}"
  )

endfunction()


set(_CATCH_DISCOVER_TESTS_SCRIPT
  ${CMAKE_CURRENT_LIST_DIR}/CatchAddTests.cmake
  CACHE INTERNAL "Catch2 full path to CatchAddTests.cmake helper file"
)
