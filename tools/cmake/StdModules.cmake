include_guard(GLOBAL)

# The experimental UUID is specific to the pinned CMake release and must be
# set before project() discovers CXX. Targets opt in through Toolchain.cmake.
if(NOT CMAKE_VERSION VERSION_EQUAL "4.4.3")
    message(FATAL_ERROR "Native tools standard modules require exactly CMake 4.4.3")
endif()
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
