// SPDX-License-Identifier: GPL-3.0-or-later
#include "GlRenderer.h"
#include "Blend.h"
#include "Filters.h"
#include "QtConvert.h"
#include "Raster.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGenericMatrix>
#include <QOffscreenSurface>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPainterPathStroker>
#include <QThread>
#include <QVector2D>
#include <QVector4D>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace vx {

namespace {

// --- shaders ------------------------------------------------------------------------
//
// Framebuffer rows are device rows (row 0 on top), so gl_FragCoord is the
// device pixel centre and nothing is ever flipped.

const char* kVertex = R"(
in vec2 pos;
uniform vec2 viewport;
uniform mat3 xform;   // geometry -> device pixels
void main() {
    vec2 p = (xform * vec3(pos, 1.0)).xy;
    gl_Position = vec4(p / viewport * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kStencil = R"(
out vec4 fragColor;
void main() { fragColor = vec4(0.0); }
)";

const char* kFill = R"(
uniform int kind;          // 0 solid, 1 linear, 2 radial
uniform vec4 color;        // premultiplied
uniform mat3 toGradient;   // device -> gradient space
uniform float focal;
uniform int spread;        // SpreadMode: 0 pad, 1 reflect, 2 repeat
uniform sampler2D lut;     // 256 premultiplied colours
out vec4 fragColor;
float spreadT(float t) {
    if (spread == 0) return clamp(t, 0.0, 1.0);
    if (spread == 2) return t - floor(t);
    float m = mod(abs(t), 2.0);
    return m > 1.0 ? 2.0 - m : m;
}
void main() {
    if (kind == 0) { fragColor = color; return; }
    vec2 g = (toGradient * vec3(gl_FragCoord.xy, 1.0)).xy;
    float t;
    if (kind == 1) {
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
    int i = int(clamp(floor(spreadT(t) * 255.0 + 0.5), 0.0, 255.0));
    fragColor = texelFetch(lut, ivec2(i, 0), 0);
}
)";

// Blend modes, in the order of vx::BlendMode (see Blend.cpp for the CPU version).
const char* kComposite = R"(
uniform sampler2D src;
uniform sampler2D dst;
uniform vec2 srcOrigin;
uniform vec2 srcSize;
uniform vec2 dstSize;
uniform int mode;
uniform float opacity;
uniform int useDst;
out vec4 fragColor;

float lum(vec3 c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }
vec3 clipColor(vec3 c) {
    float l = lum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0) c = l + (c - l) * l / (l - n);
    if (x > 1.0) c = l + (c - l) * (1.0 - l) / (x - l);
    return c;
}
vec3 setLum(vec3 c, float l) { return clipColor(c + (l - lum(c))); }
float sat(vec3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }
vec3 setSat(vec3 c, float s) {
    float mn = min(c.r, min(c.g, c.b));
    float mx = max(c.r, max(c.g, c.b));
    if (mx <= mn) return vec3(0.0);
    return (c - mn) * s / (mx - mn);
}
float channel(int m, float cb, float cs) {
    if (m == 2) return min(cb, cs);                                   // Darken
    if (m == 3) return cb * cs;                                       // Multiply
    if (m == 4) return max(cb, cs);                                   // Lighten
    if (m == 5) return cb + cs - cb * cs;                             // Screen
    if (m == 6) return cb <= 0.5 ? 2.0 * cb * cs : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs); // Overlay
    if (m == 7) return cs <= 0.5 ? 2.0 * cb * cs : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs); // Hard light
    if (m == 8) return min(1.0, cb + cs);                             // Add
    if (m == 9) return max(0.0, cb - cs);                             // Subtract
    if (m == 10) return abs(cb - cs);                                 // Difference
    if (m == 11) return 1.0 - cb;                                     // Invert
    if (m == 14) {                                                    // Color burn
        if (cb >= 1.0) return 1.0;
        if (cs <= 0.0) return 0.0;
        return 1.0 - min(1.0, (1.0 - cb) / cs);
    }
    if (m == 15) return max(0.0, cb + cs - 1.0);                      // Linear burn
    if (m == 16) {                                                    // Color dodge
        if (cb <= 0.0) return 0.0;
        if (cs >= 1.0) return 1.0;
        return min(1.0, cb / (1.0 - cs));
    }
    if (m == 17) {                                                    // Soft light
        if (cs <= 0.5) return cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
        float d = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : sqrt(cb);
        return cb + (2.0 * cs - 1.0) * (d - cb);
    }
    if (m == 18) {                                                    // Vivid light
        if (cs <= 0.5) return cs <= 0.0 ? 0.0 : max(0.0, 1.0 - (1.0 - cb) / (2.0 * cs));
        return cs >= 1.0 ? 1.0 : min(1.0, cb / (2.0 * (1.0 - cs)));
    }
    if (m == 19) return clamp(cb + 2.0 * cs - 1.0, 0.0, 1.0);         // Linear light
    if (m == 20) return cs <= 0.5 ? min(cb, 2.0 * cs) : max(cb, 2.0 * cs - 1.0); // Pin light
    if (m == 21) return cb + cs - 2.0 * cb * cs;                      // Exclusion
    if (m == 22) return cs <= 0.0 ? (cb > 0.0 ? 1.0 : 0.0) : min(1.0, cb / cs); // Divide
    return cs;
}
vec3 blendColor(int m, vec3 cb, vec3 cs) {
    if (m == 23) return setLum(setSat(cs, sat(cb)), lum(cb));        // Hue
    if (m == 24) return setLum(setSat(cb, sat(cs)), lum(cb));        // Saturation
    if (m == 25) return setLum(cs, lum(cb));                         // Color
    if (m == 26) return setLum(cb, lum(cs));                         // Luminosity
    return vec3(channel(m, cb.r, cs.r), channel(m, cb.g, cs.g), channel(m, cb.b, cs.b));
}
void main() {
    vec4 s = texture(src, (gl_FragCoord.xy - srcOrigin) / srcSize) * opacity;
    if (useDst == 0) { fragColor = s; return; }
    vec4 d = texture(dst, gl_FragCoord.xy / dstSize);
    vec3 cs = s.a > 0.0 ? s.rgb / s.a : vec3(0.0);
    vec3 cb = d.a > 0.0 ? d.rgb / d.a : vec3(0.0);
    vec3 bl = blendColor(mode, cb, cs);
    float ra = s.a + d.a * (1.0 - s.a);
    vec3 rc = s.rgb * (1.0 - d.a) + d.rgb * (1.0 - s.a) + s.a * d.a * bl;
    fragColor = vec4(min(rc, vec3(ra)), ra);
}
)";

