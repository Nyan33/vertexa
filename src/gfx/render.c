#include "gfx/render.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_glue.h"

// One uber-shader draws everything: solid fills, glyphs, images, SDF rounded rects and
// circles, the hue ring, the saturation/value square and anti-aliased capsules (lines).
static const char *HLSL_VS =
    "cbuffer vs_params : register(b0) { float4 xf; };\n"
    "struct vs_in { float2 pos : TEXCOORD0; float2 uv : TEXCOORD1; float4 col : TEXCOORD2; float4 p : TEXCOORD3; float4 k : TEXCOORD4; };\n"
    "struct vs_out { float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float4 p : TEXCOORD2; float4 k : TEXCOORD3; float4 pos : SV_Position; };\n"
    "vs_out vs_main(vs_in i) {\n"
    "  vs_out o;\n"
    "  o.pos = float4(i.pos.x * xf.x - 1.0, 1.0 - i.pos.y * xf.y, 0.0, 1.0);\n"
    "  o.uv = i.uv; o.col = i.col; o.p = i.p; o.k = i.k;\n"
    "  return o;\n"
    "}\n";

static const char *HLSL_PS =
    "Texture2D<float4> tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct vs_out { float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float4 p : TEXCOORD2; float4 k : TEXCOORD3; float4 pos : SV_Position; };\n"
    "float sd_rbox(float2 q, float2 b, float r) { float2 d = abs(q) - b + r; return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r; }\n"
    "float3 hsv(float h, float s, float v) { float3 c = saturate(abs(frac(h + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0); return v * lerp(float3(1.0, 1.0, 1.0), c, s); }\n"
    "float4 ps_main(vs_out i) : SV_Target0 {\n"
    "  int kind = (int)(i.k.x + 0.5);\n"
    "  float4 c = i.col;\n"
    "  float a = 1.0;\n"
    "  if (kind == 1) { a = pow(abs(tex.Sample(smp, i.uv).r), i.k.y); }\n"
    "  else if (kind == 2) { return tex.Sample(smp, i.uv) * c; }\n"
    "  else if (kind == 10) {\n"
    "    float t = i.p.x > 0.5 ? length(i.uv) : (i.uv.x + 1.0) * 0.5;\n"
    "    if (i.p.y > 1.5) t = frac(t); else if (i.p.y > 0.5) t = 1.0 - abs(frac(t * 0.5) * 2.0 - 1.0);\n"
    "    return tex.Sample(smp, float2((saturate(t) * 255.0 + 0.5) / 256.0, 0.5)) * c;\n"
    "  }\n"
    "  else if (kind == 11) { float4 t = tex.Sample(smp, i.uv); return float4(t.rgb * c.rgb + i.p.rgb * t.a, t.a * c.a); }\n"
    "  else if (kind == 13) { float4 t = tex.Sample(smp, i.uv); a = saturate(t.a - dot(t.rgb, float3(0.299, 0.587, 0.114))); }\n"
    "  else if (kind == 12) { float4 t = tex.Sample(smp, i.uv); float l = dot(t.rgb + (1.0 - t.a), float3(0.299, 0.587, 0.114)); a = saturate(lerp(1.0, l, i.p.x)); }\n"
    "  else if (kind == 3) { float d = sd_rbox(i.uv, i.p.xy, i.p.z); if (i.p.w > 0.0) d = abs(d + i.p.w * 0.5) - i.p.w * 0.5; a = saturate(0.5 - d); }\n"
    "  else if (kind == 4) { float r = length(i.uv); float d = max(r - i.p.x, i.p.y - r); a = saturate(0.5 - d); float h = atan2(i.uv.y, i.uv.x) / 6.2831853; c.rgb = hsv(frac(h + 1.0), 0.86, 1.0); }\n"
    "  else if (kind == 5) { float2 t = (i.uv + i.p.xy) / (2.0 * i.p.xy); c.rgb = hsv(i.k.y, saturate(t.x), saturate(1.0 - t.y)); a = saturate(0.5 - sd_rbox(i.uv, i.p.xy, i.p.z)); }\n"
    "  else if (kind == 6) { float d = length(float2(max(abs(i.uv.x) - i.p.x, 0.0), i.uv.y)) - i.p.y; a = saturate(0.5 - d); }\n"
    "  else if (kind == 8) { float d = sd_rbox(i.uv, i.p.xy, i.p.z); a = 1.0 - smoothstep(-i.p.w, i.p.w, d); a = a * a; }\n"
    "  else if (kind == 9) { float s = frac((i.pos.x + i.pos.y) / i.k.y); float t = s < 0.5 ? min(s, 0.5 - s) : -min(s - 0.5, 1.0 - s); a = lerp(0.3, 1.0, saturate(t * i.k.y * 0.7071 + 0.5)); }\n"
    "  else if (kind == 7) { float2 q = floor((i.uv + i.p.xy) / 5.0); float ch = fmod(q.x + q.y, 2.0); c.rgb = lerp(float3(0.85, 0.85, 0.87), float3(0.6, 0.6, 0.63), ch); c.a = 1.0; a = saturate(0.5 - sd_rbox(i.uv, i.p.xy, i.p.z)); }\n"
    "  return float4(c.rgb * c.a * a, c.a * a);\n"
    "}\n";

