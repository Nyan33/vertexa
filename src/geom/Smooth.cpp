// SPDX-License-Identifier: GPL-3.0-or-later
#include "Smooth.h"

#include <algorithm>
#include <cmath>

namespace vx {

namespace {

InputSample mix(const InputSample& a, const InputSample& b, double t)
{
    InputSample r;
    r.pos = lerp(a.pos, b.pos, t);
    r.pressure = a.pressure + (b.pressure - a.pressure) * t;
    r.tiltX = a.tiltX + (b.tiltX - a.tiltX) * t;
    r.tiltY = a.tiltY + (b.tiltY - a.tiltY) * t;
    r.rotation = a.rotation + (b.rotation - a.rotation) * t;
    r.tangential = a.tangential + (b.tangential - a.tangential) * t;
    r.time = a.time + (b.time - a.time) * t;
    return r;
}

} // namespace

void Stabilizer::reset(double smoothing, double scale)
{
    m_smoothing = std::clamp(smoothing, 0.0, 100.0);
    m_scale = scale > 0 ? scale : 1.0;
    m_started = false;
}

std::vector<InputSample> Stabilizer::push(const InputSample& s)
{
    if (!m_started) {
        m_started = true;
        m_last = s;
        m_raw = s;
        return {s};
    }
    m_raw = s;
    if (m_smoothing <= 0.0) {
        m_last = s;
        return {s};
    }
    const double k = m_smoothing / 100.0;
    const double stringLen = k * 12.0 * m_scale;
    InputSample target = s;
    const double d = distance(m_last.pos, s.pos);
    if (d <= stringLen) {
        // Inside the dead zone of the string: only pressure follows.
        m_last.pressure += (s.pressure - m_last.pressure) * 0.35;
        return {};
    }
    target.pos = m_last.pos + (s.pos - m_last.pos) * ((d - stringLen) / d);
    const double alpha = 1.0 - 0.8 * k;
    InputSample out = mix(m_last, target, alpha);
    out.time = s.time;
    m_last = out;
    return {out};
}

std::vector<InputSample> Stabilizer::finish()
{
    std::vector<InputSample> out;
    if (!m_started) return out;
    const double d = distance(m_last.pos, m_raw.pos);
    const double step = std::max(1e-6, 1.0 * m_scale);
    const int n = int(std::ceil(d / step));
    for (int i = 1; i <= n; ++i) out.push_back(mix(m_last, m_raw, double(i) / n));
    if (n > 0) m_last = m_raw;
    return out;
}

std::vector<InputSample> Stabilizer::smoothStroke(const std::vector<InputSample>& in, double smoothing, double scale)
{
    if (in.size() < 3 || smoothing <= 0.0) return in;
    // Resample by arc length first so the kernel is speed independent.
    std::vector<InputSample> pts;
    const double spacing = std::max(1e-6, 0.75 * scale);
    pts.push_back(in.front());
    double carry = 0.0;
    for (size_t i = 0; i + 1 < in.size(); ++i) {
        const double L = distance(in[i].pos, in[i + 1].pos);
        if (L <= 0) continue;
        double s = spacing - carry;
        while (s <= L) {
            pts.push_back(mix(in[i], in[i + 1], s / L));
            s += spacing;
        }
        carry = L - (s - spacing);
    }
    pts.push_back(in.back());
    const double sigma = (smoothing / 100.0) * 10.0; // in samples
    if (sigma < 0.3 || pts.size() < 3) return pts;
    const int radius = int(std::ceil(sigma * 2.5));
    std::vector<double> w(radius + 1);
    for (int i = 0; i <= radius; ++i) w[i] = std::exp(-0.5 * (i * i) / (sigma * sigma));
    std::vector<InputSample> out(pts.size());
    const int n = int(pts.size());
    for (int i = 0; i < n; ++i) {
        // Shrink the window symmetrically near the ends so the end points stay put.
        const int r = std::min({radius, i, n - 1 - i});
        Vec2 acc;
        double pacc = 0.0, wsum = 0.0;
        for (int k = -r; k <= r; ++k) {
            const double wk = w[std::abs(k)];
            acc += pts[i + k].pos * wk;
            pacc += pts[i + k].pressure * wk;
            wsum += wk;
        }
        out[i] = pts[i];
        out[i].pos = acc / wsum;
        out[i].pressure = pacc / wsum;
    }
    return out;
}

} // namespace vx
