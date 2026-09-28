// SPDX-License-Identifier: GPL-3.0-or-later
// UI level tests: drive the real StageView with synthesized mouse input and
// check the resulting documents.
#include "TestMain.h"

#include "app/DemoDocument.h"
#include "app/Editor.h"
#include "app/MainWindow.h"
#include "app/StageView.h"
#include "app/Theme.h"
#include "app/panels/PropertiesPanel.h"
#include "app/panels/ToolsPanel.h"
#include "core/DocumentOps.h"
#include "core/Evaluate.h"
#include "core/VectorBrush.h"
#include "render/QtConvert.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QAction>
#include <QContextMenuEvent>
#include <QUndoStack>

using namespace vx;
using namespace vx::app;

namespace {

struct Fixture {
    Editor ed;
    StageView view{&ed};

    Fixture()
    {
        view.resize(1000, 700);
        view.show();
        QApplication::processEvents();
        view.fitStage();
    }

    QPointF w(Vec2 p) const { return toQPoint(view.timelineToWidget().map(p)); }

    void send(QEvent::Type type, Vec2 p, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = {})
    {
        const QPointF pos = w(p);
        QMouseEvent ev(type, pos, view.mapToGlobal(pos), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, mods);
        QApplication::sendEvent(&view, &ev);
    }

    void drag(ToolId tool, const std::vector<Vec2>& pts, Qt::KeyboardModifiers mods = {})
    {
        ed.setTool(tool);
        send(QEvent::MouseButtonPress, pts.front(), Qt::LeftButton, mods);
        for (size_t i = 1; i < pts.size(); ++i) send(QEvent::MouseMove, pts[i], Qt::LeftButton, mods);
        send(QEvent::MouseButtonRelease, pts.back(), Qt::NoButton, mods);
        // Paint brush strokes are merged in the background.
        while (view.hasPendingWork()) QApplication::processEvents(QEventLoop::AllEvents, 5);
    }

    void line(ToolId tool, Vec2 a, Vec2 b, int steps = 30, Qt::KeyboardModifiers mods = {})
    {
        std::vector<Vec2> pts;
        for (int i = 0; i <= steps; ++i) pts.push_back(lerp(a, b, double(i) / steps));
        drag(tool, pts, mods);
    }

    void click(ToolId tool, Vec2 p, Qt::KeyboardModifiers mods = {}) { drag(tool, {p}, mods); }

    double fillArea() const
    {
        ShapeGraphPtr g = ed.mergeShape(ed.layerIndex());
        return g ? g->fillRegion(0).area() : 0.0;
    }
};

} // namespace

VX_TEST(brush_and_eraser)
{
    Fixture f;
    f.ed.settings().brushSmoothing = 0;
    f.ed.settings().brushPressure = false;
    f.line(ToolId::Brush, {300, 360}, {900, 360});
    const double a = f.fillArea();
    CHECK(a > 1000.0);
    // Erasing across the stroke splits it into two pieces.
    f.ed.settings().eraserSize = 30;
    f.line(ToolId::Eraser, {600, 250}, {600, 470});
    const double b = f.fillArea();
    CHECK(b < a);
    ShapeGraphPtr g = f.ed.mergeShape(f.ed.layerIndex());
    CHECK(g && g->fillRegion(0).contours.size() == 2);
    // Undo brings the stroke back.
    f.ed.undoStack()->undo();
    CHECK_NEAR(f.fillArea(), a, 1e-6);
}

VX_TEST(rectangle_bucket_and_ink)
{
    Fixture f;
    f.drag(ToolId::Rectangle, {{400, 200}, {600, 300}, {800, 500}});
    ShapeGraphPtr g = f.ed.mergeShape(f.ed.layerIndex());
    CHECK(g != nullptr);
    CHECK_NEAR(f.fillArea(), 400.0 * 300.0, 1e-6);
    CHECK(!g->strokes.empty());
    f.ed.settings().fill = FillStyle::solid(Color(255, 0, 0));
    f.click(ToolId::PaintBucket, {600, 350});
    g = f.ed.mergeShape(f.ed.layerIndex());
    CHECK(g->fills.size() == 1 && g->fills[0].color == Color(255, 0, 0));
    StrokeStyle s;
    s.width = 9;
    f.ed.settings().stroke = s;
    f.click(ToolId::InkBottle, {600, 350});
    g = f.ed.mergeShape(f.ed.layerIndex());
    bool thick = false;
    for (const StrokeStyle& st : g->strokes) thick |= st.width == 9;
    CHECK(thick);
}