// The blend shader: the group (t0) over a copy of what is underneath (t1), both read texel for texel.
// Premultiplied, W3C compositing with the separable blend B(Cs, Cb) of the mode.
static const char *HLSL_COMP =
    "Texture2D<float4> src_t : register(t0);\n"
    "Texture2D<float4> dst_t : register(t1);\n"
    "SamplerState smp : register(s0);\n"
    "struct vs_out { float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float4 p : TEXCOORD2; float4 k : TEXCOORD3; float4 pos : SV_Position; };\n"
    "float3 blendf(int m, float3 s, float3 d) {\n"
    "  float3 lo = 2.0 * s * d, hi = 1.0 - 2.0 * (1.0 - s) * (1.0 - d);\n"
    "  if (m == 1) return min(s, d);\n"
    "  if (m == 2) return max(s, d);\n"
    "  if (m == 3) return abs(s - d);\n"
    "  if (m == 4) return lerp(hi, lo, step(d, 0.5));\n"
    "  if (m == 5) return lerp(hi, lo, step(s, 0.5));\n"
    "  if (m == 6) return 1.0 - d;\n"
    "  return s;\n"
    "}\n"
    "float4 ps_main(vs_out i) : SV_Target0 {\n"
    "  int3 q = int3((int)i.pos.x, (int)i.pos.y, 0);\n"
    "  float4 S = src_t.Load(q) * i.k.y;\n"
    "  float4 D = dst_t.Load(q);\n"
    "  float3 s = S.a > 0.0 ? S.rgb / S.a : float3(0.0, 0.0, 0.0);\n"
    "  float3 d = D.a > 0.0 ? D.rgb / D.a : float3(0.0, 0.0, 0.0);\n"
    "  float3 B = saturate(blendf((int)(i.k.x + 0.5), s, d));\n"
    "  float3 c = S.rgb * (1.0 - D.a) + D.rgb * (1.0 - S.a) + S.a * D.a * B;\n"
    "  return float4(c, S.a + D.a * (1.0 - S.a));\n"
    "}\n";

// Filters, one pass at a time, texel loads (t0, and t1 = the object for glow / shadow):
//   1 blur along dir; 3 alpha blurred along dir, read at -off; 6 the same of 1 - alpha (inner);
//   4 outer glow / shadow: t0 = alpha blurred one way, blurred the other way here, under the object;
//   5 inner glow / shadow, over the object, inside it; 7 colour matrix; 8 copy
static const char *HLSL_FX =
    "Texture2D<float4> t0 : register(t0);\n"
    "Texture2D<float4> t1 : register(t1);\n"
    "SamplerState smp : register(s0);\n"
    "cbuffer fs_params : register(b0) { float4 P[9]; };\n"
    "struct vs_out { float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float4 p : TEXCOORD2; float4 k : TEXCOORD3; float4 pos : SV_Position; };\n"
    "float4 ps_main(vs_out i) : SV_Target0 {\n"
    "  int mode = (int)(P[0].x + 0.5);\n"
    "  int2 q = int2((int)i.pos.x, (int)i.pos.y);\n"
    "  if (mode == 8) return t0.Load(int3(q, 0));\n"
    "  if (mode == 7) {\n"
    "    float4 c = t0.Load(int3(q, 0));\n"
    "    float4 v = float4(c.a > 0.0 ? c.rgb / c.a : float3(0.0, 0.0, 0.0), c.a);\n"
    "    float4 o = saturate(float4(dot(P[4], v), dot(P[5], v), dot(P[6], v), dot(P[7], v)) + P[8]);\n"
    "    return float4(o.rgb * o.a, o.a);\n"
    "  }\n"
    "  float sigma = max(P[0].y, 0.001), stp = P[0].z;\n"
    "  int taps = (int)(P[3].y + 0.5);\n"
    "  float2 dir = P[1].xy, off = P[1].zw;\n"
    "  float4 acc = float4(0.0, 0.0, 0.0, 0.0); float ws = 0.0;\n"
    "  [loop] for (int k = -taps; k <= taps; k++) {\n"
    "    float x = k * stp, w = exp(-0.5 * x * x / (sigma * sigma));\n"
    "    int2 s = int2(floor(i.pos.xy - off + dir * x));\n"
    "    float4 c = t0.Load(int3(s, 0));\n"
    "    if (mode == 3) c = c.aaaa; else if (mode == 6) c = (1.0 - c.a).xxxx;\n"
    "    acc += c * w; ws += w;\n"
    "  }\n"
    "  acc /= ws;\n"
    "  if (mode == 1 || mode == 3 || mode == 6) return acc;\n"
    "  float4 obj = t1.Load(int3(q, 0));\n"
    "  float fl = P[3].x;\n"
    "  bool knock = fmod(fl, 2.0) >= 1.0, hide = fmod(floor(fl / 2.0), 2.0) >= 1.0;\n"
    "  float s = saturate(acc.a * P[0].w);\n"
    "  float4 col = P[2];\n"
    "  if (mode == 4) { float4 sh = col * s; if (knock) return sh * (1.0 - obj.a); if (hide) return sh; return obj + sh * (1.0 - obj.a); }\n"
    "  s *= obj.a;\n"
    "  if (knock) return col * s;\n"
    "  return col * s + obj * (1.0 - col.a * s);\n"
    "}\n";

