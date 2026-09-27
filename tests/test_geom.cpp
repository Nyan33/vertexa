// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestMain.h"

#include "geom/Arrangement.h"
#include "geom/Fit.h"
#include "geom/Intersect.h"
#include "geom/Outline.h"
#include "geom/Polynomial.h"
#include "geom/Region.h"

using namespace vx;

namespace {

double circleArea4(double r)
{
    // Area of the standard 4-segment cubic circle approximation.
    Region c = Region::circle({0, 0}, r);
    return c.area();
}

} // namespace

VX_TEST(polynomial_roots)
{
    double r[3];
    // (t - 0.2)(t - 0.5)(t - 0.9)
    const double a = 1, b = -(0.2 + 0.5 + 0.9), c = 0.2 * 0.5 + 0.2 * 0.9 + 0.5 * 0.9, d = -0.2 * 0.5 * 0.9;
    const int n = solveCubicInRange(a, b, c, d, 0, 1, r);
    CHECK(n == 3);
    CHECK_NEAR(r[0], 0.2, 1e-14);
    CHECK_NEAR(r[1], 0.5, 1e-14);
    CHECK_NEAR(r[2], 0.9, 1e-14);
    // Double root (tangential touch) at 0.5: (t - 0.5)^2
    const int m = solveCubicInRange(0, 1, -1, 0.25, 0, 1, r);
    CHECK(m == 1);
    CHECK_NEAR(r[0], 0.5, 1e-7);
}

VX_TEST(bezier_split_and_sub)
{
    const Cubic c{{0, 0}, {10, 30}, {40, -20}, {50, 10}};
    auto [l, r] = c.split(0.3);
    CHECK(l.p3 == r.p0);
    for (double t : {0.0, 0.25, 0.5, 1.0}) {
        CHECK_NEAR(distance(l.eval(t), c.eval(0.3 * t)), 0.0, 1e-12);
        CHECK_NEAR(distance(r.eval(t), c.eval(0.3 + 0.7 * t)), 0.0, 1e-12);
    }
    const Cubic s = c.sub(0.2, 0.7);
    CHECK_NEAR(distance(s.eval(0.5), c.eval(0.45)), 0.0, 1e-12);
    const Cubic rs = c.sub(0.7, 0.2);
    CHECK_NEAR(distance(rs.eval(0.0), c.eval(0.7)), 0.0, 1e-12);
    Cubic joined;
    CHECK(tryJoinCubics(l, r, joined));
    CHECK_NEAR(distance(joined.p1, c.p1), 0.0, 1e-9);
    CHECK_NEAR(distance(joined.p2, c.p2), 0.0, 1e-9);
}

VX_TEST(bezier_bounds_length_area)
{
    const Cubic line = Cubic::line({0, 0}, {3, 4});
    CHECK_NEAR(line.length(), 5.0, 1e-12);
    const Cubic c{{0, 0}, {0, 100}, {100, 100}, {100, 0}};
    const Rect b = c.bounds();
    CHECK_NEAR(b.y1, 75.0, 1e-9);
    CHECK_NEAR(b.y0, 0.0, 1e-12);
    Region sq = Region::rect({0, 0, 10, 20});
    CHECK_NEAR(sq.area(), 200.0, 1e-9);
    // Cubic circle approximation: area within 0.03% of pi r^2.
    CHECK_NEAR(circleArea4(10.0) / (kPi * 100.0), 1.0, 3e-4);
}

VX_TEST(intersect_lines_and_curves)
{
    std::vector<CurveHit> hits;
    intersectCurves(Cubic::line({0, 0}, {10, 10}), Cubic::line({0, 10}, {10, 0}), hits);
    CHECK(hits.size() == 1);
    CHECK_NEAR(hits[0].p.x, 5.0, 1e-12);
    CHECK_NEAR(hits[0].p.y, 5.0, 1e-12);

    hits.clear();
    const Cubic arch{{0, 0}, {0, 20}, {20, 20}, {20, 0}}; // peak y = 15
    intersectCurves(arch, Cubic::line({-5, 10}, {25, 10}), hits);
    CHECK(hits.size() == 2);
    for (const CurveHit& h : hits) CHECK_NEAR(h.p.y, 10.0, 1e-9);

    hits.clear();
    const Cubic dip{{0, 20}, {0, 0}, {20, 0}, {20, 20}}; // bottom y = 5
    intersectCurves(arch, dip, hits);
    CHECK(hits.size() == 2);
    for (const CurveHit& h : hits) CHECK_NEAR(distance(arch.eval(h.t1), dip.eval(h.t2)), 0.0, 1e-9);
}