const char* kMask = R"(
uniform sampler2D maskTex;
uniform vec2 size;
out vec4 fragColor;
void main() { fragColor = vec4(texture(maskTex, gl_FragCoord.xy / size).a); }
)";

const char* kColorTransform = R"(
uniform sampler2D img;
uniform vec2 size;
uniform vec4 mult;
uniform vec4 add;   // 0..1
out vec4 fragColor;
void main() {
    vec4 c = texture(img, gl_FragCoord.xy / size);
    vec3 rgb = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
    rgb = clamp(rgb * mult.rgb + add.rgb, 0.0, 1.0);
    float a = clamp(c.a * mult.a + add.a, 0.0, 1.0);
    fragColor = vec4(rgb * a, a);
}
)";

const char* kCopy = R"(
uniform sampler2D img;
uniform vec2 size;
out vec4 fragColor;
void main() { fragColor = texture(img, gl_FragCoord.xy / size); }
)";

bool g_enabled = qEnvironmentVariable("VERTEXA_GPU") != QLatin1String("0");

/// Software OpenGL implementations run on the CPU and are slower than the
/// CPU renderer; they only count with VERTEXA_GPU=force (tests use Mesa's).
bool softwareRenderer(const QString& name)
{
    static const char* kSoftware[] = {"llvmpipe", "softpipe", "swrast", "software rasterizer", "swiftshader",
                                      "basic render", "gdi generic", "software renderer"};
    const QString n = name.toLower();
    for (const char* s : kSoftware)
        if (n.contains(QLatin1String(s))) return true;
    return false;
}
GlRenderer* g_instance = nullptr;
bool g_tried = false;

QRect deviceRect(const Rect& r)
{
    if (r.isEmpty()) return {};
    return QRect(QPoint(int(std::floor(r.x0)) - 1, int(std::floor(r.y0)) - 1),
                 QPoint(int(std::ceil(r.x1)) + 1, int(std::ceil(r.y1)) + 1));
}

/// Paint of a fill or stroke: a colour, or a gradient through a LUT.
struct GlPaint {
    int kind = 0;
    float color[4] = {0, 0, 0, 0};
    Affine toGradient;
    float focal = 0;
    int spread = 0;
    std::array<uint8_t, 256 * 4> lut{};

    explicit GlPaint(const FillStyle& f, const Affine& toDevice)
    {
        if (f.kind == FillStyle::Kind::Solid) {
            const float a = f.color.a / 255.0f;
            color[0] = f.color.r / 255.0f * a;
            color[1] = f.color.g / 255.0f * a;
            color[2] = f.color.b / 255.0f * a;
            color[3] = a;
            return;
        }
        kind = f.kind == FillStyle::Kind::Linear ? 1 : 2;
        toGradient = (toDevice * f.gradient.matrix).inverted();
        focal = float(std::clamp(f.gradient.focal, -0.98, 0.98));
        spread = int(f.gradient.spread);
        for (int i = 0; i < 256; ++i) {
            const Color c = f.gradient.colorAt(i / 255.0);
            const float a = c.a / 255.0f;
            lut[size_t(i) * 4 + 0] = uint8_t(std::lround(c.r * a));
            lut[size_t(i) * 4 + 1] = uint8_t(std::lround(c.g * a));
            lut[size_t(i) * 4 + 2] = uint8_t(std::lround(c.b * a));
            lut[size_t(i) * 4 + 3] = c.a;
        }
    }
};

