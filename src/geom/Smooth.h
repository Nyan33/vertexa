// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — input smoothing for freehand tools. Two stages, like Krita's
// stabilizer and Animate's "Smoothing" slider:
//  1. a real-time "pulled string" + exponential filter while drawing,
//  2. a final Gaussian pass over the whole stroke when the pen is lifted.
#pragma once

#include "Vec2.h"

#include <vector>

namespace vx {

struct InputSample {
    Vec2 pos;                 ///< document coordinates
    double pressure = 1.0;    ///< 0..1 after the pressure curve
    double tiltX = 0.0;       ///< degrees, -60..60
    double tiltY = 0.0;
    double rotation = 0.0;    ///< barrel rotation in degrees
    double tangential = 0.0;  ///< airbrush wheel, -1..1
    double time = 0.0;        ///< seconds
};

class Stabilizer {
public:
    /// smoothing: 0 (raw input) .. 100 (very smooth). `scale` converts screen
    /// pixels into document units (1 / zoom) so the feel is zoom independent.
    void reset(double smoothing, double scale);
    /// Feed a raw sample, returns the stabilised samples produced by it.
    std::vector<InputSample> push(const InputSample& s);
    /// Pen lifted: returns catch-up samples that reach the last raw position.
    std::vector<InputSample> finish();

    /// Final zero-phase smoothing of a complete stroke (keeps end points).
    static std::vector<InputSample> smoothStroke(const std::vector<InputSample>& in, double smoothing, double scale);

private:
    double m_smoothing = 0.0;
    double m_scale = 1.0;
    bool m_started = false;
    InputSample m_last;
    InputSample m_raw;
};

} // namespace vx