static struct {
    sg_shader shd, comp, fx;
    sg_buffer vbuf, ibuf;
    size_t vcap, icap;
    sg_sampler smp, smp_near, smp_mip, smp_rep;
    PipeSet swap;
    Vtx *vstage;
    u32 *istage;
} G;

static sg_buffer make_buf(size_t size, bool index) {
    return sg_make_buffer(&(sg_buffer_desc){
        .size = size,
        .usage = {.vertex_buffer = !index, .index_buffer = index, .write_transient = true},
        .label = index ? "vx-ibuf" : "vx-vbuf",
    });
}

void gfx_init(void) {
    G.shd = sg_make_shader(&(sg_shader_desc){
        .vertex_func = {.source = HLSL_VS, .entry = "vs_main", .d3d11_target = "vs_5_0"},
        .fragment_func = {.source = HLSL_PS, .entry = "ps_main", .d3d11_target = "ps_5_0"},
        .attrs = {
            [0] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 0},
            [1] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 1},
            [2] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 2},
            [3] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 3},
            [4] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 4},
        },
        .uniform_blocks[0] = {.stage = SG_SHADERSTAGE_VERTEX, .size = 16, .hlsl_register_b_n = 0},
        .views[0].texture = {.stage = SG_SHADERSTAGE_FRAGMENT, .image_type = SG_IMAGETYPE_2D, .sample_type = SG_IMAGESAMPLETYPE_FLOAT, .hlsl_register_t_n = 0},
        .samplers[0] = {.stage = SG_SHADERSTAGE_FRAGMENT, .sampler_type = SG_SAMPLERTYPE_FILTERING, .hlsl_register_s_n = 0},
        .texture_sampler_pairs[0] = {.stage = SG_SHADERSTAGE_FRAGMENT, .view_slot = 0, .sampler_slot = 0},
        .label = "vx-uber",
    });
    G.comp = sg_make_shader(&(sg_shader_desc){
        .vertex_func = {.source = HLSL_VS, .entry = "vs_main", .d3d11_target = "vs_5_0"},
        .fragment_func = {.source = HLSL_COMP, .entry = "ps_main", .d3d11_target = "ps_5_0"},
        .attrs = {
            [0] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 0},
            [1] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 1},
            [2] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 2},
            [3] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 3},
            [4] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 4},
        },
        .uniform_blocks[0] = {.stage = SG_SHADERSTAGE_VERTEX, .size = 16, .hlsl_register_b_n = 0},
        .views[0].texture = {.stage = SG_SHADERSTAGE_FRAGMENT, .image_type = SG_IMAGETYPE_2D, .sample_type = SG_IMAGESAMPLETYPE_UNFILTERABLE_FLOAT, .hlsl_register_t_n = 0},
        .views[1].texture = {.stage = SG_SHADERSTAGE_FRAGMENT, .image_type = SG_IMAGETYPE_2D, .sample_type = SG_IMAGESAMPLETYPE_UNFILTERABLE_FLOAT, .hlsl_register_t_n = 1},
        .samplers[0] = {.stage = SG_SHADERSTAGE_FRAGMENT, .sampler_type = SG_SAMPLERTYPE_NONFILTERING, .hlsl_register_s_n = 0},   // unused (texels are loaded)
        .texture_sampler_pairs[0] = {.stage = SG_SHADERSTAGE_FRAGMENT, .view_slot = 0, .sampler_slot = 0},
        .texture_sampler_pairs[1] = {.stage = SG_SHADERSTAGE_FRAGMENT, .view_slot = 1, .sampler_slot = 0},
        .label = "vx-composite",
    });
    G.fx = sg_make_shader(&(sg_shader_desc){
        .vertex_func = {.source = HLSL_VS, .entry = "vs_main", .d3d11_target = "vs_5_0"},
        .fragment_func = {.source = HLSL_FX, .entry = "ps_main", .d3d11_target = "ps_5_0"},
        .attrs = {
            [0] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 0},
            [1] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 1},
            [2] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 2},
            [3] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 3},
            [4] = {.hlsl_sem_name = "TEXCOORD", .hlsl_sem_index = 4},
        },
        .uniform_blocks[0] = {.stage = SG_SHADERSTAGE_VERTEX, .size = 16, .hlsl_register_b_n = 0},
        .uniform_blocks[1] = {.stage = SG_SHADERSTAGE_FRAGMENT, .size = FX_P * 4, .hlsl_register_b_n = 0},
        .views[0].texture = {.stage = SG_SHADERSTAGE_FRAGMENT, .image_type = SG_IMAGETYPE_2D, .sample_type = SG_IMAGESAMPLETYPE_UNFILTERABLE_FLOAT, .hlsl_register_t_n = 0},
        .views[1].texture = {.stage = SG_SHADERSTAGE_FRAGMENT, .image_type = SG_IMAGETYPE_2D, .sample_type = SG_IMAGESAMPLETYPE_UNFILTERABLE_FLOAT, .hlsl_register_t_n = 1},
        .samplers[0] = {.stage = SG_SHADERSTAGE_FRAGMENT, .sampler_type = SG_SAMPLERTYPE_NONFILTERING, .hlsl_register_s_n = 0},
        .texture_sampler_pairs[0] = {.stage = SG_SHADERSTAGE_FRAGMENT, .view_slot = 0, .sampler_slot = 0},
        .texture_sampler_pairs[1] = {.stage = SG_SHADERSTAGE_FRAGMENT, .view_slot = 1, .sampler_slot = 0},
        .label = "vx-filter",
    });
    G.smp_near = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
    G.smp_mip = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_LINEAR, .mipmap_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE, .max_anisotropy = 4,
    });
    G.smp_rep = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_LINEAR, .mipmap_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_REPEAT, .wrap_v = SG_WRAP_REPEAT, .max_anisotropy = 4,
    });
    G.smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
    G.vcap = 24u << 20; G.icap = 8u << 20;
    G.vbuf = make_buf(G.vcap, false);
    G.ibuf = make_buf(G.icap, true);
    sg_environment env = sglue_environment();
    G.swap = gfx_make_pipes(env.defaults.sample_count, env.defaults.color_format, env.defaults.depth_format);
}

