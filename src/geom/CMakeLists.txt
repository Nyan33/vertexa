add_library(vx_geom STATIC
    Vec2.h
    Affine.h
    Polynomial.h Polynomial.cpp
    Bezier.h Bezier.cpp
    Intersect.h Intersect.cpp
    Arrangement.h Arrangement.cpp
    Region.h Region.cpp
    Fit.h Fit.cpp
    Outline.h Outline.cpp
    Smooth.h Smooth.cpp
)
target_include_directories(vx_geom PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(vx_geom PUBLIC cxx_std_20)
set_target_properties(vx_geom PROPERTIES POSITION_INDEPENDENT_CODE ON)
