// SPDX-License-Identifier: GPL-3.0-or-later
// Vector brushes: every kind must produce plain vector fills that follow the
// path, textures must be anchored to the canvas and art brushes must map the
// artwork's width along the stroke.
#include "TestMain.h"

#include "core/Serialize.h"
#include "core/ShapeOps.h"
#include "core/VectorBrush.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

using namespace vx;

namespace {

std::vector<BrushPoint> straight(Vec2 a, Vec2 b, double r, int n = 60)
{
    std::vector<BrushPoint> out;
    for (int i = 0; i <= n; ++i) out.push_back({lerp(a, b, double(i) / n), r, 0.0});
    return out;
}

std::vector<BrushPoint> wave(double r)
{
    std::vector<BrushPoint> out;
    for (int i = 0; i <= 80; ++i) {
        const double t = i / 80.0;
        out.push_back({{t * 300.0, std::sin(t * 6.0) * 30.0}, r * (0.4 + 0.6 * std::sin(t * kPi)), 0.0});
    }
    return out;
}

double area(const std::vector<BrushPiece>& pieces)
{
    double a = 0;
    for (const BrushPiece& p : pieces) a += p.region.area();
    return a;
}

Rect bounds(const std::vector<BrushPiece>& pieces)
{
    Rect r;
    for (const BrushPiece& p : pieces) r.include(p.region.bounds());
    return r;
}

const VectorBrushPreset& builtin(const char* id)
{
    const VectorBrushPreset* p = builtinVectorBrush(id);
    if (!p) std::abort();
    return *p;
}

} // namespace

VX_TEST(every_builtin_paints_vector_fills)
{
    const FillStyle ink = FillStyle::solid(Color(20, 20, 30));
    CHECK(builtinVectorBrushes().size() >= 8);
    for (const VectorBrushPreset& p : builtinVectorBrushes()) {
        const std::vector<BrushPoint> path = wave(p.size * 0.5);
        const std::vector<BrushPiece> pieces = vectorBrushStroke(p, path, ink, 5, 0.05);
        CHECK(!pieces.empty());
        CHECK(area(pieces) > 100.0);
        // The result stays around the path.
        const Rect b = bounds(pieces);
        CHECK(b.x0 > -p.size * 1.5 && b.x1 < 300 + p.size * 1.5);
        CHECK(b.y0 > -30 - p.size * 1.5 && b.y1 < 30 + p.size * 1.5);
        // And is an ordinary shape graph with fills only.
        const ShapeGraph g = vectorBrushGraph(pieces);
        CHECK(!g.fills.empty() && g.strokes.empty());
        CHECK(std::abs(g.fillRegion(0).area() - normalizeRegion([&] {
                  Region all;
                  for (const BrushPiece& piece : pieces)
                      for (const Contour& c : piece.region.contours) all.contours.push_back(c);
                  return all;
              }()).area()) < 1.0 + 0.01 * area(pieces));
    }
}

VX_TEST(textured_brush_has_rough_edges_and_grain)
{
    const VectorBrushPreset& chalk = builtin("chalk");
    const auto path = straight({0, 0}, {400, 0}, 10);
    const std::vector<BrushPiece> pieces = vectorBrushStroke(chalk, path, FillStyle::solid(Color(0, 0, 0)), 1, 0.05);
    CHECK(pieces.size() == 1);
    const Region& r = pieces[0].region;
    // Grain holes are extra contours; the area is below the smooth sweep's.
    CHECK(r.contours.size() > 20);
    const double smooth = 400 * 20 + kPi * 100;
    CHECK(r.area() < smooth * 0.97 && r.area() > smooth * 0.5);
    // The texture is anchored to the canvas: the same place gives the same
    // edge whatever the stroke's seed, so overlapping strokes line up.
    const auto again = vectorBrushStroke(chalk, path, FillStyle::solid(Color(0, 0, 0)), 99, 0.05);
    CHECK(std::abs(area(again) - r.area()) < 1e-6);
    CHECK(brushNoise({12.5, 7.25}, 6) == brushNoise({12.5, 7.25}, 6));
    CHECK(std::abs(brushNoise({3, 4}, 6)) <= 1.0);
}