VX_TEST(selection_move_bend_and_marquee)
{
    Fixture f;
    f.ed.settings().strokeEnabled = false;
    f.drag(ToolId::Rectangle, {{400, 200}, {600, 400}});
    // Click the fill: selected; drag it away.
    f.click(ToolId::Selection, {500, 300});
    CHECK(f.ed.shapePick().valid());
    f.drag(ToolId::Selection, {{500, 300}, {550, 300}, {600, 300}});
    Rect b = f.ed.mergeShape(f.ed.layerIndex())->bounds(false);
    CHECK_NEAR(b.x0, 500.0, 1e-6);
    // Bend the (unselected) top edge upwards.
    f.ed.clearSelection();
    f.drag(ToolId::Selection, {{600, 200}, {600, 170}, {600, 140}});
    CHECK(f.fillArea() > 200.0 * 200.0 + 1000.0);
    // Marquee over the right half cuts the shape; delete removes it.
    f.ed.clearSelection();
    f.drag(ToolId::Selection, {{650, 100}, {750, 300}, {900, 500}});
    CHECK(f.ed.shapePick().valid());
    const double before = f.fillArea();
    f.ed.deleteSelection();
    CHECK(f.fillArea() < before);
}

VX_TEST(pencil_and_vector_paint_brush)
{
    Fixture f;
    f.ed.settings().pencilMode = PencilMode::Smooth;
    std::vector<Vec2> pts;
    for (int i = 0; i <= 40; ++i) pts.push_back({300.0 + i * 12, 300.0 + std::sin(i * 0.3) * 40});
    f.drag(ToolId::Pencil, pts);
    ShapeGraphPtr g = f.ed.mergeShape(f.ed.layerIndex());
    CHECK(g && !g->strokes.empty() && g->fills.empty());
    // The paint brush adds vector fills in the stroke colour to the merge shape.
    f.ed.settings().stroke.paint = FillStyle::solid(Color(200, 40, 20));
    f.ed.settings().paint = *builtinVectorBrush("chalk");
    f.ed.settings().paint.size = 30;
    std::vector<Vec2> low;
    for (const Vec2& p : pts) low.push_back(p + Vec2{0, 150});
    f.drag(ToolId::PaintBrush, low);
    g = f.ed.mergeShape(f.ed.layerIndex());
    CHECK(g && g->fills.size() == 1 && g->fills[0].color == Color(200, 40, 20));
    const double painted = f.fillArea();
    CHECK(painted > 480 * 30 * 0.4);
    CHECK(f.ed.currentLayer()->keyAt(0)->elements.size() == 1); // one merge shape
    // An art brush along the same path; Undo removes it again.
    f.ed.settings().paint = *builtinVectorBrush("ink-taper");
    f.ed.settings().paint.size = 20;
    std::vector<Vec2> high;
    for (const Vec2& p : pts) high.push_back(p - Vec2{0, 150});
    f.drag(ToolId::PaintBrush, high);
    CHECK(f.fillArea() > painted + 1000);
    f.ed.undoStack()->undo();
    CHECK(std::abs(f.fillArea() - painted) < 1.0);
    // Erasing with the brush removes paint along its (textured) path.
    f.ed.settings().paint = *builtinVectorBrush("charcoal");
    f.ed.settings().paint.size = 60;
    f.ed.settings().paintErase = true;
    f.drag(ToolId::PaintBrush, {{600, 350}, {600, 400}, {600, 500}});
    CHECK(f.fillArea() < painted - 500);
    f.ed.settings().paintErase = false;
}

