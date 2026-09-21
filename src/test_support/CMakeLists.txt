include(MmltkComponent)
add_library(mmltk_test_support STATIC EXCLUDE_FROM_ALL)
mmltk_configure_component(mmltk_test_support PUBLIC_DECLARATIONS)
mmltk_register_retained_module_importers(mmltk_test_support
    catch_main.cpp environment_test_utils.cpp filesystem_test_utils.cpp profile_runner_common.cpp)
mmltk_register_public_headers(mmltk_test_support
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
    console_output.h
    async_test_utils.hpp
    cuda_test_utils.hpp
    filesystem_test_utils.hpp
    environment_test_utils.hpp
    linux_process_test_utils.hpp
    error_expectation_test_utils.hpp
    subprocess_test_utils.hpp
    profile_runner_common.h
    cli_option_table.h)
target_link_libraries(mmltk_test_support PUBLIC
    Catch2::Catch2 CUDA::cudart mmltk::common::io mmltk::common::logging stb)
mmltk_register_header_isolation(mmltk_test_support)

add_executable(mmltk_test_support_tests EXCLUDE_FROM_ALL)
mmltk_configure_component(mmltk_test_support_tests PRIVATE_DECLARATIONS)
mmltk_register_ordinary_sources(mmltk_test_support_tests console_output.test.cpp)
target_link_libraries(mmltk_test_support_tests PRIVATE mmltk_test_support)
catch_discover_tests(mmltk_test_support_tests TEST_PREFIX "test-support::"
    DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)
