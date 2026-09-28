// SPDX-License-Identifier: GPL-3.0-or-later
// The GPU renderer must draw what the CPU renderer draws. Every scene is
// rendered both ways and compared pixel by pixel: multisampled edges may
// differ slightly from exact coverage, everything else must match.
//
// Every GPU path available is checked: the OpenGL renderer and, when built
// with RHI, the RHI renderer on Vulkan and on OpenGL.
//
// Opt-in: set VERTEXA_TEST_GPU=1 (CI runs it under xvfb-run with Mesa's
// software OpenGL and Vulkan). Paths without a device are skipped.
#include "TestMain.h"

#include "core/DocumentOps.h"
#include "core/ShapeOps.h"
#include "core/io/FlaImport.h"
#include "render/GlRenderer.h"
#include "render/LayerCache.h"
#ifdef VERTEXA_HAVE_RHI
#include "render/RhiRenderer.h"
#endif

#include <QDir>
#include <QGuiApplication>

#include <cmath>
#include <cstdio>
#include <functional>
#include <cstdlib>

using namespace vx;

namespace {

/// A GPU path: renders a frame into a transparent image.
struct Backend {
    QString name;
    std::function<QImage(const Document&, const Timeline&, int, QSize, const Affine&, const RenderOptions&)> render;
};

std::vector<Backend>& backends()
{
    static std::vector<Backend> list;
    static bool built = false;
    if (built) return list;
    built = true;
    if (qEnvironmentVariable("VERTEXA_TEST_GPU") != QLatin1String("1")) {
        std::printf("  set VERTEXA_TEST_GPU=1 to compare GPU and CPU rendering\n");
        return list;
    }
    if (GlRenderer* gl = GlRenderer::instance()) {
        std::printf("  OpenGL renderer: %s, %dx MSAA\n", qPrintable(gl->deviceName()), gl->samples());
        list.push_back({"GL", [gl](const Document& d, const Timeline& tl, int frame, QSize size, const Affine& view, const RenderOptions& o) {
                            QImage img(size, QImage::Format_ARGB32_Premultiplied);
                            img.fill(0);
                            if (!gl->render(img, d, tl, frame, view, {}, o)) return QImage();
                            return img;
                        }});
    } else {
        std::printf("  no OpenGL 3.3 / ES 3.0 context: OpenGL renderer skipped\n");
    }
#ifdef VERTEXA_HAVE_RHI
    for (const char* api : {"vulkan", "opengl"}) {
        QString why;
        std::shared_ptr<RhiRenderer> rhi = RhiRenderer::createOffscreen(api, &why);
        if (!rhi || !rhi->isValid()) {
            std::printf("  RHI %s: %s, skipped\n", api, qPrintable(why));
            continue;
        }
        std::printf("  RHI renderer: %s, %dx MSAA\n", qPrintable(rhi->deviceName()), rhi->samples());
        list.push_back({"RHI " + rhi->backendName(),
                        [rhi](const Document& d, const Timeline& tl, int frame, QSize size, const Affine& view, const RenderOptions& o) {
                            return rhi->renderImage(size, [&](Surface& s) { Renderer(d, o).render(s, tl, frame, view); });
                        }});
    }
#endif
    return list;
}

bool haveGpu() { return !backends().empty(); }

struct Diff {
    double mean = 0;     ///< mean absolute channel difference (0..255)
    double bad = 0;      ///< fraction of pixels differing by more than 48 in a channel
};

Diff compare(const QImage& a, const QImage& b)
{
    Diff d;
    long long sum = 0, bad = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto* pa = reinterpret_cast<const uint32_t*>(a.constScanLine(y));
        const auto* pb = reinterpret_cast<const uint32_t*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            int worst = 0;
            for (int s = 0; s < 32; s += 8) {
                const int v = std::abs(int((pa[x] >> s) & 0xff) - int((pb[x] >> s) & 0xff));
                sum += v;
                worst = std::max(worst, v);
            }
            bad += worst > 48;
        }
    }
    const double n = double(a.width()) * a.height();
    d.mean = sum / (n * 4);
    d.bad = bad / n;
    return d;
}

