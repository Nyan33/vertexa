// SPDX-License-Identifier: GPL-3.0-or-later
#include "Filters.h"

#include "geom/Vec2.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vx {

namespace {

/// A float plane (one channel) the size of the image.
struct Plane {
    int w = 0, h = 0;
    std::vector<float> v;
    Plane() = default;
    Plane(int w_, int h_, float fill = 0.f) : w(w_), h(h_), v(size_t(w_) * size_t(h_), fill) {}
    float& at(int x, int y) { return v[size_t(y) * size_t(w) + size_t(x)]; }
    float at(int x, int y) const { return v[size_t(y) * size_t(w) + size_t(x)]; }
    /// Bilinear sample with zero outside.
    float sample(double x, double y) const
    {
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const float fx = float(x - x0), fy = float(y - y0);
        auto get = [&](int xx, int yy) { return xx < 0 || yy < 0 || xx >= w || yy >= h ? 0.f : at(xx, yy); };
        const float a = get(x0, y0) * (1 - fx) + get(x0 + 1, y0) * fx;
        const float b = get(x0, y0 + 1) * (1 - fx) + get(x0 + 1, y0 + 1) * fx;
        return a * (1 - fy) + b * fy;
    }
};

/// One box blur pass along a line with a (possibly fractional) radius.
void boxLine(const float* in, float* out, int n, int stride, double radius, std::vector<double>& prefix)
{
    if (radius <= 0.0) {
        for (int i = 0; i < n; ++i) out[i * stride] = in[i * stride];
        return;
    }
    prefix.assign(size_t(n) + 1, 0.0);
    for (int i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + in[i * stride];
    auto sumTo = [&](int i) { return prefix[size_t(std::clamp(i, 0, n))]; }; // sum of [0, i)
    const int r0 = int(std::floor(radius));
    const double f = radius - r0;
    const double w0 = 2.0 * r0 + 1.0, w1 = 2.0 * (r0 + 1) + 1.0;
    for (int i = 0; i < n; ++i) {
        const double s0 = (sumTo(i + r0 + 1) - sumTo(i - r0)) / w0;
        double s = s0;
        if (f > 1e-6) {
            const double s1 = (sumTo(i + r0 + 2) - sumTo(i - r0 - 1)) / w1;
            s = s0 * (1 - f) + s1 * f;
        }
        out[i * stride] = float(s);
    }
}

void blurPlane(Plane& p, double rx, double ry, int passes)
{
    if (rx <= 0 && ry <= 0) return;
    std::vector<float> tmp(p.v.size());
    std::vector<double> prefix;
    for (int pass = 0; pass < passes; ++pass) {
        if (rx > 0) {
            for (int y = 0; y < p.h; ++y) boxLine(&p.v[size_t(y) * p.w], &tmp[size_t(y) * p.w], p.w, 1, rx, prefix);
            p.v.swap(tmp);
        }
        if (ry > 0) {
            for (int x = 0; x < p.w; ++x) boxLine(&p.v[x], &tmp[x], p.h, p.w, ry, prefix);
            p.v.swap(tmp);
        }
    }
}

struct Rgba {
    Plane r, g, b, a; ///< premultiplied, 0..1
};

Rgba toPlanes(const QImage& img)
{
    Rgba o{Plane(img.width(), img.height()), Plane(img.width(), img.height()), Plane(img.width(), img.height()),
           Plane(img.width(), img.height())};
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb px = line[x];
            o.r.at(x, y) = qRed(px) / 255.f;
            o.g.at(x, y) = qGreen(px) / 255.f;
            o.b.at(x, y) = qBlue(px) / 255.f;
            o.a.at(x, y) = qAlpha(px) / 255.f;
        }
    }
    return o;
}

void fromPlanes(const Rgba& p, QImage& img)
{
    for (int y = 0; y < img.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const float a = std::clamp(p.a.at(x, y), 0.f, 1.f);
            auto ch = [a](float v) { return int(std::lround(std::clamp(v, 0.f, a) * 255.f)); };
            line[x] = qRgba(ch(p.r.at(x, y)), ch(p.g.at(x, y)), ch(p.b.at(x, y)), int(std::lround(a * 255.f)));
        }
    }
}

/// Source-over of a premultiplied colour layer (`col` * `cov`) onto `dst`.
void over(Rgba& dst, const Plane& cov, const Color& c, bool below)
{
    const float cr = c.r / 255.f, cg = c.g / 255.f, cb = c.b / 255.f, ca = c.a / 255.f;
    for (size_t i = 0; i < dst.a.v.size(); ++i) {
        const float k = std::clamp(cov.v[i], 0.f, 1.f) * ca;
        if (k <= 0.f) continue;
        float& r = dst.r.v[i];
        float& g = dst.g.v[i];
        float& b = dst.b.v[i];
        float& a = dst.a.v[i];
        if (below) { // layer behind the existing content
            const float t = 1.f - a;
            r += cr * k * t;
            g += cg * k * t;
            b += cb * k * t;
            a += k * t;
        } else {
            r = cr * k + r * (1.f - k);
            g = cg * k + g * (1.f - k);
            b = cb * k + b * (1.f - k);
            a = k + a * (1.f - k);
        }
    }
}

