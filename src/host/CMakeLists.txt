find_package(Qt6 6.12.0 EXACT REQUIRED COMPONENTS QuickControls2)
qt_add_library(formaview_desktop STATIC desktop.cpp desktop.hpp)
target_link_libraries(formaview_desktop PUBLIC FormaView::Core Qt6::Quick PRIVATE formaview_options)
target_include_directories(formaview_desktop PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(formaview_desktop PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(TARGET formaview_image_conversion)
    qt_add_qml_module(formaview_desktop URI FormaView.Images
        OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/FormaView/Images
        SOURCES image_session.cpp image_session.hpp image_view.cpp image_view.hpp image_jobs.cpp image_jobs.hpp)
    target_link_libraries(formaview_desktop PUBLIC FormaView::ImageConversion PRIVATE Qt6::GuiPrivate)
    target_compile_definitions(formaview_desktop PUBLIC FORMAVIEW_IMAGE_UI)
    qt_add_shaders(formaview_desktop image_shaders PREFIX "/shaders" FILES image.vert image.frag)
    # Qt creates separate translation units for static plugin registration.
    # Instrument them too so MSVC's STL container annotations match the host.
    foreach(plugin_target IN ITEMS formaview_desktopplugin formaview_desktopplugin_init)
        if(TARGET ${plugin_target})
            target_link_libraries(${plugin_target} PRIVATE formaview_options)
        endif()
    endforeach()
endif()
qt_add_executable(formaview main.cpp)
target_compile_definitions(formaview PRIVATE
    $<$<AND:$<BOOL:${FORMAVIEW_QML_PREVIEW}>,$<CONFIG:Debug,RelWithDebInfo>>:QT_QML_DEBUG>
    $<$<AND:$<BOOL:${FORMAVIEW_QML_PREVIEW}>,$<CONFIG:Debug,RelWithDebInfo>>:FORMAVIEW_QML_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}">)
set_target_properties(formaview PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")
set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set(desktop_qml Main.qml Theme.qml AppIcon.qml NavButton.qml NavigationRail.qml Tool.qml Field.qml Readout.qml Choice.qml AppPopupMenu.qml AppMenuItem.qml SectionHeader.qml PageTab.qml OptionCheck.qml FormGroup.qml NumberField.qml FilesPane.qml
    Inspector.qml PreviewPane.qml TasksPane.qml SettingsDialog.qml GeneralSettings.qml SettingsGroup.qml SettingsRow.qml Toggle.qml AppMenu.qml Workspace.qml)
if(TARGET formaview_image_conversion)
    list(APPEND desktop_qml ImagePane.qml ImageInspector.qml PixelPanel.qml ExposureSpinBox.qml ConversionWorkspace.qml ConversionSettings.qml ConversionInputs.qml ConversionOverrides.qml ConversionResults.qml ConversionItemRow.qml ImageJobsPane.qml McpSettings.qml)
endif()
qt_add_qml_module(formaview URI FormaView.Engineering
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/FormaView/Engineering
    QML_FILES ${desktop_qml})
target_link_libraries(formaview PRIVATE formaview_desktop Qt6::QuickControls2 formaview_options)
file(GLOB_RECURSE desktop_icons CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/icons/*.svg")
qt_add_resources(formaview ui_icons PREFIX "/formaview" BASE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${desktop_icons})
add_custom_command(TARGET formaview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/icons/UPSTREAM-LICENSE"
        "$<TARGET_FILE_DIR:formaview>/FormaView-Icons-LICENSE.txt")
if(TARGET formaview_desktopplugin)
    target_link_libraries(formaview PRIVATE formaview_desktopplugin)
endif()
if(BUILD_TESTING)
    find_package(Qt6 6.12.0 EXACT REQUIRED COMPONENTS Test)
    qt_add_executable(formaview_desktop_tests ../../tests/desktop_tests.cpp)
    qt_add_resources(formaview_desktop_tests ui_icons_test PREFIX "/formaview" BASE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${desktop_icons})
    set_target_properties(formaview_desktop_tests PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")
    qt_add_qml_module(formaview_desktop_tests URI FormaView.TestUI
        OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/FormaView/TestUI QML_FILES ${desktop_qml})
    target_link_libraries(formaview_desktop_tests PRIVATE formaview_desktop Qt6::QuickControls2 Qt6::Test formaview_options)
    if(TARGET formaview_desktopplugin)
        target_link_libraries(formaview_desktop_tests PRIVATE formaview_desktopplugin)
    endif()
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
                uiImeStateAndLanguages viewHistoryAndResourceRecovery keyboardMouseDropAndReadOnly
                gridDirectionalNavigation paneFocusRecovery parentNavigationKeepsSession thumbnailBudget)
            if(TARGET formaview_image_conversion)
                list(APPEND desktop_test_cases imagePrecisionAndViewport imageCacheInvalidation imageFormatMatrix imageStructuresAndLayers imageConversionGuiLifecycle imageGuiWriteMatrix mcpSettingsInteraction imageDirectorySelection)
            endif()
        endif()
        add_test(NAME desktop_integration COMMAND formaview_desktop_tests ${desktop_test_cases}
            -o "${FORMAVIEW_UI_TEST_ROOT}/desktop-tests.txt,txt")
        set_tests_properties(desktop_integration PROPERTIES TIMEOUT 180
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
            ENVIRONMENT "FORMAVIEW_UI_TEST_ROOT=${FORMAVIEW_UI_TEST_ROOT};QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins;QML_IMPORT_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../qml")
        if(TARGET formaview_image_conversion)
            set_tests_properties(desktop_integration PROPERTIES DEPENDS "professional_image;image_conversion")
        endif()
    endif()
    add_test(NAME host_smoke COMMAND formaview --smoke)
    set_tests_properties(host_smoke PROPERTIES TIMEOUT 20)
    add_test(NAME host_capabilities COMMAND formaview --capabilities)
    if(TARGET formaview_image_conversion)
        set_tests_properties(host_capabilities PROPERTIES PASS_REGULAR_EXPRESSION "image.convert")
    else()
        set_tests_properties(host_capabilities PROPERTIES PASS_REGULAR_EXPRESSION "\"capabilities\":\\[\\]")
    endif()
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
