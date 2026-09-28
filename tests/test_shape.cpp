// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestMain.h"

#include "core/ShapeOps.h"

#include <map>

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

VX_TEST(local_merge_matches_full_merge)
{
    // A layer with a background, many separate islands (some with holes,
    // some touching each other) and strokes; painting and erasing locally
    // must give exactly what the full arrangement gives.
    const FillStyle bg = FillStyle::solid(Color(20, 120, 40)), red = FillStyle::solid(Color(220, 30, 30)),
                    blue = FillStyle::solid(Color(30, 60, 220));
    StrokeStyle line;
    line.width = 2;
    ShapeGraph layer = graphFromRegion(Region::rect({-50, -50, 650, 650}), bg);
    for (int i = 0; i < 12; ++i)
        for (int j = 0; j < 12; ++j) {
            const Vec2 c(i * 50.0 + 20, j * 50.0 + 20);
            const bool round = (i + j) % 2;
            Region r = round ? Region::circle(c, 14) : Region::rect({c.x - 12, c.y - 12, c.x + 12, c.y + 12});
            if ((i * 7 + j) % 5 == 0) r = booleanOp(r, Region::circle(c, 5), BoolOp::Subtract);
            layer = overlay(layer, graphFromRegion(r, (i + j) % 3 ? red : blue), {.localized = false});
        }
    layer = overlay(layer, graphFromPaths({{Cubic::line({0, 300}, {600, 310})}}, line), {.localized = false});
    CHECK(layer.edges.size() > 128);

    auto sameFills = [](const ShapeGraph& a, const ShapeGraph& b) {
        if (a.fills.size() != b.fills.size()) return false;
        for (const FillStyle& f : a.fills) {
            const int ia = [&] { for (size_t k = 0; k < a.fills.size(); ++k) if (a.fills[k] == f) return int(k) + 1; return 0; }();
            const int ib = [&] { for (size_t k = 0; k < b.fills.size(); ++k) if (b.fills[k] == f) return int(k) + 1; return 0; }();
            if (!ib) return false;
            const double aa = a.fillRegion(ia).area(), ab = b.fillRegion(ib).area();
            if (std::abs(aa - ab) > 1e-6 * std::max(1.0, aa)) return false;
        }
        const Region ua = a.fillRegion(0), ub = b.fillRegion(0);
        return std::abs(ua.area() - ub.area()) < 1e-6 * std::max(1.0, ua.area()) &&
               std::abs(booleanOp(ua, ub, BoolOp::Xor).area()) < 1e-3;
    };
    const ShapeGraph stroke = graphFromRegion(Region::circle({137, 262}, 30), blue);
    for (PaintMode mode : {PaintMode::Normal, PaintMode::Fills, PaintMode::Behind}) {
        OverlayOptions local, full;
        local.mode = full.mode = mode;
        full.localized = false;
        const ShapeGraph a = overlay(layer, stroke, local), b = overlay(layer, stroke, full);
        CHECK(sameFills(a, b));
        CHECK(a.edges.size() + 4 >= b.edges.size() && a.edges.size() <= b.edges.size() + 4);
        size_t strokesA = 0, strokesB = 0;
        for (const GEdge& e : a.edges) strokesA += e.stroke != 0;
        for (const GEdge& e : b.edges) strokesB += e.stroke != 0;
        CHECK(strokesA == strokesB);
    }
    // Painting in the middle of the background (no island reached).
    const ShapeGraph dab = graphFromRegion(Region::circle({45, 45}, 3), red);
    CHECK(sameFills(overlay(layer, dab), overlay(layer, dab, {.localized = false})));
    // Erasing.
    const Region eraser = Region::circle({320, 330}, 40);
    CHECK(sameFills(erase(layer, eraser, EraseMode::Normal), erase(layer, eraser, EraseMode::Normal, nullptr, false)));
    CHECK(sameFills(erase(layer, eraser, EraseMode::Fills), erase(layer, eraser, EraseMode::Fills, nullptr, false)));
}

VX_TEST(clean_region_graph_matches_full_build)
{
    const FillStyle f = FillStyle::solid(Color(10, 20, 30));
    Region ring = booleanOp(Region::circle({0, 0}, 40), Region::circle({0, 0}, 20), BoolOp::Subtract);
    Region islands = ring;
    for (const Contour& c : Region::circle({0, 0}, 8).contours) islands.contours.push_back(c);
    islands = normalizeRegion(islands);
    for (const Region& r : {normalizeRegion(Region::rect({0, 0, 10, 10})), ring, islands, ring.reversed()}) {
        const ShapeGraph a = graphFromCleanRegion(r, f), b = graphFromRegion(r, f);
        const Region ra = a.fillRegion(0), rb = b.fillRegion(0);
        CHECK(std::abs(ra.area() - rb.area()) < 1e-6 * std::max(1.0, rb.area()));
        CHECK(std::abs(booleanOp(ra, rb, BoolOp::Xor).area()) < 1e-6);
        // Every end point is shared: the graph is closed.
        std::map<std::pair<double, double>, int> degree;
        for (const GEdge& e : a.edges) {
            ++degree[{e.c.p0.x, e.c.p0.y}];
            ++degree[{e.c.p3.x, e.c.p3.y}];
        }
        bool even = true;
        for (const auto& [p, d] : degree) even &= d % 2 == 0;
        CHECK(even);
    }
}

VX_TEST(planar_drawings_merge_the_same)
{
    // Merged drawings are marked planar, so merging into them skips testing
    // their edges against each other: the result must not change.
    ShapeGraph g;
    for (int i = 0; i < 12; ++i) {
        const Vec2 c{40.0 + i * 23.0, 60.0 + (i % 4) * 17.0};
        std::vector<Cubic> wave;
        for (int k = 0; k < 6; ++k) wave.push_back(Cubic{c + Vec2{k * 20.0, 0}, c + Vec2{k * 20.0 + 7, -25}, c + Vec2{k * 20.0 + 13, 25}, c + Vec2{k * 20.0 + 20, 0}});
        g = overlay(g, graphFromRegion(Region::circle(c, 18 + i % 5), i % 2 ? kRed : kBlue));
        g = overlay(g, graphFromPaths({wave}, blackStroke(1.5)));
    }
    CHECK(g.isPlanar());
    ShapeGraph plain; // the same edges, not marked
    plain.fills = g.fills;
    plain.strokes = g.strokes;
    plain.edges = g.edges;
    CHECK(!plain.isPlanar());
    const ShapeGraph top = graphFromRegion(Region::rect({30, 40, 260, 90}), kRed);
    const ShapeGraph a = overlay(g, top), b = overlay(plain, top);
    CHECK(a.isPlanar());
    CHECK(a.edges.size() == b.edges.size());
    CHECK(std::abs(fillArea(a, kRed) - fillArea(b, kRed)) < 1e-6 * fillArea(b, kRed));
    CHECK(std::abs(fillArea(a, kBlue) - fillArea(b, kBlue)) < 1e-6 * std::max(1.0, fillArea(b, kBlue)));
    CHECK(countStroked(a) == countStroked(b));
    // Editing the edges drops the mark; moving the drawing keeps it.
    CHECK(g.transformed(Affine::rotate(0.3) * Affine::scale(2)).isPlanar());
    ShapeGraph edited = g;
    edited.edges.front().c.p1 += Vec2{1, 1};
    CHECK(!edited.isPlanar());
}

VX_TEST_MAIN()
