// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — 8-bit RGBA colour (like Flash, colours are stored exactly so that
// "same colour" fills merge deterministically) and Flash colour transforms.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace vx {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;

    constexpr Color() = default;
    constexpr Color(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255) : r(r_), g(g_), b(b_), a(a_) {}
    static constexpr Color fromRgb(uint32_t rgb, uint8_t alpha = 255)
    {
        return {uint8_t((rgb >> 16) & 0xff), uint8_t((rgb >> 8) & 0xff), uint8_t(rgb & 0xff), alpha};
    }
    static Color fromFloat(double r, double g, double b, double a = 1.0)
    {
        auto q = [](double v) { return uint8_t(std::clamp(std::lround(v * 255.0), 0L, 255L)); };
        return {q(r), q(g), q(b), q(a)};
    }
    constexpr uint32_t rgb() const { return (uint32_t(r) << 16) | (uint32_t(g) << 8) | b; }
    constexpr uint32_t argb() const { return (uint32_t(a) << 24) | rgb(); }
    constexpr bool operator==(const Color&) const = default;
    constexpr bool isOpaque() const { return a == 255; }

    std::string hex() const
    {
        static const char* d = "0123456789ABCDEF";
        std::string s = "#";
        for (uint8_t v : {r, g, b}) {
            s += d[v >> 4];
            s += d[v & 15];
        }
        return s;
    }
    static bool parseHex(const std::string& s, Color& out)
    {
        std::string h = s;
        if (!h.empty() && h[0] == '#') h.erase(0, 1);
        if (h.size() == 3) h = {h[0], h[0], h[1], h[1], h[2], h[2]};
        if (h.size() != 6 && h.size() != 8) return false;
        auto hexv = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        uint8_t v[4] = {0, 0, 0, 255};
        for (size_t i = 0; i < h.size() / 2; ++i) {
            const int hi = hexv(h[2 * i]), lo = hexv(h[2 * i + 1]);
            if (hi < 0 || lo < 0) return false;
            v[i] = uint8_t(hi * 16 + lo);
        }
        out = {v[0], v[1], v[2], v[3]};
        return true;
    }
};

inline Color lerpColor(const Color& a, const Color& b, double t)
{
    auto l = [t](uint8_t x, uint8_t y) { return uint8_t(std::clamp(std::lround(x + (y - x) * t), 0L, 255L)); };
    return {l(a.r, b.r), l(a.g, b.g), l(a.b, b.b), l(a.a, b.a)};
}

/// Flash colour transform: c' = c * mult + offset (offset in 0..255 units).
struct ColorTransform {
    double rm = 1, gm = 1, bm = 1, am = 1;
    double ro = 0, go = 0, bo = 0, ao = 0;

    bool isIdentity() const { return rm == 1 && gm == 1 && bm == 1 && am == 1 && ro == 0 && go == 0 && bo == 0 && ao == 0; }
    bool operator==(const ColorTransform&) const = default;

    Color apply(const Color& c) const
    {
        auto ch = [](uint8_t v, double m, double o) { return uint8_t(std::clamp(std::lround(v * m + o), 0L, 255L)); };
        return {ch(c.r, rm, ro), ch(c.g, gm, go), ch(c.b, bm, bo), ch(c.a, am, ao)};
    }
    /// (this ∘ inner): apply inner first, then this.
    ColorTransform operator*(const ColorTransform& inner) const
    {
        ColorTransform o;
        o.rm = rm * inner.rm; o.ro = inner.ro * rm + ro;
        o.gm = gm * inner.gm; o.go = inner.go * gm + go;
        o.bm = bm * inner.bm; o.bo = inner.bo * bm + bo;
        o.am = am * inner.am; o.ao = inner.ao * am + ao;
        return o;
    }
    static ColorTransform lerp(const ColorTransform& a, const ColorTransform& b, double t)
    {
        auto l = [t](double x, double y) { return x + (y - x) * t; };
        return {l(a.rm, b.rm), l(a.gm, b.gm), l(a.bm, b.bm), l(a.am, b.am),
                l(a.ro, b.ro), l(a.go, b.go), l(a.bo, b.bo), l(a.ao, b.ao)};
    }
};

/// Instance "Color Effect" as presented in the Properties panel of Animate.
struct ColorEffect {
    enum class Kind { None, Brightness, Tint, Alpha, Advanced };
    Kind kind = Kind::None;
    double brightness = 0.0;  ///< -1..1
    Color tint{255, 255, 255};
    double tintAmount = 0.0;  ///< 0..1
    double alpha = 1.0;       ///< 0..1
    ColorTransform advanced;

    bool operator==(const ColorEffect&) const = default;

    ColorTransform toTransform() const
    {
        ColorTransform t;
        switch (kind) {
        case Kind::None: break;
        case Kind::Brightness:
            if (brightness >= 0) {
                t.rm = t.gm = t.bm = 1.0 - brightness;
                t.ro = t.go = t.bo = 255.0 * brightness;
            } else {
                t.rm = t.gm = t.bm = 1.0 + brightness;
            }
            break;
        case Kind::Tint:
            t.rm = t.gm = t.bm = 1.0 - tintAmount;
            t.ro = tint.r * tintAmount;
            t.go = tint.g * tintAmount;
            t.bo = tint.b * tintAmount;
            break;
        case Kind::Alpha: t.am = alpha; break;
        case Kind::Advanced: t = advanced; break;
        }
        return t;
    }
    static ColorEffect lerp(const ColorEffect& a, const ColorEffect& b, double t)
    {
        if (a.kind == b.kind) {
            ColorEffect o = a;
            o.brightness = a.brightness + (b.brightness - a.brightness) * t;
            o.tint = lerpColor(a.tint, b.tint, t);
            o.tintAmount = a.tintAmount + (b.tintAmount - a.tintAmount) * t;
            o.alpha = a.alpha + (b.alpha - a.alpha) * t;
            o.advanced = ColorTransform::lerp(a.advanced, b.advanced, t);
            return o;
        }
        ColorEffect o;
        o.kind = Kind::Advanced;
        o.advanced = ColorTransform::lerp(a.toTransform(), b.toTransform(), t);
        return o;
    }
};

} // namespace vx
