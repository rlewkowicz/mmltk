function(mmltk_target_project_dirs target)
    target_include_directories(${target} PUBLIC ${CMAKE_SOURCE_DIR}/include)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
endfunction()

function(mmltk_target_private_src_dir target)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
endfunction()

function(mmltk_target_warning_options target visibility)
    target_compile_options(${target} ${visibility}
        -Wall
        -Wextra
        -Wpedantic
    )
endfunction()

function(mmltk_target_cuda_architectures target)
    if(DEFINED CMAKE_CUDA_ARCHITECTURES AND NOT CMAKE_CUDA_ARCHITECTURES STREQUAL "")
        set_target_properties(${target} PROPERTIES
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        )
    endif()
endfunction()

function(mmltk_enable_torch target visibility)
    if(NOT MMLTK_NEEDS_TORCH)
        message(FATAL_ERROR "Torch support requested for ${target} but Torch is not enabled")
    endif()
    target_include_directories(${target} SYSTEM ${visibility} ${TORCH_INCLUDE_DIRS})
    target_compile_options(${target} ${visibility} ${TORCH_CXX_FLAGS_LIST})
    if(TARGET mmltk_torch_warning_suppression_options)
        target_link_libraries(${target} ${visibility} mmltk_torch_warning_suppression_options)
    endif()
    target_link_libraries(${target} ${visibility} ${TORCH_LIBRARIES})
    if(MMLTK_TORCH_EXTRA_LIBRARIES)
        target_link_libraries(${target} ${visibility} ${MMLTK_TORCH_EXTRA_LIBRARIES})
    endif()
endfunction()

function(mmltk_prefer_system_protobuf_headers target visibility)
    if(DEFINED MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR AND
       NOT MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR STREQUAL "")
        target_compile_options(${target} BEFORE ${visibility}
            "-I${MMLTK_SYSTEM_PROTOBUF_REDIRECT_DIR}")
    elseif(Protobuf_INCLUDE_DIRS)
        foreach(_mmltk_protobuf_include_dir IN LISTS Protobuf_INCLUDE_DIRS)
            target_compile_options(${target} BEFORE ${visibility} "-I${_mmltk_protobuf_include_dir}")
        endforeach()
    endif()
endfunction()

function(mmltk_enable_python_checkpoint_loader target visibility)
    if(NOT MMLTK_NEEDS_PYTHON)
        message(FATAL_ERROR "Python checkpoint loader requested for ${target} but it is not enabled")
    endif()
    target_compile_definitions(${target} ${visibility}
        MMLTK_RFDETR_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
        MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE="${MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE}"
    )
endfunction()

function(mmltk_enable_native_runtime_rpath target)
    set(_mmltk_build_rpath "")
    foreach(_mmltk_system_libdir IN ITEMS
            /lib64
            /usr/lib64
            /lib/x86_64-linux-gnu
            /usr/lib/x86_64-linux-gnu)
        if(EXISTS "${_mmltk_system_libdir}")
            list(APPEND _mmltk_build_rpath "${_mmltk_system_libdir}")
        endif()
    endforeach()
    if(DEFINED TORCH_LIBRARY_DIR AND NOT TORCH_LIBRARY_DIR STREQUAL "")
        list(APPEND _mmltk_build_rpath "${TORCH_LIBRARY_DIR}")
    endif()
    if(DEFINED ONNXRUNTIME_LIBRARY_DIR AND NOT ONNXRUNTIME_LIBRARY_DIR STREQUAL "")
        list(APPEND _mmltk_build_rpath "${ONNXRUNTIME_LIBRARY_DIR}")
    endif()
    if(DEFINED MMLTK_ONNX_LIBRARY_DIR AND NOT MMLTK_ONNX_LIBRARY_DIR STREQUAL "")
        list(APPEND _mmltk_build_rpath "${MMLTK_ONNX_LIBRARY_DIR}")
    endif()
    if(_mmltk_build_rpath)
        list(JOIN _mmltk_build_rpath ";" _mmltk_build_rpath_value)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "${_mmltk_build_rpath_value}")
    endif()
    set(_mmltk_install_rpath
        "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
        "${MMLTK_INSTALL_TORCH_ROOT}/lib"
        "${MMLTK_CUDA_TOOLKIT_ROOT}/lib64"
    )
    list(JOIN _mmltk_install_rpath ";" _mmltk_install_rpath_value)
    set_target_properties(${target} PROPERTIES INSTALL_RPATH "${_mmltk_install_rpath_value}")
endfunction()

function(mmltk_install_target target)
    set(options SHARED)
    cmake_parse_arguments(MMLTK_INSTALL_TARGET "${options}" "" "" ${ARGN})
    if(MMLTK_INSTALL_TARGET_SHARED)
        install(TARGETS ${target}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    else()
        install(TARGETS ${target}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()
endfunction()

function(mmltk_install_directory source destination)
    install(DIRECTORY "${source}" DESTINATION "${destination}")
endfunction()

function(mmltk_install_file source destination)
    set(options)
    set(oneValueArgs RENAME)
    cmake_parse_arguments(MMLTK_INSTALL_FILE "${options}" "${oneValueArgs}" "" ${ARGN})
    if(MMLTK_INSTALL_FILE_RENAME)
        install(FILES "${source}" DESTINATION "${destination}" RENAME "${MMLTK_INSTALL_FILE_RENAME}")
    else()
        install(FILES "${source}" DESTINATION "${destination}")
    endif()
endfunction()

function(mmltk_target_append_rpath target property path)
    if(path STREQUAL "")
        return()
    endif()
    get_target_property(_mmltk_existing_rpath ${target} ${property})
    if(_mmltk_existing_rpath STREQUAL "NOTFOUND")
        set(_mmltk_existing_rpath "")
    endif()
    set(_mmltk_rpaths ${_mmltk_existing_rpath})
    list(APPEND _mmltk_rpaths "${path}")
    list(REMOVE_DUPLICATES _mmltk_rpaths)
    set_target_properties(${target} PROPERTIES
        ${property} "${_mmltk_rpaths}"
    )
endfunction()
