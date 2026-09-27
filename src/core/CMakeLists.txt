add_library(vx_core STATIC
    Color.h
    Style.h
    BlendMode.h
    ShapeGraph.h ShapeGraph.cpp
    ShapeOps.h ShapeOps.cpp
    BrushPreset.h BrushPreset.cpp
    Element.h
    Easing.h Easing.cpp
    Timeline.h Timeline.cpp
    Document.h Document.cpp
    Tween.h Tween.cpp
    ShapeTween.h ShapeTween.cpp
    Evaluate.h Evaluate.cpp
    TimelineOps.h TimelineOps.cpp
    DocumentOps.h DocumentOps.cpp
    Serialize.h Serialize.cpp
)
target_include_directories(vx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vx_core PUBLIC vx_geom Qt6::Core)
set_target_properties(vx_core PROPERTIES AUTOMOC OFF POSITION_INDEPENDENT_CODE ON)
