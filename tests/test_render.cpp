// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestMain.h"

#include "core/DocumentOps.h"
#include "core/VectorBrush.h"
#include "render/Blend.h"
#include "render/Filters.h"
#include "render/LayerCache.h"
#include "render/Raster.h"
#include "render/QtConvert.h"
#include "render/Renderer.h"
#include "render/SvgExport.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>

using namespace vx;

namespace {

QImage blank(int w, int h, QRgb c = 0)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(c);
    return img;
}

int maxDiff(const QImage& a, const QImage& b)
{
    if (a.size() != b.size()) return 256;
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto* pa = reinterpret_cast<const quint32*>(a.constScanLine(y));
        const auto* pb = reinterpret_cast<const quint32*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            for (int sh = 0; sh < 32; sh += 8) worst = std::max(worst, std::abs(int((pa[x] >> sh) & 0xff) - int((pb[x] >> sh) & 0xff)));
    }
    return worst;
}

std::shared_ptr<ShapeElement> box(Rect r, Color c, bool object = true)
{
    return makeShapeElement(graphFromRegion(Region::rect(r), FillStyle::solid(c)), object);
}

/// A scene with a bit of everything the renderer does: static layers, a
/// classic tween of a filtered movie clip, a looping graphic, a locked mask,
/// layer opacity and blending, an instance blend mode and a hidden layer.
Document richScene()
{
    Document d = Document::createDefault();
    d.width = 400;
    d.height = 300;
    Timeline& tl = d.scenes[0];
    tl.layers.clear();
    auto layer = [&](const std::string& name) -> Layer& {
        tl.layers.push_back(d.makeLayer(name));
        tl.layers.back().keys[0].duration = 24;
        return tl.layers.back();
    };
    // Bottom to top in the layer list order below: index 0 is the top layer.
    std::vector<Layer> stack;
    {
        Layer& l = layer("static");
        for (int i = 0; i < 30; ++i)
            l.keys[0].elements.push_back(box({10.0 + i * 12, 20.0 + (i % 5) * 50, 30.0 + i * 12, 60.0 + (i % 5) * 50},
                                             Color(40 * (i % 6), 200 - 5 * i, 90)));
    }
    {
        Layer& l = layer("tween");
        auto clip = convertToSymbol(d, {box({0, 0, 40, 30}, Color(250, 180, 0))}, "Clip", SymbolType::MovieClip, {0, 0});
        clip->filters = {Filter::defaults(FilterType::DropShadow)};
        Keyframe k0 = l.keys[0];
        k0.duration = 12;
        k0.tween = TweenType::Classic;
        k0.elements = {clip};
        auto moved = std::static_pointer_cast<InstanceElement>(clip->clone());
        moved->matrix = Affine::translate(300, 200) * Affine::rotate(0.8);
        Keyframe k1;
        k1.start = 12;
        k1.duration = 12;
        k1.elements = {moved};
        l.keys = {k0, k1};
    }
    {
        // A graphic symbol that plays its own frames.
        Symbol s;
        s.id = d.newSymbolId();
        s.name = "Blink";
        s.type = SymbolType::Graphic;
        Layer sl = d.makeLayer("Layer 1");
        sl.keys.clear();
        for (int f = 0; f < 4; ++f) {
            Keyframe k;
            k.start = f;
            k.duration = 1;
            k.elements = {box({0, 0, 20.0 + f * 10, 20}, Color(0, 60 * f, 255))};
            sl.keys.push_back(k);
        }
        s.timeline.layers.push_back(sl);
        d.symbols.push_back(s);
        auto g = std::make_shared<InstanceElement>();
        g->symbolId = s.id;
        g->behavior = SymbolType::Graphic;
        g->matrix = Affine::translate(200, 30);
        layer("graphic").keys[0].elements = {g};
    }
    {
        Layer& mask = layer("mask");
        mask.type = LayerType::Mask;
        mask.locked = true;
        mask.keys[0].elements = {box({50, 100, 250, 180}, Color(0, 0, 0))};
        Layer& masked = layer("masked");
        masked.parentId = tl.layers[tl.layers.size() - 2].id;
        masked.keys[0].elements = {box({0, 120, 400, 160}, Color(200, 0, 200))};
    }
    {
        Layer& l = layer("half multiply");
        l.opacity = 0.5;
        l.blend = BlendMode::Multiply;
        l.keys[0].elements = {box({100, 50, 300, 250}, Color(0, 200, 250))};
    }
    {
        Layer& l = layer("screen instance");
        auto sc = convertToSymbol(d, {box({0, 0, 80, 80}, Color(90, 90, 200))}, "Glow", SymbolType::MovieClip, {0, 0});
        sc->blend = BlendMode::Screen;
        sc->matrix = Affine::translate(20, 180);
        l.keys[0].elements = {sc};
    }
    {
        Layer& l = layer("hidden");
        l.visible = false;
        l.keys[0].elements = {box({0, 0, 400, 300}, Color(255, 0, 0))};
    }
    {
        Layer& l = layer("faded top");
        l.opacity = 0.6;
        l.keys[0].elements = {box({150, 10, 380, 60}, Color(20, 20, 20))};
    }
    std::reverse(tl.layers.begin(), tl.layers.end()); // "faded top" first
    // Masked layers follow their mask in the list.
    for (size_t i = 0; i + 1 < tl.layers.size(); ++i)
        if (tl.layers[i].name == "masked" && tl.layers[i + 1].name == "mask") std::swap(tl.layers[i], tl.layers[i + 1]);
    return d;
}

