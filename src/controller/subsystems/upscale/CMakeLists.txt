include(MmltkComponent)

add_library(mmltk_controller_upscale_system STATIC)
add_library(mmltk::controller::upscale_system ALIAS
    mmltk_controller_upscale_system)
mmltk_configure_component(
    mmltk_controller_upscale_system PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_upscale_system
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES upscale_system.h)
mmltk_register_retained_module_importers(
    mmltk_controller_upscale_system upscale_system.cpp)
target_link_libraries(mmltk_controller_upscale_system
    PUBLIC
        mmltk::controller::application_boundary
        mmltk::controller::visual_contract
        mmltk::frameworks::gpu
    PRIVATE
        mmltk::controller::visual_runtime
        mmltk::backend::imaging::raster
        mmltk::backend::imaging::upscale)
mmltk_link_declarations(mmltk_controller_upscale_system PUBLIC
    mmltk::controller::visual_runtime
    mmltk::backend::imaging::upscale)
mmltk_register_header_isolation(mmltk_controller_upscale_system)
