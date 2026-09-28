#version 440
// A colour transform over a copy of the surface.
layout(location = 0) out vec4 fragColor;
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
layout(binding = 1) uniform sampler2D img;
void main()
{
    vec4 c = texture(img, gl_FragCoord.xy / viewport.xy);
    vec3 rgb = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
    rgb = clamp(rgb * mult.rgb + add.rgb, 0.0, 1.0);
    float a = clamp(c.a * mult.a + add.a, 0.0, 1.0);
    fragColor = vec4(rgb * a, a);
}
