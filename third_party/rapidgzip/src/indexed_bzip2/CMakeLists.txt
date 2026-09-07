add_library(indexed_bzip2 INTERFACE)
target_include_directories(indexed_bzip2 SYSTEM INTERFACE ${CMAKE_CURRENT_LIST_DIR}/..)
target_sources(indexed_bzip2 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/bzip2.hpp
)
target_link_libraries(indexed_bzip2 INTERFACE core libhuffman)
target_compile_features(indexed_bzip2 INTERFACE cxx_std_17)
