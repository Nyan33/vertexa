add_executable(test_geom test_geom.cpp TestMain.h)
target_link_libraries(test_geom PRIVATE vx_geom)
add_test(NAME geom COMMAND test_geom)

add_executable(test_shape test_shape.cpp TestMain.h)
target_link_libraries(test_shape PRIVATE vx_core)
add_test(NAME shape COMMAND test_shape)

add_executable(test_core test_core.cpp TestMain.h)
target_link_libraries(test_core PRIVATE vx_core)
add_test(NAME core COMMAND test_core)

add_executable(test_brush test_brush.cpp TestMain.h)
target_link_libraries(test_brush PRIVATE vx_core)
add_test(NAME brush COMMAND test_brush)

add_executable(test_fla test_fla.cpp TestMain.h)
target_link_libraries(test_fla PRIVATE vx_core)
add_test(NAME fla COMMAND test_fla)

if(TARGET vx_render)
    add_executable(test_render test_render.cpp TestMain.h)
    target_link_libraries(test_render PRIVATE vx_render)
    add_test(NAME render COMMAND test_render)
    set_tests_properties(render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(TARGET vx_render)
    add_executable(test_gpu test_gpu.cpp TestMain.h)
    target_link_libraries(test_gpu PRIVATE vx_render)
    add_test(NAME gpu COMMAND test_gpu)
endif()

if(TARGET vx_app)
    add_executable(test_app test_app.cpp TestMain.h)
    target_link_libraries(test_app PRIVATE vx_app)
    add_test(NAME app COMMAND test_app)
    set_tests_properties(app PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_crash test_crash.cpp TestMain.h)
    target_link_libraries(test_crash PRIVATE vx_app)
    set_target_properties(test_crash PROPERTIES ENABLE_EXPORTS ON)
    add_test(NAME crash COMMAND test_crash)

    add_executable(test_stage_gpu test_stage_gpu.cpp TestMain.h)
    target_link_libraries(test_stage_gpu PRIVATE vx_app)
    add_test(NAME stage_gpu COMMAND test_stage_gpu)
endif()