void clearContent(Rgba& p)
{
    std::fill(p.r.v.begin(), p.r.v.end(), 0.f);
    std::fill(p.g.v.begin(), p.g.v.end(), 0.f);
    std::fill(p.b.v.begin(), p.b.v.end(), 0.f);
    std::fill(p.a.v.begin(), p.a.v.end(), 0.f);
}

Plane shifted(const Plane& p, double dx, double dy)
{
    Plane o(p.w, p.h);
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) o.at(x, y) = p.sample(x - dx, y - dy);
    return o;
}

void shadowOrGlow(Rgba& img, const Filter& f, double scale, bool glow)
{
    const Plane alpha = img.a;
    Plane src = alpha;
    if (f.inner)
        for (float& v : src.v) v = 1.f - v;
    const double a = f.angle * kPi / 180.0;
    const double dist = glow ? 0.0 : f.distance * scale;
    blurPlane(src, f.blurX * scale * 0.5, f.blurY * scale * 0.5, f.quality);
    Plane m = std::abs(dist) > 1e-6 ? shifted(src, std::cos(a) * dist, std::sin(a) * dist) : src;
    const float strength = float(f.strength);
    for (size_t i = 0; i < m.v.size(); ++i) {
        float v = std::clamp(m.v[i] * strength, 0.f, 1.f);
        v *= f.inner ? alpha.v[i] : 1.f - (f.knockout ? alpha.v[i] : 0.f);
        m.v[i] = v;
    }
    if (f.inner) {
        if (f.knockout) clearContent(img);
        over(img, m, f.color, false);
        return;
    }
    if (f.knockout || f.hideObject) clearContent(img);
    over(img, m, f.color, true);
}

void bevel(Rgba& img, const Filter& f, double scale, bool gradient)
{
    const Plane alpha = img.a;
    Plane src = alpha;
    blurPlane(src, f.blurX * scale * 0.5, f.blurY * scale * 0.5, f.quality);
    const double a = f.angle * kPi / 180.0;
    const double dx = std::cos(a) * f.distance * scale, dy = std::sin(a) * f.distance * scale;
    const Plane toward = shifted(src, -dx, -dy); // samples in the light direction
    const Plane away = shifted(src, dx, dy);
    Plane hi(src.w, src.h), lo(src.w, src.h), t(src.w, src.h);
    const float strength = float(f.strength);
    for (size_t i = 0; i < src.v.size(); ++i) {
        float d = (toward.v[i] - away.v[i]) * strength;
        float mask = 1.f;
        if (f.bevel == BevelKind::Inner) mask = alpha.v[i];
        else if (f.bevel == BevelKind::Outer) mask = 1.f - alpha.v[i];
        d = std::clamp(d, -1.f, 1.f) * mask;
        hi.v[i] = std::max(0.f, d);
        lo.v[i] = std::max(0.f, -d);
        t.v[i] = d;
    }
    if (f.knockout) clearContent(img);
    if (!gradient) {
        over(img, hi, f.highlight, false);
        over(img, lo, f.color, false);
        return;
    }
    for (size_t i = 0; i < t.v.size(); ++i) {
        const float v = t.v[i];
        if (std::abs(v) < 1e-4f) continue;
        const Color c = f.gradient.colorAt(0.5 + 0.5 * v);
        const float k = std::abs(v) * c.a / 255.f;
        img.r.v[i] = c.r / 255.f * k + img.r.v[i] * (1 - k);
        img.g.v[i] = c.g / 255.f * k + img.g.v[i] * (1 - k);
        img.b.v[i] = c.b / 255.f * k + img.b.v[i] * (1 - k);
        img.a.v[i] = k + img.a.v[i] * (1 - k);
    }
}

