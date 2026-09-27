// SPDX-License-Identifier: GPL-3.0-or-later
#include "Tween.h"

#include <algorithm>
#include <cmath>

namespace vx {

namespace {

double wrapPi(double a)
{
    a = std::fmod(a + kPi, 2.0 * kPi);
    if (a < 0) a += 2.0 * kPi;
    return a - kPi;
}

double wrapPositive(double a)
{
    a = std::fmod(a, 2.0 * kPi);
    if (a < 0) a += 2.0 * kPi;
    return a;
}

} // namespace

Affine tweenMatrix(const Affine& a, const Affine& b, Vec2 pivotA, Vec2 pivotB, double t, RotateMode mode,
                   int rotations, bool scale, const Vec2* position, double extraRotation)
{
    const AffineParts pa = AffineParts::decompose(a), pb = AffineParts::decompose(b);
    double dy = pb.skewY - pa.skewY;
    double dx = pb.skewX - pa.skewX;
    const double skewDiff = wrapPi(dx - dy);
    switch (mode) {
    case RotateMode::None:
    case RotateMode::Auto: dy = wrapPi(dy); break;
    case RotateMode::Clockwise:
        // y points down on screen: positive angles turn clockwise.
        dy = wrapPositive(dy);
        if (dy == 0.0 && rotations == 0) dy = 0.0;
        dy += 2.0 * kPi * std::max(0, rotations);
        break;
    case RotateMode::CounterClockwise:
        dy = wrapPositive(dy);
        if (dy > 0.0) dy -= 2.0 * kPi;
        dy -= 2.0 * kPi * std::max(0, rotations);
        break;
    }
    dx = dy + skewDiff;

    AffineParts p;
    p.skewY = pa.skewY + dy * t + extraRotation;
    p.skewX = pa.skewX + dx * t + extraRotation;
    p.scaleX = scale ? pa.scaleX + (pb.scaleX - pa.scaleX) * t : pa.scaleX;
    p.scaleY = scale ? pa.scaleY + (pb.scaleY - pa.scaleY) * t : pa.scaleY;
    p.tx = p.ty = 0.0;
    Affine m = p.compose();

    const Vec2 pivot = lerp(pivotA, pivotB, t);
    const Vec2 target = position ? *position : lerp(a.map(pivotA), b.map(pivotB), t);
    const Vec2 off = target - m.mapVector(pivot);
    m.tx = off.x;
    m.ty = off.y;
    return m;
}

GuidePath::GuidePath(std::vector<Cubic> chain) : m_curves(std::move(chain))
{
    m_cum.reserve(m_curves.size());
    for (const Cubic& c : m_curves) {
        m_cum.push_back(m_total);
        m_total += c.length();
    }
}

int GuidePath::locate(double s, double& local) const
{
    s = std::clamp(s, 0.0, m_total);
    auto it = std::upper_bound(m_cum.begin(), m_cum.end(), s);
    int i = std::max(0, int(std::distance(m_cum.begin(), it)) - 1);
    local = s - m_cum[i];
    return i;
}

Vec2 GuidePath::pointAt(double s) const
{
    if (m_curves.empty()) return {};
    double local;
    const int i = locate(s, local);
    const Cubic& c = m_curves[i];
    return c.eval(c.paramAtLength(local));
}

Vec2 GuidePath::tangentAt(double s) const
{
    if (m_curves.empty()) return {1, 0};
    double local;
    const int i = locate(s, local);
    const Cubic& c = m_curves[i];
    return c.tangent(c.paramAtLength(local));
}

double GuidePath::project(Vec2 p, double* dist) const
{
    double best = 1e300, bestS = 0.0;
    for (size_t i = 0; i < m_curves.size(); ++i) {
        double d;
        const double t = m_curves[i].nearest(p, &d);
        if (d < best) {
            best = d;
            bestS = m_cum[i] + m_curves[i].length(0.0, t);
        }
    }
    if (dist) *dist = best;
    return bestS;
}

GuidePath chooseGuide(const std::vector<std::vector<Cubic>>& chains, Vec2 from, Vec2 to)
{
    GuidePath best;
    double bestCost = 1e300;
    for (const auto& ch : chains) {
        GuidePath g(ch);
        if (g.isEmpty()) continue;
        double d0, d1;
        g.project(from, &d0);
        g.project(to, &d1);
        if (d0 + d1 < bestCost) {
            bestCost = d0 + d1;
            best = g;
        }
    }
    return best;
}

} // namespace vx
