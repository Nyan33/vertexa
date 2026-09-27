// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — classic (motion) tween math.
#pragma once

#include "Timeline.h"

#include <vector>

namespace vx {

/// Interpolates two element matrices the way Animate's classic tween does:
/// the transformation point moves linearly (or along a motion guide) while
/// scale and skew/rotation are interpolated as decomposed components.
/// `position` (optional) overrides the transformation point position and
/// `extraRotation` is added (orient to path).
Affine tweenMatrix(const Affine& a, const Affine& b, Vec2 pivotA, Vec2 pivotB, double t, RotateMode mode,
                   int rotations, bool scale, const Vec2* position = nullptr, double extraRotation = 0.0);

/// Arc-length parametrised path used as a motion guide.
class GuidePath {
public:
    GuidePath() = default;
    explicit GuidePath(std::vector<Cubic> chain);
    bool isEmpty() const { return m_curves.empty(); }
    double length() const { return m_total; }
    Vec2 pointAt(double s) const;
    Vec2 tangentAt(double s) const;
    /// Arc-length position of the point of the path closest to p.
    double project(Vec2 p, double* dist = nullptr) const;

private:
    std::vector<Cubic> m_curves;
    std::vector<double> m_cum; ///< cumulative length at the start of each curve
    double m_total = 0.0;
    int locate(double s, double& local) const;
};

/// Picks the guide chain that best fits a motion from `from` to `to`.
GuidePath chooseGuide(const std::vector<std::vector<Cubic>>& chains, Vec2 from, Vec2 to);

} // namespace vx