VX_TEST(scatter_brush_depends_on_seed)
{
    const VectorBrushPreset& stipple = builtin("stipple");
    const auto path = straight({0, 0}, {300, 0}, 12);
    const auto a = vectorBrushStroke(stipple, path, FillStyle::solid(Color(0, 0, 0)), 3, 0.05);
    const auto b = vectorBrushStroke(stipple, path, FillStyle::solid(Color(0, 0, 0)), 3, 0.05);
    const auto c = vectorBrushStroke(stipple, path, FillStyle::solid(Color(0, 0, 0)), 4, 0.05);
    CHECK(a.size() == 1 && a[0].region.contours.size() > 10);
    CHECK(area(a) == area(b));
    CHECK(area(a) != area(c));
}

VX_TEST(art_brush_follows_the_path)
{
    const VectorBrushPreset& ink = builtin("ink-taper");
    // Straight path: the lens stretches over the whole length and the width.
    const auto pieces = vectorBrushStroke(ink, straight({0, 0}, {200, 0}, 7), FillStyle::solid(Color(200, 0, 0)), 1, 0.02);
    CHECK(pieces.size() == 1);
    CHECK(pieces[0].fill.mainColor() == Color(200, 0, 0)); // colourised
    const Rect b = bounds(pieces);
    CHECK(std::abs(b.x0) < 0.5 && std::abs(b.x1 - 200) < 0.5);
    CHECK(b.height() > 7 && b.height() <= 14.5);
    // Pointed ends: the tips are much narrower than the middle.
    CHECK(pieces[0].region.winding({100, 5}) != 0);
    CHECK(pieces[0].region.winding({2, 5}) == 0);
    // A bent path bends the artwork: the middle follows the arc.
    std::vector<BrushPoint> arc;
    for (int i = 0; i <= 60; ++i) {
        const double a = kPi * i / 60.0;
        arc.push_back({{100 - 100 * std::cos(a), -100 * std::sin(a)}, 7, 0.0});
    }
    const auto bent = vectorBrushStroke(ink, arc, FillStyle::solid(Color(0, 0, 0)), 1, 0.02);
    CHECK(bent.size() == 1 && bent[0].region.winding({100, -100}) != 0);
    CHECK(bent[0].region.winding({100, -50}) == 0);
}

VX_TEST(pattern_brush_repeats_tiles)
{
    VectorBrushPreset dashes = builtin("dashes");
    dashes.pressureSize = false;
    // Tile = 60 x 20 artwork at width 8 -> 24 long, plus a 60% gap.
    const auto pieces = vectorBrushStroke(dashes, straight({0, 0}, {384, 0}, 4), FillStyle::solid(Color(0, 0, 0)), 1, 0.02);
    CHECK(pieces.size() == 1);
    const double tile = 24 * 1.6;
    CHECK(int(pieces[0].region.contours.size()) == int(std::lround(384 / tile)));
    // Colours of a non-colourised pattern are kept (vine: stem + leaves).
    const auto vine = vectorBrushStroke(builtin("vine"), straight({0, 0}, {300, 0}, 13), FillStyle::solid(Color(0, 0, 0)), 1, 0.05);
    CHECK(vine.size() == 2);
    CHECK(!(vine[0].fill == vine[1].fill));
}

VX_TEST(art_brush_from_selection)
{
    // A shape with a red fill and a thick blue line.
    ShapeGraph g = graphFromRegion(Region::rect({0, -10, 80, 10}), FillStyle::solid(Color(255, 0, 0)));
    StrokeStyle line;
    line.width = 4;
    line.paint = FillStyle::solid(Color(0, 0, 255));
    g = overlay(g, graphFromPaths({{Cubic::line({0, 14}, {80, 14})}}, line));
    const ShapeGraph filled = linesToFills(g);
    CHECK(filled.strokes.empty() && filled.fills.size() == 2);
    const VectorBrushPreset b = makeArtBrush(g, false, "Flag");
    CHECK(b.kind == VectorBrushKind::Art && b.art && !b.colorize);
    CHECK(std::abs(b.size - 26) < 0.5); // height of the artwork incl. the line
    const auto pieces = vectorBrushStroke(b, straight({0, 0}, {300, 0}, b.size * 0.5), FillStyle::solid(Color(0, 0, 0)), 1, 0.05);
    CHECK(pieces.size() == 2);
    // Plain black artwork makes a colourised brush.
    const VectorBrushPreset black = makeArtBrush(graphFromRegion(Region::circle({0, 0}, 5), FillStyle::solid(Color(0, 0, 0))), true, "Dots");
    CHECK(black.colorize && black.kind == VectorBrushKind::Pattern);
}