VX_TEST(symbols_tweens_and_edit_in_place)
{
    Fixture f;
    f.drag(ToolId::Oval, {{500, 300}, {600, 400}});
    f.ed.selectAll();
    f.ed.convertSelectionToSymbol("Ball", SymbolType::Graphic, 4);
    CHECK(f.ed.doc().symbols.size() == 1);
    CHECK(f.ed.selectedElements().size() == 1);
    // Keyframe at frame 10 and move the instance there.
    f.ed.setFrame(9);
    f.ed.insertKeyframe(false);
    f.ed.selectAll();
    f.ed.nudge(200, 0);
    f.ed.setFrame(0);
    f.ed.createTween(TweenType::Classic);
    const auto items = evaluateLayer(f.ed.doc(), f.ed.timeline(), 0, 5);
    CHECK(items.size() == 1);
    const Vec2 p = items[0].element->matrix.map(items[0].element->pivot);
    CHECK(p.x > 560 && p.x < 760);
    // Double-click edits the symbol in place and drawing goes into it.
    f.ed.setTool(ToolId::Selection);
    f.send(QEvent::MouseButtonDblClick, {550, 350}, Qt::LeftButton);
    CHECK(f.ed.inSymbol());
    f.ed.settings().brushSmoothing = 0;
    f.line(ToolId::Brush, {-40, 0}, {40, 0});
    const Symbol* s = f.ed.doc().symbol(f.ed.contextStack().back().symbolId);
    CHECK(s && keyframeMergeShape(s->timeline.layers[0].keys[0]).fills.size() >= 1);
    f.ed.exitContext();
    CHECK(!f.ed.inSymbol());
}

VX_TEST(tweened_frames_select_and_transform_what_is_shown)
{
    Fixture f;
    f.drag(ToolId::Oval, {{100, 300}, {160, 360}});
    f.ed.selectAll();
    f.ed.convertSelectionToSymbol("Dot", SymbolType::MovieClip, 4);
    f.ed.setFrame(10);
    f.ed.insertKeyframe(false);
    f.ed.selectAll();
    f.ed.nudge(400, 0);
    f.ed.setFrame(0);
    f.ed.createTween(TweenType::Classic);
    // Half way the selection box sits on the tweened instance.
    f.ed.setFrame(5);
    f.ed.setTool(ToolId::Selection);
    f.click(ToolId::Selection, {330, 330});
    CHECK(f.ed.selection().size() == 1);
    const Rect b = f.ed.selectionBounds();
    CHECK(std::abs(b.center().x - 330) < 2 && std::abs(b.center().y - 330) < 2);
    // Moving it there keys the frame with the tweened state first.
    const int keys = int(f.ed.currentLayer()->keys.size());
    f.ed.nudge(0, 50);
    CHECK(int(f.ed.currentLayer()->keys.size()) == keys + 1);
    CHECK(f.ed.currentLayer()->keys[1].start == 5 && f.ed.currentLayer()->keys[1].tween == TweenType::Classic);
    const Rect moved = f.ed.selectionBounds();
    CHECK(std::abs(moved.center().x - 330) < 2 && std::abs(moved.center().y - 380) < 2);
    // Both halves still tween; the ends did not move.
    auto centreAt = [&](int frame) {
        const auto items = evaluateLayer(f.ed.doc(), f.ed.timeline(), f.ed.layerIndex(), frame);
        return items.empty() ? Vec2{} : elementBounds(f.ed.doc(), *items[0].element).center();
    };
    CHECK(std::abs(centreAt(0).x - 130) < 2 && std::abs(centreAt(0).y - 330) < 2);
    CHECK(std::abs(centreAt(10).x - 530) < 2 && std::abs(centreAt(10).y - 330) < 2);
    CHECK(centreAt(3).y > 335 && centreAt(8).y > 335);
}