QMatrix3x3 toMatrix(const Affine& m)
{
    const float rows[9] = {float(m.a), float(m.c), float(m.tx), float(m.b), float(m.d), float(m.ty), 0.0f, 0.0f, 1.0f};
    return QMatrix3x3(rows);
}

/// Largest scale factor of a transform (flattening must be fine enough for it).
double maxScale(const Affine& m) { return std::max({std::hypot(m.a, m.b), std::hypot(m.c, m.d), 1e-9}); }

/// Triangles whose winding numbers add up to the polygon's (counted by the
/// stencil buffer). A long contour is split into chunks, each fanned from
/// its first point, plus one fan over the chunk start points: the chords
/// cancel, and triangles stay local instead of sweeping across the shape
/// from a single point, which saves most of the fill work.
void appendFan(const std::vector<Vec2>& poly, std::vector<float>& tri, Rect& bounds)
{
    const size_t n = poly.size();
    if (n < 3) return;
    auto push = [&](Vec2 a, Vec2 b, Vec2 c) {
        for (Vec2 p : {a, b, c}) {
            tri.push_back(float(p.x));
            tri.push_back(float(p.y));
        }
    };
    auto at = [&](size_t i) { return poly[i % n]; };
    constexpr size_t kChunk = 24;
    if (n <= 2 * kChunk) {
        for (size_t i = 1; i + 1 < n; ++i) push(poly[0], poly[i], poly[i + 1]);
    } else {
        std::vector<Vec2> skeleton;
        for (size_t s = 0; s < n; s += kChunk) {
            skeleton.push_back(poly[s]);
            const size_t e = std::min(s + kChunk, n); // the last chunk closes on poly[0]
            for (size_t i = s + 1; i + 1 <= e; ++i) push(poly[s], at(i), at(i + 1));
        }
        for (size_t i = 1; i + 1 < skeleton.size(); ++i) push(skeleton[0], skeleton[i], skeleton[i + 1]);
    }
    for (const Vec2& p : poly) bounds.include(p);
}

void appendQPolygons(const QList<QPolygonF>& polys, std::vector<float>& tri, Rect& bounds)
{
    std::vector<Vec2> pts;
    for (const QPolygonF& poly : polys) {
        pts.clear();
        for (const QPointF& p : poly) pts.push_back({p.x(), p.y()});
        appendFan(pts, tri, bounds);
    }
}

void setupStroker(QPainterPathStroker& st, const StrokeStyle& s, double& width, bool& cosmetic)
{
    cosmetic = s.pattern == StrokePattern::Hairline || !s.scaleWithTransform;
    width = s.pattern == StrokePattern::Hairline ? 1.0 : s.width;
    st.setWidth(std::max(0.01, width));
    st.setCapStyle(s.cap == CapStyle::Round ? Qt::RoundCap : s.cap == CapStyle::Square ? Qt::SquareCap : Qt::FlatCap);
    st.setJoinStyle(s.join == JoinStyle::Round ? Qt::RoundJoin : s.join == JoinStyle::Miter ? Qt::MiterJoin : Qt::BevelJoin);
    st.setMiterLimit(s.miterLimit);
    const double w = std::max(0.1, width);
    if (s.pattern == StrokePattern::Dashed) {
        st.setDashPattern(QList<qreal>{std::max(0.1, s.dash / w), std::max(0.1, s.gap / w)});
    } else if (s.pattern == StrokePattern::Dotted) {
        st.setCapStyle(Qt::RoundCap);
        st.setDashPattern(QList<qreal>{0.01, std::max(0.5, (s.gap + s.width) / w)});
    }
}

} // namespace

// --- engine ------------------------------------------------------------------------------

struct GlRenderer::Impl {
    QOffscreenSurface surface;
    QOpenGLContext ctx;
    QOpenGLExtraFunctions* gl = nullptr;
    QOpenGLVertexArrayObject vao;
    QOpenGLBuffer vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLShaderProgram stencilProg, fillProg, compositeProg, maskProg, ctProg, copyProg;
    GLuint lutTex = 0, uploadTex = 0;
    int samples = 0;
    QString device;
    std::vector<std::unique_ptr<QOpenGLFramebufferObject>> msPool, texPool;