VX_TEST(brush_path_uses_pressure_curve)
{
    VectorBrushPreset p = builtin("chalk");
    p.size = 20;
    p.minSize = 0.25;
    InputSample light, full;
    light.pressure = 0.0;
    full.pressure = 1.0;
    const auto path = vectorBrushPath(p, {light, full});
    CHECK(std::abs(path[0].r - 2.5) < 1e-9 && std::abs(path[1].r - 10) < 1e-9);
    p.pressureSize = false;
    CHECK(std::abs(vectorBrushPath(p, {light})[0].r - 10) < 1e-9);
    p.pressureSize = true;
    p.sizeCurve.points = {{0, 0}, {0.5, 1}, {1, 1}};
    light.pressure = 0.5;
    CHECK(std::abs(vectorBrushPath(p, {light})[0].r - 10) < 1e-6);
}

VX_TEST(brushes_serialize)
{
    Document d = Document::createDefault();
    d.brushes.push_back(builtin("chalk"));
    d.brushes.back().id = "doc.1";
    d.brushes.push_back(makeArtBrush(graphFromRegion(Region::circle({0, 0}, 5), FillStyle::solid(Color(0, 128, 0))), true, "Dots"));
    d.brushes.back().id = "doc.2";
    d.brushes.back().sizeCurve.points = {{0, 0}, {0.3, 0.7}, {1, 1}};
    const QByteArray data = serializeDocument(d);
    Document back;
    CHECK(deserializeDocument(data, back));
    CHECK(back.brushes.size() == 2);
    CHECK(back.brushes[0].kind == VectorBrushKind::Textured && back.brushes[0].grain == d.brushes[0].grain);
    CHECK(back.brushes[1].kind == VectorBrushKind::Pattern && back.brushes[1].art && !back.brushes[1].art->isEmpty());
    CHECK(back.brushes[1].sizeCurve == d.brushes[1].sizeCurve);
    CHECK(serializeDocument(back) == data);
}

VX_TEST(old_raster_paint_is_migrated)
{
    // A document saved with the raster Paint Brush of Vertexa 0.1.
    Document d = Document::createDefault();
    QJsonObject root = QJsonDocument::fromJson(serializeDocument(d)).object();
    QJsonArray samples;
    for (int i = 0; i <= 20; ++i)
        for (double v : {10.0 + i * 10, 50.0, 0.8, 0.0, 0.0, 0.0}) samples.append(v);
    QJsonObject stroke{{"brush", 0}, {"color", "#ff0000"}, {"seed", 3}, {"samples", samples}};
    QJsonObject brush{{"id", "chalk"}, {"size", 20.0}, {"pressureSize", false}};
    QJsonObject paint{{"type", "paint"}, {"pivot", QJsonArray{0, 0}}, {"brushes", QJsonArray{brush}}, {"strokes", QJsonArray{stroke}}};
    QJsonArray scenes = root["scenes"].toArray();
    QJsonObject scene = scenes[0].toObject();
    QJsonArray layers = scene["layers"].toArray();
    QJsonObject layer = layers[0].toObject();
    QJsonArray keys = layer["keys"].toArray();
    QJsonObject key = keys[0].toObject();
    key["elements"] = QJsonArray{paint};
    keys[0] = key;
    layer["keys"] = keys;
    layers[0] = layer;
    scene["layers"] = layers;
    scenes[0] = scene;
    root["scenes"] = scenes;
    Document back;
    CHECK(deserializeDocument(QJsonDocument(root).toJson(), back));
    const auto& els = back.scenes[0].layers[0].keys[0].elements;
    CHECK(els.size() == 1);
    const ShapeElement* sh = asShape(els.front());
    CHECK(sh && sh->isObject && sh->graph->fills.size() == 1);
    CHECK(sh->graph->fills[0].color == Color(255, 0, 0));
    const Rect b = sh->graph->bounds(false);
    CHECK(b.x0 < 5 && b.x1 > 205 && b.y0 < 45 && b.y1 > 55);
}

VX_TEST_MAIN()