void gfx_shutdown(void) { arrfree(G.vstage); arrfree(G.istage); }
PipeSet *gfx_swapchain_pipes(void) { return &G.swap; }
sg_sampler gfx_linear_sampler(void) { return G.smp; }

void gfx_free_pipes(PipeSet *ps) {
    for (int i = 0; i < PIPE_COUNT; i++) if (ps->p[i].id) { sg_destroy_pipeline(ps->p[i]); ps->p[i].id = 0; }
}

PipeSet gfx_make_pipes(int sample_count, sg_pixel_format color, sg_pixel_format depth) {
    PipeSet ps = {0};
    sg_pipeline_desc d = {
        .shader = G.shd,
        .layout.attrs = {
            [0].format = SG_VERTEXFORMAT_FLOAT2,
            [1].format = SG_VERTEXFORMAT_FLOAT2,
            [2].format = SG_VERTEXFORMAT_UBYTE4N,
            [3].format = SG_VERTEXFORMAT_FLOAT4,
            [4].format = SG_VERTEXFORMAT_FLOAT4,
        },
        .index_type = SG_INDEXTYPE_UINT32,
        .cull_mode = SG_CULLMODE_NONE,
        .sample_count = sample_count,
        .colors[0] = {
            .pixel_format = color,
            .blend = {.enabled = true, .src_factor_rgb = SG_BLENDFACTOR_ONE, .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                      .src_factor_alpha = SG_BLENDFACTOR_ONE, .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA},
        },
        .depth = {.pixel_format = depth},
        .label = "vx-normal",
    };
    ps.p[PIPE_NORMAL] = sg_make_pipeline(&d);
    // group blend modes, premultiplied. Multiply in two draws, exact: the first keeps the backdrop's alpha
    // (src*dst + dst*(1-sa)), the second adds src*(1-da) and the alpha
    #define BLEND(P, sr, dr, sa, da) do { sg_pipeline_desc bm = d; \
        bm.colors[0].blend = (sg_blend_state){.enabled = true, .src_factor_rgb = SG_BLENDFACTOR_##sr, .dst_factor_rgb = SG_BLENDFACTOR_##dr, \
                                              .src_factor_alpha = SG_BLENDFACTOR_##sa, .dst_factor_alpha = SG_BLENDFACTOR_##da}; \
        ps.p[P] = sg_make_pipeline(&bm); } while (0)
    BLEND(PIPE_MULTIPLY, DST_COLOR, ONE_MINUS_SRC_ALPHA, ZERO, ONE);
    BLEND(PIPE_MULTIPLY2, ONE_MINUS_DST_ALPHA, ONE, ONE, ONE_MINUS_SRC_ALPHA);
    BLEND(PIPE_SCREEN, ONE, ONE_MINUS_SRC_COLOR, ONE, ONE_MINUS_SRC_ALPHA);
    BLEND(PIPE_ADD, ONE, ONE, ONE, ONE_MINUS_SRC_ALPHA);
    BLEND(PIPE_ALPHA, ZERO, SRC_ALPHA, ZERO, SRC_ALPHA);              // keeps what is below where the group is
    BLEND(PIPE_ERASE_OUT, ZERO, ONE_MINUS_SRC_ALPHA, ZERO, ONE_MINUS_SRC_ALPHA);
    #undef BLEND
    {
        sg_pipeline_desc bm = d;
        bm.colors[0].blend = (sg_blend_state){.enabled = true, .src_factor_rgb = SG_BLENDFACTOR_ONE, .dst_factor_rgb = SG_BLENDFACTOR_ONE, .op_rgb = SG_BLENDOP_MAX,
                                              .src_factor_alpha = SG_BLENDFACTOR_ONE, .dst_factor_alpha = SG_BLENDFACTOR_ONE, .op_alpha = SG_BLENDOP_MAX};
        bm.label = "vx-max";
        ps.p[PIPE_MAX] = sg_make_pipeline(&bm);
    }
    {   // subtract: below minus the group, the alpha below kept
        sg_pipeline_desc bm = d;
        bm.colors[0].blend = (sg_blend_state){.enabled = true, .src_factor_rgb = SG_BLENDFACTOR_ONE, .dst_factor_rgb = SG_BLENDFACTOR_ONE, .op_rgb = SG_BLENDOP_REVERSE_SUBTRACT,
                                              .src_factor_alpha = SG_BLENDFACTOR_ZERO, .dst_factor_alpha = SG_BLENDFACTOR_ONE};
        ps.p[PIPE_SUBTRACT] = sg_make_pipeline(&bm);
        bm.colors[0].blend = (sg_blend_state){.enabled = false};
        bm.label = "vx-copy";
        ps.p[PIPE_COPY] = sg_make_pipeline(&bm);
        bm.shader = G.comp;
        bm.label = "vx-composite";
        ps.p[PIPE_COMPOSITE] = sg_make_pipeline(&bm);
        bm.shader = G.fx;
        bm.label = "vx-filter";
        ps.p[PIPE_FILTER] = sg_make_pipeline(&bm);
    }
    if (depth == SG_PIXELFORMAT_DEPTH_STENCIL) {
        // stencil pass: count coverage of every (convex, same-winding) sub-polygon, never wraps
        sg_pipeline_desc s = d;
        s.colors[0].write_mask = SG_COLORMASK_NONE;
        s.colors[0].blend.enabled = false;
        s.stencil = (sg_stencil_state){
            .enabled = true, .read_mask = 0xFF, .write_mask = 0xFF, .ref = 0,
            .front = {.compare = SG_COMPAREFUNC_ALWAYS, .pass_op = SG_STENCILOP_INCR_CLAMP, .fail_op = SG_STENCILOP_KEEP, .depth_fail_op = SG_STENCILOP_KEEP},
            .back = {.compare = SG_COMPAREFUNC_ALWAYS, .pass_op = SG_STENCILOP_INCR_CLAMP, .fail_op = SG_STENCILOP_KEEP, .depth_fail_op = SG_STENCILOP_KEEP},
        };
        s.label = "vx-stencil";
        ps.p[PIPE_STENCIL] = sg_make_pipeline(&s);
        // even-odd parity for fills with holes
        sg_pipeline_desc eo = s;
        eo.stencil.front.pass_op = SG_STENCILOP_INVERT; eo.stencil.back.pass_op = SG_STENCILOP_INVERT;
        eo.stencil.write_mask = 0x01;
        eo.label = "vx-stencil-eo";
        ps.p[PIPE_STENCIL_EO] = sg_make_pipeline(&eo);
        // cover pass: paint where the stencil is set and clear it on the way
        sg_pipeline_desc c = d;
        c.stencil = (sg_stencil_state){
            .enabled = true, .read_mask = 0xFF, .write_mask = 0xFF, .ref = 0,
            .front = {.compare = SG_COMPAREFUNC_NOT_EQUAL, .pass_op = SG_STENCILOP_ZERO, .fail_op = SG_STENCILOP_KEEP, .depth_fail_op = SG_STENCILOP_KEEP},
            .back = {.compare = SG_COMPAREFUNC_NOT_EQUAL, .pass_op = SG_STENCILOP_ZERO, .fail_op = SG_STENCILOP_KEEP, .depth_fail_op = SG_STENCILOP_KEEP},
        };
        c.label = "vx-cover";
        ps.p[PIPE_COVER] = sg_make_pipeline(&c);
        // erase: cover that removes what is underneath (destination-out)
        sg_pipeline_desc e = c;
        e.colors[0].blend = (sg_blend_state){.enabled = true, .src_factor_rgb = SG_BLENDFACTOR_ZERO, .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                                             .src_factor_alpha = SG_BLENDFACTOR_ZERO, .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA};
        e.label = "vx-erase";
        ps.p[PIPE_ERASE] = sg_make_pipeline(&e);
    }
    return ps;
}

