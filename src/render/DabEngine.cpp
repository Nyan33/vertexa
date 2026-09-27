// SPDX-License-Identifier: GPL-3.0-or-later
#include "DabEngine.h"
#include "Blend.h"
#include "BrushResources.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vx {

namespace {

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    float next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return float(s & 0xffffff) / float(0x1000000);
    }
};

inline uint32_t pixelHash(int x, int y, uint32_t seed)
{
    uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ seed * 83492791u;
    h = (h ^ (h >> 15)) * 2246822519u;
    return h ^ (h >> 13);
}

struct Sample {
    Vec2 p;  // device
    double pressure, tiltX, tiltY, rotation;
};

Sample mix(const Sample& a, const Sample& b, double t)
{
    return {lerp(a.p, b.p, t), a.pressure + (b.pressure - a.pressure) * t, a.tiltX + (b.tiltX - a.tiltX) * t,
            a.tiltY + (b.tiltY - a.tiltY) * t, a.rotation + (b.rotation - a.rotation) * t};
}

struct StrokeRenderer {
    const BrushPreset& p;
    const DabContext& ctx;
    GrayImagePtr tip, tex;
    Affine devToLocal;
    double baseDiameter = 1.0;
    double deviceRotation = 0.0;
    Rng rng;
    int bx0 = 0, by0 = 0, bw = 0, bh = 0;
    std::vector<float> alpha;
    uint32_t seed;

    StrokeRenderer(const BrushPreset& preset, const DabContext& c, uint32_t sd) : p(preset), ctx(c), rng(sd), seed(sd) {}

    double diameterFor(double pressure) const
    {
        double f = 1.0;
        if (p.pressureSize) f = p.minSize + (1.0 - p.minSize) * p.sizeCurve.eval(pressure);
        return std::max(0.25, baseDiameter * f);
    }

    double spacingFor(double pressure) const { return std::max(0.35, p.spacing * diameterFor(pressure)); }

    float textureAt(double px, double py) const
    {
        if (!tex) return 1.0f;
        const Vec2 local = devToLocal.map({px, py});
        const double sc = std::max(0.01, p.textureScale);
        double v = tex->sampleWrapped(local.x / sc, local.y / sc);
        if (p.textureInvert) v = 1.0 - v;
        v = std::clamp((v - 0.5) * p.textureContrast + 0.5 + p.textureBrightness, 0.0, 1.0);
        return float(v);
    }

    float tipAt(double lx, double ly, double radiusPx) const
    {
        if (tip) {
            const double u = (lx + 1.0) * 0.5 * tip->width, v = (ly + 1.0) * 0.5 * tip->height;
            return float(tip->sampleClamped(u, v));
        }
        const double d = p.autoShape == AutoTipShape::Circle ? std::hypot(lx, ly) : std::max(std::abs(lx), std::abs(ly));
        if (d >= 1.0) return 0.0f;
        const double h = std::clamp(p.hardness, 0.0, 1.0);
        double fall = 1.0;
        if (d > h) {
            const double t = (d - h) / std::max(1e-6, 1.0 - h);
            fall = 1.0 - t * t * (3.0 - 2.0 * t);
        }
        const double edge = std::clamp((1.0 - d) * radiusPx, 0.0, 1.0);
        return float(std::min(fall, edge));
    }

