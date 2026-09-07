cmake_minimum_required(VERSION 3.17)
# CMake 3.17 added LibArchive::LibArchive target to FindLibArchive

project("Indexed BZip2/Gzip Decoder" CXX)

option(LIBRAPIDARCHIVE_CODE_COVERAGE "Enable coverage reporting" OFF)
option(LIBRAPIDARCHIVE_USE_SYSTEM_ZLIB "Use the system-installed zlib library instead of compiling it from source" OFF)
option(LIBRAPIDARCHIVE_USE_ZLIB_NG
       "Statically compile with zlib-ng instead of zlib. Will be ignored if LIBRAPIDARCHIVE_USE_SYSTEM_ZLIB=ON." ON)
option(LIBRAPIDARCHIVE_WITH_RPMALLOC "Compile with rpmalloc for faster memory allocations" ON)
option(LIBRAPIDARCHIVE_WITH_ISAL "Compile with ISA-l for more than twice as fast decompression than zlib" ON)
option(LIBRAPIDARCHIVE_RUN_CLANG_TIDY "Runs clang-tidy while building targets" OFF)

if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "riscv64")
    set(LIBRAPIDARCHIVE_WITH_RPMALLOC OFF)
endif()

# There are too many bugs on macOS and I don't have a local test system.
# See https://github.com/mxmlnkn/indexed_bzip2/pull/22#issuecomment-3156326658
# Help from macOS users in the form of a PR fixing the linked issue would be very welcome!
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(WITH_ISAL OFF)
endif()

set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)

# https://stackoverflow.com/questions/71797349/is-it-possible-to-ignore-a-header-with-clang-tidy
# Clang-tidy has no negative matching, so painstakingly list all except for the "external" folder.
# Not listing the "benchmark" and "test" folders also shouldn't hurt.
set(CLANG_TIDY_COMMAND "clang-tidy;--config-file;${CMAKE_CURRENT_SOURCE_DIR}/src/.clang-tidy;--header-filter=/src/\(core\|huffman\|indexed_bzip2\|rapidgzip\|tools\)")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Chile the build type: Debug, Release, RelWithDebInfo" FORCE)
endif()

add_compile_options(
    "$<$<OR:$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>,$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>>:-Wall;-Wextra;-Wshadow;-Werror=return-type;-Wno-unknown-attributes>"
    "$<$<OR:$<COMPILE_LANG_AND_ID:CXX,GNU>,$<COMPILE_LANG_AND_ID:C,GNU>>:-Wall;-Wextra;-Wshadow;-Wunused;-Werror=return-type;-Wno-attributes>"
    "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wsuggest-override>"
    "$<$<AND:$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>,$<BOOL:${LIBRAPIDARCHIVE_CODE_COVERAGE}>>:-O0;-g;--coverage>"
    # Add some hardening. See e.g.:
    # https://www.phoronix.com/news/GCC-fhardened-Hardening-Option
    # https://developers.redhat.com/blog/2018/03/21/compiler-and-linker-flags-gcc
    # I have not observed any performance impact for these.
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fpie;-fstack-protector-strong>"
    # -fstack-clash-protection crashes the MINGW compiler https://github.com/msys2/MINGW-packages/issues/5348
    "$<$<AND:$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>,$<NOT:$<PLATFORM_ID:Windows>>>:-fstack-clash-protection>"
    # Fix error with MINGW: Fatal error: can't write 94 bytes to section .text of testCLI.cpp.obj: 'file too big'
    "$<$<AND:$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>,$<PLATFORM_ID:Windows>>:-Wa,-mbig-obj>"
    "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/bigobj>"
)

# I had some issues with the per-target enabling of C++20 not working on Windows -.-. This works.
if(${LIBRAPIDARCHIVE_BUILD_TEST} OR ${SLIBRAPIDARCHIVE_BUILD_BENCHMARKS})
    add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/std:c++20>")
endif()

if(NOT ${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm" AND NOT ${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch" AND NOT ${CMAKE_SYSTEM_PROCESSOR} MATCHES "riscv64")
    add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fcf-protection=full>")
endif()

add_link_options(
    "$<$<AND:$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>,$<BOOL:${LIBRAPIDARCHIVE_CODE_COVERAGE}>>:--coverage>"
    # See the note about hardening inside add_compile_options.
    "$<$<AND:$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>,$<NOT:$<PLATFORM_ID:Windows>>>:-Wl,-pie,-z,relro,-z,now,-z,defs>"
)

if(LIBRAPIDARCHIVE_CODE_COVERAGE)
    add_definitions(-DSHORT_TESTS -D_FORTIFY_SOURCE=2 -D_GLIBCXX_ASSERTIONS)
endif()


if(LIBRAPIDARCHIVE_USE_SYSTEM_ZLIB)
    # Previously, I was looking for a static library, maybe to not introduce confusion about the zlibstatic alias.
    # However, I do not see why that is necessary. I guess the best way forward would be yet another option, something
    # like USE_STATIC_SYSTEM_ZLIB, and then use the CMake 3.24+ feature set(ZLIB_USE_STATIC_LIBS "ON") for find_package.
    # It would have to be guarded with if(CMAKE_VERSION VERSION_LESS "3.24.0").
    # Maybe ZLIB_USE_STATIC_LIBS could or already is exposed with -DZLIB_USE_STATIC_LIBS, so all of that is unnecessary?
    find_package(ZLIB REQUIRED)
    add_library(zlibstatic ALIAS ZLIB::ZLIB)
endif()
find_package(LibArchive QUIET)
find_package(Threads REQUIRED)


add_subdirectory(src)


# Add beautify target

find_package(Git QUIET)

if(GIT_FOUND)
    add_custom_target(beautify-all
        COMMAND ${GIT_EXECUTABLE} ls-tree -r --name-only HEAD > ${CMAKE_BINARY_DIR}/.beautify.lst
        COMMAND sed -i -E "/[.](h|c)(pp)?$/!d; /external\\//d; /indexed_bzip2.cpp/d;" ${CMAKE_BINARY_DIR}/.beautify.lst
        COMMAND uncrustify -c uncrustify.cfg -F ${CMAKE_BINARY_DIR}/.beautify.lst --no-backup
        VERBATIM
        # git ls-tree needs to be executed in git root
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endif()
