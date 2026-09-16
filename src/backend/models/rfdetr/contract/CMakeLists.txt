include(MmltkComponent)

add_library(mmltk_backend_models_rfdetr_contract STATIC)
add_library(
    mmltk::backend::models::rfdetr::contract
    ALIAS mmltk_backend_models_rfdetr_contract
)
mmltk_configure_component(
    mmltk_backend_models_rfdetr_contract PUBLIC_DECLARATIONS)

mmltk_register_public_headers(
    mmltk_backend_models_rfdetr_contract
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        class_layout.h
        evaluation_metrics.h
        training_metrics.h
        output_roles.h
        artifacts.h
        cli.h
        contract.h
        model_config.h
        preset_catalog.h
        prediction_limits.h
        train_recipe.h
        training_supervision.h
        weight_catalog.h
        workflow_requests.h)
mmltk_register_ordinary_sources(
    mmltk_backend_models_rfdetr_contract
    model_config.cpp
    weight_catalog.cpp
    workflow_requests.cpp
)
target_link_libraries(mmltk_backend_models_rfdetr_contract
    PUBLIC
        mmltk::backend::models::contract
        mmltk::backend::data::catalog
        mmltk::frameworks::reflection
    PRIVATE
        mmltk::common::types
)

function(mmltk_add_backend_models_rfdetr_contract_tests)
    set(sources
        ${CMAKE_SOURCE_DIR}/src/backend/models/rfdetr/contract/tests/training_supervision.test.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/models/rfdetr/contract/tests/weight_catalog.test.cpp)
    if(BUILD_RFDETR_NATIVE)
        list(APPEND sources
            ${CMAKE_SOURCE_DIR}/src/backend/models/rfdetr/contract/tests/cli_aliases.test.cpp)
    endif()
    add_executable(mmltk_backend_models_rfdetr_contract_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_backend_models_rfdetr_contract_tests PRIVATE_DECLARATIONS)
    mmltk_register_retained_module_importers(
        mmltk_backend_models_rfdetr_contract_tests ${sources})
    target_link_libraries(
        mmltk_backend_models_rfdetr_contract_tests
        PRIVATE
            mmltk_test_support
            mmltk::backend::models::rfdetr::contract
            mmltk::frameworks::serialization
    )
    if(BUILD_RFDETR_NATIVE)
        target_link_libraries(
            mmltk_backend_models_rfdetr_contract_tests PRIVATE
            mmltk_entrypoints_cli_test_support)
    endif()
    catch_discover_tests(
        mmltk_backend_models_rfdetr_contract_tests
        TEST_PREFIX "backend::models::rfdetr::contract::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