QImage renderDirect(const Document& d, int frame, QSize size, const Affine& view)
{
    QImage img = blank(size.width(), size.height());
    CpuSurface surface(img);
    Renderer(d).render(surface, d.scenes[0], frame, view);
    return img;
}

} // namespace

VX_TEST(no_seams_between_adjacent_fills)
{
    // Red and blue halves share an edge at a fractional x on a white background.
    ShapeGraph g = graphFromRegion(Region::rect({0.0, 0.0, 10.37, 20.0}), FillStyle::solid(Color(255, 0, 0)));
    g = overlay(g, graphFromRegion(Region::rect({10.37, 0.0, 20.0, 20.0}), FillStyle::solid(Color(0, 0, 255))));
    QImage img = blank(20, 20, 0xffffffff);
    Renderer::renderShape(img, g.renderData(), Affine{}, {}, img.rect());
    const QRgb seam = img.pixel(10, 10);
    // Pixel 10 is 37% red and 63% blue: no white must leak through.
    CHECK(qGreen(seam) <= 1);
    CHECK(std::abs(qRed(seam) - 94) <= 2);
    CHECK(std::abs(qBlue(seam) - 161) <= 2);
    CHECK(img.pixel(3, 3) == qRgb(255, 0, 0));
    CHECK(img.pixel(15, 3) == qRgb(0, 0, 255));
}

VX_TEST(antialiased_coverage_is_exact)
{
    // A half-pixel wide vertical bar gives 50% coverage.
    ShapeGraph g = graphFromRegion(Region::rect({2.0, 0.0, 2.5, 4.0}), FillStyle::solid(Color(0, 0, 0)));
    QImage img = blank(4, 4);
    Renderer::renderShape(img, g.renderData(), Affine{}, {}, img.rect());
    CHECK(std::abs(qAlpha(img.pixel(2, 1)) - 128) <= 1);
    CHECK(qAlpha(img.pixel(1, 1)) == 0);
    // Holes stay empty.
    ShapeGraph ring = graphFromRegion(booleanOp(Region::rect({0, 0, 8, 8}), Region::rect({2, 2, 6, 6}), BoolOp::Subtract),
                                      FillStyle::solid(Color(0, 0, 0)));
    QImage r = blank(8, 8);
    Renderer::renderShape(r, ring.renderData(), Affine{}, {}, r.rect());
    CHECK(qAlpha(r.pixel(4, 4)) == 0);
    CHECK(qAlpha(r.pixel(1, 4)) == 255);
}