    void dab(Vec2 c, double diameter, double angle, float flow, float cap)
    {
        const double r = diameter * 0.5;
        const double aspect = std::clamp(p.roundness, 0.05, 1.0);
        // Tiny dabs: keep a minimum footprint and fade instead.
        float fade = 1.0f;
        double rr = r;
        if (rr < 0.6) {
            fade = float((rr * rr) / 0.36);
            rr = 0.6;
        }
        const int x0 = std::max(bx0, int(std::floor(c.x - rr - 1))), x1 = std::min(bx0 + bw - 1, int(std::ceil(c.x + rr + 1)));
        const int y0 = std::max(by0, int(std::floor(c.y - rr - 1))), y1 = std::min(by0 + bh - 1, int(std::ceil(c.y + rr + 1)));
        if (x0 > x1 || y0 > y1) return;
        const double cs = std::cos(-angle), sn = std::sin(-angle);
        const double invR = 1.0 / rr, invRy = 1.0 / (rr * aspect);
        for (int y = y0; y <= y1; ++y) {
            float* row = alpha.data() + size_t(y - by0) * bw;
            for (int x = x0; x <= x1; ++x) {
                const double dx = x + 0.5 - c.x, dy = y + 0.5 - c.y;
                const double lx = (dx * cs - dy * sn) * invR, ly = (dx * sn + dy * cs) * invRy;
                if (lx * lx + ly * ly > 2.0) continue;
                float m = tipAt(lx, ly, rr * aspect);
                if (m <= 0.0f) continue;
                if (p.tipDensity < 1.0 && (pixelHash(x, y, seed) & 0xffff) / 65535.0 > p.tipDensity) continue;
                if (tex) {
                    const float t = textureAt(x + 0.5, y + 0.5);
                    const float s = float(p.textureStrength);
                    switch (p.textureMode) {
                    case TextureMode::Multiply: m *= 1.0f - s + s * t; break;
                    case TextureMode::Subtract: m = std::max(0.0f, m - s * (1.0f - t)); break;
                    case TextureMode::Height: m = std::clamp((m - s * (1.0f - t)) * (1.0f + s), 0.0f, 1.0f); break;
                    }
                }
                const float a = m * flow * fade;
                if (a <= 0.0f) continue;
                float& cur = row[x - bx0];
                if (p.buildUp) cur = cur + (1.0f - cur) * a * cap;
                else if (cur < cap) cur = cur + (cap - cur) * a;
            }
        }
    }

    void place(const Sample& s, Vec2 dir)
    {
        const double pr = std::clamp(s.pressure, 0.0, 1.0);
        double diameter = diameterFor(pr);
        if (p.sizeJitter > 0) diameter *= 1.0 - p.sizeJitter * rng.next();
        float cap = float(p.opacity * (p.pressureOpacity ? p.opacityCurve.eval(pr) : 1.0));
        if (p.opacityJitter > 0) cap *= 1.0f - float(p.opacityJitter) * rng.next();
        const float flow = float(p.flow * (p.pressureFlow ? p.flowCurve.eval(pr) : 1.0));
        double angle = p.angle * kPi / 180.0 + deviceRotation;
        if (p.tiltAngle && (s.tiltX != 0.0 || s.tiltY != 0.0)) angle += std::atan2(s.tiltY, s.tiltX);
        if (p.followDirection && dir.lengthSq() > 0) angle += dir.angle();
        const Vec2 n = dir.lengthSq() > 0 ? dir.normalized().perp() : Vec2{0, 1};
        for (int k = 0; k < std::max(1, p.count); ++k) {
            double a = angle;
            if (p.rotationJitter > 0) a += (rng.next() - 0.5) * 2.0 * kPi * p.rotationJitter;
            Vec2 c = s.p;
            if (p.scatter > 0) {
                c += n * ((rng.next() * 2.0 - 1.0) * p.scatter * diameter);
                c += dir.normalized() * ((rng.next() * 2.0 - 1.0) * p.scatter * diameter * 0.3);
            }
            dab(c, diameter, a, flow, cap);
        }
    }
};

} // namespace

