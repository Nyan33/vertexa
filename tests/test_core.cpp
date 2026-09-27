// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestMain.h"

#include "core/DocumentOps.h"
#include "core/Evaluate.h"
#include "core/Serialize.h"
#include "core/ShapeTween.h"
#include "core/TimelineOps.h"
#include "core/Tween.h"

using namespace vx;

namespace {

ElementPtr redSquare(double x, double y, double s = 10)
{
    return makeShapeElement(graphFromRegion(Region::rect({x, y, x + s, y + s}), FillStyle::solid(Color(255, 0, 0))), false);
}

Document docWithSymbol(std::string& symId)
{
    Document d = Document::createDefault();
    auto inst = convertToSymbol(d, {redSquare(0, 0)}, "Box", SymbolType::Graphic, {5, 5});
    symId = inst->symbolId;
    d.scenes[0].layers[0].keys[0].elements = {inst};
    return d;
}

} // namespace

VX_TEST(easing_basics)
{
    Ease e;
    CHECK_NEAR(e.apply(0.3), 0.3, 1e-12);
    e.kind = EaseKind::Classic;
    e.strength = 100;
    CHECK_NEAR(e.apply(0.5), 0.75, 1e-12); // ease out
    e.strength = -100;
    CHECK_NEAR(e.apply(0.5), 0.25, 1e-12); // ease in
    for (int k = 0; k < int(EaseKind::Custom); ++k) {
        Ease x;
        x.kind = EaseKind(k);
        x.strength = 50;
        CHECK_NEAR(x.apply(0.0), 0.0, 1e-9);
        CHECK_NEAR(x.apply(1.0), 1.0, 1e-9);
    }
    Ease c;
    c.kind = EaseKind::Custom;
    c.curve = {Cubic{{0, 0}, {0.5, 0}, {0.5, 1}, {1, 1}}};
    CHECK_NEAR(c.apply(0.5), 0.5, 1e-9);
}

VX_TEST(frame_commands)
{
    Document d = Document::createDefault();
    Timeline& tl = d.scenes[0];
    Layer& l = tl.layers[0];
    l.keys[0].elements = {redSquare(0, 0)};
    insertFrames(l, 9); // F5 at frame 10 extends to 10 frames
    CHECK(l.length() == 10);
    CHECK(insertKeyframe(d, tl, 0, 5, false) == 5);
    CHECK(tl.layers[0].keys.size() == 2);
    CHECK(tl.layers[0].keys[1].elements.size() == 1);
    CHECK(insertKeyframe(d, tl, 0, 7, true) == 7);
    CHECK(tl.layers[0].keys[2].elements.empty());
    // F6 on an existing keyframe converts the next frame.
    CHECK(insertKeyframe(d, tl, 0, 5, false) == 6);
    CHECK(clearKeyframe(tl.layers[0], 6));
    removeFrames(tl.layers[0], 0, 2);
    CHECK(tl.layers[0].length() == 8);
    CHECK(tl.layers[0].keys[1].start == 3);
    // Past the end: F7 creates frames up to the new keyframe.
    CHECK(insertKeyframe(d, tl, 0, 20, true) == 20);
    CHECK(tl.layers[0].length() == 21);
    // Copy / paste / reverse keep the layer contiguous.
    auto frames = copyFrames(tl.layers[0], 0, 4);
    pasteFrames(tl.layers[0], 21, frames, false);
    CHECK(tl.layers[0].length() == 26);
    reverseFrames(tl.layers[0], 0, 25);
    int pos = 0;
    for (const Keyframe& k : tl.layers[0].keys) {
        CHECK(k.start == pos);
        pos = k.end();
    }
    moveFrames(tl.layers[0], 0, 0, 3);
    CHECK(tl.layers[0].length() >= 26);
}

VX_TEST(classic_tween_interpolates)
{
    std::string sym;
    Document d = docWithSymbol(sym);
    Timeline& tl = d.scenes[0];
    insertFrames(tl.layers[0], 9);
    CHECK(insertKeyframe(d, tl, 0, 10, false) == 10);
    auto moved = tl.layers[0].keys[1].elements[0]->withMatrix(Affine::translate(105, 5) * Affine::rotate(kPi / 2));
    tl.layers[0].keys[1].elements[0] = moved;
    CHECK(createClassicTween(d, tl, 0, 0));
    const auto items = evaluateLayer(d, tl, 0, 5);
    CHECK(items.size() == 1);
    const Affine& m = items[0].element->matrix;
    const Vec2 pivot = m.map(items[0].element->pivot);
    CHECK_NEAR(pivot.x, 55.0, 1e-9); // pivot moves linearly (half way)
    CHECK_NEAR(AffineParts::decompose(m).rotation(), kPi / 4, 1e-9);
    // Clockwise with one extra turn.
    tl.layers[0].keys[0].classic.rotate = RotateMode::Clockwise;
    tl.layers[0].keys[0].classic.rotations = 1;
    const auto m2 = evaluateLayer(d, tl, 0, 5)[0].element->matrix;
    const double rot = AffineParts::decompose(m2).rotation();
    CHECK_NEAR(std::remainder(rot - (kPi / 4 + kPi), 2 * kPi), 0.0, 1e-9);
}