VX_TEST(blend_modes)
{
    QImage dst = blank(1, 1, qRgba(128, 128, 128, 255));
    QImage src = blank(1, 1, qRgba(255, 0, 0, 255));
    QImage d1 = dst;
    compositeImage(d1, src, {0, 0}, BlendMode::Multiply, 1.0);
    CHECK(std::abs(qRed(d1.pixel(0, 0)) - 128) <= 1 && qGreen(d1.pixel(0, 0)) == 0);
    QImage d2 = dst;
    compositeImage(d2, src, {0, 0}, BlendMode::Screen, 1.0);
    CHECK(qRed(d2.pixel(0, 0)) == 255 && std::abs(qGreen(d2.pixel(0, 0)) - 128) <= 1);
    QImage d3 = dst;
    compositeImage(d3, src, {0, 0}, BlendMode::Erase, 1.0);
    CHECK(qAlpha(d3.pixel(0, 0)) == 0);
    QImage d4 = dst;
    compositeImage(d4, src, {0, 0}, BlendMode::Difference, 1.0);
    CHECK(std::abs(qRed(d4.pixel(0, 0)) - 127) <= 1 && std::abs(qGreen(d4.pixel(0, 0)) - 128) <= 1);
    QImage d5 = dst;
    compositeImage(d5, src, {0, 0}, BlendMode::Normal, 0.5);
    CHECK(std::abs(qRed(d5.pixel(0, 0)) - 192) <= 1);
}

VX_TEST(vector_brushes_render)
{
    // Every built-in brush stroke renders as ordinary vector fills.
    for (const VectorBrushPreset& p : builtinVectorBrushes()) {
        std::vector<InputSample> samples;
        for (int i = 0; i <= 40; ++i) {
            InputSample q;
            q.pos = {20 + i * 3.0, 30 + std::sin(i * 0.2) * 8};
            q.pressure = 0.8;
            samples.push_back(q);
        }
        VectorBrushPreset small = p;
        small.size = std::min(p.size, 16.0);
        const ShapeGraph g = vectorBrushGraph(vectorBrushStroke(small, vectorBrushPath(small, samples), FillStyle::solid(Color(0, 0, 0)), 3, 0.05));
        QImage img = blank(160, 60);
        Renderer::renderShape(img, g.renderData(), Affine{}, {}, img.rect());
        int painted = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) painted += qAlpha(img.pixel(x, y)) > 0;
        CHECK(painted > 50);
    }
}

