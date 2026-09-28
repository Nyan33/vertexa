#version 440
// Layers composited with a blend mode, in the order of vx::BlendMode (the
// CPU formulas of Blend.cpp). Simple modes use fixed-function blending and
// only scale the source; the others read a copy of the destination.
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
layout(binding = 1) uniform sampler2D srcTex;
layout(binding = 2) uniform sampler2D dstTex;

float lum(vec3 c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }
vec3 clipColor(vec3 c)
{
    float l = lum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0) c = l + (c - l) * l / (l - n);
    if (x > 1.0) c = l + (c - l) * (1.0 - l) / (x - l);
    return c;
}
vec3 setLum(vec3 c, float l) { return clipColor(c + (l - lum(c))); }
float sat(vec3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }
vec3 setSat(vec3 c, float s)
{
    float mn = min(c.r, min(c.g, c.b));
    float mx = max(c.r, max(c.g, c.b));
    if (mx <= mn) return vec3(0.0);
    return (c - mn) * s / (mx - mn);
}
float channel(int m, float cb, float cs)
{
    if (m == 2) return min(cb, cs);                                                  // Darken
    if (m == 3) return cb * cs;                                                      // Multiply
    if (m == 4) return max(cb, cs);                                                  // Lighten
    if (m == 5) return cb + cs - cb * cs;                                            // Screen
    if (m == 6) return cb <= 0.5 ? 2.0 * cb * cs : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs); // Overlay
    if (m == 7) return cs <= 0.5 ? 2.0 * cb * cs : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs); // Hard light
    if (m == 8) return min(1.0, cb + cs);                                            // Add
    if (m == 9) return max(0.0, cb - cs);                                            // Subtract
    if (m == 10) return abs(cb - cs);                                                // Difference
    if (m == 11) return 1.0 - cb;                                                    // Invert
    if (m == 14) {                                                                   // Color burn
        if (cb >= 1.0) return 1.0;
        if (cs <= 0.0) return 0.0;
        return 1.0 - min(1.0, (1.0 - cb) / cs);
    }
    if (m == 15) return max(0.0, cb + cs - 1.0);                                     // Linear burn
    if (m == 16) {                                                                   // Color dodge
        if (cb <= 0.0) return 0.0;
        if (cs >= 1.0) return 1.0;
        return min(1.0, cb / (1.0 - cs));
    }
    if (m == 17) {                                                                   // Soft light
        if (cs <= 0.5) return cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
        float d = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : sqrt(cb);
        return cb + (2.0 * cs - 1.0) * (d - cb);
    }
    if (m == 18) {                                                                   // Vivid light
        if (cs <= 0.5) return cs <= 0.0 ? 0.0 : max(0.0, 1.0 - (1.0 - cb) / (2.0 * cs));
        return cs >= 1.0 ? 1.0 : min(1.0, cb / (2.0 * (1.0 - cs)));
    }
    if (m == 19) return clamp(cb + 2.0 * cs - 1.0, 0.0, 1.0);                        // Linear light
    if (m == 20) return cs <= 0.5 ? min(cb, 2.0 * cs) : max(cb, 2.0 * cs - 1.0);     // Pin light
    if (m == 21) return cb + cs - 2.0 * cb * cs;                                     // Exclusion
    if (m == 22) return cs <= 0.0 ? (cb > 0.0 ? 1.0 : 0.0) : min(1.0, cb / cs);      // Divide
    return cs;
}
vec3 blendColor(int m, vec3 cb, vec3 cs)
{
    if (m == 23) return setLum(setSat(cs, sat(cb)), lum(cb)); // Hue
    if (m == 24) return setLum(setSat(cb, sat(cs)), lum(cb)); // Saturation
    if (m == 25) return setLum(cs, lum(cb));                  // Color
    if (m == 26) return setLum(cb, lum(cs));                  // Luminosity
    return vec3(channel(m, cb.r, cs.r), channel(m, cb.g, cs.g), channel(m, cb.b, cs.b));
}
void main()
{
    vec4 s = texture(srcTex, (gl_FragCoord.xy - src.xy) / src.zw) * dst.w;
    if (extra.x < 0.5) {
        fragColor = s;
        return;
    }
    vec4 d = texture(dstTex, gl_FragCoord.xy / dst.xy);
    vec3 cs = s.a > 0.0 ? s.rgb / s.a : vec3(0.0);
    vec3 cb = d.a > 0.0 ? d.rgb / d.a : vec3(0.0);
    vec3 bl = blendColor(int(dst.z + 0.5), cb, cs);
    float ra = s.a + d.a * (1.0 - s.a);
    vec3 rc = s.rgb * (1.0 - d.a) + d.rgb * (1.0 - s.a) + s.a * d.a * bl;
    fragColor = vec4(min(rc, vec3(ra)), ra);
}