    /// Shape geometry flattened in shape space and kept in GPU buffers while
    /// its render data lives: a shape is flattened once, not every frame.
    struct Fans {
        std::unique_ptr<QOpenGLBuffer> vbo;
        int vertices = 0;
        Rect bounds; ///< shape space
    };
    struct Geometry {
        std::weak_ptr<const ShapeRenderData> owner;
        int bucket = 0;
        std::vector<Fans> fills, strokes; ///< strokes: empty for cosmetic ones
        size_t bytes = 0;
        uint64_t used = 0;
    };
    std::unordered_map<const ShapeRenderData*, Geometry> geometry;
    size_t geometryBytes = 0;
    uint64_t tick = 0;
    double lastSubmitMs = 0.0; ///< CPU time spent walking and submitting the last frame
    static constexpr size_t kGeometryBudget = size_t(256) << 20;

    ~Impl()
    {
        if (!ctx.isValid() || !ctx.makeCurrent(&surface)) return;
        geometry.clear();
        msPool.clear();
        texPool.clear();
        if (lutTex) gl->glDeleteTextures(1, &lutTex);
        if (uploadTex) gl->glDeleteTextures(1, &uploadTex);
        vbo.destroy();
        vao.destroy();
        ctx.doneCurrent();
    }

    bool link(QOpenGLShaderProgram& p, const char* fragment)
    {
        const QByteArray head = ctx.isOpenGLES() ? QByteArray("#version 300 es\nprecision highp float;\nprecision highp int;\n")
                                                 : QByteArray("#version 330 core\n");
        if (!p.addShaderFromSourceCode(QOpenGLShader::Vertex, head + kVertex)) return false;
        if (!p.addShaderFromSourceCode(QOpenGLShader::Fragment, head + fragment)) return false;
        p.bindAttributeLocation("pos", 0);
        return p.link();
    }