VX_TEST(filters)
{
    // A white 20x20 square in the middle of a transparent 60x60 image.
    auto square = [] {
        QImage img = blank(60, 60);
        ShapeGraph g = graphFromRegion(Region::rect({20, 20, 40, 40}), FillStyle::solid(Color(255, 255, 255)));
        Renderer::renderShape(img, g.renderData(), Affine{}, {}, img.rect());
        return img;
    };
    // Drop shadow at 45 degrees: below-right of the square, not above-left.
    {
        QImage img = square();
        Filter f = Filter::defaults(FilterType::DropShadow);
        f.blurX = f.blurY = 2;
        f.distance = 8;
        applyFilters(img, {f}, 1.0);
        CHECK(qAlpha(img.pixel(44, 44)) > 150);
        CHECK(qRed(img.pixel(44, 44)) < 30);       // black shadow
        CHECK(qAlpha(img.pixel(16, 16)) == 0);
        CHECK(img.pixel(30, 30) == qRgba(255, 255, 255, 255)); // object on top
        // Scale doubles the distance (zoomed view).
        QImage z = square();
        applyFilters(z, {f}, 2.0);
        CHECK(qAlpha(z.pixel(49, 49)) > 100);
        CHECK(qAlpha(img.pixel(49, 49)) < 40);
    }
    // Glow spreads evenly; knockout removes the object.
    {
        QImage img = square();
        Filter g = Filter::defaults(FilterType::Glow);
        g.blurX = g.blurY = 8;
        g.knockout = true;
        applyFilters(img, {g}, 1.0);
        CHECK(qAlpha(img.pixel(30, 30)) == 0);
        CHECK(qAlpha(img.pixel(18, 30)) > 40 && qRed(img.pixel(18, 30)) > qGreen(img.pixel(18, 30)));
        CHECK(std::abs(qAlpha(img.pixel(18, 30)) - qAlpha(img.pixel(41, 30))) <= 12);
    }
    // Blur softens the edge symmetrically and keeps total coverage.
    {
        QImage img = square();
        Filter b = Filter::defaults(FilterType::Blur);
        b.blurX = b.blurY = 6;
        applyFilters(img, {b}, 1.0);
        CHECK(qAlpha(img.pixel(19, 30)) > 20 && qAlpha(img.pixel(19, 30)) < 235);
        long sum = 0;
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 60; ++x) sum += qAlpha(img.pixel(x, y));
        CHECK(std::abs(sum - 400L * 255) < 400L * 255 / 50);
    }
    // Adjust colour: a hue rotation turns red towards green / blue.
    {
        QImage img = blank(4, 4, qRgba(255, 0, 0, 255));
        Filter a = Filter::defaults(FilterType::AdjustColor);
        a.hue = 120;
        applyFilters(img, {a}, 1.0);
        CHECK(qRed(img.pixel(1, 1)) < qGreen(img.pixel(1, 1)));
        a.hue = 0;
        CHECK(!hasActiveFilters({a}));
    }
    // Inside a document: a movie clip with a glow renders outside its shape.
    {
        Document d = Document::createDefault();
        auto inst = convertToSymbol(d, {makeShapeElement(graphFromRegion(Region::rect({0, 0, 20, 20}),
                                                                           FillStyle::solid(Color(0, 0, 255))), false)},
                                    "Box", SymbolType::MovieClip, {0, 0});
        auto glowing = inst->cloneAs<InstanceElement>();
        glowing->matrix = Affine::translate(100, 100);
        Filter g = Filter::defaults(FilterType::Glow);
        g.blurX = g.blurY = 10;
        glowing->filters = {g};
        d.scenes[0].layers[0].keys[0].elements = {glowing};
        const QImage frame = Renderer::renderFrame(d, d.scenes[0], 0, 1.0, true);
        CHECK(qAlpha(frame.pixel(97, 110)) > 30);       // glow left of the box
        CHECK(qRed(frame.pixel(97, 110)) > qBlue(frame.pixel(97, 110)));
        CHECK(qBlue(frame.pixel(110, 110)) > 200);      // the box itself
        CHECK(qAlpha(frame.pixel(60, 110)) == 0);
        const QString svg = frameToSvg(d, d.scenes[0], 0);
        CHECK(svg.contains("<feDropShadow") && svg.contains("filter=\"url(#f"));
    }
}

VX_TEST(document_frame_render)
{
    Document d = Document::createDefault();
    d.width = 100;
    d.height = 50;
    auto shape = makeShapeElement(graphFromRegion(Region::rect({0, 0, 20, 20}), FillStyle::solid(Color(0, 128, 0))), false);
    auto inst = convertToSymbol(d, {shape}, "Box", SymbolType::MovieClip, {0, 0});
    inst->matrix = Affine::translate(40, 10);
    inst->color.kind = ColorEffect::Kind::Tint;
    inst->color.tint = Color(255, 0, 0);
    inst->color.tintAmount = 1.0;
    d.scenes[0].layers[0].keys[0].elements = {inst};
    const QImage img = Renderer::renderFrame(d, d.scenes[0], 0, 1.0, false);
    CHECK(img.pixel(50, 20) == qRgb(255, 0, 0));
    CHECK(img.pixel(5, 5) == qRgb(255, 255, 255));
    // Mask layer (locked) clips the masked layer.
    Layer mask = d.makeLayer("Mask");
    mask.type = LayerType::Mask;
    mask.locked = true;
    mask.keys[0].elements = {makeShapeElement(graphFromRegion(Region::rect({40, 10, 50, 30}), FillStyle::solid(Color(0, 0, 0))), false)};
    d.scenes[0].layers[0].parentId = mask.id;
    d.scenes[0].layers.insert(d.scenes[0].layers.begin(), mask);
    const QImage masked = Renderer::renderFrame(d, d.scenes[0], 0, 1.0, false);
    CHECK(masked.pixel(45, 20) == qRgb(255, 0, 0));
    CHECK(masked.pixel(55, 20) == qRgb(255, 255, 255));
}

