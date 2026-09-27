// SPDX-License-Identifier: GPL-3.0-or-later
// UI level tests: drive the real StageView with synthesized mouse input and
// check the resulting documents.
#include "TestMain.h"

#include "app/Editor.h"
#include "app/MainWindow.h"
#include "app/StageView.h"
#include "app/Theme.h"
#include "core/DocumentOps.h"
#include "core/Evaluate.h"
#include "core/VectorBrush.h"
#include "render/QtConvert.h"

#include <QApplication>
#include <QMouseEvent>
#include <QAction>
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

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setOrganizationName("VertexaTests");
    app.setApplicationName("VertexaTests");
    vx::ui::Theme::init(app);
    return vxtest::runAll(argc, argv);
}
