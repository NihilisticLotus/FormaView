qt_add_executable(formaview main.cpp)
qt_add_qml_module(formaview URI FormaView.Engineering QML_FILES Main.qml)
target_link_libraries(formaview PRIVATE FormaView::Core Qt6::Quick formaview_options)
if(BUILD_TESTING)
    add_test(NAME host_smoke COMMAND formaview --smoke)
    set_tests_properties(host_smoke PROPERTIES TIMEOUT 20)
    add_test(NAME host_capabilities COMMAND formaview --capabilities)
    set_tests_properties(host_capabilities PROPERTIES PASS_REGULAR_EXPRESSION "\"capabilities\":\\[\\]")
    set_tests_properties(host_smoke host_capabilities PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        ENVIRONMENT "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins;QML_IMPORT_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../qml")
endif()