void paintStroke(QImage& buffer, const PaintStroke& stroke, const DabContext& ctx)
{
    if (!stroke.brush || stroke.samples.empty() || buffer.isNull()) return;
    const BrushPreset& p = *stroke.brush;
    StrokeRenderer r(p, ctx, stroke.seed);
    r.baseDiameter = p.size * ctx.toDevice.meanScale();
    r.deviceRotation = AffineParts::decompose(ctx.toDevice).rotation();
    r.devToLocal = ctx.toDevice.inverted();
    if (p.tipType == TipType::Image) r.tip = BrushResources::image(p.tipImage, ctx.doc);
    if (p.textureEnabled) r.tex = BrushResources::image(p.texture, ctx.doc);

    std::vector<Sample> pts;
    pts.reserve(stroke.samples.size());
    Rect bb;
    for (const PaintSample& s : stroke.samples) {
        pts.push_back({ctx.toDevice.map(s.pos), s.pressure, s.tiltX, s.tiltY, s.rotation});
        bb.include(pts.back().p);
    }
    const double reach = r.baseDiameter * (0.5 + p.scatter * 1.5 + p.sizeJitter) + 2.0;
    bb = bb.inflated(reach);
    const QRect area = QRect(QPoint(int(std::floor(bb.x0)), int(std::floor(bb.y0))),
                             QPoint(int(std::ceil(bb.x1)), int(std::ceil(bb.y1))))
                           .intersected(buffer.rect());
    if (area.isEmpty()) return;
    r.bx0 = area.left();
    r.by0 = area.top();
    r.bw = area.width();
    r.bh = area.height();
    r.alpha.assign(size_t(r.bw) * r.bh, 0.0f);

    // Walk the polyline placing dabs at the spacing distance.
    Vec2 dir = pts.size() > 1 ? (pts[1].p - pts[0].p) : Vec2{1, 0};
    r.place(pts[0], dir);
    double acc = 0.0;
    double spacing = r.spacingFor(pts[0].pressure);
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const Sample& a = pts[i];
        const Sample& b = pts[i + 1];
        const double L = distance(a.p, b.p);
        if (L <= 1e-9) continue;
        dir = b.p - a.p;
        double pos = 0.0;
        while (pos + (spacing - acc) <= L) {
            pos += spacing - acc;
            acc = 0.0;
            const Sample s = mix(a, b, pos / L);
            r.place(s, dir);
            spacing = r.spacingFor(s.pressure);
        }
        acc += L - pos;
    }

    // Composite the stroke mask with the stroke colour.
    const Color col = ctx.color.apply(stroke.color);
    QImage src(r.bw, r.bh, QImage::Format_ARGB32_Premultiplied);
    src.fill(0);
    const float ca = col.a / 255.0f;
    for (int y = 0; y < r.bh; ++y) {
        auto* line = reinterpret_cast<uint32_t*>(src.scanLine(y));
        const float* ar = r.alpha.data() + size_t(y) * r.bw;
        for (int x = 0; x < r.bw; ++x) {
            const float a = ar[x] * ca;
            if (a <= 0.0f) continue;
            auto q = [](float v) { return uint32_t(std::clamp(int(v * 255.0f + 0.5f), 0, 255)); };
            line[x] = (q(a) << 24) | (q(col.r / 255.0f * a) << 16) | (q(col.g / 255.0f * a) << 8) | q(col.b / 255.0f * a);
        }
    }
    compositeImage(buffer, src, area.topLeft(), stroke.erase ? BlendMode::Erase : p.blend, 1.0);
}

void paintElement(QImage& buffer, const PaintElement& element, const DabContext& ctx)
{
    for (const PaintStroke& s : element.strokes) paintStroke(buffer, s, ctx);
}

QImage brushPreview(const BrushPreset& preset, const Color& color, int width, int height, const Document* doc)
{
    QImage img(width, height, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    PaintStroke s;
    auto pp = std::make_shared<BrushPreset>(preset);
    const double maxSize = height * 0.55;
    const double scale = preset.size > maxSize ? maxSize / preset.size : 1.0;
    s.brush = pp;
    s.color = color;
    s.seed = 7;
    const int n = 64;
    for (int i = 0; i <= n; ++i) {
        const double t = double(i) / n;
        const double x = width * (0.1 + 0.8 * t) / scale;
        const double y = (height * 0.5 + std::sin(t * 2.0 * kPi) * height * 0.18) / scale;
        PaintSample ps;
        ps.pos = {x, y};
        ps.pressure = float(std::sin(t * kPi) * 0.9 + 0.1);
        ps.tiltX = 20.0f;
        ps.tiltY = 20.0f;
        s.samples.push_back(ps);
    }
    DabContext ctx;
    ctx.toDevice = Affine::scale(scale);
    ctx.doc = doc;
    paintStroke(img, s, ctx);
    return img;
}

} // namespace vx
