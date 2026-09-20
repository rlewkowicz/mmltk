include(MmltkComponent)

add_library(mmltk_controller_shell STATIC)
add_library(mmltk::controller::shell ALIAS mmltk_controller_shell)
mmltk_configure_component(mmltk_controller_shell PUBLIC_DECLARATIONS)

mmltk_register_public_headers(
    mmltk_controller_shell
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        application_shell.h application_lifecycle_event.h)
mmltk_register_private_headers(
    mmltk_controller_shell
    application_system_storage.h)
mmltk_register_ordinary_sources(
    mmltk_controller_shell
    application_shell.cpp
    application_system_storage.cpp
)
target_include_directories(mmltk_controller_shell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(mmltk_controller_shell
    PUBLIC
        mmltk::common::system
        mmltk::controller::application_runtime
        mmltk::controller::browser::direct
        mmltk::controller::contracts
        mmltk::frameworks::gpu
        mmltk::frameworks::reflection
        mmltk::frameworks::transport
    PRIVATE
        mmltk::controller::visual_runtime
        mmltk::frameworks::serialization
)
mmltk_register_header_isolation(mmltk_controller_shell)

function(mmltk_add_controller_shell_tests)
    add_executable(mmltk_controller_shell_tests EXCLUDE_FROM_ALL)
    mmltk_register_ordinary_sources(
        mmltk_controller_shell_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/application_lifecycle_contract.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/upscale_explore_wiring.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/dataset_wiring.test.cpp
    )
    mmltk_configure_component(
        mmltk_controller_shell_tests PRIVATE_DECLARATIONS)
    target_include_directories(mmltk_controller_shell_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(mmltk_controller_shell_tests PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests
    )
    target_include_directories(mmltk_controller_shell_tests SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/uWebSockets/src
    )
    target_link_libraries(mmltk_controller_shell_tests PRIVATE
        mmltk_test_support
        mmltk::controller::contracts
        mmltk::controller::shell
        mmltk_frameworks_gpu_test_support
        mmltk::controller::direct_services
        nlohmann_json
        CUDA::cudart
    )
    catch_discover_tests(
        mmltk_controller_shell_tests
        TEST_PREFIX "controller::shell::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