void gfx_upload(DrawList **lists, int n) {
    arrsetlen(G.vstage, 0);
    arrsetlen(G.istage, 0);
    for (int l = 0; l < n; l++) {
        DrawList *dl = lists[l];
        u32 bv = (u32)arrlen(G.vstage);
        dl->base_v = bv;
        dl->base_i = (u32)arrlen(G.istage);
        if (arrlen(dl->v)) memcpy(arraddnptr(G.vstage, arrlen(dl->v)), dl->v, sizeof(Vtx) * arrlen(dl->v));
        u32 *dst = arrlen(dl->idx) ? arraddnptr(G.istage, arrlen(dl->idx)) : NULL;
        for (ptrdiff_t i = 0; i < arrlen(dl->idx); i++) dst[i] = dl->idx[i] + bv;
    }
    size_t vb = sizeof(Vtx) * arrlen(G.vstage), ib = sizeof(u32) * arrlen(G.istage);
    if (vb > G.vcap) { sg_destroy_buffer(G.vbuf); while (G.vcap < vb) G.vcap *= 2; G.vbuf = make_buf(G.vcap, false); }
    if (ib > G.icap) { sg_destroy_buffer(G.ibuf); while (G.icap < ib) G.icap *= 2; G.ibuf = make_buf(G.icap, true); }
    if (vb) sg_write_buffer_transient(&(sg_write_buffer_desc){.src.data = {G.vstage, vb}, .dst.buffer = G.vbuf});
    if (ib) sg_write_buffer_transient(&(sg_write_buffer_desc){.src.data = {G.istage, ib}, .dst.buffer = G.ibuf});
}

