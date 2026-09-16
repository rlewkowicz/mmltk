include(MmltkComponent)
add_library(mmltk_controller_runtime STATIC)
add_library(mmltk::controller::runtime ALIAS mmltk_controller_runtime)
mmltk_configure_component(mmltk_controller_runtime PUBLIC_DECLARATIONS)
mmltk_register_public_headers(mmltk_controller_runtime
    BASE_DIRS ${CMAKE_SOURCE_DIR} FILES local_run.h)
mmltk_register_ordinary_sources(mmltk_controller_runtime local_run.cpp)
target_link_libraries(mmltk_controller_runtime PUBLIC
    mmltk::common::system mmltk::controller::application_boundary)
mmltk_register_header_isolation(mmltk_controller_runtime)