VX_TEST(buttons_folders_and_nine_slice_guides)
{
    Fixture f;
    f.drag(ToolId::Rectangle, {{500, 300}, {600, 360}});
    f.ed.selectAll();
    f.ed.convertSelectionToSymbol("Play", SymbolType::Button, 4, "UI/Buttons");
    const std::string id = f.ed.doc().symbols.back().id;
    CHECK(f.ed.doc().symbol(id)->folder == "UI/Buttons");
    f.ed.clearSelection();
    // With simple buttons enabled a click presses the button instead of
    // selecting it.
    f.ed.setSimpleButtons(true);
    f.click(ToolId::Selection, {550, 330});
    CHECK(!f.ed.hasSelection());
    f.ed.setSimpleButtons(false);
    f.click(ToolId::Selection, {550, 330});
    CHECK(f.ed.selection().size() == 1);
    // Library folders.
    f.ed.renameLibraryFolder("UI", "Interface");
    CHECK(f.ed.doc().symbol(id)->folder == "Interface/Buttons");
    f.ed.createLibraryFolder("Interface/Empty");
    f.ed.deleteLibraryFolder("Interface");
    CHECK(f.ed.doc().symbol(id)->folder == "Buttons");
    CHECK(f.ed.doc().libraryFolders == std::vector<std::string>{"Empty"});
    f.ed.moveSymbolToFolder(id, "");
    CHECK(f.ed.doc().symbol(id)->folder.empty());
    f.ed.undoStack()->undo();
    CHECK(f.ed.doc().symbol(id)->folder == "Buttons");
    // 9-slice guides are dragged while editing the symbol.
    f.ed.setSymbolScale9(id, f.ed.defaultScale9(id));
    const Rect g = *f.ed.doc().symbol(id)->scale9;
    f.ed.enterSymbol(id);
    QApplication::processEvents();
    const double mid = (g.y0 + g.y1) / 2;
    f.drag(ToolId::Selection, {{g.x0, mid}, {g.x0 - 5, mid}, {g.x0 - 10, mid}});
    const Rect moved = *f.ed.doc().symbol(id)->scale9;
    CHECK(std::abs(moved.x0 - (g.x0 - 10)) < 1.0 && moved.x1 == g.x1 && moved.y0 == g.y0);
}

VX_TEST(free_transform_keeps_the_result)
{
    Fixture f;
    f.drag(ToolId::Rectangle, {{400, 300}, {500, 380}});
    f.ed.selectAll();
    f.ed.convertSelectionToSymbol("Box", SymbolType::MovieClip, 4);
    f.ed.clearSelection();
    // Drag the body: moves.
    f.drag(ToolId::FreeTransform, {{450, 340}, {480, 360}, {550, 400}});
    Rect b = f.ed.selectionBounds();
    CHECK(std::abs(b.center().x - 550) < 2 && std::abs(b.center().y - 400) < 2);
    CHECK(!f.ed.hasPreview());
    // Drag the right edge handle: scales the width, the left edge stays.
    const double left = b.x0, right = b.x1, mid = b.center().y;
    f.drag(ToolId::FreeTransform, {{right, mid}, {right + 30, mid}, {right + 100, mid}});
    b = f.ed.selectionBounds();
    CHECK(std::abs(b.x0 - left) < 2 && std::abs(b.x1 - (right + 100)) < 3);
    // Undo restores the previous step, not the original position.
    f.ed.undoStack()->undo();
    const auto& els = f.ed.currentLayer()->keyAt(0)->elements;
    CHECK(els.size() == 1 && std::abs(elementBounds(f.ed.doc(), *els.back()).center().x - 550) < 2);
}

VX_TEST(loop_range_playback)
{
    Fixture f;
    f.ed.setFrame(9);
    f.ed.insertKeyframe(false); // ten frames
    const Document& d = f.ed.doc();
    f.ed.setStageSettings(d.width, d.height, 100, d.background);
    auto playFor = [&](int ms, std::vector<int>& frames) {
        frames.clear();
        auto c = QObject::connect(&f.ed, &Editor::frameChanged, [&](int fr) { frames.push_back(fr); });
        f.ed.setPlaying(true);
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms && f.ed.isPlaying()) QApplication::processEvents(QEventLoop::AllEvents, 5);
        f.ed.setPlaying(false);
        QObject::disconnect(c);
    };
    // Off by default: playback stops on the last frame.
    CHECK(!f.ed.loopPlayback());
    std::vector<int> frames;
    f.ed.setFrame(6);
    playFor(2000, frames);
    CHECK(f.ed.frame() == 9 && !frames.empty() && frames.back() == 9);
    // With frames 3-6 selected the loop plays exactly those, starting inside.
    FrameSelection sel;
    sel.layerFrom = sel.layerTo = 0;
    sel.frameFrom = 2;
    sel.frameTo = 5;
    f.ed.setFrameSelection(sel);
    f.ed.setLoopPlayback(true);
    CHECK(f.ed.loopStart() == 2 && f.ed.loopEnd() == 5);
    f.ed.setFrame(0);
    playFor(250, frames);
    CHECK(frames.size() > 6);
    bool inside = true, wrapped = false;
    for (size_t i = 0; i < frames.size(); ++i) {
        inside &= frames[i] >= 2 && frames[i] <= 5;
        if (i > 0 && frames[i - 1] == 5 && frames[i] == 2) wrapped = true;
    }
    CHECK(inside && wrapped);
    // The range follows edits and reaches the end when dragged there.
    f.ed.setLoopRange(7, 42);
    CHECK(f.ed.loopStart() == 7 && f.ed.loopEnd() == 9);
}

