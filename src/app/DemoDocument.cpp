// SPDX-License-Identifier: GPL-3.0-or-later
#include "DemoDocument.h"

#include "core/DocumentOps.h"
#include "core/TimelineOps.h"
#include "core/VectorBrush.h"
#include "geom/Fit.h"
#include "geom/Outline.h"

#include <cmath>

namespace vx::app {

namespace {

FillStyle linear(Color a, Color b, const Rect& r, double angle = kPi / 2)
{
    FillStyle f;
    f.kind = FillStyle::Kind::Linear;
    f.gradient.stops = {{0.0, a}, {1.0, b}};
    f.gradient.matrix = Affine::translate(r.center()) * Affine::rotate(angle) * Affine::scale(r.height() * 0.5, r.width() * 0.5);
    return f;
}

FillStyle radial(Color a, Color b, Vec2 c, double r)
{
    FillStyle f;
    f.kind = FillStyle::Kind::Radial;
    f.gradient.stops = {{0.0, a}, {1.0, b}};
    f.gradient.matrix = Affine::translate(c) * Affine::scale(r);
    return f;
}

Region brushStroke(const std::vector<Vec2>& pts, double r0, double r1)
{
    std::vector<BrushPoint> bp;
    for (size_t i = 0; i < pts.size(); ++i) {
        const double t = pts.size() > 1 ? double(i) / double(pts.size() - 1) : 0.0;
        bp.push_back({pts[i], r0 + (r1 - r0) * std::sin(t * kPi), 0.0});
    }
    return refitRegion(sweptRegion(resampleStroke(bp, 1.0), BrushTip{}), 0.08);
}

Layer& addLayer(Document& d, Timeline& tl, const std::string& name, int frames)
{
    Layer l = d.makeLayer(name);
    extendTo(l, frames - 1);
    tl.layers.push_back(std::move(l));
    return tl.layers.back();
}

} // namespace

Document createDemoDocument()
{
    Document d = Document::createDefault();
    d.width = 1280;
    d.height = 720;
    d.fps = 24;
    d.background = Color(0xF4, 0xF1, 0xEA);
    Timeline& tl = d.scenes[0];
    tl.layers.clear();
    const int frames = 48;

    // Symbols -----------------------------------------------------------------
    StrokeStyle ink;
    ink.width = 4;
    ink.paint = FillStyle::solid(Color(0x1B, 0x1A, 0x22));
    // Sun: radial gradient disc with rays (graphic symbol).
    ShapeGraph sun = graphFromRegion(Region::circle({0, 0}, 70), radial(Color(0xFF, 0xE0, 0x6B), Color(0xFF, 0x7A, 0x2E), {-18, -18}, 90));
    for (int i = 0; i < 12; ++i) {
        const double a = i * kPi / 6;
        const std::vector<Vec2> ray{fromAngle(a, 88), fromAngle(a, 112)};
        sun = overlay(sun, graphFromRegion(brushStroke(ray, 3, 7), FillStyle::solid(Color(0xFF, 0x9A, 0x2E))));
    }
    auto sunInst = convertToSymbol(d, {makeShapeElement(sun, false)}, "Sun", SymbolType::Graphic, {0, 0});
    // Ball with a highlight (movie clip, screen blend).
    ShapeGraph ball = graphFromShape(Region::circle({0, 0}, 34), nullptr, &ink);
    ball = overlay(graphFromRegion(Region::circle({0, 0}, 34), radial(Color(0x8B, 0x6C, 0xFF), Color(0x3D, 0x2B, 0xA8), {-12, -12}, 46)), ball);
    auto ballInst = convertToSymbol(d, {makeShapeElement(ball, false)}, "Ball", SymbolType::MovieClip, {0, 0});

    // Layers (top to bottom in the timeline) ------------------------------------
    // Vector brushes: textured, art, pattern and scatter strokes (all fills).
    {
        Layer& l = addLayer(d, tl, "Vector brushes", frames);
        ShapeGraph merged;
        auto strokeOf = [&](const char* id, Color c, double y0, double amp, uint32_t seed) {
            const VectorBrushPreset* p = builtinVectorBrush(id);
            if (!p) return;
            std::vector<InputSample> samples;
            for (int i = 0; i <= 80; ++i) {
                const double t = i / 80.0;
                InputSample q;
                q.pos = {860 + t * 360, y0 + std::sin(t * 7.0) * amp};
                q.pressure = 0.25 + 0.75 * std::sin(t * kPi);
                samples.push_back(q);
            }
            const ShapeGraph g = vectorBrushGraph(vectorBrushStroke(*p, vectorBrushPath(*p, samples), FillStyle::solid(c), seed, 0.05));
            merged = merged.isEmpty() ? g : overlay(merged, g);
        };
        strokeOf("chalk", Color(0xFF, 0x5B, 0x2E), 512, 14, 3);
        strokeOf("ink-taper", Color(0x1B, 0x1A, 0x22), 550, 12, 5);
        strokeOf("vine", Color(0x2E, 0x6B, 0x3A), 592, 10, 7);
        strokeOf("stipple", Color(0x2B, 0xD9, 0xA8), 634, 14, 9);
        strokeOf("rope", Color(0x3D, 0x8B, 0xFF), 668, 8, 11);
        setKeyframeMergeShape(l.keys[0], std::move(merged));
    }
    // Motion guide + ball following it.
    {
        Layer guide = d.makeLayer("Guide: Ball");
        guide.type = LayerType::Guide;
        extendTo(guide, frames - 1);
        StrokeStyle g;
        g.width = 2;
        g.paint = FillStyle::solid(Color(0x8B, 0x6C, 0xFF));
        const Cubic arc{{120, 560}, {260, 240}, {520, 260}, {660, 520}};
        guide.keys[0].elements.push_back(makeShapeElement(graphFromPaths({{arc}}, g), false));
        Layer ball = d.makeLayer("Ball");
        ball.parentId = guide.id;
        ball.keys[0].duration = frames - 1;
        // Soft glow and a drop shadow that lengthens along the flight (filters tween).
        Filter glow = Filter::defaults(FilterType::Glow);
        glow.color = Color(0x8B, 0x6C, 0xFF, 150);
        glow.blurX = glow.blurY = 22;
        glow.quality = 2;
        Filter shadow = Filter::defaults(FilterType::DropShadow);
        shadow.color = Color(0x1B, 0x1A, 0x22, 120);
        shadow.blurX = shadow.blurY = 10;
        shadow.distance = 6;
        shadow.quality = 2;
        auto flying = [&](const Affine& m, double distance) {
            auto b = ballInst->cloneAs<InstanceElement>();
            b->matrix = m;
            shadow.distance = distance;
            b->filters = {glow, shadow};
            return b;
        };
        ball.keys[0].elements.push_back(flying(Affine::translate(120, 560), 6));
        ball.keys[0].tween = TweenType::Classic;
        ball.keys[0].classic.ease.kind = EaseKind::SineInOut;
        ball.keys[0].classic.orientToPath = true;
        Keyframe end;
        end.duration = 1;
        end.elements.push_back(flying(Affine::translate(660, 520), 22));
        ball.keys.push_back(end);
        ball.normalize();
        tl.layers.push_back(guide);
        tl.layers.push_back(ball);
    }
    // Sun with a rotating classic tween.
    {
        Layer& l = addLayer(d, tl, "Sun", frames);
        l.keys[0].duration = frames - 1;
        l.keys[0].elements.push_back(sunInst->withMatrix(Affine::translate(1040, 170)));
        l.keys[0].tween = TweenType::Classic;
        l.keys[0].classic.rotate = RotateMode::Clockwise;
        l.keys[0].classic.rotations = 1;
        l.keys[0].classic.ease.kind = EaseKind::Classic;
        l.keys[0].classic.ease.strength = 60;
        l.keys[0].label = "sunrise";
        Keyframe end;
        end.duration = 1;
        auto s = sunInst->cloneAs<InstanceElement>();
        s->matrix = Affine::translate(1040, 140) * Affine::scale(1.15);
        s->color.kind = ColorEffect::Kind::Tint;
        s->color.tint = Color(0xFF, 0x4F, 0xA3);
        s->color.tintAmount = 0.35;
        end.elements.push_back(s);
        l.keys.push_back(end);
        l.normalize();
    }
    // Shape tween: square -> blob.
    {
        Layer& l = addLayer(d, tl, "Morph", frames);
        const FillStyle a = FillStyle::solid(Color(0x2B, 0xD9, 0xA8));
        const FillStyle b = FillStyle::solid(Color(0xFF, 0x4F, 0xA3));
        l.keys[0].duration = frames - 1;
        l.keys[0].elements.push_back(makeShapeElement(graphFromShape(Region::roundedRect({470, 90, 610, 230}, 18), &a, &ink), false));
        l.keys[0].tween = TweenType::Shape;
        l.keys[0].shape.ease.kind = EaseKind::BackInOut;
        Keyframe end;
        end.duration = 1;
        Region blob;
        {
            std::vector<Vec2> pts;
            for (int i = 0; i < 64; ++i) {
                const double t = 2 * kPi * i / 64.0;
                const double r = 80 + 18 * std::sin(3 * t);
                pts.push_back({560 + std::cos(t) * r, 170 + std::sin(t) * r});
            }
            FitOptions opt;
            opt.closed = true;
            opt.tolerance = 0.3;
            blob.contours.push_back(fitCurves(pts, opt));
        }
        end.elements.push_back(makeShapeElement(graphFromShape(blob, &b, &ink), false));
        l.keys.push_back(end);
        l.normalize();
    }
    // Merge drawing: hills made of brush strokes and a bucket-style fill.
    {
        Layer& l = addLayer(d, tl, "Hills", frames);
        ShapeGraph g = graphFromRegion(Region::rect({0, 540, 1280, 720}), linear(Color(0x2F, 0x6B, 0x4F), Color(0x1B, 0x3D, 0x2E), {0, 540, 1280, 720}));
        std::vector<Vec2> ridge;
        for (int i = 0; i <= 64; ++i) ridge.push_back({i * 20.0, 560 - 60 * std::sin(i * 0.15) - 20 * std::sin(i * 0.5)});
        g = overlay(g, graphFromRegion(brushStroke(ridge, 6, 26), FillStyle::solid(Color(0x2F, 0x6B, 0x4F))));
        StrokeStyle outline;
        outline.width = 3;
        outline.paint = FillStyle::solid(Color(0x1B, 0x1A, 0x22));
        std::vector<Vec2> path;
        for (int i = 0; i <= 64; ++i) path.push_back({i * 20.0, 600 - 30 * std::sin(i * 0.23)});
        FitOptions fo;
        fo.tolerance = 0.5;
        g = overlay(g, graphFromPaths({fitCurves(path, fo)}, outline));
        setKeyframeMergeShape(l.keys[0], g);
    }
    // Sky.
    {
        Layer& l = addLayer(d, tl, "Sky", frames);
        l.locked = true;
        const Rect r{0, 0, 1280, 720};
        setKeyframeMergeShape(l.keys[0], graphFromRegion(Region::rect(r), linear(Color(0xFF, 0xD7, 0xB8), Color(0x9F, 0xC5, 0xFF), r)));
    }
    return d;
}

} // namespace vx::app
