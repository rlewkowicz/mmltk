add_library(mmltk_controller_application_runtime INTERFACE)
add_library(mmltk::controller::application_runtime ALIAS
    mmltk_controller_application_runtime)
target_link_libraries(mmltk_controller_application_runtime INTERFACE
    mmltk::controller::annotation_system
    mmltk::controller::data_compute_systems
    mmltk::controller::direct_services
    mmltk::controller::explore_system
    mmltk::controller::live_system
    mmltk::controller::presentation_system
    mmltk::controller::upscale_system)
