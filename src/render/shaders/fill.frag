#version 440
// Solid colours and gradients (a 256-entry premultiplied ramp per LUT row).
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
layout(binding = 1) uniform sampler2D lut;
float spreadT(float t, float spread)
{
    if (spread < 0.5) return clamp(t, 0.0, 1.0);     // pad
    if (spread > 1.5) return t - floor(t);           // repeat
    float m = mod(abs(t), 2.0);                      // reflect
    return m > 1.0 ? 2.0 - m : m;
}
void main()
{
    if (paint.x < 0.5) {
        fragColor = color;
        return;
    }
    vec2 g = (toGradient * vec4(gl_FragCoord.xy, 0.0, 1.0)).xy;
    float t;
    float focal = paint.y;
    if (paint.x < 1.5) {
        t = (g.x + 1.0) * 0.5;
    } else if (focal == 0.0) {
        t = length(g);
    } else {
        vec2 F = vec2(focal, 0.0);
        vec2 d = g - F;
        float dd = dot(d, d);
        if (dd < 1e-18) {
            t = 0.0;
        } else {
            float fd = dot(F, d);
            float disc = fd * fd - dd * (dot(F, F) - 1.0);
            float s = (-fd + sqrt(max(0.0, disc))) / dd;
            t = s > 0.0 ? 1.0 / s : 1.0;
        }
    }
    int i = int(clamp(floor(spreadT(t, paint.z) * 255.0 + 0.5), 0.0, 255.0));
    fragColor = texelFetch(lut, ivec2(i, int(paint.w)), 0);
}
