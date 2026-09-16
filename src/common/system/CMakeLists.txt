include(MmltkComponent)

add_library(mmltk_common_system STATIC)
add_library(mmltk::common::system ALIAS mmltk_common_system)
mmltk_configure_component(mmltk_common_system PUBLIC_DECLARATIONS)

mmltk_register_public_headers(mmltk_common_system
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        cpu_affinity.h
        execution_policy.h
        numa_topology.h
        numa_memory.h
        time_utils.h
        runtime_paths.h
)
mmltk_register_ordinary_sources(
    mmltk_common_system
    cpu_affinity.cpp
    execution_policy.cpp
    numa_topology.cpp
    numa_memory.cpp
    runtime_paths.cpp
    time_utils.cpp)

target_link_libraries(mmltk_common_system
    PUBLIC
        mmltk::common::types
        Threads::Threads
        numa
)

add_library(mmltk_common_system_test_support INTERFACE)
mmltk_register_public_headers(mmltk_common_system_test_support
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES tests/denied_syscall.h)
target_link_libraries(mmltk_common_system_test_support INTERFACE mmltk::common::system)

function(mmltk_add_common_system_tests)
    add_executable(mmltk_common_system_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(mmltk_common_system_tests PRIVATE_DECLARATIONS)
    mmltk_register_ordinary_sources(mmltk_common_system_tests
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/execution_policy.test.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/numa_memory.test.cpp)
    target_link_libraries(mmltk_common_system_tests PRIVATE
        mmltk_common_system_test_support
        mmltk_test_support mmltk::common::system mmltk::common::concurrency)
    catch_discover_tests(mmltk_common_system_tests TEST_PREFIX "common::system::"
        DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)
endfunction()
