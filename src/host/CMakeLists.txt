find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS QuickControls2)
qt_add_library(formaview_desktop STATIC desktop.cpp desktop.hpp)
target_link_libraries(formaview_desktop PUBLIC FormaView::Core Qt6::Quick PRIVATE formaview_options)
target_include_directories(formaview_desktop PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
qt_add_executable(formaview main.cpp)
set_target_properties(formaview PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")
set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set(desktop_qml Main.qml Theme.qml Tool.qml Field.qml Readout.qml FilesPane.qml
    Inspector.qml PreviewPane.qml TasksPane.qml SettingsDialog.qml AppMenu.qml Workspace.qml)
qt_add_qml_module(formaview URI FormaView.Engineering
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/FormaView/Engineering
    QML_FILES ${desktop_qml})
target_link_libraries(formaview PRIVATE formaview_desktop Qt6::QuickControls2 formaview_options)
if(BUILD_TESTING)
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Test)
    qt_add_executable(formaview_desktop_tests ../../tests/desktop_tests.cpp)
    set_target_properties(formaview_desktop_tests PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")
    qt_add_qml_module(formaview_desktop_tests URI FormaView.TestUI
        OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/FormaView/TestUI QML_FILES ${desktop_qml})
    target_link_libraries(formaview_desktop_tests PRIVATE formaview_desktop Qt6::QuickControls2 Qt6::Test formaview_options)
    target_compile_definitions(formaview_desktop_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(WIN32)
        target_link_libraries(formaview_desktop_tests PRIVATE dxgi)
    endif()
    set(FORMAVIEW_UI_TEST_ROOT "" CACHE PATH "External desktop test fixtures and evidence")
    if(FORMAVIEW_UI_TEST_ROOT)
        set(desktop_test_cases)
        if(FORMAVIEW_ASAN)
            # Instrumented runs check ownership and state, not release timing gates.
            set(desktop_test_cases scanSortFilterAndSelection staleCancellationAndFailure
                uiImeStateAndLanguages viewHistoryAndResourceRecovery keyboardMouseDropAndReadOnly thumbnailBudget)
        endif()
        add_test(NAME desktop_integration COMMAND formaview_desktop_tests ${desktop_test_cases}
            -o "${FORMAVIEW_UI_TEST_ROOT}/desktop-tests.txt,txt")
        set_tests_properties(desktop_integration PROPERTIES TIMEOUT 180
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
            ENVIRONMENT "FORMAVIEW_UI_TEST_ROOT=${FORMAVIEW_UI_TEST_ROOT};QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins;QML_IMPORT_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../qml")
    endif()
    add_test(NAME host_smoke COMMAND formaview --smoke)
    set_tests_properties(host_smoke PROPERTIES TIMEOUT 20)
    add_test(NAME host_capabilities COMMAND formaview --capabilities)
    set_tests_properties(host_capabilities PROPERTIES PASS_REGULAR_EXPRESSION "\"capabilities\":\\[\\]")
    set_tests_properties(host_smoke host_capabilities PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        ENVIRONMENT "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins;QML_IMPORT_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../qml")
    if(MSVC AND FORMAVIEW_ASAN)
        get_filename_component(compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set_property(TEST host_smoke host_capabilities APPEND PROPERTY
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${compiler_bin}")
        if(FORMAVIEW_UI_TEST_ROOT)
            set_property(TEST desktop_integration APPEND PROPERTY
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${compiler_bin}")
        endif()
    endif()
endif()
