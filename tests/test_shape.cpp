// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestMain.h"

#include "core/ShapeOps.h"

using namespace vx;

namespace {

const FillStyle kRed = FillStyle::solid(Color(255, 0, 0));
const FillStyle kBlue = FillStyle::solid(Color(0, 0, 255));

StrokeStyle blackStroke(double w = 2.0)
{
    StrokeStyle s;
    s.width = w;
    return s;
}

double fillArea(const ShapeGraph& g, const FillStyle& f)
{
    for (size_t i = 0; i < g.fills.size(); ++i)
        if (g.fills[i] == f) return g.fillRegion(int(i) + 1).area();
    return 0.0;
}

int countStroked(const ShapeGraph& g)
{
    int n = 0;
    for (const GEdge& e : g.edges) n += e.stroke ? 1 : 0;
    return n;
}

} // namespace

VX_TEST(merge_same_color_fuses)
{
    const ShapeGraph a = graphFromRegion(Region::rect({0, 0, 10, 10}), kRed);
    const ShapeGraph b = graphFromRegion(Region::rect({5, 0, 15, 10}), kRed);
    const ShapeGraph m = overlay(a, b);
    CHECK(m.fills.size() == 1);
    CHECK_NEAR(fillArea(m, kRed), 150.0, 1e-9);
    // Fused into one rectangle: 4 edges, no interior seam.
    CHECK(m.edges.size() == 4);
}

VX_TEST(merge_other_color_cuts)
{
    const ShapeGraph a = graphFromRegion(Region::rect({0, 0, 10, 10}), kRed);
    const ShapeGraph b = graphFromRegion(Region::circle({10, 5}, 4), kBlue);
    const ShapeGraph m = overlay(a, b);
    CHECK(m.fills.size() == 2);
    const double circle = Region::circle({0, 0}, 4).area();
    CHECK_NEAR(fillArea(m, kBlue), circle, 1e-6);
    CHECK_NEAR(fillArea(m, kRed), 100.0 - circle / 2.0, 1e-6);
    // Behind mode keeps the red where they overlap.
    OverlayOptions behind;
    behind.mode = PaintMode::Behind;
    const ShapeGraph m2 = overlay(a, b, behind);
    CHECK_NEAR(fillArea(m2, kRed), 100.0, 1e-6);
    CHECK_NEAR(fillArea(m2, kBlue), circle / 2.0, 1e-6);
}

VX_TEST(line_splits_fill_and_paint_removes_lines)
{
    const ShapeGraph a = graphFromRegion(Region::rect({0, 0, 10, 10}), kRed);
    const ShapeGraph line = graphFromPaths({{Cubic::line({-2, 5}, {12, 5})}}, blackStroke());
    const ShapeGraph m = overlay(a, line);
    // The fill is split in two faces by the line.
    int redFaces = 0;
    const Arrangement& t = m.topology();
    for (int f = 1; f < t.faceCount(); ++f)
        if (t.value(f, 0)) ++redFaces;
    CHECK(redFaces == 2);
    CHECK(countStroked(m) >= 1);
    // Painting over the middle removes the covered part of the line (Normal).
    const ShapeGraph paint = graphFromRegion(Region::rect({3, 3, 7, 7}), kBlue);
    const ShapeGraph p = overlay(m, paint);
    for (const GEdge& e : p.edges)
        if (e.stroke) {
            const Vec2 mid = e.c.eval(0.5);
            CHECK(!(mid.x > 3.01 && mid.x < 6.99));
        }
    // Paint Fills mode keeps the line.
    OverlayOptions fills;
    fills.mode = PaintMode::Fills;
    const ShapeGraph q = overlay(m, paint, fills);
    bool lineInside = false;
    for (const GEdge& e : q.edges)
        if (e.stroke && e.c.eval(0.5).x > 3.01 && e.c.eval(0.5).x < 6.99) lineInside = true;
    CHECK(lineInside);
}

VX_TEST(erase_modes)
{
    ShapeGraph s = graphFromShape(Region::rect({0, 0, 10, 10}), &kRed, nullptr);
    s = overlay(s, graphFromPaths({{Cubic::line({-5, 5}, {15, 5})}}, blackStroke()));
    const Region er = Region::circle({5, 5}, 2);
    const ShapeGraph e1 = erase(s, er, EraseMode::Normal);
    CHECK_NEAR(fillArea(e1, kRed), 100.0 - er.area(), 1e-6);
    bool strokeInside = false;
    for (const GEdge& e : e1.edges)
        if (e.stroke && distance(e.c.eval(0.5), {5, 5}) < 1.9) strokeInside = true;
    CHECK(!strokeInside);
    const ShapeGraph e2 = erase(s, er, EraseMode::Lines);
    CHECK_NEAR(fillArea(e2, kRed), 100.0, 1e-6);
    const ShapeGraph e3 = erase(s, er, EraseMode::Fills);
    bool strokeKept = false;
    for (const GEdge& e : e3.edges)
        if (e.stroke && e.c.bounds().contains({5, 5}, 1e-9)) strokeKept = true;
    CHECK(strokeKept);
}