VX_TEST(intersect_overlaps)
{
    std::vector<CurveHit> hits;
    intersectCurves(Cubic::line({0, 0}, {10, 0}), Cubic::line({5, 0}, {15, 0}), hits);
    CHECK(hits.size() == 2);
    const Cubic c{{0, 0}, {10, 30}, {40, -20}, {50, 10}};
    hits.clear();
    intersectCurves(c.sub(0.0, 0.6), c.sub(0.3, 1.0), hits);
    CHECK(hits.size() == 2);
    for (const CurveHit& h : hits) CHECK(h.overlap);
}

VX_TEST(boolean_squares)
{
    const Region a = Region::rect({0, 0, 2, 2}), b = Region::rect({1, 1, 3, 3});
    CHECK_NEAR(booleanOp(a, b, BoolOp::Union).area(), 7.0, 1e-9);
    CHECK_NEAR(booleanOp(a, b, BoolOp::Intersect).area(), 1.0, 1e-9);
    CHECK_NEAR(booleanOp(a, b, BoolOp::Subtract).area(), 3.0, 1e-9);
    CHECK_NEAR(booleanOp(a, b, BoolOp::Xor).area(), 6.0, 1e-9);
    // Shared edge: the union is a single rectangle made of 4 straight segments.
    const Region u = booleanOp(Region::rect({0, 0, 1, 1}), Region::rect({1, 0, 2, 1}), BoolOp::Union);
    CHECK(u.contours.size() == 1);
    CHECK(u.curveCount() == 4);
    CHECK_NEAR(u.area(), 2.0, 1e-12);
}

VX_TEST(boolean_holes_and_circles)
{
    const Region outer = Region::rect({0, 0, 4, 4}), inner = Region::rect({1, 1, 3, 3});
    const Region ring = booleanOp(outer, inner, BoolOp::Subtract);
    CHECK(ring.contours.size() == 2);
    CHECK_NEAR(ring.area(), 12.0, 1e-9);
    CHECK(!ring.contains({2, 2}));
    CHECK(ring.contains({0.5, 0.5}));

    // Two circles: curves stay exact cubics (no flattening).
    const Region c1 = Region::circle({0, 0}, 10), c2 = Region::circle({10, 0}, 10);
    const Region lens = booleanOp(c1, c2, BoolOp::Intersect);
    CHECK(lens.contours.size() == 1);
    CHECK(lens.curveCount() <= 6);
    const double exact = 2 * 100 * std::acos(0.5) - 5 * std::sqrt(400 - 100); // analytic lens area
    CHECK_NEAR(lens.area() / exact, 1.0, 2e-3);
    const Region uni = booleanOp(c1, c2, BoolOp::Union);
    CHECK_NEAR(uni.area(), c1.area() + c2.area() - lens.area(), 1e-6);
}

VX_TEST(boolean_identical_and_touching)
{
    const Region a = Region::circle({0, 0}, 5);
    CHECK_NEAR(booleanOp(a, a, BoolOp::Union).area(), a.area(), 1e-9);
    CHECK(booleanOp(a, a, BoolOp::Subtract).isEmpty());
    // Touching at a single point.
    const Region s1 = Region::rect({0, 0, 1, 1}), s2 = Region::rect({1, 1, 2, 2});
    CHECK_NEAR(booleanOp(s1, s2, BoolOp::Union).area(), 2.0, 1e-12);
}