VX_TEST(motion_guide)
{
    std::string sym;
    Document d = docWithSymbol(sym);
    Timeline& tl = d.scenes[0];
    // Guide layer with an arc above the moving layer.
    Layer guide = d.makeLayer("Guide");
    guide.type = LayerType::Guide;
    const Cubic arc{{5, 5}, {5, -100}, {105, -100}, {105, 5}};
    StrokeStyle st;
    guide.keys[0].elements = {makeShapeElement(graphFromPaths({{arc}}, st), false)};
    guide.keys[0].duration = 11;
    tl.layers[0].parentId = guide.id;
    tl.layers.insert(tl.layers.begin(), guide);
    insertFrames(tl.layers[1], 9);
    insertKeyframe(d, tl, 1, 10, false);
    tl.layers[1].keys[1].elements[0] = tl.layers[1].keys[1].elements[0]->withMatrix(Affine::translate(105, 5));
    createClassicTween(d, tl, 1, 0);
    const auto items = evaluateLayer(d, tl, 1, 5);
    const Vec2 p = items[0].element->matrix.map(items[0].element->pivot);
    CHECK(p.y < -50.0); // follows the arc instead of the straight line
}

VX_TEST(shape_tween_morphs)
{
    Document d = Document::createDefault();
    Timeline& tl = d.scenes[0];
    tl.layers[0].keys[0].elements = {redSquare(0, 0, 10)};
    insertFrames(tl.layers[0], 9);
    insertKeyframe(d, tl, 0, 10, true);
    tl.layers[0].keys[1].elements = {makeShapeElement(
        graphFromRegion(Region::circle({50, 5}, 10), FillStyle::solid(Color(0, 0, 255))), false)};
    CHECK(createShapeTween(d, tl, 0, 0));
    const auto items = evaluateLayer(d, tl, 0, 5);
    CHECK(items.size() == 1);
    CHECK(items[0].element->type() == ElementType::Morph);
    const auto& rd = *static_cast<const MorphElement&>(*items[0].element).data;
    CHECK(rd.fills.size() == 1);
    const Color c = rd.fills[0].style.color;
    CHECK(c.r > 100 && c.b > 100);
    const Rect b = rd.bounds;
    CHECK(b.center().x > 20 && b.center().x < 40);
    // Inserting a keyframe mid-tween bakes an editable shape.
    CHECK(insertKeyframe(d, tl, 0, 5, false) == 5);
    const ShapeGraph baked = keyframeMergeShape(tl.layers[0].keys[1]);
    CHECK(!baked.isEmpty());
}

VX_TEST(symbols_and_break_apart)
{
    std::string sym;
    Document d = docWithSymbol(sym);
    CHECK(d.useCount(sym) == 1);
    const ElementPtr inst = d.scenes[0].layers[0].keys[0].elements[0];
    const Rect b = elementBounds(d, *inst);
    CHECK_NEAR(b.x0, 0.0, 1e-9);
    CHECK_NEAR(b.x1, 10.0, 1e-9);
    // Registration point at (5,5): symbol content is centred on the origin.
    const Symbol* s = d.symbol(sym);
    CHECK(s != nullptr);
    CHECK_NEAR(timelineBounds(d, s->timeline, 0).x0, -5.0, 1e-9);
    const auto parts = breakApart(d, inst->withMatrix(Affine::translate(100, 0)));
    CHECK(parts.size() == 1);
    CHECK_NEAR(elementBounds(d, *parts[0]).x0, 95.0, 1e-9);
    CHECK(d.symbolContains(sym, sym));
    const std::string dup = duplicateSymbol(d, sym, "");
    CHECK(d.symbol(dup) != nullptr);
    deleteSymbol(d, sym);
    CHECK(d.scenes[0].layers[0].keys[0].elements.empty());
}

VX_TEST(graphic_symbol_loop_modes)
{
    Document d = Document::createDefault();
    auto inst = convertToSymbol(d, {redSquare(0, 0)}, "Anim", SymbolType::Graphic, {0, 0});
    Symbol* s = d.symbol(inst->symbolId);
    insertFrames(s->timeline.layers[0], 9); // 10 frames
    InstanceElement i = *inst;
    i.loop = LoopMode::Loop;
    CHECK(instanceSymbolFrame(d, i, 13) == 3);
    i.loop = LoopMode::PlayOnce;
    CHECK(instanceSymbolFrame(d, i, 13) == 9);
    i.loop = LoopMode::SingleFrame;
    i.firstFrame = 4;
    CHECK(instanceSymbolFrame(d, i, 13) == 4);
    i.loop = LoopMode::LoopReverse;
    i.firstFrame = 0;
    CHECK(instanceSymbolFrame(d, i, 1) == 8);
}