VX_TEST(button_states_and_nine_slice_render)
{
    Document d = Document::createDefault();
    // Button: red Up, green Over, blue Down squares.
    Symbol b;
    b.id = d.newSymbolId();
    b.name = "Btn";
    b.type = SymbolType::Button;
    Layer bl = d.makeLayer("Layer 1");
    bl.keys.clear();
    const Color colors[3] = {Color(255, 0, 0), Color(0, 255, 0), Color(0, 0, 255)};
    for (int f = 0; f < 3; ++f) {
        Keyframe k;
        k.start = f;
        k.duration = 1;
        k.elements = {makeShapeElement(graphFromRegion(Region::rect({0, 0, 20, 20}), FillStyle::solid(colors[f])), false)};
        bl.keys.push_back(k);
    }
    b.timeline.layers.push_back(bl);
    d.symbols.push_back(b);
    auto inst = std::make_shared<InstanceElement>();
    inst->symbolId = b.id;
    inst->behavior = SymbolType::Button;
    inst->matrix = Affine::translate(10, 10);
    d.scenes[0].layers[0].keys[0].elements = {inst};
    auto pixel = [&](RenderOptions o, int x, int y) {
        QImage img = blank(120, 60);
        Renderer(d, o).render(img, d.scenes[0], 0, Affine{});
        return img.pixel(x, y);
    };
    CHECK(qRed(pixel({}, 20, 20)) == 255);
    RenderOptions over;
    over.hotButton = d.scenes[0].layers[0].keys[0].elements[0].get();
    CHECK(qGreen(pixel(over, 20, 20)) == 255 && qRed(pixel(over, 20, 20)) == 0);
    over.hotState = ButtonState::Down;
    CHECK(qBlue(pixel(over, 20, 20)) == 255);

    // 9-slice: a 40x20 panel with a 4 px dark border scaled 2.5x wide keeps a
    // 4 px border on the left instead of a 10 px one.
    Document p = Document::createDefault();
    ShapeGraph panel = graphFromRegion(Region::rect({0, 0, 40, 20}), FillStyle::solid(Color(0, 0, 0)));
    panel = overlay(panel, graphFromRegion(Region::rect({4, 4, 36, 16}), FillStyle::solid(Color(255, 255, 255))));
    auto pi = convertToSymbol(p, {makeShapeElement(panel, false)}, "Panel", SymbolType::MovieClip, {0, 0});
    pi->matrix = Affine::scale(2.5, 1.0);
    p.scenes[0].layers[0].keys[0].elements = {pi};
    auto render = [&]() {
        QImage img = blank(120, 30);
        Renderer(p).render(img, p.scenes[0], 0, Affine{});
        return img;
    };
    QImage plain = render();
    CHECK(qRed(plain.pixel(6, 10)) < 20); // border stretched to 10 px
    p.symbols.back().scale9 = Rect(4, 4, 36, 16);
    QImage sliced = render();
    CHECK(qRed(sliced.pixel(6, 10)) > 235);  // inside the white middle
    CHECK(qRed(sliced.pixel(2, 10)) < 20);   // border still 4 px wide
    CHECK(qRed(sliced.pixel(97, 10)) < 20);  // right border ends at 100
    CHECK(qRed(sliced.pixel(94, 10)) > 235);
}