VX_TEST(arrangement_faces_from_strokes)
{
    // A "#" made of four open strokes encloses exactly one bounded face.
    Arrangement arr;
    arr.addCurve(Cubic::line({0, 1}, {3, 1}), 0);
    arr.addCurve(Cubic::line({0, 2}, {3, 2}), 0);
    arr.addCurve(Cubic::line({1, 0}, {1, 3}), 0);
    arr.addCurve(Cubic::line({2, 0}, {2, 3}), 0);
    arr.build();
    const int f = arr.locate({1.5, 1.5});
    CHECK(f != 0);
    CHECK_NEAR(arr.faceArea(f), 1.0, 1e-12);
    CHECK(arr.locate({0.5, 0.5}) == 0);
    // Nested component: a small square inside the face is a hole.
    Arrangement arr2;
    arr2.addContour(Region::rect({0, 0, 10, 10}).contours[0], 0);
    arr2.addContour(Region::rect({4, 4, 6, 6}).contours[0], 0);
    arr2.build();
    const int outer = arr2.locate({1, 1});
    CHECK(outer != 0);
    CHECK_NEAR(arr2.faceArea(outer), 96.0, 1e-9);
    CHECK(arr2.locate({5, 5}) != outer);
}

VX_TEST(round_brush_outline)
{
    std::vector<BrushPoint> pts;
    for (int i = 0; i <= 100; ++i) pts.push_back({{double(i), 0.0}, 5.0, 0.0});
    const Region r = sweptRegion(pts, BrushTip{});
    CHECK_NEAR(r.area(), 100.0 * 10.0 + circleArea4(5.0), 1e-6);
    CHECK(r.contours.size() == 1);
    // A tight zig-zag must stay free of holes.
    std::vector<BrushPoint> zz;
    for (int i = 0; i < 40; ++i) zz.push_back({{double(i % 2 ? 3 : 0), double(i) * 0.5}, 4.0, 0.0});
    const Region z = sweptRegion(zz, BrushTip{});
    CHECK(z.contours.size() == 1);
    CHECK(z.contains({1.5, 10.0}));
    // Variable radius: taper.
    std::vector<BrushPoint> taper;
    for (int i = 0; i <= 50; ++i) taper.push_back({{double(i) * 2.0, std::sin(i * 0.2) * 10}, 1.0 + i * 0.1, 0.0});
    const Region t = sweptRegion(taper, BrushTip{});
    CHECK(t.contours.size() == 1);
    CHECK(t.area() > 0);
}

VX_TEST(square_brush_outline)
{
    std::vector<BrushPoint> pts{{{0, 0}, 2.0, 0}, {{10, 0}, 2.0, 0}};
    BrushTip tip;
    tip.shape = TipShape::Square;
    const Region r = sweptRegion(pts, tip);
    CHECK_NEAR(r.area(), 14.0 * 4.0, 1e-9);
}

VX_TEST(fit_recovers_curve)
{
    const Cubic c{{0, 0}, {30, 60}, {90, -40}, {120, 20}};
    std::vector<Vec2> pts;
    for (int i = 0; i <= 200; ++i) pts.push_back(c.eval(i / 200.0));
    FitOptions opt;
    opt.tolerance = 0.05;
    const auto fit = fitCurves(pts, opt);
    CHECK(!fit.empty());
    CHECK(fit.size() <= 3);
    double maxErr = 0;
    for (const Vec2& p : pts) {
        double best = 1e9;
        for (const Cubic& f : fit) best = std::min(best, f.distanceTo(p));
        maxErr = std::max(maxErr, best);
    }
    CHECK(maxErr <= 0.06);
}

VX_TEST(refit_region_is_smooth)
{
    std::vector<BrushPoint> pts;
    for (int i = 0; i <= 60; ++i) pts.push_back({{double(i) * 3, std::sin(i * 0.1) * 30}, 6.0, 0.0});
    const Region raw = sweptRegion(pts, BrushTip{});
    const Region fit = refitRegion(raw, 0.05);
    CHECK(fit.contours.size() == raw.contours.size());
    CHECK(fit.curveCount() < raw.curveCount());
    CHECK_NEAR(fit.area() / raw.area(), 1.0, 2e-3);
}

VX_TEST_MAIN()