VX_TEST(serialization_roundtrip)
{
    std::string sym;
    Document d = docWithSymbol(sym);
    Timeline& tl = d.scenes[0];
    Layer extra = d.makeLayer("Paint");
    d.brushes.push_back(builtinVectorBrushes()[2]);
    d.brushes.back().id = "doc.42";
    extra.keys[0].elements = {makeShapeElement(graphFromRegion(Region::circle({50, 50}, 10), FillStyle::solid(Color(10, 20, 30, 200))), true),
                              redSquare(1.0 / 3.0, 2.0 / 7.0)};
    extra.blend = BlendMode::Multiply;
    tl.layers.push_back(extra);
    insertFrames(tl.layers[0], 4);
    insertKeyframe(d, tl, 0, 5, false);
    createClassicTween(d, tl, 0, 0);
    tl.layers[0].keys[0].classic.ease.kind = EaseKind::BounceOut;

    const QByteArray data = serializeDocument(d);
    Document back;
    QString err;
    CHECK(deserializeDocument(data, back, &err));
    CHECK(serializeDocument(back) == data); // lossless
    CHECK(back.symbols.size() == d.symbols.size());
    CHECK(back.scenes[0].layers[1].blend == BlendMode::Multiply);
    CHECK(back.brushes.size() == 1 && back.brushes[0].id == "doc.42" && back.brushes[0].name == builtinVectorBrushes()[2].name);
    const ShapeElement* sq = asShape(back.scenes[0].layers[1].keys[0].elements[1]);
    CHECK(sq && sq->graph->edges[0].c.p0.x == 1.0 / 3.0);

    // Clipboard carries referenced symbols into another document.
    Document other = Document::createDefault();
    const auto pasted = deserializeClipboard(serializeClipboard(d, {tl.layers[0].keys[0].elements[0]}), other);
    CHECK(pasted.size() == 1);
    CHECK(other.symbols.size() >= 1);
}

VX_TEST(layer_drag_keeps_children)
{
    Timeline tl;
    auto layer = [&](uint32_t id, LayerType type, uint32_t parent) {
        Layer l;
        l.id = id;
        l.name = "L" + std::to_string(id);
        l.type = type;
        l.parentId = parent;
        l.normalize();
        tl.layers.push_back(l);
    };
    layer(1, LayerType::Normal, 0);
    layer(2, LayerType::Folder, 0);
    layer(3, LayerType::Normal, 2);
    layer(4, LayerType::Mask, 2);
    layer(5, LayerType::Normal, 4);
    layer(6, LayerType::Normal, 0);
    auto order = [&] {
        std::string s;
        for (const Layer& l : tl.layers) s += std::to_string(l.id);
        return s;
    };
    CHECK(layerBlockSize(tl, 1) == 3);
    CHECK(layerBlockSize(tl, 3) == 1);
    CHECK(moveLayer(tl, 1, 2) == -1); // onto itself

    // The folder travels with everything inside it.
    CHECK(moveLayer(tl, 1, 6) == 2);
    CHECK(order() == "162345");
    CHECK(tl.layers[2].parentId == 0);
    CHECK(tl.maskOf(tl.layerIndex(5)) != nullptr);

    // Dropped right under the expanded folder: goes inside.
    CHECK(moveLayer(tl, 0, 3) == 2);
    CHECK(order() == "621345");
    CHECK(tl.layers[2].parentId == 2);

    // Dragged back to the top: leaves the folder.
    CHECK(moveLayer(tl, 2, 0) == 0);
    CHECK(tl.layers[0].parentId == 0);

    // Under a mask a layer becomes masked, a mask never nests into a mask.
    CHECK(moveLayer(tl, 1, 5) == 4);
    CHECK(order() == "123465");
    CHECK(tl.maskOf(4) != nullptr);
    Layer m2;
    m2.id = 7;
    m2.type = LayerType::Mask;
    m2.normalize();
    tl.layers.push_back(m2);
    CHECK(moveLayer(tl, 6, 4) == 4);
    CHECK(tl.layers[4].parentId == 2);

    // A collapsed folder hides its children: dropping below it stays outside.
    tl.layers[tl.layerIndex(2)].expanded = false;
    const int top = tl.layerIndex(1);
    CHECK(moveLayer(tl, top, int(tl.layers.size())) == int(tl.layers.size()) - 1);
    CHECK(tl.layers.back().parentId == 0);
}

VX_TEST_MAIN()