VX_TEST(layer_opacity_and_blending)
{
    Document d = Document::createDefault();
    d.width = 60;
    d.height = 40;
    d.scenes[0].layers[0].keys[0].elements = {box({10, 10, 50, 30}, Color(255, 0, 0))};
    d.scenes[0].layers[0].opacity = 0.5;
    QImage img = Renderer::renderFrame(d, d.scenes[0], 0, 1.0, false);
    CHECK(qRed(img.pixel(30, 20)) == 255 && std::abs(qGreen(img.pixel(30, 20)) - 128) <= 1);
    CHECK(img.pixel(5, 5) == qRgb(255, 255, 255));
    // Overlapping objects inside a half transparent layer do not show through
    // each other: the layer is composited as a whole.
    d.scenes[0].layers[0].keys[0].elements.push_back(box({30, 10, 55, 30}, Color(0, 0, 255)));
    img = Renderer::renderFrame(d, d.scenes[0], 0, 1.0, false);
    CHECK(qRed(img.pixel(40, 20)) >= 126 && qRed(img.pixel(40, 20)) <= 129 && qBlue(img.pixel(40, 20)) == 255);
    // Multiply on a layer darkens what lies below.
    Layer top = d.makeLayer("Top");
    top.blend = BlendMode::Multiply;
    top.keys[0].elements = {box({0, 0, 60, 40}, Color(128, 128, 128))};
    d.scenes[0].layers.insert(d.scenes[0].layers.begin(), top);
    img = Renderer::renderFrame(d, d.scenes[0], 0, 1.0, false);
    CHECK(std::abs(qRed(img.pixel(5, 5)) - 128) <= 1);
}

VX_TEST(stroke_outlines_match_the_pen)
{
    // Strokes are filled from their outlines (cached per chain); the result
    // matches QPainter stroking the same path.
    for (const CapStyle cap : {CapStyle::Round, CapStyle::Square, CapStyle::None}) {
        StrokeStyle st;
        st.width = 9;
        st.cap = cap;
        st.join = cap == CapStyle::Square ? JoinStyle::Miter : JoinStyle::Round;
        st.paint = FillStyle::solid(Color(20, 40, 200));
        const std::vector<Cubic> chain = {Cubic{{10, 10}, {60, -20}, {90, 80}, {120, 30}}, Cubic::line({120, 30}, {40, 70})};
        const ShapeGraph g = graphFromPaths({chain}, st);
        for (const double scale : {0.7, 2.5}) {
            const Affine m = Affine::scale(scale) * Affine::translate(8, 30);
            const QSize size(int(140 * scale), int(120 * scale));
            const auto rd = g.renderDataPtr();
            QImage once = blank(size.width(), size.height(), 0xffffffff);
            QImage cached = blank(size.width(), size.height(), 0xffffffff);
            QImage pen = blank(size.width(), size.height(), 0xffffffff);
            Renderer::renderShape(once, rd, m, {}, once.rect());
            Renderer::renderShape(cached, rd, m, {}, cached.rect()); // outlines from the cache
            CHECK(cached == once);
            {
                QPainter p(&pen);
                p.setRenderHint(QPainter::Antialiasing);
                p.setTransform(toQTransform(m));
                QPen qpen(QColor(20, 40, 200), st.width);
                qpen.setCapStyle(cap == CapStyle::Round ? Qt::RoundCap : cap == CapStyle::Square ? Qt::SquareCap : Qt::FlatCap);
                qpen.setJoinStyle(st.join == JoinStyle::Miter ? Qt::MiterJoin : Qt::RoundJoin);
                qpen.setMiterLimit(st.miterLimit);
                p.setPen(qpen);
                QPainterPath path;
                appendChain(path, chain, false);
                p.drawPath(path);
            }
            double sum = 0;
            int n = 0;
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x) {
                    const QRgb a = once.pixel(x, y), b = pen.pixel(x, y);
                    sum += std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b));
                    ++n;
                }
            std::printf("  cap %d scale %.1f: mean difference %.3f\n", int(cap), scale, sum / (3.0 * n));
            CHECK(sum / (3.0 * n) < 0.6);
        }
    }
}

