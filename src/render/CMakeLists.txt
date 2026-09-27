add_library(vx_render STATIC
    QtConvert.h
    Raster.h Raster.cpp
    Blend.h Blend.cpp
    Filters.h Filters.cpp
    Surface.h Surface.cpp
    Renderer.h Renderer.cpp
    GlRenderer.h GlRenderer.cpp
    SvgExport.h SvgExport.cpp
)
target_include_directories(vx_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vx_render PUBLIC vx_core Qt6::Gui Qt6::OpenGL)
set_target_properties(vx_render PROPERTIES AUTOMOC OFF POSITION_INDEPENDENT_CODE ON)