void gfx_draw(DrawList *dl, PipeSet *ps, int fb_w, int fb_h) {
    if (!arrlen(dl->cmd)) return;
    float xf[4] = {2.0f / fb_w, 2.0f / fb_h, 0, 0};
    int cur = -1;
    for (ptrdiff_t i = 0; i < arrlen(dl->cmd); i++) {
        DrawCmd *c = &dl->cmd[i];
        if (!c->count) continue;
        Rect r = c->clip;
        int x0 = (int)floorf(r.x), y0 = (int)floorf(r.y), x1 = (int)ceilf(r.x + r.w), y1 = (int)ceilf(r.y + r.h);
        x0 = CLAMP(x0, 0, fb_w); y0 = CLAMP(y0, 0, fb_h); x1 = CLAMP(x1, 0, fb_w); y1 = CLAMP(y1, 0, fb_h);
        if (x1 <= x0 || y1 <= y0) continue;
        if (c->pipe != cur) {
            cur = c->pipe;
            sg_apply_pipeline(ps->p[cur]);
            sg_apply_uniforms(0, &SG_RANGE(xf));
        }
        sg_apply_scissor_rect(x0, y0, x1 - x0, y1 - y0, true);
        if (c->pipe == PIPE_FILTER && c->ub >= 0) sg_apply_uniforms(1, &(sg_range){dl->ub + c->ub, FX_P * sizeof(float)});
        if (c->pipe == PIPE_COMPOSITE || c->pipe == PIPE_FILTER)
            sg_apply_bindings(&(sg_bindings){
                .vertex_buffers[0] = G.vbuf,
                .index_buffer = G.ibuf,
                .views[0] = (sg_view){c->tex},
                .views[1] = (sg_view){c->tex2},
                .samplers[0] = G.smp_near,
            });
        else
            sg_apply_bindings(&(sg_bindings){
                .vertex_buffers[0] = G.vbuf,
                .index_buffer = G.ibuf,
                .views[0] = (sg_view){c->tex},
                .samplers[0] = c->smp == SMP_NEAREST ? G.smp_near : c->smp == SMP_MIP ? G.smp_mip : c->smp == SMP_REPEAT ? G.smp_rep : G.smp,
            });
        sg_draw((int)(dl->base_i + c->first), (int)c->count, 1);
    }
    sg_apply_scissor_rect(0, 0, fb_w, fb_h, true);
}

/* ── draw list building ── */

static void new_cmd(DrawList *dl) {
    DrawCmd c = {.first = (u32)arrlen(dl->idx), .count = 0, .clip = arrlast(dl->clips), .tex = dl->tex, .tex2 = dl->tex2, .pipe = dl->pipe, .smp = dl->smp, .ub = dl->ubi};
    arrput(dl->cmd, c);
}
static DrawCmd *cur_cmd(DrawList *dl) {
    if (!arrlen(dl->cmd)) new_cmd(dl);
    return &arrlast(dl->cmd);
}

void dl_reset(DrawList *dl, float scale, Rect full) {
    arrsetlen(dl->v, 0); arrsetlen(dl->idx, 0); arrsetlen(dl->cmd, 0); arrsetlen(dl->clips, 0);
    dl->scale = scale;
    dl->pipe = PIPE_NORMAL;
    dl->tex2 = 0; dl->smp = SMP_LINEAR;
    arrsetlen(dl->ub, 0); dl->ubi = -1;
    Rect fc = {full.x * scale, full.y * scale, full.w * scale, full.h * scale};
    arrput(dl->clips, fc);
    new_cmd(dl);
}
Rect dl_clip(DrawList *dl) { Rect c = arrlast(dl->clips); float s = dl->scale; return (Rect){c.x / s, c.y / s, c.w / s, c.h / s}; }
void dl_push_clip(DrawList *dl, Rect r) {
    float s = dl->scale;
    Rect px = {roundf(r.x * s), roundf(r.y * s), roundf((r.x + r.w) * s) - roundf(r.x * s), roundf((r.y + r.h) * s) - roundf(r.y * s)};
    Rect nc = rect_isect(arrlast(dl->clips), px);
    arrput(dl->clips, nc);
    new_cmd(dl);
}
void dl_pop_clip(DrawList *dl) { if (arrlen(dl->clips) > 1) arrpop(dl->clips); new_cmd(dl); }
void dl_set_tex(DrawList *dl, u32 view) { if (dl->tex == view) return; dl->tex = view; new_cmd(dl); }
void dl_set_pipe(DrawList *dl, u8 pipe) { if (dl->pipe == pipe) return; dl->pipe = pipe; new_cmd(dl); }
void dl_set_tex2(DrawList *dl, u32 view) { if (dl->tex2 == view) return; dl->tex2 = view; new_cmd(dl); }
void dl_set_sampler(DrawList *dl, u8 smp) { if (dl->smp == smp) return; dl->smp = smp; new_cmd(dl); }

