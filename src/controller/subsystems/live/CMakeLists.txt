include(MmltkComponent)

add_library(mmltk_controller_live_system STATIC)
add_library(mmltk::controller::live_system ALIAS
    mmltk_controller_live_system)
mmltk_configure_component(
    mmltk_controller_live_system PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_live_system
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES live_system.h)
mmltk_register_ordinary_sources(
    mmltk_controller_live_system
    live_receiver_copy.cpp
    live_system.cpp)
mmltk_register_private_headers(
    mmltk_controller_live_system live_receiver_copy.h)
mmltk_register_retained_module_importers(
    mmltk_controller_live_system native_live_algorithm.cpp)
target_link_libraries(mmltk_controller_live_system
    PUBLIC
        mmltk::controller::application_boundary
        mmltk::controller::visual_contract
        mmltk::frameworks::gpu
    PRIVATE
        mmltk::controller::visual_runtime
        mmltk::backend::media::capture
        mmltk::backend::media::live)
mmltk_link_declarations(mmltk_controller_live_system PUBLIC
    mmltk::controller::visual_runtime)
mmltk_register_header_isolation(mmltk_controller_live_system)