VX_TEST(banded_render_matches_single_pass)
{
    // Large frames are rendered in bands on several threads.
    const Document d = richScene();
    const Affine view = Affine::scale(3.5);
    for (int frame : {0, 5, 13}) {
        QImage banded = blank(1400, 1050);
        Renderer(d).render(banded, d.scenes[0], frame, view);
        CHECK(maxDiff(banded, renderDirect(d, frame, banded.size(), view)) <= 1);
    }
}

VX_TEST(zoomed_in_render_matches_larger_render)
{
    // Curves away from the visible area are replaced by their chords: the
    // pixels that are drawn must not change.
    ShapeGraph g = graphFromRegion(Region::circle({100, 100}, 90), FillStyle::solid(Color(20, 120, 220)));
    g = overlay(g, graphFromRegion(Region::ellipse({150, 80}, 60, 30), FillStyle::solid(Color(230, 60, 20))));
    const Affine view = Affine::translate(-900, -300) * Affine::scale(12);
    QImage small = blank(320, 240, qRgb(255, 255, 255));
    Renderer::renderShape(small, g.renderData(), view, {}, small.rect());
    QImage large = blank(1600, 1200, qRgb(255, 255, 255));
    Renderer::renderShape(large, g.renderData(), view, {}, large.rect());
    CHECK(maxDiff(small, large.copy(0, 0, 320, 240)) <= 1);
}

VX_TEST(fast_source_over)
{
    QImage dst = blank(2, 1, qRgba(255, 255, 255, 255));
    QImage src = blank(2, 1);
    src.setPixel(0, 0, qRgba(64, 32, 0, 128)); // premultiplied
    src.setPixel(1, 0, qRgba(10, 20, 30, 255));
    compositeImage(dst, src, {0, 0}, BlendMode::Normal, 1.0);
    CHECK(dst.pixel(0, 0) == qRgba(64 + 127, 32 + 127, 127, 255));
    CHECK(dst.pixel(1, 0) == qRgba(10, 20, 30, 255));
}

VX_TEST(layer_cache_matches_direct_render)
{
    Document d = richScene();
    const QSize size(400, 300);
    const Affine view;
    LayerCache cache;
    // Playhead moves, pauses, jumps back; an edit on one layer in between.
    const std::vector<int> frames = {0, 0, 1, 2, 3, 4, 5, 6, 6, 6, 6, 6, 7, 12, 13, 13, 13, 13, 13, 20, 0, 1};
    for (size_t i = 0; i < frames.size(); ++i) {
        if (i == 10) d.scenes[0].layers.back().keys[0].elements.push_back(box({5, 5, 50, 50}, Color(0, 0, 0)));
        if (i == 16) d.scenes[0].layers[0].opacity = 0.3;
        QImage img = blank(size.width(), size.height());
        cache.render(img, d, d.scenes[0], frames[i], view, {}, "0", true);
        CHECK(maxDiff(img, renderDirect(d, frames[i], size, view)) <= 2);
    }
    // Resting on a frame: only layers that blend with what lies below are drawn.
    for (int k = 0; k < 6; ++k) {
        QImage img = blank(size.width(), size.height());
        cache.render(img, d, d.scenes[0], 1, view, {}, "0", true);
        CHECK(maxDiff(img, renderDirect(d, 1, size, view)) <= 2);
    }
    const int layers = cache.stats().layers;
    CHECK(layers == 7); // the hidden layer is skipped, the mask draws with its layer
    CHECK(cache.stats().drawn <= 2);
    // A new view draws everything again.
    QImage img = blank(size.width(), size.height());
    cache.render(img, d, d.scenes[0], 1, Affine::scale(0.5), {}, "0", true);
    CHECK(cache.stats().drawn == layers);
    CHECK(maxDiff(img, renderDirect(d, 1, size, Affine::scale(0.5))) <= 2);
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    return vxtest::runAll(argc, argv);
}