VX_TEST(paint_bucket_closed_and_gaps)
{
    // A square outline made of four strokes.
    const StrokeStyle st = blackStroke();
    std::vector<std::vector<Cubic>> sq{{Cubic::line({0, 0}, {10, 0}), Cubic::line({10, 0}, {10, 10}),
                                        Cubic::line({10, 10}, {0, 10}), Cubic::line({0, 10}, {0, 0})}};
    const ShapeGraph outline = graphFromPaths(sq, st);
    ShapeGraph filled;
    CHECK(paintBucket(outline, {5, 5}, kBlue, 0.0, filled));
    CHECK_NEAR(fillArea(filled, kBlue), 100.0, 1e-9);
    CHECK(!paintBucket(outline, {20, 20}, kBlue, 0.0, filled));

    // Open square with a 1 unit gap: needs gap closing.
    std::vector<std::vector<Cubic>> open{{Cubic::line({1, 0}, {10, 0}), Cubic::line({10, 0}, {10, 10}),
                                          Cubic::line({10, 10}, {0, 10}), Cubic::line({0, 10}, {0, 1})}};
    const ShapeGraph og = graphFromPaths(open, st);
    ShapeGraph out;
    CHECK(!paintBucket(og, {5, 5}, kBlue, 0.0, out));
    CHECK(paintBucket(og, {5, 5}, kBlue, 2.0, out));
    CHECK(fillArea(out, kBlue) > 99.0);

    // Recolour an existing fill.
    ShapeGraph recol;
    CHECK(paintBucket(filled, {5, 5}, kRed, 0.0, recol));
    CHECK_NEAR(fillArea(recol, kRed), 100.0, 1e-9);
    CHECK(fillArea(recol, kBlue) == 0.0);
}

VX_TEST(selection_lift_and_drop)
{
    ShapeGraph s = graphFromShape(Region::rect({0, 0, 10, 10}), &kRed, nullptr);
    ShapeGraph rest, lifted;
    cutByRegion(s, Region::rect({5, -1, 20, 20}), rest, lifted);
    CHECK_NEAR(fillArea(rest, kRed), 50.0, 1e-9);
    CHECK_NEAR(fillArea(lifted, kRed), 50.0, 1e-9);
    // Dropping it back fuses into the original rectangle.
    const ShapeGraph back = overlay(rest, lifted);
    CHECK_NEAR(fillArea(back, kRed), 100.0, 1e-9);
    CHECK(back.edges.size() == 4);
    // Moving it away leaves a gap.
    const ShapeGraph moved = overlay(rest, lifted.transformed(Affine::translate(20, 0)));
    CHECK_NEAR(fillArea(moved, kRed), 100.0, 1e-9);
    CHECK(moved.fillAt({7, 5}) == 0);
}

VX_TEST(hit_test_and_runs)
{
    const StrokeStyle st = blackStroke(1.0);
    ShapeGraph s = graphFromShape(Region::rect({0, 0, 10, 10}), &kRed, &st);
    ShapeHit h = hitTest(s, {5, 0.2}, 0.5);
    CHECK(h.kind == ShapeHit::Kind::Stroke);
    // A single side of a rectangle (stops at the corners).
    CHECK(selectStrokeRun(s, h.arrEdge).edges.size() == 1);
    CHECK(selectConnected(s, h).edges.size() == 4);
    h = hitTest(s, {5, 5}, 0.5);
    CHECK(h.kind == ShapeHit::Kind::Fill);
    const ShapeSelection both = selectConnected(s, h);
    CHECK(both.faces.size() == 1);
    CHECK(both.edges.size() == 4);
    ShapeGraph rest, lifted;
    liftSelection(s, both, rest, lifted);
    CHECK(rest.isEmpty());
    CHECK_NEAR(fillArea(lifted, kRed), 100.0, 1e-9);
}

VX_TEST(bend_and_move_vertex)
{
    ShapeGraph s = graphFromShape(Region::rect({0, 0, 10, 10}), &kRed, nullptr);
    int top = -1;
    for (int i = 0; i < int(s.edges.size()); ++i)
        if (std::abs(s.edges[i].c.eval(0.5).y) < 1e-9) top = i;
    CHECK(top >= 0);
    const ShapeGraph bent = bendEdge(s, top, 0.5, {5, -4});
    CHECK(fillArea(bent, kRed) > 100.0);
    Vec2 v;
    CHECK(nearestVertex(s, {10.2, 10.1}, 0.5, v));
    const ShapeGraph moved = moveVertex(s, v, {14, 10});
    CHECK_NEAR(fillArea(moved, kRed), 120.0, 1e-9);
}

VX_TEST(mirror_keeps_fill_sides)
{
    const ShapeGraph s = graphFromRegion(Region::rect({0, 0, 10, 10}), kRed);
    const ShapeGraph m = s.transformed(Affine::scale(-1, 1));
    CHECK(m.fillAt({-5, 5}) != 0);
    CHECK(m.fillAt({5, 5}) == 0);
}

VX_TEST_MAIN()
