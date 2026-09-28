#version 440
// The stage as shown in the window: margin, the stage's soft drop shadow,
// the stage colour and the rendered frame over it (all in device pixels).
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
layout(binding = 1) uniform sampler2D frameTex;
float roundedRect(vec2 p, vec4 r, float radius)
{
    vec2 c = (r.xy + r.zw) * 0.5;
    vec2 h = (r.zw - r.xy) * 0.5 - vec2(radius);
    vec2 q = abs(p - c) - h;
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
}
void main()
{
    vec2 d = vec2(gl_FragCoord.x, viewport.w > 0.5 ? viewport.y - gl_FragCoord.y : gl_FragCoord.y);
    float dpr = extra.x;
    vec3 c = add.rgb;
    for (int i = 6; i >= 1; --i) {
        float r = float(i) * dpr;
        vec4 box = mult + vec4(-r, -r + 3.0 * dpr, r, r + 3.0 * dpr);
        float cover = clamp(0.5 - roundedRect(d, box, r), 0.0, 1.0);
        c = mix(c, vec3(0.0), extra.y * cover);
    }
    if (d.x >= mult.x && d.x < mult.z && d.y >= mult.y && d.y < mult.w) c = paper.rgb;
    vec4 f = texture(frameTex, d / viewport.xy);
    fragColor = vec4(f.rgb + c * (1.0 - f.a), 1.0);
}
