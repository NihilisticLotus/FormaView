function(formaview_add_windows_version target)
    if(WIN32)
        set(FORMAVIEW_RESOURCE_TARGET "${target}")
        configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/WindowsVersion.rc.in"
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc"
            @ONLY
        )
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    endif()
endfunction()