bool sameOnGpu(const Document& doc, const Timeline& tl, int frame, QSize size, const Affine& view, const char* what,
               RenderOptions opts = {}, double maxBad = 0.004)
{
    QImage cpu(size, QImage::Format_ARGB32_Premultiplied);
    cpu.fill(0);
    Renderer(doc, opts).render(cpu, tl, frame, view);
    bool ok = true;
    for (const Backend& b : backends()) {
        const QImage gpu = b.render(doc, tl, frame, size, view, opts);
        if (gpu.size() != size) {
            std::printf("  %-10s %-28s NOT DRAWN\n", qPrintable(b.name), what);
            ok = false;
            continue;
        }
        const Diff d = compare(cpu, gpu);
        std::printf("  %-10s %-28s mean %.3f  edge-ish %.3f%%\n", qPrintable(b.name), what, d.mean, d.bad * 100);
        if (!(d.mean < 1.5 && d.bad < maxBad)) {
            const QString tag = QString("%1-%2").arg(b.name, what).replace(' ', '_');
            cpu.save(QString("gpu-fail-%1-cpu.png").arg(tag));
            gpu.save(QString("gpu-fail-%1-gpu.png").arg(tag));
            ok = false;
        }
    }
    return ok;
}

FillStyle gradient(FillStyle::Kind kind, SpreadMode spread, double focal, const Rect& r)
{
    FillStyle f;
    f.kind = kind;
    f.gradient.stops = {{0.0, Color(255, 60, 20)}, {0.5, Color(40, 200, 90, 200)}, {1.0, Color(30, 60, 230)}};
    f.gradient.spread = spread;
    f.gradient.focal = focal;
    f.gradient.matrix = Affine::translate(r.center()) * Affine::scale(r.width() * 0.25, r.height() * 0.25);
    return f;
}

ElementPtr shape(const Region& r, const FillStyle& f) { return makeShapeElement(graphFromRegion(r, f), false); }

/// One scene with every kind of fill and stroke.
Document shapesScene()
{
    Document d = Document::createDefault();
    Keyframe& k = d.scenes[0].layers[0].keys[0];
    ShapeGraph g = graphFromRegion(Region::rect({10, 10, 390, 290}), FillStyle::solid(Color(240, 236, 228)));
    g = overlay(g, graphFromRegion(booleanOp(Region::circle({90, 90}, 60), Region::circle({90, 90}, 25), BoolOp::Subtract),
                                   FillStyle::solid(Color(30, 30, 40))));
    g = overlay(g, graphFromRegion(Region::rect({60, 60, 200, 140}), FillStyle::solid(Color(255, 80, 40, 160))));
    int i = 0;
    for (SpreadMode sp : {SpreadMode::Pad, SpreadMode::Reflect, SpreadMode::Repeat}) {
        const Rect r(210 + i * 60, 20, 260 + i * 60, 90);
        g = overlay(g, graphFromRegion(Region::rect(r), gradient(FillStyle::Kind::Linear, sp, 0, r)));
        const Rect q(210 + i * 60, 100, 260 + i * 60, 170);
        g = overlay(g, graphFromRegion(Region::ellipse(q.center(), 25, 35), gradient(FillStyle::Kind::Radial, sp, i == 1 ? 0.6 : 0.0, q)));
        ++i;
    }
    k.elements = {makeShapeElement(g, false)};
    // Strokes: caps, joins, dashes, hairlines, gradient strokes, scaled and not.
    const std::vector<Cubic> zig{Cubic::line({30, 200}, {80, 250}), Cubic::line({80, 250}, {130, 190}),
                                 Cubic(Vec2{130, 190}, Vec2{160, 150}, Vec2{200, 280}, Vec2{230, 220})};
    StrokeStyle s;
    s.paint = FillStyle::solid(Color(20, 20, 30));
    s.width = 9;
    for (auto [cap, join, pattern, dy] : {std::tuple{CapStyle::Round, JoinStyle::Round, StrokePattern::Solid, 0.0},
                                          std::tuple{CapStyle::Square, JoinStyle::Miter, StrokePattern::Solid, 20.0},
                                          std::tuple{CapStyle::None, JoinStyle::Bevel, StrokePattern::Dashed, 40.0},
                                          std::tuple{CapStyle::Round, JoinStyle::Round, StrokePattern::Hairline, 60.0}}) {
        s.cap = cap;
        s.join = join;
        s.pattern = pattern;
        std::vector<Cubic> path;
        for (const Cubic& c : zig) path.push_back(c.translated({dy * 2, 0}));
        k.elements.push_back(makeShapeElement(graphFromPaths({path}, s), true, Affine::translate(0, dy * 0.2)));
    }
    s.pattern = StrokePattern::Solid;
    s.width = 14;
    s.paint = gradient(FillStyle::Kind::Linear, SpreadMode::Pad, 0, Rect(250, 180, 380, 280));
    k.elements.push_back(makeShapeElement(graphFromPaths({{Cubic::line({250, 190}, {380, 280})}}, s), true));
    return d;
}

