if(NOT DEFINED OUTPUT_PATH OR OUTPUT_PATH STREQUAL "")
    message(FATAL_ERROR "OUTPUT_PATH must be provided")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/mmltk_cef_distribution.cmake")

mmltk_write_pinned_cef_distribution_env("${OUTPUT_PATH}")
