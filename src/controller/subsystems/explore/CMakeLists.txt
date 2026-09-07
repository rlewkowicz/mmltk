include(MmltkComponent)

add_library(mmltk_controller_explore_system STATIC)
add_library(mmltk::controller::explore_system ALIAS
    mmltk_controller_explore_system)
mmltk_configure_component(
    mmltk_controller_explore_system PUBLIC_DECLARATIONS)
mmltk_register_public_headers(
    mmltk_controller_explore_system
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES explore_system.h)
mmltk_register_ordinary_sources(
    mmltk_controller_explore_system explore_system.cpp native_explore_storage.cpp)
mmltk_register_private_headers(
    mmltk_controller_explore_system native_explore_storage.h detail/gallery_stream.h)
mmltk_register_retained_module_importers(
    mmltk_controller_explore_system native_explore_algorithm.cpp gallery_stream.cpp)
target_link_libraries(mmltk_controller_explore_system
    PUBLIC
        mmltk::controller::application_boundary
        mmltk::controller::contracts
        mmltk::controller::visual_contract
        mmltk::frameworks::gpu
    PRIVATE
        mmltk::controller::visual_runtime
        mmltk::backend::data
        mmltk::backend::imaging::explore
        mmltk::backend::models::rfdetr::augmentation
        mmltk::common::concurrency
        mmltk::common::io
        mmltk::common::system
        mmltk::controller::data_compute_systems)
mmltk_link_declarations(mmltk_controller_explore_system PUBLIC
    mmltk::controller::visual_runtime)
mmltk_register_header_isolation(mmltk_controller_explore_system)
