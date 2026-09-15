include(MmltkComponent)

add_library(mmltk_frameworks_serialization STATIC)
add_library(mmltk::frameworks::serialization ALIAS mmltk_frameworks_serialization)
mmltk_configure_component(
    mmltk_frameworks_serialization PUBLIC_DECLARATIONS)

mmltk_register_public_headers(
    mmltk_frameworks_serialization
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        serialization.h
        json_scalar.h
        reflected_json.h
        cbor_field_annotations.h
        reflected_cbor.h
        reflected_cbor_detail.h
        cbor_wire.h)
mmltk_register_ordinary_sources(
    mmltk_frameworks_serialization
    cbor_wire.cpp
    reflected_cbor.cpp)
target_link_libraries(mmltk_frameworks_serialization
    PUBLIC
        mmltk::common::io
        mmltk::common::types
        mmltk::frameworks::reflection
    PRIVATE
        nlohmann_json
)

function(mmltk_add_frameworks_serialization_tests)
    add_executable(mmltk_frameworks_serialization_tests EXCLUDE_FROM_ALL)
    mmltk_configure_component(
        mmltk_frameworks_serialization_tests PRIVATE_DECLARATIONS)
    mmltk_register_private_headers(
        mmltk_frameworks_serialization_tests
        ${CMAKE_SOURCE_DIR}/src/frameworks/serialization/tests/reflected_cbor_test_types.h)
    mmltk_register_retained_module_importers(
        mmltk_frameworks_serialization_tests
        ${CMAKE_SOURCE_DIR}/src/frameworks/serialization/tests/cbor_wire.test.cpp)
    target_link_libraries(mmltk_frameworks_serialization_tests PRIVATE
        mmltk_acceptance_support
        mmltk::frameworks::serialization
    )
    catch_discover_tests(
        mmltk_frameworks_serialization_tests
        TEST_PREFIX "frameworks::serialization::"
        DISCOVERY_MODE PRE_TEST
        ADD_TAGS_AS_LABELS
    )
endfunction()