/// Symbols with colour effects, blend modes, filters, a mask and layer opacity.
Document compositingScene(BlendMode mode)
{
    Document d = Document::createDefault();
    Timeline& tl = d.scenes[0];
    // Background layer (bottom).
    tl.layers[0].keys[0].elements = {shape(Region::rect({0, 0, 400, 300}), gradient(FillStyle::Kind::Linear, SpreadMode::Pad, 0, Rect(0, 0, 400, 300)))};
    // A symbol: two overlapping discs.
    auto inst = convertToSymbol(d, {shape(Region::circle({0, 0}, 60), FillStyle::solid(Color(250, 200, 40, 220))),
                                    shape(Region::circle({40, 20}, 45), FillStyle::solid(Color(40, 120, 250)))},
                                "Discs", SymbolType::MovieClip, {0, 0});
    Layer top = d.makeLayer("Top");
    auto a = inst->cloneAs<InstanceElement>();
    a->matrix = Affine::translate(120, 120) * Affine::rotate(0.3) * Affine::scale(1.2, 0.9);
    a->blend = mode;
    auto b = inst->cloneAs<InstanceElement>();
    b->matrix = Affine::translate(290, 170);
    b->color.kind = ColorEffect::Kind::Tint;
    b->color.tint = Color(255, 0, 120);
    b->color.tintAmount = 0.5;
    auto c = inst->cloneAs<InstanceElement>();
    c->matrix = Affine::translate(200, 240) * Affine::scale(0.6);
    c->color.kind = ColorEffect::Kind::Alpha;
    c->color.alpha = 0.5;
    top.keys[0].elements = {a, b, c};
    top.opacity = mode == BlendMode::Normal ? 0.8 : 1.0;
    tl.layers.insert(tl.layers.begin(), top);
    return d;
}

} // namespace

VX_TEST(gpu_fills_gradients_and_strokes)
{
    if (!haveGpu()) return;
    const Document d = shapesScene();
    CHECK(sameOnGpu(d, d.scenes[0], 0, {400, 300}, Affine{}, "shapes"));
    CHECK(sameOnGpu(d, d.scenes[0], 0, {800, 600}, Affine::scale(2.0), "shapes x2"));
    CHECK(sameOnGpu(d, d.scenes[0], 0, {300, 240}, Affine::translate(150, -20) * Affine::rotate(0.7) * Affine::scale(0.6), "shapes rotated"));
    RenderOptions outline;
    outline.forceOutline = true;
    outline.outlineColor = QColor(20, 160, 90);
    // Hairlines: multisampled 1 px strokes are a little crisper than QPainter's.
    CHECK(sameOnGpu(d, d.scenes[0], 0, {400, 300}, Affine{}, "outlines", outline, 0.05));
}

VX_TEST(gpu_blend_modes_and_colour_effects)
{
    if (!haveGpu()) return;
    for (int m = 0; m < int(BlendMode::Count); ++m) {
        const BlendMode mode = BlendMode(m);
        if (mode == BlendMode::Alpha || mode == BlendMode::Erase) continue; // need a Layer parent, below
        const Document d = compositingScene(mode);
        CHECK(sameOnGpu(d, d.scenes[0], 0, {400, 300}, Affine{}, qPrintable(QString("blend %1").arg(m))));
    }
    // Alpha and Erase inside a layer-blended parent.
    for (BlendMode mode : {BlendMode::Alpha, BlendMode::Erase}) {
        Document d = compositingScene(mode);
        auto holder = convertToSymbol(d, d.scenes[0].layers[0].keys[0].elements, "Holder", SymbolType::MovieClip, {0, 0});
        holder->blend = BlendMode::Layer;
        d.scenes[0].layers[0].keys[0].elements = {holder};
        CHECK(sameOnGpu(d, d.scenes[0], 0, {400, 300}, Affine{}, mode == BlendMode::Alpha ? "blend alpha" : "blend erase"));
    }
}

VX_TEST(gpu_layer_cache_draws_over_cached_layers)
{
    if (!haveGpu() || !GlRenderer::instance()) return;
    // The top layer multiplies with the (cached) layers below it: it is drawn
    // on the GPU over their pixels.
    const Document d = compositingScene(BlendMode::Multiply);
    const QSize size(400, 300);
    QImage cpu(size, QImage::Format_ARGB32_Premultiplied);
    cpu.fill(0);
    Renderer(d).render(cpu, d.scenes[0], 0, Affine{});
    LayerCache cache;
    for (int i = 0; i < 7; ++i) {
        QImage img(size, QImage::Format_ARGB32_Premultiplied);
        img.fill(0);
        cache.render(img, d, d.scenes[0], 0, Affine{}, {}, "0", true);
        const Diff diff = compare(cpu, img);
        CHECK(diff.mean < 1.5 && diff.bad < 0.004);
    }
    CHECK(cache.stats().drawn == 1);
    // Drawing one layer over pixels already in the target.
    RenderOptions below, top;
    below.onlyLayers = {0, 1};
    top.onlyLayers = {1, 0};
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    Renderer(d, below).render(img, d.scenes[0], 0, Affine{});
    CHECK(GlRenderer::instance()->render(img, d, d.scenes[0], 0, Affine{}, {}, top, false));
    const Diff diff = compare(cpu, img);
    std::printf("  %-28s mean %.3f  edge-ish %.3f%%\n", "drawn over pixels", diff.mean, diff.bad * 100);
    CHECK(diff.mean < 1.5 && diff.bad < 0.004);
}

