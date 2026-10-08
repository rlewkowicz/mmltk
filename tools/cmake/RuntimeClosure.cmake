include_guard(GLOBAL)

function(mmltk_tools_install_runtime target)
    # DT_RPATH also applies to indirect dependencies in the adjacent closure;
    # a host invocation never depends on a build-container library directory.
    set_target_properties(${target} PROPERTIES
        BUILD_RPATH "/opt/gcc-16.2/lib64"
        INSTALL_RPATH "$ORIGIN/lib"
        INSTALL_RPATH_USE_LINK_PATH OFF)
    target_link_options(${target} PRIVATE -Wl,--disable-new-dtags)
    install(TARGETS ${target} RUNTIME_DEPENDENCY_SET ${target}_runtime
        RUNTIME DESTINATION . COMPONENT Runtime)
    install(RUNTIME_DEPENDENCY_SET ${target}_runtime
        DIRECTORIES "/opt/gcc-16.2/lib64"
        PRE_EXCLUDE_REGEXES "^linux-vdso\\.so"
        POST_EXCLUDE_REGEXES
            "/ld-linux[^/]*\\.so[^/]*$"
            "/lib(c|m|pthread|dl|rt|resolv)\\.so\\.[0-9]+$"
        LIBRARY DESTINATION lib COMPONENT Runtime)
endfunction()