static u32 push_v(DrawList *dl, float x, float y, float u, float v, Color c, const float p[4], const float k[4]) {
    Vtx vx = {x, y, u, v, c, {p[0], p[1], p[2], p[3]}, {k[0], k[1], k[2], k[3]}};
    arrput(dl->v, vx);
    return (u32)arrlen(dl->v) - 1;
}
static void quad_idx(DrawList *dl, u32 a) {
    u32 *i = arraddnptr(dl->idx, 6);
    i[0] = a; i[1] = a + 1; i[2] = a + 2; i[3] = a; i[4] = a + 2; i[5] = a + 3;
    cur_cmd(dl)->count += 6;
}
// quad whose uv is the local offset (in pixels) from the quad center, for SDF kinds
static void sdf_quad(DrawList *dl, float cx, float cy, float hw, float hh, float pad, Color c, const float p[4], const float k[4]) {
    float x0 = cx - hw - pad, y0 = cy - hh - pad, x1 = cx + hw + pad, y1 = cy + hh + pad;
    u32 a = push_v(dl, x0, y0, x0 - cx, y0 - cy, c, p, k);
    push_v(dl, x1, y0, x1 - cx, y0 - cy, c, p, k);
    push_v(dl, x1, y1, x1 - cx, y1 - cy, c, p, k);
    push_v(dl, x0, y1, x0 - cx, y1 - cy, c, p, k);
    quad_idx(dl, a);
}

void dl_quad_px(DrawList *dl, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Color c, int kind, float k1) {
    float p[4] = {0}, k[4] = {(float)kind, k1, 0, 0};
    u32 a = push_v(dl, x0, y0, u0, v0, c, p, k);
    push_v(dl, x1, y0, u1, v0, c, p, k);
    push_v(dl, x1, y1, u1, v1, c, p, k);
    push_v(dl, x0, y1, u0, v1, c, p, k);
    quad_idx(dl, a);
}
u32 dl_vtx_px(DrawList *dl, float x, float y, Color c) {
    static const float z[4] = {0};
    return push_v(dl, x, y, 0, 0, c, z, z);
}
u32 dl_vtx_k(DrawList *dl, float x, float y, Color c, int kind, float k1) {
    static const float z[4] = {0};
    float k[4] = {(float)kind, k1, 0, 0};
    return push_v(dl, x, y, 0, 0, c, z, k);
}
u32 dl_vtx_uv(DrawList *dl, float x, float y, float u, float v, Color c, int kind) {
    static const float z[4] = {0};
    float k[4] = {(float)kind, 0, 0, 0};
    return push_v(dl, x, y, u, v, c, z, k);
}
u32 dl_vtx_uvp(DrawList *dl, float x, float y, float u, float v, Color c, int kind, const float p[4]) {
    float k[4] = {(float)kind, 0, 0, 0};
    return push_v(dl, x, y, u, v, c, p, k);
}
void dl_filter_quad(DrawList *dl, Rect px, u32 t0, u32 t1, const float P[FX_P]) {
    dl->ubi = (int)arrlen(dl->ub);
    memcpy(arraddnptr(dl->ub, FX_P), P, sizeof(float) * FX_P);
    dl->pipe = PIPE_FILTER; dl->tex = t0; dl->tex2 = t1;
    new_cmd(dl);
    dl_quad_px(dl, px.x, px.y, px.x + px.w, px.y + px.h, 0, 0, 1, 1, 0xffffffff, 0, 0);
    dl->ubi = -1;
}
void dl_idx(DrawList *dl, u32 a, u32 b, u32 c) {
    u32 *i = arraddnptr(dl->idx, 3);
    i[0] = a; i[1] = b; i[2] = c;
    cur_cmd(dl)->count += 3;
}