VX_TEST(main_window_smoke)
{
    Editor ed;
    MainWindow w(&ed);
    w.show();
    QApplication::processEvents();
    for (const char* a : {"insertFrame", "insertKeyframe", "insertBlankKeyframe", "newLayer", "newFolder", "createClassicTween",
                          "onionSkin", "nextFrame", "zoomIn", "fit", "selectAllFrames", "copyFrames", "pasteFrames", "reverseFrames"}) {
        QAction* act = w.action(a);
        CHECK(act != nullptr);
        if (act) act->trigger();
        QApplication::processEvents();
    }
    CHECK(ed.timeline().layers.size() >= 3);
    CHECK(ed.undoStack()->count() > 0);
}

VX_TEST(frame_changes_and_edits_redo_only_what_changed)
{
    Editor ed;
    MainWindow w(&ed);
    w.resize(1400, 900);
    w.show();
    ed.setDocument(createDemoDocument(), {});
    // Let the layout settle first (the tools panel may go to two columns,
    // which resizes the stage and starts its layer cache afresh).
    QSize stageSize;
    for (int i = 0, stable = 0; i < 100 && stable < 3; ++i) {
        QApplication::processEvents();
        stable = w.stage()->size() == stageSize ? stable + 1 : 0;
        stageSize = w.stage()->size();
    }
    auto* panel = w.findChild<PropertiesPanel*>();
    CHECK(panel != nullptr);
    if (!panel) return;
    ed.setFrame(3);
    QApplication::processEvents();
    QWidget* content = panel->widget();
    for (int f : {4, 5, 6}) {
        ed.setFrame(f);
        QApplication::processEvents();
    }
    CHECK(panel->widget() == content); // only the Frame section was rebuilt
    QApplication::processEvents();
    for (int i = 0; i < 6; ++i) {
        w.stage()->invalidate();
        w.stage()->repaint();
    }
    // Painting on another layer leaves the panel alone and redraws that layer only.
    const Timeline& tl = ed.timeline();
    int other = -1;
    for (int i = 0; i < int(tl.layers.size()) && other < 0; ++i)
        if (i != ed.layerIndex() && tl.layers[i].type == LayerType::Normal && !tl.layers[i].locked && tl.layers[i].keyAt(6)) other = i;
    CHECK(other >= 0);
    if (other < 0) return;
    ed.edit("Paint", [&](Document& d) {
        ed.mutableTimeline(d).layers[other].keyAt(6)->elements.push_back(
            makeShapeElement(graphFromRegion(Region::circle({100, 100}, 30), FillStyle::solid(Color(0, 0, 0))), true));
        return true;
    });
    QApplication::processEvents();
    w.stage()->repaint();
    CHECK(panel->widget() == content);
    CHECK(w.stage()->size() == stageSize);
    CHECK(w.stage()->renderStats().drawn < w.stage()->renderStats().layers);
    // The current layer's opacity is shown in the panel: that rebuilds it.
    ed.setLayerProperty(ed.layerIndex(), [](Layer& l) { l.opacity = 0.4; }, "Opacity");
    QApplication::processEvents();
    CHECK(panel->widget() != content);
}

