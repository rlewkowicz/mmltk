include(MmltkComponent)

add_library(mmltk_common_logging STATIC)
add_library(mmltk::common::logging ALIAS mmltk_common_logging)
mmltk_configure_component(mmltk_common_logging PUBLIC_DECLARATIONS)

mmltk_register_retained_module_providers(
    mmltk_common_logging profile_utils.cppm)
mmltk_register_retained_module_providers(
    mmltk_common_logging mmltk_logging.cppm)
mmltk_register_retained_module_implementation_units(
    mmltk_common_logging
    mmltk_logging.cpp
    profile_utils.cpp)
target_include_directories(mmltk_common_logging SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
)
target_compile_definitions(mmltk_common_logging PUBLIC
    SPDLOG_HEADER_ONLY
    SPDLOG_USE_STD_FORMAT
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
)
target_link_libraries(mmltk_common_logging
    PUBLIC
        mmltk::common::io
        mmltk::common::system
        mmltk::common::types
        Threads::Threads
    PRIVATE
        CUDA::cudart
)