void dl_rect(DrawList *dl, Rect r, Color c) {
    float s = dl->scale;
    float x0 = roundf(r.x * s), y0 = roundf(r.y * s), x1 = roundf((r.x + r.w) * s), y1 = roundf((r.y + r.h) * s);
    if (x1 <= x0 || y1 <= y0) return;
    dl_quad_px(dl, x0, y0, x1, y1, 0, 0, 0, 0, c, K_SOLID, 0);
}
void dl_rrect(DrawList *dl, Rect r, float radius, Color c) {
    float s = dl->scale, hw = r.w * s / 2, hh = r.h * s / 2;
    float p[4] = {hw, hh, MIN(radius * s, MIN(hw, hh)), 0}, k[4] = {K_RRECT, 0, 0, 0};
    sdf_quad(dl, r.x * s + hw, r.y * s + hh, hw, hh, 1, c, p, k);
}
void dl_rrect_border(DrawList *dl, Rect r, float radius, float width, Color c) {
    float s = dl->scale, hw = r.w * s / 2, hh = r.h * s / 2;
    float p[4] = {hw, hh, MIN(radius * s, MIN(hw, hh)), width * s}, k[4] = {K_RRECT, 0, 0, 0};
    sdf_quad(dl, r.x * s + hw, r.y * s + hh, hw, hh, 1, c, p, k);
}
void dl_circle(DrawList *dl, V2 c, float r, Color col) { dl_rrect(dl, rect(c.x - r, c.y - r, 2 * r, 2 * r), r, col); }
void dl_circle_border(DrawList *dl, V2 c, float r, float width, Color col) { dl_rrect_border(dl, rect(c.x - r, c.y - r, 2 * r, 2 * r), r, width, col); }
void dl_checker(DrawList *dl, Rect r, float radius) {
    float s = dl->scale, hw = r.w * s / 2, hh = r.h * s / 2;
    float p[4] = {hw, hh, MIN(radius * s, MIN(hw, hh)), 0}, k[4] = {K_CHECKER, 0, 0, 0};
    sdf_quad(dl, r.x * s + hw, r.y * s + hh, hw, hh, 1, 0xffffffff, p, k);
}

void dl_line(DrawList *dl, V2 a, V2 b, float width, Color c) {
    float s = dl->scale;
    float ax = a.x * s, ay = a.y * s, bx = b.x * s, by = b.y * s;
    float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) { dx = 1; dy = 0; len = 0; } else { dx /= len; dy /= len; }
    float hl = len / 2, hw = width * s / 2, pad = 1.0f;
    float cx = (ax + bx) / 2, cy = (ay + by) / 2;
    float p[4] = {hl, hw, 0, 0}, k[4] = {K_CAPSULE, 0, 0, 0};
    float ex = hl + hw + pad, ey = hw + pad;
    float corners[4][2] = {{-ex, -ey}, {ex, -ey}, {ex, ey}, {-ex, ey}};
    u32 base = 0;
    for (int i = 0; i < 4; i++) {
        float lx = corners[i][0], ly = corners[i][1];
        float x = cx + lx * dx - ly * dy, y = cy + lx * dy + ly * dx;
        u32 id = push_v(dl, x, y, lx, ly, c, p, k);
        if (i == 0) base = id;
    }
    quad_idx(dl, base);
}

void dl_tri(DrawList *dl, V2 a, V2 b, V2 c, Color col) {
    float s = dl->scale;
    u32 i0 = dl_vtx_px(dl, a.x * s, a.y * s, col), i1 = dl_vtx_px(dl, b.x * s, b.y * s, col), i2 = dl_vtx_px(dl, c.x * s, c.y * s, col);
    dl_idx(dl, i0, i1, i2);
}

void dl_image(DrawList *dl, Rect r, u32 view, Color tint, float u0, float v0, float u1, float v1) {
    u32 old = dl->tex;
    dl_set_tex(dl, view);
    float s = dl->scale;
    dl_quad_px(dl, roundf(r.x * s), roundf(r.y * s), roundf((r.x + r.w) * s), roundf((r.y + r.h) * s), u0, v0, u1, v1, tint, K_IMAGE, 0);
    dl_set_tex(dl, old);
}

void dl_hue_ring(DrawList *dl, V2 c, float r_out, float r_in) {
    float s = dl->scale;
    float p[4] = {r_out * s, r_in * s, 0, 0}, k[4] = {K_RING, 0, 0, 0};
    sdf_quad(dl, c.x * s, c.y * s, r_out * s, r_out * s, 1, 0xffffffff, p, k);
}
void dl_sv_square(DrawList *dl, Rect r, float radius, float hue) {
    float s = dl->scale, hw = r.w * s / 2, hh = r.h * s / 2;
    float p[4] = {hw, hh, radius * s, 0}, k[4] = {K_SV, hue, 0, 0};
    sdf_quad(dl, r.x * s + hw, r.y * s + hh, hw, hh, 1, 0xffffffff, p, k);
}

void dl_shadow(DrawList *dl, Rect r, float radius, float blur, Color c) {
    float s = dl->scale, hw = r.w * s / 2, hh = r.h * s / 2, b = blur * s;
    float p[4] = {hw, hh, MIN(radius * s, MIN(hw, hh)), b}, k[4] = {K_SHADOW, 0, 0, 0};
    sdf_quad(dl, r.x * s + hw, r.y * s + hh, hw, hh, b + 1, c, p, k);
}