VX_TEST(classic_tween_of_a_transformed_drawing)
{
    // A star drawn on frame 1, the same star moved, turned and scaled on
    // frame 20: the tween shows one symbol going from one to the other.
    Fixture f;
    f.drag(ToolId::PolyStar, {{400, 300}, {450, 350}, {500, 400}});
    f.ed.setFrame(19);
    f.ed.insertFrames();
    f.ed.insertKeyframe(false);
    CHECK(f.ed.currentLayer()->keys.size() == 2);
    f.ed.selectAll();
    f.ed.transformSelection(Affine::translate(200, 50) * Affine::about({400, 300}, Affine::rotate(0.8) * Affine::scale(1.5)), "Move");
    f.ed.clearSelection();
    const Rect end = elementBounds(f.ed.doc(), *f.ed.currentLayer()->keys[1].elements.front());
    FrameSelection s;
    s.layerFrom = s.layerTo = f.ed.layerIndex();
    s.frameFrom = 0;
    s.frameTo = 19;
    f.ed.setFrameSelection(s);
    f.ed.createTween(TweenType::Classic);
    const Layer& l = *f.ed.currentLayer();
    CHECK(l.keys[0].tween == TweenType::Classic);
    CHECK(l.keys[1].tween == TweenType::None); // the last keyframe only ends the tween
    CHECK(tweenAnimates(l, 0));
    const auto* a = asInstance(l.keys[0].elements.front());
    const auto* b = asInstance(l.keys[1].elements.front());
    CHECK(a && b && a->symbolId == b->symbolId);
    if (!a || !b) return;
    // The end keyframe looks as before.
    CHECK(elementBounds(f.ed.doc(), *l.keys[1].elements.front()).center().x > end.center().x - 1);
    auto centerAt = [&](int frame) {
        const auto items = evaluateLayer(f.ed.doc(), f.ed.timeline(), f.ed.layerIndex(), frame);
        return items.empty() ? Vec2{} : items.front().element->matrix.map(items.front().element->pivot);
    };
    const Vec2 c0 = centerAt(0), c10 = centerAt(10), c19 = centerAt(19);
    CHECK(c10.x > c0.x + 50 && c10.x < c19.x - 50);
    const double s10 = evaluateLayer(f.ed.doc(), f.ed.timeline(), f.ed.layerIndex(), 10).front().element->matrix.meanScale();
    CHECK(s10 > 1.1 && s10 < 1.45);
}

VX_TEST(classic_tween_between_different_drawings)
{
    // The second keyframe holds another drawing: two symbols, and the tween
    // still moves the first towards the second (it is swapped at the end).
    Fixture f;
    f.drag(ToolId::Rectangle, {{300, 300}, {400, 400}});
    f.ed.setFrame(9);
    f.ed.insertKeyframe(true);
    f.drag(ToolId::Oval, {{700, 300}, {800, 380}});
    f.ed.setFrame(0);
    f.ed.createTween(TweenType::Classic);
    const Layer& l = *f.ed.currentLayer();
    const auto* a = asInstance(l.keys[0].elements.front());
    const auto* b = asInstance(l.keys[1].elements.front());
    CHECK(a && b && a->symbolId != b->symbolId);
    CHECK(tweenAnimates(l, 0));
    const auto mid = evaluateLayer(f.ed.doc(), f.ed.timeline(), f.ed.layerIndex(), 5);
    CHECK(!mid.empty() && elementBounds(f.ed.doc(), *mid.front().element).center().x > 450);
}

VX_TEST(shape_tween_needs_shapes)
{
    Fixture f;
    f.drag(ToolId::Rectangle, {{300, 300}, {400, 400}});
    f.ed.setFrame(9);
    f.ed.insertKeyframe(true);
    f.drag(ToolId::Oval, {{700, 300}, {800, 380}});
    f.ed.setFrame(0);
    f.ed.createTween(TweenType::Shape);
    CHECK(tweenAnimates(*f.ed.currentLayer(), 0));
    const auto mid = evaluateLayer(f.ed.doc(), f.ed.timeline(), f.ed.layerIndex(), 5);
    CHECK(mid.size() == 1 && mid.front().element->type() == ElementType::Morph);
    // Symbols don't morph: the tween is shown as broken.
    f.ed.removeTween();
    f.ed.createTween(TweenType::Classic);
    f.ed.removeTween();
    f.ed.createTween(TweenType::Shape);
    CHECK(!tweenAnimates(*f.ed.currentLayer(), 0));
}

