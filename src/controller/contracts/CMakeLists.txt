include(MmltkComponent)

add_library(mmltk_controller_application_boundary INTERFACE)
add_library(mmltk::controller::application_boundary ALIAS
    mmltk_controller_application_boundary)
mmltk_register_public_headers(
    mmltk_controller_application_boundary
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        application_boundary.h
        integration_control.h
        visual_source.h)
target_link_libraries(mmltk_controller_application_boundary INTERFACE
    mmltk::frameworks::reflection)

add_library(mmltk_controller_application_systems_projection INTERFACE)
add_library(mmltk::controller::application_systems_projection ALIAS
    mmltk_controller_application_systems_projection)
mmltk_register_public_headers(
    mmltk_controller_application_systems_projection
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES application_systems.h)
target_link_libraries(mmltk_controller_application_systems_projection INTERFACE
    mmltk::controller::application_boundary
    mmltk::frameworks::reflection
    mmltk::frameworks::serialization)
mmltk_link_declarations(
    mmltk_controller_application_systems_projection INTERFACE
    mmltk::controller::contracts
    mmltk::controller::direct_services
    mmltk::controller::presentation_system)
if(BUILD_RFDETR_NATIVE)
    mmltk_link_declarations(
        mmltk_controller_application_systems_projection INTERFACE
        mmltk::controller::annotation_system
        mmltk::controller::data_compute_systems
        mmltk::controller::explore_system
        mmltk::controller::live_system
        mmltk::controller::upscale_system)
endif()

add_library(mmltk_controller_contracts STATIC)
add_library(mmltk::controller::contracts ALIAS mmltk_controller_contracts)
mmltk_configure_component(mmltk_controller_contracts PUBLIC_DECLARATIONS)

mmltk_register_public_headers(
    mmltk_controller_contracts
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        annotation_limits.h
        annotation.h
        artifact.h
        compute.h
        default_state.h
        diagnostic_context.h
        explore_filter.h
        gui_settings.h
        gui_settings_mutation.h
        gui_settings_states.h
        model.h
        model_selection.h
        model_selection_types.h
        provider.h
        settings.h
        settings_commands.h
        settings_vocabulary.h
        terminal_presentation.h
        view_state.h
        workflow_path_dialogs.h
        workflows.h
        workspace.h
        workspace_input.h)

mmltk_register_ordinary_sources(
    mmltk_controller_contracts
    model_selection.cpp
    annotation_serialization.cpp
    gui_settings_mutation.cpp
    gui_settings_states.cpp
)
mmltk_register_retained_module_importers(
    mmltk_controller_contracts
    gui_settings.cpp)
target_include_directories(mmltk_controller_contracts PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mmltk_controller_contracts
    PUBLIC
        mmltk::backend::data::catalog
        mmltk::backend::data
        mmltk::controller::application_boundary
        mmltk::backend::models::catalog
        mmltk::backend::models::rfdetr::contract
        mmltk::common::types
        mmltk::frameworks::reflection
    PRIVATE
        mmltk::backend::imaging::primitives
        mmltk::common::io
        mmltk::common::logging
        mmltk::frameworks::serialization
        nlohmann_json
)
