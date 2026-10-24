qt_add_library(formaview_render STATIC render_item.cpp ${PROJECT_SOURCE_DIR}/include/formaview/render_item.hpp)
target_include_directories(formaview_render PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(formaview_render PUBLIC Qt6::Quick PRIVATE Qt6::GuiPrivate formaview_options dxgi)
target_compile_definitions(formaview_render PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
qt_add_shaders(formaview_render "preview_shaders"
    PREFIX "/shaders"
    FILES preview.vert preview.frag
)
qt_add_shaders(formaview_render "skin_shaders"
    PREFIX "/shaders"
    GLSL "300es,330"
    FILES skin.vert skin_probe.frag
)
add_library(FormaView::Render ALIAS formaview_render)
if(BUILD_TESTING AND TARGET formaview_professional)
    qt_add_executable(formaview_render_probe ${PROJECT_SOURCE_DIR}/tests/render_probe.cpp)
    target_link_libraries(formaview_render_probe PRIVATE FormaView::Render FormaView::Professional formaview_options psapi)
    target_compile_definitions(formaview_render_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(BUILD_TESTING AND TARGET formaview_core)
    qt_add_executable(formaview_skeleton_compare_tests ${PROJECT_SOURCE_DIR}/tests/skeleton_compare_tests.cpp)
    target_link_libraries(formaview_skeleton_compare_tests PRIVATE formaview_core formaview_options Qt6::Core)
    target_compile_definitions(formaview_skeleton_compare_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(TARGET formaview_fbx)
        target_link_libraries(formaview_skeleton_compare_tests PRIVATE formaview_fbx)
        target_compile_definitions(formaview_skeleton_compare_tests PRIVATE FORMAVIEW_HAVE_FBX)
    endif()
    add_test(NAME skeleton_compare_synthetic
             COMMAND formaview_skeleton_compare_tests)
    qt_add_executable(formaview_animation_post_tests ${PROJECT_SOURCE_DIR}/tests/animation_post_tests.cpp)
    target_link_libraries(formaview_animation_post_tests PRIVATE formaview_core formaview_options Qt6::Core)
    target_compile_definitions(formaview_animation_post_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME animation_post_synthetic
             COMMAND formaview_animation_post_tests)
    if(TARGET formaview_cgltf)
        qt_add_executable(formaview_gltf_anim_tests ${PROJECT_SOURCE_DIR}/tests/gltf_anim_tests.cpp)
        target_link_libraries(formaview_gltf_anim_tests PRIVATE formaview_cgltf formaview_core formaview_options Qt6::Core)
        target_compile_definitions(formaview_gltf_anim_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        add_test(NAME gltf_anim_synthetic COMMAND formaview_gltf_anim_tests)
    endif()
    qt_add_executable(formaview_animation_loop_tests ${PROJECT_SOURCE_DIR}/tests/animation_loop_tests.cpp)
    target_link_libraries(formaview_animation_loop_tests PRIVATE FormaView::Render formaview_core formaview_options psapi)
    target_compile_definitions(formaview_animation_loop_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