VX_TEST(free_transform_takes_only_what_is_clicked)
{
    Fixture f;
    f.drag(ToolId::Rectangle, {{200, 200}, {300, 300}});
    f.drag(ToolId::Rectangle, {{600, 200}, {700, 300}});
    f.ed.clearSelection();
    auto squares = [&]() {
        std::vector<Rect> out;
        for (const Contour& c : f.ed.mergeShape(f.ed.layerIndex())->fillRegion(0).contours) {
            Rect b;
            for (const Cubic& cu : c) b.include(cu.p0);
            out.push_back(b);
        }
        std::sort(out.begin(), out.end(), [](const Rect& a, const Rect& b) { return a.x0 < b.x0; });
        return out;
    };
    // A click selects the square under the pointer, not the whole layer.
    f.click(ToolId::FreeTransform, {250, 250});
    CHECK(f.ed.selection().empty() && f.ed.shapePick().valid());
    Rect b = f.ed.selectionBounds();
    CHECK(b.x1 < 320);
    // The transformation point moves (and snaps to the corner)...
    f.drag(ToolId::FreeTransform, {{250, 250}, {230, 230}, {203, 203}});
    CHECK(f.ed.selectionPivot().has_value());
    const Vec2 pivot = f.ed.selectionPivot().value_or(Vec2{});
    CHECK(distance(pivot, {b.x0, b.y0}) < 1e-6);
    // ...and the square turns about it: a quarter turn around its top left corner.
    std::vector<Vec2> path;
    const Vec2 grab{b.x1 + 8, b.y1 + 8};
    for (int i = 0; i <= 10; ++i) {
        const double a = i * (kPi / 2) / 10;
        const Vec2 d = grab - pivot;
        path.push_back(pivot + Vec2{d.x * std::cos(a) - d.y * std::sin(a), d.x * std::sin(a) + d.y * std::cos(a)});
    }
    f.drag(ToolId::FreeTransform, path);
    const auto sq = squares();
    CHECK(sq.size() == 2);
    if (sq.size() == 2) {
        CHECK(std::abs(sq[0].x1 - pivot.x) < 1.5 && std::abs(sq[0].y0 - pivot.y) < 1.5);
        CHECK(std::abs(sq[1].x0 - 600) < 1e-6 && std::abs(sq[1].x1 - 700) < 1e-6); // the other one stays
    }
    // A marquee in empty space takes both.
    f.drag(ToolId::FreeTransform, {{20, 20}, {400, 400}, {900, 500}});
    b = f.ed.selectionBounds();
    CHECK(b.x0 < 150 && b.x1 > 690);
}

VX_TEST(free_transform_scales_from_a_moved_pivot)
{
    Fixture f;
    f.drag(ToolId::Rectangle, {{300, 300}, {400, 400}});
    f.ed.selectAll();
    f.ed.convertSelectionToSymbol("Box", SymbolType::MovieClip, 4);
    const Rect b = f.ed.selectionBounds();
    const double cy = b.center().y;
    // Left in the centre, the opposite side stays put.
    f.drag(ToolId::FreeTransform, {{b.x1, cy}, {b.x1 + 20, cy}, {b.x1 + 50, cy}});
    Rect s = f.ed.selectionBounds();
    CHECK(std::abs(s.x0 - b.x0) < 1 && std::abs(s.x1 - (b.x1 + 50)) < 1.5);
    f.ed.undoStack()->undo();
    f.ed.setSelection({{f.ed.currentLayer()->id, 0}});
    // Moved, the transformation point is what the object scales from.
    const double px = b.x0 + b.width() / 4;
    f.drag(ToolId::FreeTransform, {b.center(), {b.center().x - 10, cy}, {px, cy}});
    const double k = (b.x1 + 50 - px) / (b.x1 - px);
    f.drag(ToolId::FreeTransform, {{b.x1, cy}, {b.x1 + 20, cy}, {b.x1 + 50, cy}});
    s = f.ed.selectionBounds();
    CHECK(std::abs(s.x0 - (px + (b.x0 - px) * k)) < 1.5 && std::abs(s.x1 - (b.x1 + 50)) < 1.5);
    // The point stays where it was put.
    const auto els = f.ed.selectedElements();
    CHECK(els.size() == 1 && std::abs(els.front()->matrix.map(els.front()->pivot).x - px) < 1e-6);
    // Alt: from the opposite side again.
    const Rect before = f.ed.selectionBounds();
    f.drag(ToolId::FreeTransform, {{before.x1, cy}, {before.x1 + 20, cy}, {before.x1 + 30, cy}}, Qt::AltModifier);
    s = f.ed.selectionBounds();
    CHECK(std::abs(s.x0 - before.x0) < 1 && std::abs(s.x1 - (before.x1 + 30)) < 1.5);
}

