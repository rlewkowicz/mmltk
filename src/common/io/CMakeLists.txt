include(MmltkComponent)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(mmltk_common_io STATIC)
add_library(mmltk::common::io ALIAS mmltk_common_io)
mmltk_configure_component(mmltk_common_io PUBLIC_DECLARATIONS)

mmltk_register_public_headers(mmltk_common_io
    BASE_DIRS ${CMAKE_SOURCE_DIR}
    FILES
        event_fd.h
        file_digest.h
        file_memory.h
        json_file.h
        noexcept_io.h
        scoped_fd.h
        staging_directory.h
)
mmltk_register_ordinary_sources(
    mmltk_common_io
    file_digest.cpp
    file_memory.cpp
    event_fd.cpp
    noexcept_io.cpp
    scoped_fd.cpp
    staging_directory.cpp
    json_file.cpp
    path_cleanup.cpp)

target_link_libraries(mmltk_common_io
    PUBLIC
        mmltk::common::math
        mmltk::common::system
        mmltk::common::types
        nlohmann_json
    PRIVATE
        OpenSSL::Crypto
)