    bool init()
    {
        QSurfaceFormat fmt;
        fmt.setRenderableType(QSurfaceFormat::OpenGL);
        fmt.setVersion(3, 3);
        fmt.setProfile(QSurfaceFormat::CoreProfile);
        fmt.setDepthBufferSize(0);
        fmt.setStencilBufferSize(0);
        surface.setFormat(fmt);
        surface.create();
        ctx.setFormat(fmt);
        bool ok = ctx.create() && ctx.format().version() >= qMakePair(3, 3);
        if (!ok) {
            fmt.setRenderableType(QSurfaceFormat::OpenGLES);
            fmt.setVersion(3, 0);
            fmt.setProfile(QSurfaceFormat::NoProfile);
            ctx.setFormat(fmt);
            ok = ctx.create() && ctx.isOpenGLES() && ctx.format().version() >= qMakePair(3, 0);
        }
        if (!ok || !surface.isValid() || !ctx.makeCurrent(&surface)) return false;
        gl = ctx.extraFunctions();
        GLint maxSamples = 0;
        gl->glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        samples = std::clamp(maxSamples, 0, 8);
        const QString rendererName = reinterpret_cast<const char*>(gl->glGetString(GL_RENDERER));
        device = QString("OpenGL %1 — %2").arg(reinterpret_cast<const char*>(gl->glGetString(GL_VERSION)), rendererName);
        if (softwareRenderer(rendererName) && qEnvironmentVariable("VERTEXA_GPU") != QLatin1String("force")) {
            ctx.doneCurrent();
            return false;
        }
        ok = link(stencilProg, kStencil) && link(fillProg, kFill) && link(compositeProg, kComposite) && link(maskProg, kMask) &&
             link(ctProg, kColorTransform) && link(copyProg, kCopy) && vao.create() && vbo.create();
        if (ok) {
            vao.bind();
            vbo.setUsagePattern(QOpenGLBuffer::StreamDraw);
            vbo.bind();
            gl->glEnableVertexAttribArray(0);
            for (GLuint* t : {&lutTex, &uploadTex}) {
                gl->glGenTextures(1, t);
                gl->glBindTexture(GL_TEXTURE_2D, *t);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
            // A framebuffer we can draw into proves the whole path works.
            ok = take({4, 4}, true) != nullptr;
        }
        ctx.doneCurrent();
        return ok;
    }

    std::unique_ptr<QOpenGLFramebufferObject> take(QSize size, bool multisample)
    {
        auto& pool = multisample ? msPool : texPool;
        for (auto it = pool.begin(); it != pool.end(); ++it)
            if ((*it)->size() == size) {
                auto f = std::move(*it);
                pool.erase(it);
                return f;
            }
        QOpenGLFramebufferObjectFormat fmt;
        fmt.setInternalTextureFormat(GL_RGBA8);
        fmt.setAttachment(multisample ? QOpenGLFramebufferObject::CombinedDepthStencil : QOpenGLFramebufferObject::NoAttachment);
        fmt.setSamples(multisample ? samples : 0);
        auto f = std::make_unique<QOpenGLFramebufferObject>(size, fmt);
        if (!f->isValid()) return nullptr;
        if (!multisample) {
            gl->glBindTexture(GL_TEXTURE_2D, f->texture());
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        return f;
    }

    void give(std::unique_ptr<QOpenGLFramebufferObject> f, bool multisample)
    {
        if (!f) return;
        auto& pool = multisample ? msPool : texPool;
        pool.push_back(std::move(f));
        if (pool.size() > 12) pool.erase(pool.begin());
    }

    void draw(const std::vector<float>& xy)
    {
        if (xy.empty()) return;
        vao.bind();
        vbo.bind();
        vbo.allocate(xy.data(), int(xy.size() * sizeof(float)));
        gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        gl->glDrawArrays(GL_TRIANGLES, 0, GLsizei(xy.size() / 2));
    }

    void draw(const Fans& f)
    {
        if (!f.vbo || f.vertices == 0) return;
        vao.bind();
        f.vbo->bind();
        gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        gl->glDrawArrays(GL_TRIANGLES, 0, GLsizei(f.vertices));
    }

    Fans upload(const std::vector<float>& tri, const Rect& bounds)
    {
        Fans f;
        f.bounds = bounds;
        f.vertices = int(tri.size() / 2);
        if (f.vertices == 0) return f;
        f.vbo = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
        f.vbo->setUsagePattern(QOpenGLBuffer::StaticDraw);
        f.vbo->create();
        f.vbo->bind();
        f.vbo->allocate(tri.data(), int(tri.size() * sizeof(float)));
        return f;
    }

    /// Cached geometry of `rd`, flattened finely enough for scale `scale`
    /// (in half-octave steps, so zooming within a step reuses it).
    Geometry& geometryFor(const std::shared_ptr<const ShapeRenderData>& rd, double scale)
    {
        const int bucket = int(std::floor(std::log2(scale) * 2.0));
        const double bucketScale = std::exp2((bucket + 1) / 2.0);
        const double tol = 0.2 / bucketScale;
        Geometry& g = geometry[rd.get()];
        if (g.owner.lock() == rd && g.bucket == bucket && !(g.fills.empty() && g.strokes.empty() && !rd->isEmpty())) {
            g.used = ++tick;
            return g;
        }
        geometryBytes -= std::min(geometryBytes, g.bytes);
        g = Geometry{};
        g.owner = rd;
        g.bucket = bucket;
        g.used = ++tick;
        std::vector<float> tri;
        std::vector<Vec2> pts;
        for (const auto& fp : rd->fills) {
            tri.clear();
            Rect bounds;
            for (const Contour& c : fp.contours) {
                pts.clear();
                flattenContour(c, Affine{}, tol, pts);
                appendFan(pts, tri, bounds);
            }
            g.bytes += tri.size() * sizeof(float);
            g.fills.push_back(upload(tri, bounds));
        }
        for (const auto& sp : rd->strokes) {
            double width = 1.0;
            bool cosmetic = false;
            QPainterPathStroker st;
            setupStroker(st, sp.style, width, cosmetic);
            if (cosmetic) {
                g.strokes.emplace_back(); // drawn in device space every time
                continue;
            }
            QPainterPath path;
            for (size_t i = 0; i < sp.chains.size(); ++i) appendChain(path, sp.chains[i], sp.closed[i]);
            // Flatten at the bucket's scale, then back to shape space.
            QList<QPolygonF> polys = st.createStroke(path).toSubpathPolygons(QTransform::fromScale(bucketScale, bucketScale));
            for (QPolygonF& poly : polys)
                for (QPointF& p : poly) p /= bucketScale;
            tri.clear();
            Rect bounds;
            appendQPolygons(polys, tri, bounds);
            g.bytes += tri.size() * sizeof(float);
            g.strokes.push_back(upload(tri, bounds));
        }
        geometryBytes += g.bytes;
        return g;
    }

    /// Drops geometry whose shape is gone, then the least recently used
    /// entries while over budget.
    void sweepGeometry()
    {
        for (auto it = geometry.begin(); it != geometry.end();) {
            if (it->second.owner.expired()) {
                geometryBytes -= std::min(geometryBytes, it->second.bytes);
                it = geometry.erase(it);
            } else {
                ++it;
            }
        }
        while (geometryBytes > kGeometryBudget && !geometry.empty()) {
            auto oldest = geometry.begin();
            for (auto it = geometry.begin(); it != geometry.end(); ++it)
                if (it->second.used < oldest->second.used) oldest = it;
            geometryBytes -= std::min(geometryBytes, oldest->second.bytes);
            geometry.erase(oldest);
        }
    }

    static std::vector<float> quad(const QRect& r)
    {
        const float x0 = float(r.left()), y0 = float(r.top()), x1 = float(r.right() + 1), y1 = float(r.bottom() + 1);
        return {x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1};
    }
};

// --- surfaces ---------------------------------------------------------------------------

class GlSurface final : public Surface {
public:
    GlSurface(GlRenderer::Impl& e, QSize size) : m_e(e), m_size(size.expandedTo({1, 1}))
    {
        m_ms = m_e.take(m_size, true);
        m_tex = m_e.take(m_size, false);
        if (!valid()) return;
        bindDraw();
        m_e.gl->glClearColor(0, 0, 0, 0);
        m_e.gl->glClearStencil(0);
        m_e.gl->glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    ~GlSurface() override
    {
        m_e.give(std::move(m_ms), true);
        m_e.give(std::move(m_tex), false);
    }

    bool valid() const { return m_ms && m_tex; }
    QSize size() const override { return m_size; }

    void drawShape(const std::shared_ptr<const ShapeRenderData>& rd, const Affine& m, const ColorTransform& ct,
                   const QRect& clip) override
    {
        if (!rd || rd->isEmpty() || !valid()) return;
        const QRect box = clip.intersected(rect());
        if (box.isEmpty()) return;
        if (!rd->bounds.isEmpty() && !deviceRect(m.mapRect(rd->bounds)).intersects(box)) return;
        GlRenderer::Impl::Geometry& g = m_e.geometryFor(rd, maxScale(m));
        for (size_t i = 0; i < rd->fills.size() && i < g.fills.size(); ++i) {
            const auto& fp = rd->fills[i];
            fill(g.fills[i], m, GlPaint(ct.isIdentity() ? fp.style : fp.style.withColorTransform(ct), m), box);
        }
        for (size_t i = 0; i < rd->strokes.size() && i < g.strokes.size(); ++i) {
            const auto& sp = rd->strokes[i];
            const FillStyle paint = ct.isIdentity() ? sp.style.paint : sp.style.paint.withColorTransform(ct);
            if (g.strokes[i].vbo) {
                fill(g.strokes[i], m, GlPaint(paint, m), box);
                continue;
            }
            // Cosmetic strokes keep their width on screen: stroked in device space.
            QPainterPath path;
            for (size_t k = 0; k < sp.chains.size(); ++k) appendChain(path, sp.chains[k], sp.closed[k]);
            double width = 1.0;
            bool cosmetic = false;
            QPainterPathStroker st;
            setupStroker(st, sp.style, width, cosmetic);
            std::vector<float> tri;
            Rect bounds;
            appendQPolygons(st.createStroke(toQTransform(m).map(path)).toSubpathPolygons(), tri, bounds);
            fill(tri, bounds, GlPaint(paint, m), box);
        }
    }

    void drawOutline(const ShapeRenderData& rd, const Affine& m, const QColor& color, const QRect& clip) override
    {
        if (rd.isEmpty() || !valid()) return;
        const QRect box = clip.intersected(rect());
        if (box.isEmpty()) return;
        QPainterPath path;
        for (const auto& fp : rd.fills)
            for (const Contour& c : fp.contours) appendChain(path, c, true);
        for (const auto& sp : rd.strokes)
            for (size_t i = 0; i < sp.chains.size(); ++i) appendChain(path, sp.chains[i], sp.closed[i]);
        QPainterPathStroker st;
        st.setWidth(1.0);
        std::vector<float> tri;
        Rect bounds;
        appendQPolygons(st.createStroke(toQTransform(m).map(path)).toSubpathPolygons(), tri, bounds);
        fill(tri, bounds, GlPaint(FillStyle::solid(fromQColor(color)), m), box);
    }

    std::unique_ptr<Surface> makeLayer(QSize size) override { return std::make_unique<GlSurface>(m_e, size); }

    void composite(Surface& layerSurface, QPoint at, BlendMode mode, double opacity) override
    {
        auto& layer = static_cast<GlSurface&>(layerSurface);
        const QRect area = QRect(at, layer.size()).intersected(rect());
        if (area.isEmpty() || opacity <= 0.0 || !valid() || !layer.valid()) return;
        const bool simple = mode == BlendMode::Normal || mode == BlendMode::Layer || mode == BlendMode::Alpha || mode == BlendMode::Erase;
        const GLuint src = layer.resolved();
        const GLuint dst = simple ? 0 : resolved();
        auto* gl = m_e.gl;
        bindDraw();
        scissor(area);
        QOpenGLShaderProgram& p = m_e.compositeProg;
        p.bind();
        setQuadUniforms(p);
        p.setUniformValue("srcOrigin", QVector2D(at.x(), at.y()));
        p.setUniformValue("srcSize", QVector2D(layer.size().width(), layer.size().height()));
        p.setUniformValue("dstSize", QVector2D(m_size.width(), m_size.height()));
        p.setUniformValue("mode", int(mode));
        p.setUniformValue("opacity", float(std::clamp(opacity, 0.0, 1.0)));
        p.setUniformValue("useDst", simple ? 0 : 1);
        p.setUniformValue("src", 0);
        p.setUniformValue("dst", 1);
        gl->glActiveTexture(GL_TEXTURE1);
        gl->glBindTexture(GL_TEXTURE_2D, dst ? dst : src);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, src);
        if (simple) {
            gl->glEnable(GL_BLEND);
            if (mode == BlendMode::Alpha) gl->glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
            else if (mode == BlendMode::Erase) gl->glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
            else gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        m_e.draw(GlRenderer::Impl::quad(area));
        endDraw();
    }

    void applyMask(Surface& maskSurface) override
    {
        auto& mask = static_cast<GlSurface&>(maskSurface);
        if (!valid() || !mask.valid()) return;
        const GLuint tex = mask.resolved();
        auto* gl = m_e.gl;
        bindDraw();
        QOpenGLShaderProgram& p = m_e.maskProg;
        p.bind();
        setQuadUniforms(p);
        p.setUniformValue("size", QVector2D(mask.size().width(), mask.size().height()));
        p.setUniformValue("maskTex", 0);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, tex);
        gl->glEnable(GL_BLEND);
        gl->glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
        m_e.draw(GlRenderer::Impl::quad(rect()));
        endDraw();
    }

    void applyFilters(const FilterList& filters, double scale) override
    {
        // Filters run on the CPU, on this (instance-sized) surface only.
        if (!valid() || !hasActiveFilters(filters)) return;
        QImage img = read();
        vx::applyFilters(img, filters, scale);
        write(img);
    }

    void applyColorTransform(const ColorTransform& ct) override
    {
        if (ct.isIdentity() || !valid()) return;
        const GLuint tex = resolved();
        auto* gl = m_e.gl;
        bindDraw();
        QOpenGLShaderProgram& p = m_e.ctProg;
        p.bind();
        setQuadUniforms(p);
        p.setUniformValue("size", QVector2D(m_size.width(), m_size.height()));
        p.setUniformValue("mult", QVector4D(float(ct.rm), float(ct.gm), float(ct.bm), float(ct.am)));
        p.setUniformValue("add", QVector4D(float(ct.ro / 255.0), float(ct.go / 255.0), float(ct.bo / 255.0), float(ct.ao / 255.0)));
        p.setUniformValue("img", 0);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, tex);
        m_e.draw(GlRenderer::Impl::quad(rect()));
        endDraw();
    }

    /// The pixels (premultiplied ARGB32).
    QImage read()
    {
        QImage img(m_size, QImage::Format_RGBA8888_Premultiplied);
        if (!valid()) {
            img.fill(0);
            return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
        resolved();
        m_tex->bind();
        m_e.gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
        m_e.gl->glReadPixels(0, 0, m_size.width(), m_size.height(), GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
        return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

private:
    void bindDraw()
    {
        m_ms->bind();
        m_e.gl->glViewport(0, 0, m_size.width(), m_size.height());
    }

    void scissor(const QRect& r)
    {
        m_e.gl->glEnable(GL_SCISSOR_TEST);
        m_e.gl->glScissor(r.x(), r.y(), r.width(), r.height());
    }

    void endDraw()
    {
        m_e.gl->glDisable(GL_BLEND);
        m_e.gl->glDisable(GL_SCISSOR_TEST);
        m_e.gl->glDisable(GL_STENCIL_TEST);
        m_dirty = true;
    }

    GLuint resolved()
    {
        if (m_dirty) {
            QOpenGLFramebufferObject::blitFramebuffer(m_tex.get(), m_ms.get(), GL_COLOR_BUFFER_BIT, GL_NEAREST);
            m_dirty = false;
        }
        return m_tex->texture();
    }

    void write(const QImage& image)
    {
        const QImage img = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        auto* gl = m_e.gl;
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, m_e.uploadTex);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
        bindDraw();
        QOpenGLShaderProgram& p = m_e.copyProg;
        p.bind();
        setQuadUniforms(p);
        p.setUniformValue("size", QVector2D(m_size.width(), m_size.height()));
        p.setUniformValue("img", 0);
        m_e.draw(GlRenderer::Impl::quad(rect()));
        endDraw();
    }

    /// Stencil-then-cover: count the winding of the fans in the stencil
    /// buffer, then paint the covered samples once and reset the stencil.
    void fill(const std::vector<float>& tri, const Rect& deviceBounds, const GlPaint& paint, const QRect& clip)
    {
        if (tri.empty() || deviceBounds.isEmpty()) return;
        stencilCover([&] { m_e.draw(tri); }, Affine{}, deviceRect(deviceBounds).intersected(clip), paint, clip);
    }

    void fill(const GlRenderer::Impl::Fans& fans, const Affine& m, const GlPaint& paint, const QRect& clip)
    {
        if (!fans.vbo || fans.bounds.isEmpty()) return;
        stencilCover([&] { m_e.draw(fans); }, m, deviceRect(m.mapRect(fans.bounds)).intersected(clip), paint, clip);
    }

    template <class DrawFans>
    void stencilCover(DrawFans drawFans, const Affine& xform, const QRect& cover, const GlPaint& paint, const QRect& clip)
    {
        if (cover.isEmpty()) return;
        auto* gl = m_e.gl;
        bindDraw();
        scissor(clip);
        gl->glEnable(GL_STENCIL_TEST);
        gl->glStencilMask(0xff);
        gl->glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        gl->glStencilFunc(GL_ALWAYS, 0, 0xff);
        gl->glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
        gl->glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
        QOpenGLShaderProgram& sp = m_e.stencilProg;
        sp.bind();
        sp.setUniformValue("viewport", QVector2D(m_size.width(), m_size.height()));
        sp.setUniformValue("xform", toMatrix(xform));
        drawFans();

        gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl->glStencilFunc(GL_NOTEQUAL, 0, 0xff);
        gl->glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);
        gl->glEnable(GL_BLEND);
        gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        QOpenGLShaderProgram& fp = m_e.fillProg;
        fp.bind();
        setQuadUniforms(fp);
        fp.setUniformValue("kind", paint.kind);
        fp.setUniformValue("color", QVector4D(paint.color[0], paint.color[1], paint.color[2], paint.color[3]));
        if (paint.kind != 0) {
            fp.setUniformValue("toGradient", toMatrix(paint.toGradient));
            fp.setUniformValue("focal", paint.focal);
            fp.setUniformValue("spread", paint.spread);
            fp.setUniformValue("lut", 0);
            gl->glActiveTexture(GL_TEXTURE0);
            gl->glBindTexture(GL_TEXTURE_2D, m_e.lutTex);
            gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, paint.lut.data());
        }
        m_e.draw(GlRenderer::Impl::quad(cover));
        endDraw();
    }

    /// Viewport and identity transform for full-surface quads.
    void setQuadUniforms(QOpenGLShaderProgram& p)
    {
        p.setUniformValue("viewport", QVector2D(m_size.width(), m_size.height()));
        p.setUniformValue("xform", toMatrix(Affine{}));
    }

    GlRenderer::Impl& m_e;
    QSize m_size;
    std::unique_ptr<QOpenGLFramebufferObject> m_ms, m_tex;
    bool m_dirty = true;
};

// --- renderer -------------------------------------------------------------------------------

GlRenderer::GlRenderer() : d(std::make_unique<Impl>()) {}
GlRenderer::~GlRenderer() = default;

GlRenderer* GlRenderer::instance()
{
    if (!g_enabled) return nullptr;
    QCoreApplication* app = QCoreApplication::instance();
    if (!app || QThread::currentThread() != app->thread()) return nullptr;
    if (!g_tried) {
        g_tried = true;
        std::unique_ptr<GlRenderer> r(new GlRenderer());
        if (r->d->init()) {
            g_instance = r.release();
            // Release the GL objects while the application still exists.
            qAddPostRoutine([] {
                delete g_instance;
                g_instance = nullptr;
            });
        }
    }
    return g_instance;
}

void GlRenderer::setEnabled(bool on) { g_enabled = on; }
bool GlRenderer::enabled() { return g_enabled; }
QString GlRenderer::deviceName() const { return d->device; }
int GlRenderer::samples() const { return d->samples; }
double GlRenderer::lastSubmitMs() const { return d->lastSubmitMs; }

bool GlRenderer::render(QImage& target, const Document& doc, const Timeline& tl, int frame, const Affine& view,
                        const ColorTransform& ct, const RenderOptions& opts, bool targetEmpty)
{
    if (target.isNull() || !d->ctx.makeCurrent(&d->surface)) return false;
    bool ok = false;
    {
        GlSurface root(*d, target.size());
        if (root.valid()) {
            QElapsedTimer submit;
            submit.start();
            Renderer(doc, opts).render(root, tl, frame, view, ct);
            d->sweepGeometry();
            d->lastSubmitMs = submit.nsecsElapsed() / 1e6;
            QImage img = root.read();
            if (targetEmpty) {
                img.setDevicePixelRatio(target.devicePixelRatio());
                target = std::move(img);
            } else {
                compositeImage(target, img, QPoint(0, 0), BlendMode::Normal, 1.0);
            }
            ok = true;
        }
    }
    d->ctx.doneCurrent();
    return ok;
}

bool renderAccelerated(QImage& target, const Document& doc, const Timeline& tl, int frame, const Affine& view,
                       const ColorTransform& ct, const RenderOptions& opts, bool targetEmpty)
{
    if (GlRenderer* gpu = GlRenderer::instance())
        if (gpu->render(target, doc, tl, frame, view, ct, opts, targetEmpty)) return true;
    Renderer(doc, opts).render(target, tl, frame, view, ct);
    return false;
}

} // namespace vx