void gradientGlow(Rgba& img, const Filter& f, double scale)
{
    const Plane alpha = img.a;
    Plane m = alpha;
    if (f.inner)
        for (float& v : m.v) v = 1.f - v;
    blurPlane(m, f.blurX * scale * 0.5, f.blurY * scale * 0.5, f.quality);
    const double a = f.angle * kPi / 180.0;
    if (std::abs(f.distance) > 1e-6)
        m = shifted(m, std::cos(a) * f.distance * scale, std::sin(a) * f.distance * scale);
    Rgba glow{Plane(m.w, m.h), Plane(m.w, m.h), Plane(m.w, m.h), Plane(m.w, m.h)};
    for (size_t i = 0; i < m.v.size(); ++i) {
        const float v = std::clamp(m.v[i] * float(f.strength), 0.f, 1.f);
        const float mask = f.inner ? alpha.v[i] : (f.knockout ? 1.f - alpha.v[i] : 1.f);
        if (v <= 0.f || mask <= 0.f) continue;
        const Color c = f.gradient.colorAt(v);
        const float k = c.a / 255.f * mask;
        glow.r.v[i] = c.r / 255.f * k;
        glow.g.v[i] = c.g / 255.f * k;
        glow.b.v[i] = c.b / 255.f * k;
        glow.a.v[i] = k;
    }
    if (f.knockout) clearContent(img);
    for (size_t i = 0; i < m.v.size(); ++i) {
        const float ga = glow.a.v[i];
        if (ga <= 0.f) continue;
        if (f.inner) { // glow over the object
            img.r.v[i] = glow.r.v[i] + img.r.v[i] * (1 - ga);
            img.g.v[i] = glow.g.v[i] + img.g.v[i] * (1 - ga);
            img.b.v[i] = glow.b.v[i] + img.b.v[i] * (1 - ga);
            img.a.v[i] = ga + img.a.v[i] * (1 - ga);
        } else { // glow behind the object
            const float t = 1 - img.a.v[i];
            img.r.v[i] += glow.r.v[i] * t;
            img.g.v[i] += glow.g.v[i] * t;
            img.b.v[i] += glow.b.v[i] * t;
            img.a.v[i] += ga * t;
        }
    }
}

void adjustColor(Rgba& img, const Filter& f)
{
    const std::array<double, 20> m = adjustColorMatrix(f);
    for (size_t i = 0; i < img.a.v.size(); ++i) {
        const float a = img.a.v[i];
        if (a <= 0.f) continue;
        const double r = img.r.v[i] / a * 255, g = img.g.v[i] / a * 255, b = img.b.v[i] / a * 255, al = a * 255;
        auto row = [&](int k) { return std::clamp(m[k * 5] * r + m[k * 5 + 1] * g + m[k * 5 + 2] * b + m[k * 5 + 3] * al + m[k * 5 + 4], 0.0, 255.0); };
        const double nr = row(0), ng = row(1), nb = row(2), na = row(3) / 255.0;
        img.r.v[i] = float(nr / 255 * na);
        img.g.v[i] = float(ng / 255 * na);
        img.b.v[i] = float(nb / 255 * na);
        img.a.v[i] = float(na);
    }
}

} // namespace

bool hasActiveFilters(const FilterList& filters)
{
    for (const Filter& f : filters) {
        if (!f.enabled) continue;
        if (f.type == FilterType::AdjustColor) {
            if (f.brightness != 0 || f.contrast != 0 || f.saturation != 0 || f.hue != 0) return true;
            continue;
        }
        return true;
    }
    return false;
}

void applyFilters(QImage& img, const FilterList& filters, double scale)
{
    if (img.isNull() || !hasActiveFilters(filters)) return;
    if (img.format() != QImage::Format_ARGB32_Premultiplied) img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    Rgba p = toPlanes(img);
    for (const Filter& f : filters) {
        if (!f.enabled) continue;
        switch (f.type) {
        case FilterType::Blur: {
            const double rx = f.blurX * scale * 0.5, ry = f.blurY * scale * 0.5;
            for (Plane* pl : {&p.r, &p.g, &p.b, &p.a}) blurPlane(*pl, rx, ry, f.quality);
            break;
        }
        case FilterType::DropShadow: shadowOrGlow(p, f, scale, false); break;
        case FilterType::Glow: shadowOrGlow(p, f, scale, true); break;
        case FilterType::Bevel: bevel(p, f, scale, false); break;
        case FilterType::GradientBevel: bevel(p, f, scale, true); break;
        case FilterType::GradientGlow: gradientGlow(p, f, scale); break;
        case FilterType::AdjustColor: adjustColor(p, f); break;
        case FilterType::Count: break;
        }
    }
    fromPlanes(p, img);
}

void applyColorTransform(QImage& img, const ColorTransform& ct)
{
    if (ct.isIdentity()) return;
    for (int y = 0; y < img.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb px = line[x];
            const int a = qAlpha(px);
            if (a == 0 && ct.ao <= 0) continue;
            auto un = [a](int v) { return a ? v * 255.0 / a : 0.0; };
            const double r = std::clamp(un(qRed(px)) * ct.rm + ct.ro, 0.0, 255.0);
            const double g = std::clamp(un(qGreen(px)) * ct.gm + ct.go, 0.0, 255.0);
            const double b = std::clamp(un(qBlue(px)) * ct.bm + ct.bo, 0.0, 255.0);
            const double na = std::clamp(a * ct.am + ct.ao, 0.0, 255.0) / 255.0;
            line[x] = qRgba(int(std::lround(r * na)), int(std::lround(g * na)), int(std::lround(b * na)), int(std::lround(na * 255)));
        }
    }
}

} // namespace vx
