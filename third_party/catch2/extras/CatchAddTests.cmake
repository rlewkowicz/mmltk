# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

function(add_command NAME)
  set(_args "")
  math(EXPR _last_arg ${ARGC}-1)
  foreach(_n RANGE 1 ${_last_arg})
    set(_arg "${ARGV${_n}}")
    if(_arg MATCHES "[^-./:a-zA-Z0-9_]")
      set(_args "${_args} [==[${_arg}]==]")
    else()
      set(_args "${_args} ${_arg}")
    endif()
  endforeach()
  set(script "${script}${NAME}(${_args})\n" PARENT_SCOPE)
endfunction()

function(catch_discover_tests_impl)

  cmake_parse_arguments(
    ""
    ""
    "TEST_EXECUTABLE;TEST_WORKING_DIR;TEST_OUTPUT_DIR;TEST_OUTPUT_PREFIX;TEST_OUTPUT_SUFFIX;TEST_PREFIX;TEST_REPORTER;TEST_SPEC;TEST_SUFFIX;TEST_LIST;CTEST_FILE"
    "TEST_EXTRA_ARGS;TEST_PROPERTIES;TEST_EXECUTOR;TEST_DL_PATHS;TEST_DL_FRAMEWORK_PATHS;ADD_TAGS_AS_LABELS"
    ${ARGN}
  )

  set(add_tags "${_ADD_TAGS_AS_LABELS}")
  set(prefix "${_TEST_PREFIX}")
  set(suffix "${_TEST_SUFFIX}")
  set(spec ${_TEST_SPEC})
  set(extra_args ${_TEST_EXTRA_ARGS})
  set(properties ${_TEST_PROPERTIES})
  set(reporter ${_TEST_REPORTER})
  set(output_dir ${_TEST_OUTPUT_DIR})
  set(output_prefix ${_TEST_OUTPUT_PREFIX})
  set(output_suffix ${_TEST_OUTPUT_SUFFIX})
  set(dl_paths ${_TEST_DL_PATHS})
  set(dl_framework_paths ${_TEST_DL_FRAMEWORK_PATHS})
  set(environment_modifications "")
  set(script)
  set(suite)
  set(tests)

  if(WIN32)
    set(dl_paths_variable_name PATH)
  elseif(APPLE)
    set(dl_paths_variable_name DYLD_LIBRARY_PATH)
  else()
    set(dl_paths_variable_name LD_LIBRARY_PATH)
  endif()

  if(NOT EXISTS "${_TEST_EXECUTABLE}")
    message(FATAL_ERROR
      "Specified test executable '${_TEST_EXECUTABLE}' does not exist"
    )
  endif()

  if(dl_paths)
    cmake_path(CONVERT "$ENV{${dl_paths_variable_name}}" TO_NATIVE_PATH_LIST env_dl_paths)
    list(PREPEND env_dl_paths "${dl_paths}")
    cmake_path(CONVERT "${env_dl_paths}" TO_NATIVE_PATH_LIST paths)
    set(ENV{${dl_paths_variable_name}} "${paths}")
  endif()

  if(APPLE AND dl_framework_paths)
    cmake_path(CONVERT "$ENV{DYLD_FRAMEWORK_PATH}" TO_NATIVE_PATH_LIST env_dl_framework_paths)
    list(PREPEND env_dl_framework_paths "${dl_framework_paths}")
    cmake_path(CONVERT "${env_dl_framework_paths}" TO_NATIVE_PATH_LIST paths)
    set(ENV{DYLD_FRAMEWORK_PATH} "${paths}")
  endif()

  execute_process(
    COMMAND ${_TEST_EXECUTOR} "${_TEST_EXECUTABLE}" ${spec} --list-tests --reporter json
    OUTPUT_VARIABLE listing_output
    RESULT_VARIABLE result
    WORKING_DIRECTORY "${_TEST_WORKING_DIR}"
  )
  if(NOT ${result} EQUAL 0)
    message(FATAL_ERROR
      "Error listing tests from executable '${_TEST_EXECUTABLE}':\n"
      "  Result: ${result}\n"
      "  Output: ${listing_output}\n"
    )
  endif()

  if(reporter)
    set(reporter_arg "--reporter ${reporter}")

    execute_process(
      COMMAND ${_TEST_EXECUTOR} "${_TEST_EXECUTABLE}" ${spec} ${reporter_arg} --list-reporters
      OUTPUT_VARIABLE reporter_check_output
      RESULT_VARIABLE reporter_check_result
      WORKING_DIRECTORY "${_TEST_WORKING_DIR}"
    )
    if(${reporter_check_result} EQUAL 255)
      message(FATAL_ERROR
        "\"${reporter}\" is not a valid reporter!\n"
      )
    elseif(NOT ${reporter_check_result} EQUAL 0)
      message(FATAL_ERROR
        "Error checking for reporter in test executable '${_TEST_EXECUTABLE}':\n"
        "  Result: ${reporter_check_result}\n"
        "  Output: ${reporter_check_output}\n"
      )
    endif()
  endif()

  if(output_dir AND NOT IS_ABSOLUTE ${output_dir})
    set(output_dir "${_TEST_WORKING_DIR}/${output_dir}")
    if(NOT EXISTS ${output_dir})
      file(MAKE_DIRECTORY ${output_dir})
    endif()
  endif()

  if(dl_paths)
    foreach(path ${dl_paths})
      cmake_path(NATIVE_PATH path native_path)
      list(PREPEND environment_modifications "${dl_paths_variable_name}=path_list_prepend:${native_path}")
    endforeach()
  endif()

  if(APPLE AND dl_framework_paths)
    foreach(path ${dl_framework_paths})
      cmake_path(NATIVE_PATH path native_path)
      list(PREPEND environment_modifications "DYLD_FRAMEWORK_PATH=path_list_prepend:${native_path}")
    endforeach()
  endif()

  string(JSON version GET "${listing_output}" "version")
  if(NOT version STREQUAL "1")
    message(FATAL_ERROR "Unsupported catch output version: '${version}'")
  endif()

  string(JSON test_listing GET "${listing_output}" "listings" "tests")
  string(JSON num_tests LENGTH "${test_listing}")

  if(num_tests STREQUAL "0")
    return()
  endif()

  math(EXPR num_tests "${num_tests} - 1")

  foreach(idx RANGE ${num_tests})
    string(JSON single_test GET ${test_listing} ${idx})
    string(JSON test_tags GET "${single_test}" "tags")
    string(JSON plain_name GET "${single_test}" "name")

    set(escaped_name "${plain_name}")
    foreach(char \\ , [ ] ;)
      string(REPLACE ${char} "\\${char}" escaped_name "${escaped_name}")
    endforeach(char)
    if(output_dir)
      string(REGEX REPLACE "[^A-Za-z0-9_]" "_" escaped_name_clean "${escaped_name}")
      set(output_dir_arg "--out ${output_dir}/${output_prefix}${escaped_name_clean}${output_suffix}")
    endif()

    add_command(add_test
      "${prefix}${plain_name}${suffix}"
      ${_TEST_EXECUTOR}
      "${_TEST_EXECUTABLE}"
      "${escaped_name}"
      ${extra_args}
      "${reporter_arg}"
      "${output_dir_arg}"
    )
    add_command(set_tests_properties
      "${prefix}${plain_name}${suffix}"
      PROPERTIES
      WORKING_DIRECTORY "${_TEST_WORKING_DIR}"
      ${properties}
    )

    if(add_tags)
      string(JSON num_tags LENGTH "${test_tags}")
      math(EXPR num_tags "${num_tags} - 1")
      set(tag_list "")
      if(num_tags GREATER_EQUAL "0")
        foreach(tag_idx RANGE ${num_tags})
          string(JSON a_tag GET "${test_tags}" "${tag_idx}")
          string(REPLACE ";" "\\;" a_tag "${a_tag}")
          list(APPEND tag_list "${a_tag}")
        endforeach()

        add_command(set_tests_properties
          "${prefix}${plain_name}${suffix}"
          PROPERTIES
          LABELS "${tag_list}"
        )
      endif()
    endif(add_tags)

    if(environment_modifications)
      add_command(set_tests_properties
        "${prefix}${plain_name}${suffix}"
        PROPERTIES
        ENVIRONMENT_MODIFICATION "${environment_modifications}")
    endif()

    list(APPEND tests "${prefix}${plain_name}${suffix}")
  endforeach()

  add_command(set ${_TEST_LIST} ${tests})

  file(WRITE "${_CTEST_FILE}" "${script}")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
  catch_discover_tests_impl(
    TEST_EXECUTABLE ${TEST_EXECUTABLE}
    TEST_EXECUTOR ${TEST_EXECUTOR}
    TEST_WORKING_DIR ${TEST_WORKING_DIR}
    TEST_SPEC ${TEST_SPEC}
    TEST_EXTRA_ARGS ${TEST_EXTRA_ARGS}
    TEST_PROPERTIES ${TEST_PROPERTIES}
    TEST_PREFIX ${TEST_PREFIX}
    TEST_SUFFIX ${TEST_SUFFIX}
    TEST_LIST ${TEST_LIST}
    TEST_REPORTER ${TEST_REPORTER}
    TEST_OUTPUT_DIR ${TEST_OUTPUT_DIR}
    TEST_OUTPUT_PREFIX ${TEST_OUTPUT_PREFIX}
    TEST_OUTPUT_SUFFIX ${TEST_OUTPUT_SUFFIX}
    TEST_DL_PATHS ${TEST_DL_PATHS}
    TEST_DL_FRAMEWORK_PATHS ${TEST_DL_FRAMEWORK_PATHS}
    CTEST_FILE ${CTEST_FILE}
    ADD_TAGS_AS_LABELS ${ADD_TAGS_AS_LABELS}
  )
endif()