VX_TEST(stage_context_menu_selects_what_is_under_the_pointer)
{
    Fixture f;
    f.drag(ToolId::Rectangle, {{200, 200}, {300, 300}});
    f.drag(ToolId::Rectangle, {{600, 200}, {700, 300}});
    f.ed.clearSelection();
    int menus = 0;
    QObject::connect(&f.view, &StageView::contextMenuRequested, [&](const QPoint&) { ++menus; });
    auto rightClick = [&](Vec2 p) {
        const QPointF w = f.w(p);
        QContextMenuEvent ev(QContextMenuEvent::Mouse, w.toPoint(), f.view.mapToGlobal(w.toPoint()));
        QApplication::sendEvent(&f.view, &ev);
    };
    rightClick({650, 250});
    CHECK(menus == 1);
    CHECK(f.ed.shapePick().valid() && f.ed.selectionBounds().x0 > 590);
    // Convert to Symbol from there takes just that drawing.
    f.ed.convertSelectionToSymbol("Square", SymbolType::Graphic, 4);
    const auto& els = f.ed.currentLayer()->keyAt(0)->elements;
    CHECK(els.size() == 2 && els.back()->type() == ElementType::Instance);
    CHECK(f.ed.mergeShape(f.ed.layerIndex())->bounds(false).x1 < 320);
    // On a selected element the selection stays.
    f.ed.selectAll();
    const size_t n = f.ed.selection().size();
    rightClick({650, 250});
    CHECK(f.ed.selection().size() == n);
}

VX_TEST(tools_panel_fits_short_screens)
{
    Editor ed;
    ToolsPanel panel(&ed);
    panel.resize(panel.width(), 1000);
    panel.show();
    QApplication::processEvents();
    CHECK(panel.columns() == 1);
    panel.resize(panel.width(), 420);
    QApplication::processEvents();
    CHECK(panel.columns() == 2);
    CHECK(panel.width() > 90);
    // Still too short: the wheel scrolls, and the current tool is kept in view.
    panel.resize(panel.width(), 200);
    QApplication::processEvents();
    ed.setTool(ToolId::Zoom);
    QApplication::processEvents();
    CHECK(panel.columns() == 2);
    panel.resize(panel.width(), 1000);
    QApplication::processEvents();
    CHECK(panel.columns() == 1 && panel.width() < 60);
}

VX_TEST(strokes_commit_in_the_background)
{
    Fixture f;
    f.ed.settings().brushSmoothing = 0;
    f.ed.settings().brushPressure = false;
    f.ed.setTool(ToolId::Brush);
    f.send(QEvent::MouseButtonPress, {300, 300}, Qt::LeftButton);
    for (int i = 1; i <= 30; ++i) f.send(QEvent::MouseMove, {300.0 + i * 10, 300}, Qt::LeftButton);
    f.send(QEvent::MouseButtonRelease, {600, 300}, Qt::NoButton);
    // Moving on right away: the stroke still lands on the frame it was drawn on.
    f.ed.setFrame(5);
    while (f.view.hasPendingWork()) QApplication::processEvents(QEventLoop::AllEvents, 5);
    const Layer& l = *f.ed.currentLayer();
    CHECK(l.keys.size() == 1 && l.keys[0].start == 0 && !l.keys[0].elements.empty());
    // One undo step per stroke.
    f.ed.setFrame(0);
    f.line(ToolId::Pencil, {300, 400}, {600, 450});
    ShapeGraphPtr g = f.ed.mergeShape(f.ed.layerIndex());
    CHECK(g && !g->strokes.empty());
    f.ed.undoStack()->undo();
    g = f.ed.mergeShape(f.ed.layerIndex());
    CHECK(g && g->strokes.empty() && g->fillRegion(0).area() > 1000);
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setOrganizationName("VertexaTests");
    app.setApplicationName("VertexaTests");
    vx::ui::Theme::init(app);
    return vxtest::runAll(argc, argv);
}
