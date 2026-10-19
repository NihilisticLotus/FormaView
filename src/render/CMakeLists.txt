qt_add_library(formaview_render STATIC render_item.cpp ${PROJECT_SOURCE_DIR}/include/formaview/render_item.hpp)
target_include_directories(formaview_render PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(formaview_render PUBLIC Qt6::Quick PRIVATE Qt6::GuiPrivate formaview_options dxgi)
target_compile_definitions(formaview_render PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
qt_add_shaders(formaview_render "preview_shaders"
    PREFIX "/shaders"
    FILES preview.vert preview.frag
)
add_library(FormaView::Render ALIAS formaview_render)
if(BUILD_TESTING AND TARGET formaview_professional)
    qt_add_executable(formaview_render_probe ${PROJECT_SOURCE_DIR}/tests/render_probe.cpp)
    target_link_libraries(formaview_render_probe PRIVATE FormaView::Render FormaView::Professional formaview_options psapi)
    target_compile_definitions(formaview_render_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