VX_TEST(gpu_masks_filters_and_nine_slice)
{
    if (!haveGpu()) return;
    // Mask layer over the compositing scene.
    Document d = compositingScene(BlendMode::Normal);
    Layer mask = d.makeLayer("Mask");
    mask.type = LayerType::Mask;
    mask.locked = true;
    mask.keys[0].elements = {shape(booleanOp(Region::circle({200, 150}, 120), Region::rect({150, 100, 250, 200}), BoolOp::Subtract),
                                   FillStyle::solid(Color(0, 0, 0)))};
    for (Layer& l : d.scenes[0].layers) l.parentId = mask.id;
    d.scenes[0].layers.insert(d.scenes[0].layers.begin(), mask);
    CHECK(sameOnGpu(d, d.scenes[0], 0, {400, 300}, Affine{}, "mask"));

    // Filters (glow, drop shadow, blur) and a colour-adjusted clip.
    Document f = compositingScene(BlendMode::Normal);
    auto& top = f.scenes[0].layers[0].keys[0].elements;
    auto a = top[0]->cloneAs<InstanceElement>();
    Filter glow = Filter::defaults(FilterType::Glow);
    glow.blurX = glow.blurY = 12;
    Filter shadow = Filter::defaults(FilterType::DropShadow);
    shadow.distance = 10;
    a->filters = {glow, shadow};
    auto b = top[1]->cloneAs<InstanceElement>();
    Filter adjust = Filter::defaults(FilterType::AdjustColor);
    adjust.hue = 90;
    adjust.saturation = -40;
    b->filters = {adjust, Filter::defaults(FilterType::Blur)};
    top[0] = a;
    top[1] = b;
    CHECK(sameOnGpu(f, f.scenes[0], 0, {400, 300}, Affine{}, "filters"));

    // 9-slice panel stretched wide.
    Document p = Document::createDefault();
    ShapeGraph panel = graphFromRegion(Region::roundedRect({0, 0, 60, 40}, 12), FillStyle::solid(Color(30, 30, 40)));
    panel = overlay(panel, graphFromRegion(Region::roundedRect({4, 4, 56, 36}, 9), gradient(FillStyle::Kind::Radial, SpreadMode::Pad, 0.3, Rect(0, 0, 60, 40))));
    auto pi = convertToSymbol(p, {makeShapeElement(panel, false)}, "Panel", SymbolType::MovieClip, {0, 0});
    p.symbols.back().scale9 = Rect(14, 12, 46, 28);
    pi->matrix = Affine::translate(20, 20) * Affine::scale(5.5, 2.5);
    p.scenes[0].layers[0].keys[0].elements = {pi};
    CHECK(sameOnGpu(p, p.scenes[0], 0, {400, 300}, Affine{}, "nine-slice"));
}

VX_TEST(gpu_real_fla_samples)
{
    // Real documents (set VERTEXA_FLA_SAMPLES to a folder of .fla files).
    const QString dir = qEnvironmentVariable("VERTEXA_FLA_SAMPLES");
    if (dir.isEmpty() || !haveGpu()) return;
    for (const QFileInfo& fi : QDir(dir).entryInfoList({"*.fla"}, QDir::Files)) {
        Document d;
        if (!io::importFla(fi.absoluteFilePath(), d)) continue;
        for (int frame : {0, 20}) {
            const double k = 0.8;
            CHECK(sameOnGpu(d, d.scenes[0], frame, QSize(int(d.width * k), int(d.height * k)), Affine::scale(k),
                            qPrintable(QString("%1 @%2").arg(fi.completeBaseName().left(20)).arg(frame))));
        }
    }
}

int main(int argc, char** argv)
{
    // Without a display there is no OpenGL: use the offscreen platform and skip.
    if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") &&
        qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    // Software OpenGL (Mesa's llvmpipe in CI) is fine for checking pixels.
    qputenv("VERTEXA_GPU", "force");
    QGuiApplication app(argc, argv);
    const int result = vxtest::runAll(argc, argv);
    backends().clear(); // GPU devices go before the application
    return result;
}
