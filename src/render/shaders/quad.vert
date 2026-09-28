#version 440
// Geometry in its own space, placed in device pixels (row 0 on top) by
// xform. viewport.z picks the NDC y direction that stores device row r in
// framebuffer row r on every backend, so gl_FragCoord is the device pixel.
layout(location = 0) in vec2 pos;
layout(std140, binding = 0) uniform buf {
    mat4 xform;        // geometry -> device pixels
    vec4 viewport;     // width, height, y sign of NDC, flip (display)
    vec4 color;        // premultiplied fill colour
    mat4 toGradient;   // device -> gradient space
    vec4 paint;        // kind (0 solid, 1 linear, 2 radial), focal, spread, LUT row
    vec4 src;          // composite: source origin xy, source size zw
    vec4 dst;          // destination size xy, blend mode, opacity
    vec4 extra;        // use destination; display: device pixel ratio, shadow alpha
    vec4 mult;         // colour transform multipliers; display: stage rect
    vec4 add;          // colour transform offsets (0..1); display: margin colour
    vec4 paper;        // display: stage colour
};
void main()
{
    vec2 p = (xform * vec4(pos, 0.0, 1.0)).xy;
    gl_Position = vec4(p.x / viewport.x * 2.0 - 1.0, viewport.z * (p.y / viewport.y * 2.0 - 1.0), 0.0, 1.0);
}
