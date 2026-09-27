// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestMain.h"

#include "core/DocumentOps.h"
#include "render/Blend.h"
#include "render/BrushResources.h"
#include "render/DabEngine.h"
#include "render/Raster.h"
#include "render/Renderer.h"

#include <QGuiApplication>

using namespace vx;

namespace {

QImage blank(int w, int h, QRgb c = 0)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(c);
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

VX_TEST(texture_brush_paints)
{
    for (const BrushPreset& p : builtinBrushPresets()) {
        const QImage prev = brushPreview(p, Color(0, 0, 0), 160, 60);
        int painted = 0;
        for (int y = 0; y < prev.height(); ++y)
            for (int x = 0; x < prev.width(); ++x) painted += qAlpha(prev.pixel(x, y)) > 0;
        CHECK(painted > 50);
    }
    CHECK(BrushResources::image("builtin:paper") != nullptr);
    CHECK(BrushResources::image("builtin:chalk") != nullptr);
    // GBR v2 round trip: 2x1 brush.
    QByteArray gbr;
    auto be = [&](quint32 v) { for (int s = 24; s >= 0; s -= 8) gbr.append(char((v >> s) & 0xff)); };
    be(28 + 5); be(2); be(2); be(1); be(1); gbr.append("GIMP"); be(25); gbr.append("test", 4); gbr.append('\0');
    gbr.append(char(0)); gbr.append(char(255));
    QString name;
    const GrayImagePtr tip = BrushResources::loadGbr(gbr, &name);
    CHECK(tip && tip->width == 2 && tip->pixels[1] == 255 && name == "test");
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

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    return vxtest::runAll(argc, argv);
}
