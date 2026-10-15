# External validation harness. This file is never copied into the old source.
# The same file runs at link preparation to observe the historical identity API.
if(DEFINED MMLTK_BASELINE_ARCHIVE)
    execute_process(COMMAND "${MMLTK_BASELINE_NM}" --defined-only --extern-only
        "${MMLTK_BASELINE_ARCHIVE}" OUTPUT_VARIABLE symbols
        COMMAND_ERROR_IS_FATAL ANY)
    string(REGEX MATCHALL "_ZN[^ \n]*training_dataset_identity[^ \n]*" matches "${symbols}")
    list(REMOVE_DUPLICATES matches)
    list(LENGTH matches count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "Expected one original training_dataset_identity symbol, got ${count}")
    endif()
    list(GET matches 0 symbol)
    file(WRITE "${MMLTK_BASELINE_WRAP_HEADER}"
        "#pragma once\n#define MMLTK_BASELINE_REAL \"__real_${symbol}\"\n#define MMLTK_BASELINE_WRAP \"__wrap_${symbol}\"\n")
    file(WRITE "${MMLTK_BASELINE_WRAP_OPTIONS}" "-Wl,--wrap=${symbol}\n")
    return()
endif()

if(NOT TARGET mmltk_backend_models_rfdetr_training)
    get_property(scheduled GLOBAL PROPERTY MMLTK_BASELINE_HARNESS_SCHEDULED)
    if(scheduled)
        return()
    endif()
    set_property(GLOBAL PROPERTY MMLTK_BASELINE_HARNESS_SCHEDULED TRUE)
    cmake_language(EVAL CODE
        "cmake_language(DEFER DIRECTORY \"${CMAKE_SOURCE_DIR}\" CALL include \"${CMAKE_CURRENT_LIST_FILE}\")")
    return()
endif()
if(TARGET mmltk_rfdetr_baseline_session)
    return()
endif()

set(wrap_header "${CMAKE_BINARY_DIR}/historical_identity_wrap.h")
set(wrap_options "${CMAKE_BINARY_DIR}/historical_identity_wrap.rsp")
add_custom_command(OUTPUT "${wrap_header}" "${wrap_options}"
    COMMAND "${CMAKE_COMMAND}"
        "-DMMLTK_BASELINE_ARCHIVE=$<TARGET_FILE:mmltk_backend_models_rfdetr_training>"
        "-DMMLTK_BASELINE_NM=${CMAKE_NM}"
        "-DMMLTK_BASELINE_WRAP_HEADER=${wrap_header}"
        "-DMMLTK_BASELINE_WRAP_OPTIONS=${wrap_options}"
        -P "${CMAKE_CURRENT_LIST_FILE}"
    DEPENDS mmltk_backend_models_rfdetr_training "${CMAKE_CURRENT_LIST_FILE}"
    VERBATIM)
add_executable(mmltk_rfdetr_baseline_session EXCLUDE_FROM_ALL)
mmltk_configure_component(mmltk_rfdetr_baseline_session PRIVATE_DECLARATIONS)
mmltk_register_ordinary_sources(mmltk_rfdetr_baseline_session
    "${CMAKE_CURRENT_LIST_DIR}/rfdetr_baseline_session.cpp")
target_sources(mmltk_rfdetr_baseline_session PRIVATE "${wrap_header}")
target_include_directories(mmltk_rfdetr_baseline_session PRIVATE
    "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
target_link_libraries(mmltk_rfdetr_baseline_session PRIVATE
    mmltk::backend::models::rfdetr::training mmltk_backend_data_test_support
    mmltk_backend_ml_torch_test_support mmltk::common::io nlohmann_json)
target_link_options(mmltk_rfdetr_baseline_session PRIVATE "@${wrap_options}")
set_property(TARGET mmltk_rfdetr_baseline_session APPEND PROPERTY LINK_DEPENDS "${wrap_options}")
mmltk_enable_native_runtime_rpath(mmltk_rfdetr_baseline_session)
