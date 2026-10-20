cmake_minimum_required(VERSION 3.25)
project(FormaViewQtPreviewFix LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "This private-ABI Preview fix is validated only with Qt 6.12.0 MSVC x64.")
endif()
find_package(Qt6 6.12.0 EXACT REQUIRED COMPONENTS Core Gui Network Qml Quick
    CorePrivate GuiPrivate QmlPrivate QuickPrivate PacketProtocolPrivate)
qt_standard_project_setup(REQUIRES 6.12)
qt_add_library(formaview_preview_core STATIC
    core/qqmlpreviewbindingpatchcontext.cpp
    core/qqmlpreviewdiff.cpp
    core/qqmlpreviewobjectpatch.cpp)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include/private")
foreach(header IN ITEMS qqmlpreviewbindingpatchcontext_p.h qqmlpreviewdiff_p.h qqmlpreviewobjectpatch_p.h)
    configure_file("core/${header}" "include/private/${header}" COPYONLY)
endforeach()
target_include_directories(formaview_preview_core PUBLIC core "${CMAKE_CURRENT_BINARY_DIR}/include")
target_link_libraries(formaview_preview_core PUBLIC Qt6::Core Qt6::QmlPrivate)
file(GLOB preview_sources CONFIGURE_DEPENDS plugin/*.cpp plugin/*.h)
qt_add_plugin(formaview_preview_plugin SHARED CLASS_NAME QQmlPreviewServiceFactory PLUGIN_TYPE qmltooling)
target_sources(formaview_preview_plugin PRIVATE ${preview_sources})
target_link_libraries(formaview_preview_plugin PRIVATE Qt6::CorePrivate Qt6::GuiPrivate Qt6::Network
    Qt6::QuickPrivate Qt6::PacketProtocolPrivate formaview_preview_core)
set_target_properties(formaview_preview_plugin PROPERTIES OUTPUT_NAME qmldbg_preview
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/plugins/qmltooling")
add_custom_command(TARGET formaview_preview_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/plugins/qmltooling"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:formaview_preview_plugin>"
        "${CMAKE_BINARY_DIR}/plugins/qmltooling/qmldbg_preview.dll"
    VERBATIM)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/plugin-$<CONFIG>.json"
    CONTENT "{\"built\":\"$<TARGET_FILE:formaview_preview_plugin>\",\"deployed\":\"${CMAKE_BINARY_DIR}/plugins/qmltooling/qmldbg_preview.dll\"}\n")
option(FORMAVIEW_PREVIEW_BUILD_TESTS "Build isolated Preview protocol regression driver" OFF)
if(FORMAVIEW_PREVIEW_BUILD_TESTS)
    find_package(Qt6 6.12.0 EXACT REQUIRED COMPONENTS Test QuickControls2 QmlDebugPrivate)
    qt_add_executable(formaview_preview_regression tests/regression.cpp)
    target_compile_definitions(formaview_preview_regression PRIVATE QT_QML_DEBUG)
    target_link_libraries(formaview_preview_regression PRIVATE Qt6::Quick Qt6::QuickControls2
        Qt6::Test Qt6::QmlDebugPrivate)
    file(GLOB test_icons "${CMAKE_CURRENT_SOURCE_DIR}/../../src/host/icons/normal/*.svg")
    qt_add_resources(formaview_preview_regression test_icons PREFIX "/formaview"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/host" FILES ${test_icons})
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/regression-$<CONFIG>.json"
        CONTENT "{\"executable\":\"$<TARGET_FILE:formaview_preview_regression>\"}\n")
endif()
