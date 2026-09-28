// SPDX-License-Identifier: GPL-3.0-or-later
#include "Blend.h"

#include <algorithm>
#include <cmath>

namespace vx {

namespace {

double lum(double r, double g, double b) { return 0.3 * r + 0.59 * g + 0.11 * b; }

void clipColor(double& r, double& g, double& b)
{
    const double l = lum(r, g, b);
    const double n = std::min({r, g, b}), x = std::max({r, g, b});
    if (n < 0.0) {
        r = l + (r - l) * l / (l - n);
        g = l + (g - l) * l / (l - n);
        b = l + (b - l) * l / (l - n);
    }
    if (x > 1.0) {
        r = l + (r - l) * (1 - l) / (x - l);
        g = l + (g - l) * (1 - l) / (x - l);
        b = l + (b - l) * (1 - l) / (x - l);
    }
}

void setLum(double& r, double& g, double& b, double l)
{
    const double d = l - lum(r, g, b);
    r += d;
    g += d;
    b += d;
    clipColor(r, g, b);
}

double sat(double r, double g, double b) { return std::max({r, g, b}) - std::min({r, g, b}); }

void setSat(double& r, double& g, double& b, double s)
{
    double* c[3] = {&r, &g, &b};
    std::sort(c, c + 3, [](double* x, double* y) { return *x < *y; });
    double& mn = *c[0];
    double& md = *c[1];
    double& mx = *c[2];
    if (mx > mn) {
        md = (md - mn) * s / (mx - mn);
        mx = s;
    } else {
        md = mx = 0.0;
    }
    mn = 0.0;
}

bool nonSeparable(BlendMode m)
{
    return m == BlendMode::Hue || m == BlendMode::Saturation || m == BlendMode::Color || m == BlendMode::Luminosity;
}

void blendNonSeparable(BlendMode m, const double cb[3], const double cs[3], double out[3])
{
    double r, g, b;
    switch (m) {
    case BlendMode::Hue:
        r = cs[0]; g = cs[1]; b = cs[2];
        setSat(r, g, b, sat(cb[0], cb[1], cb[2]));
        setLum(r, g, b, lum(cb[0], cb[1], cb[2]));
        break;
    case BlendMode::Saturation:
        r = cb[0]; g = cb[1]; b = cb[2];
        setSat(r, g, b, sat(cs[0], cs[1], cs[2]));
        setLum(r, g, b, lum(cb[0], cb[1], cb[2]));
        break;
    case BlendMode::Color:
        r = cs[0]; g = cs[1]; b = cs[2];
        setLum(r, g, b, lum(cb[0], cb[1], cb[2]));
        break;
    default: // Luminosity
        r = cb[0]; g = cb[1]; b = cb[2];
        setLum(r, g, b, lum(cs[0], cs[1], cs[2]));
        break;
    }
    out[0] = r;
    out[1] = g;
    out[2] = b;
}

} // namespace

double blendChannel(BlendMode mode, double cb, double cs)
{
    switch (mode) {
    case BlendMode::Normal:
    case BlendMode::Layer: return cs;
    case BlendMode::Darken: return std::min(cb, cs);
    case BlendMode::Multiply: return cb * cs;
    case BlendMode::Lighten: return std::max(cb, cs);
    case BlendMode::Screen: return cb + cs - cb * cs;
    case BlendMode::Overlay: return cb <= 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
    case BlendMode::HardLight: return cs <= 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
    case BlendMode::Add: return std::min(1.0, cb + cs);
    case BlendMode::Subtract: return std::max(0.0, cb - cs);
    case BlendMode::Difference: return std::abs(cb - cs);
    case BlendMode::Invert: return 1.0 - cb;
    case BlendMode::ColorBurn:
        if (cb >= 1.0) return 1.0;
        if (cs <= 0.0) return 0.0;
        return 1.0 - std::min(1.0, (1.0 - cb) / cs);
    case BlendMode::LinearBurn: return std::max(0.0, cb + cs - 1.0);
    case BlendMode::ColorDodge:
        if (cb <= 0.0) return 0.0;
        if (cs >= 1.0) return 1.0;
        return std::min(1.0, cb / (1.0 - cs));
    case BlendMode::SoftLight: {
        if (cs <= 0.5) return cb - (1 - 2 * cs) * cb * (1 - cb);
        const double d = cb <= 0.25 ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(cb);
        return cb + (2 * cs - 1) * (d - cb);
    }
    case BlendMode::VividLight:
        if (cs <= 0.5) return cs <= 0.0 ? 0.0 : std::max(0.0, 1.0 - (1.0 - cb) / (2 * cs));
        return cs >= 1.0 ? 1.0 : std::min(1.0, cb / (2 * (1.0 - cs)));
    case BlendMode::LinearLight: return std::clamp(cb + 2 * cs - 1, 0.0, 1.0);
    case BlendMode::PinLight: return cs <= 0.5 ? std::min(cb, 2 * cs) : std::max(cb, 2 * cs - 1);
    case BlendMode::Exclusion: return cb + cs - 2 * cb * cs;
    case BlendMode::Divide: return cs <= 0.0 ? (cb > 0 ? 1.0 : 0.0) : std::min(1.0, cb / cs);
    default: return cs;
    }
}

void compositeImage(QImage& dst, const QImage& src, QPoint offset, BlendMode mode, double opacity, const QImage* mask)
{
    if (dst.isNull() || src.isNull() || opacity <= 0.0) return;
    const QRect area = QRect(offset, src.size()).intersected(dst.rect());
    if (area.isEmpty()) return;
    const float op = float(std::clamp(opacity, 0.0, 1.0));
    if ((mode == BlendMode::Normal || mode == BlendMode::Layer) && !mask && op >= 1.0f) {
        // Plain source-over, the common case: integer maths, opaque pixels copied.
        for (int y = area.top(); y <= area.bottom(); ++y) {
            auto* d = reinterpret_cast<uint32_t*>(dst.scanLine(y));
            const auto* s = reinterpret_cast<const uint32_t*>(src.constScanLine(y - offset.y())) - offset.x();
            for (int x = area.left(); x <= area.right(); ++x) {
                const uint32_t sp = s[x];
                const uint32_t sa = sp >> 24;
                if (sa == 0) continue;
                if (sa == 255) {
                    d[x] = sp;
                    continue;
                }
                const uint32_t inv = 255 - sa, dp = d[x];
                auto over = [inv](uint32_t sc, uint32_t dc) {
                    const uint32_t t = dc * inv + 128;
                    return sc + ((t + (t >> 8)) >> 8);
                };
                d[x] = over(sa, dp >> 24) << 24 | over((sp >> 16) & 0xff, (dp >> 16) & 0xff) << 16 |
                       over((sp >> 8) & 0xff, (dp >> 8) & 0xff) << 8 | over(sp & 0xff, dp & 0xff);
            }
        }
        return;
    }
    for (int y = area.top(); y <= area.bottom(); ++y) {
        auto* d = reinterpret_cast<uint32_t*>(dst.scanLine(y));
        const auto* s = reinterpret_cast<const uint32_t*>(src.constScanLine(y - offset.y()));
        const uint32_t* mk = mask ? reinterpret_cast<const uint32_t*>(mask->constScanLine(y - offset.y())) : nullptr;
        for (int x = area.left(); x <= area.right(); ++x) {
            const int sx = x - offset.x();
            uint32_t sp = s[sx];
            float k = op;
            if (mk) k *= float(mk[sx] >> 24) / 255.0f;
            if ((sp >> 24) == 0 || k <= 0.0f) continue;
            // Source premultiplied, scaled by opacity/mask.
            float sa = float(sp >> 24) / 255.0f * k;
            float sr = float((sp >> 16) & 0xff) / 255.0f * k;
            float sg = float((sp >> 8) & 0xff) / 255.0f * k;
            float sb = float(sp & 0xff) / 255.0f * k;
            const uint32_t dp = d[x];
            float da = float(dp >> 24) / 255.0f;
            float dr = float((dp >> 16) & 0xff) / 255.0f;
            float dg = float((dp >> 8) & 0xff) / 255.0f;
            float db = float(dp & 0xff) / 255.0f;
            float ra, rr, rg, rb;
            switch (mode) {
            case BlendMode::Normal:
            case BlendMode::Layer:
                ra = sa + da * (1 - sa);
                rr = sr + dr * (1 - sa);
                rg = sg + dg * (1 - sa);
                rb = sb + db * (1 - sa);
                break;
            case BlendMode::Alpha:
                // Keep the destination only where the source is opaque.
                ra = da * sa;
                rr = dr * sa;
                rg = dg * sa;
                rb = db * sa;
                break;
            case BlendMode::Erase:
                ra = da * (1 - sa);
                rr = dr * (1 - sa);
                rg = dg * (1 - sa);
                rb = db * (1 - sa);
                break;
            default: {
                // W3C separable/non-separable compositing on unpremultiplied colours.
                const double cs[3] = {sa > 0 ? sr / sa : 0.0, sa > 0 ? sg / sa : 0.0, sa > 0 ? sb / sa : 0.0};
                const double cb[3] = {da > 0 ? dr / da : 0.0, da > 0 ? dg / da : 0.0, da > 0 ? db / da : 0.0};
                double bl[3];
                if (nonSeparable(mode)) {
                    blendNonSeparable(mode, cb, cs, bl);
                } else {
                    for (int i = 0; i < 3; ++i) bl[i] = blendChannel(mode, cb[i], cs[i]);
                }
                ra = sa + da * (1 - sa);
                rr = float(sr * (1 - da) + dr * (1 - sa) + sa * da * bl[0]);
                rg = float(sg * (1 - da) + dg * (1 - sa) + sa * da * bl[1]);
                rb = float(sb * (1 - da) + db * (1 - sa) + sa * da * bl[2]);
                break;
            }
            }
            auto q = [](float v) { return uint32_t(std::clamp(int(v * 255.0f + 0.5f), 0, 255)); };
            const uint32_t qa = q(ra);
            d[x] = (qa << 24) | (std::min(q(rr), qa) << 16) | (std::min(q(rg), qa) << 8) | std::min(q(rb), qa);
        }
    }
}

void applyMask(QImage& img, const QImage& mask)
{
    const int h = std::min(img.height(), mask.height()), w = std::min(img.width(), mask.width());
    for (int y = 0; y < img.height(); ++y) {
        auto* p = reinterpret_cast<uint32_t*>(img.scanLine(y));
        const uint32_t* m = y < h ? reinterpret_cast<const uint32_t*>(mask.constScanLine(y)) : nullptr;
        for (int x = 0; x < img.width(); ++x) {
            const uint32_t ma = (m && x < w) ? (m[x] >> 24) : 0;
            if (ma == 255) continue;
            const uint32_t v = p[x];
            auto mul = [ma](uint32_t c) { return (c * ma + 127) / 255; };
            p[x] = (mul(v >> 24) << 24) | (mul((v >> 16) & 0xff) << 16) | (mul((v >> 8) & 0xff) << 8) | mul(v & 0xff);
        }
    }
}

} // namespace vx
