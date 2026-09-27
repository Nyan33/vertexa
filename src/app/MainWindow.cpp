// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.h"
#include "Dialogs.h"
#include "Icons.h"
#include "StageView.h"
#include "Theme.h"
#include "Widgets.h"
#include "panels/BrushPanel.h"
#include "panels/ColorPanel.h"
#include "panels/LibraryPanel.h"
#include "panels/PropertiesPanel.h"
#include "panels/TimelinePanel.h"
#include "panels/ToolsPanel.h"

#include "core/Serialize.h"
#include "core/io/FlaImport.h"
#include "render/Renderer.h"
#include "render/SvgExport.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>

namespace vx::app {

using ui::Theme;

namespace {

const char* kFileFilter = "Vertexa document (*.vtx)";
const char* kOpenFilter = "All supported (*.vtx *.fla *.xfl);;Vertexa document (*.vtx);;Flash / Animate document (*.fla *.xfl)";
const char* kFlaFilter = "Flash / Animate document (*.fla *.xfl);;XFL folder document (DOMDocument.xml)";

} // namespace

MainWindow::MainWindow(Editor* editor, QWidget* parent) : QMainWindow(parent), m_ed(editor)
{
    setWindowIcon(ui::icon("brush"));
    setDockNestingEnabled(true);
    setCentralWidget(createStageArea());
    createActions();
    createMenus();
    createDocks();
    m_coords = new QLabel(this);
    statusBar()->addPermanentWidget(m_coords);
    statusBar()->setSizeGripEnabled(false);

    connect(m_ed, &Editor::pathChanged, this, &MainWindow::updateTitle);
    connect(m_ed, &Editor::documentChanged, this, &MainWindow::updateTitle);
    connect(m_ed, &Editor::contextChanged, this, &MainWindow::updateBreadcrumb);
    connect(m_ed, &Editor::documentChanged, this, &MainWindow::updateBreadcrumb);
    connect(m_ed, &Editor::message, this, [this](const QString& m) {
        statusBar()->showMessage(m, 5000);
        statusBar()->setStyleSheet(QString("QStatusBar { color: %1; }").arg(Theme::p().yellow.name()));
    });
    connect(m_stage, &StageView::pointerMoved, this, [this](double x, double y) {
        m_coords->setText(QString("X %1   Y %2").arg(x, 0, 'f', 1).arg(y, 0, 'f', 1));
    });
    connect(m_stage, &StageView::zoomChanged, this, [this](double z) { m_zoomLabel->setText(QString("%1%").arg(int(std::round(z * 100)))); });

    QSettings s;
    resize(1480, 920);
    restoreGeometry(s.value("ui/geometry").toByteArray());
    restoreState(s.value("ui/state").toByteArray(), 2);
    updateTitle();
    updateBreadcrumb();
    rebuildRecentMenu();
}

QAction* MainWindow::add(const QString& name, const QString& text, const QKeySequence& key, std::function<void()> fn, const QString& icon)
{
    auto* a = new QAction(text, this);
    if (!key.isEmpty()) a->setShortcut(key);
    if (!icon.isEmpty()) a->setIcon(ui::icon(icon));
    a->setToolTip(key.isEmpty() ? text : QString("%1 (%2)").arg(QString(text).remove('&'), key.toString(QKeySequence::NativeText)));
    a->setShortcutContext(Qt::WindowShortcut);
    connect(a, &QAction::triggered, this, [fn]() { fn(); });
    addAction(a);
    m_actions.insert(name, a);
    m_actionOrder.push_back(a);
    return a;
}

QAction* MainWindow::addCheck(const QString& name, const QString& text, const QKeySequence& key, bool on, std::function<void(bool)> fn)
{
    QAction* a = add(name, text, key, [] {});
    a->setCheckable(true);
    a->setChecked(on);
    connect(a, &QAction::toggled, this, [fn](bool b) { fn(b); });
    return a;
}

void MainWindow::createActions()
{
    Editor* ed = m_ed;
    // File
    add("new", tr("&New"), QKeySequence::New, [this] { newDocument(); });
    add("open", tr("&Open…"), QKeySequence::Open, [this] { open(); });
    add("importFla", tr("Import &FLA / XFL…"), QKeySequence("Ctrl+R"), [this] { importFla(); });
    add("save", tr("&Save"), QKeySequence::Save, [this] { save(); });
    add("saveAs", tr("Save &As…"), QKeySequence("Ctrl+Shift+S"), [this] { saveAs(); });
    add("exportPng", tr("Export PNG Sequence…"), QKeySequence("Ctrl+Alt+Shift+S"), [this] { exportPngSequence(); });
    add("exportSvg", tr("Export Frame as SVG…"), {}, [this] { exportSvg(); });
    add("exportVideo", tr("Export Video (FFmpeg)…"), QKeySequence("Ctrl+Alt+Shift+E"), [this] { exportVideo(); });
    add("documentSettings", tr("&Document…"), QKeySequence("Ctrl+J"), [this] { documentSettings(); });
    add("quit", tr("&Quit"), QKeySequence::Quit, [this] { close(); });

    // Edit
    QAction* undo = m_ed->undoStack()->createUndoAction(this, tr("&Undo"));
    undo->setShortcut(QKeySequence::Undo);
    addAction(undo);
    m_actions.insert("undo", undo);
    m_actionOrder.push_back(undo);
    QAction* redo = m_ed->undoStack()->createRedoAction(this, tr("&Redo"));
    redo->setShortcuts({QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z")});
    addAction(redo);
    m_actions.insert("redo", redo);
    m_actionOrder.push_back(redo);
    add("cut", tr("Cu&t"), QKeySequence::Cut, [ed] { ed->cutSelection(); });
    add("copy", tr("&Copy"), QKeySequence::Copy, [ed] { ed->copySelection(); });
    add("paste", tr("Paste in &Center"), QKeySequence::Paste, [ed] { ed->paste(false); });
    add("pasteInPlace", tr("Paste in &Place"), QKeySequence("Ctrl+Shift+V"), [ed] { ed->paste(true); });
    QAction* clear = add("clear", tr("Clea&r"), QKeySequence::Delete, [this, ed] {
        if (Tool* t = m_stage->activeTool(); t && t->busy() && t->id() == ToolId::Pen) {
            QKeyEvent ev(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
            t->keyPress(&ev);
            return;
        }
        ed->deleteSelection();
    });
    clear->setShortcuts({QKeySequence::Delete, QKeySequence(Qt::Key_Backspace)});
    add("duplicate", tr("&Duplicate"), QKeySequence("Ctrl+D"), [ed] { ed->duplicateSelection(); });
    add("selectAll", tr("Select &All"), QKeySequence::SelectAll, [ed] { ed->selectAll(); });
    add("deselectAll", tr("Deselect All"), QKeySequence("Ctrl+Shift+A"), [ed] { ed->clearSelection(); });
    add("editSymbols", tr("Edit Symbols"), QKeySequence("Ctrl+E"), [this] { toggleEditSymbol(); });
    add("tablet", tr("Tablet && Pressure…"), {}, [this] { TabletDialog(m_ed, this).exec(); }, "tablet");

    // View
    add("zoomIn", tr("Zoom &In"), QKeySequence("Ctrl+="), [this] { m_stage->zoomBy(1.5); });
    add("zoomOut", tr("Zoom &Out"), QKeySequence("Ctrl+-"), [this] { m_stage->zoomBy(1.0 / 1.5); });
    add("zoom100", tr("100%"), QKeySequence("Ctrl+1"), [this] { m_stage->zoomTo100(); });
    add("fit", tr("Fit Stage"), QKeySequence("Ctrl+2"), [this] { m_stage->fitStage(); });
    addCheck("onionSkin", tr("Onion Skin"), QKeySequence("Alt+Shift+O"), false, [ed](bool b) { ed->setOnion(b, ed->onionOutline); });
    addCheck("onionOutline", tr("Onion Skin Outlines"), {}, false, [ed](bool b) { ed->setOnion(ed->onionSkin || b, b); });
    addCheck("darkTheme", tr("Dark Theme"), {}, Theme::isDark(), [](bool b) { Theme::setDark(b); });
    connect(m_ed, &Editor::onionChanged, this, [this] {
        const QSignalBlocker b1(m_actions["onionSkin"]), b2(m_actions["onionOutline"]);
        m_actions["onionSkin"]->setChecked(m_ed->onionSkin);
        m_actions["onionOutline"]->setChecked(m_ed->onionOutline);
    });

    // Insert
    add("newSymbol", tr("New &Symbol…"), QKeySequence("Ctrl+F8"), [this, ed] {
        ConvertToSymbolDialog dlg(QString::fromStdString(ed->doc().uniqueSymbolName("Symbol 1")), ed->doc().allLibraryFolders(), this);
        dlg.setWindowTitle(tr("Create New Symbol"));
        if (dlg.exec() == QDialog::Accepted) ed->newSymbol(dlg.name(), dlg.type(), dlg.folder(), dlg.scale9());
    });
    add("newLayer", tr("New &Layer"), QKeySequence("Ctrl+Alt+N"), [ed] { ed->addLayer(); });
    add("newFolder", tr("New Layer &Folder"), {}, [ed] { ed->addFolder(); });
    add("addMotionGuide", tr("Add Classic Motion &Guide"), {}, [ed] { ed->addLayer(LayerType::Guide); });
    add("deleteLayer", tr("Delete Layer"), {}, [ed] { ed->deleteLayer(); });
    add("insertFrame", tr("Insert &Frame"), QKeySequence(Qt::Key_F5), [ed] { ed->insertFrames(); });
    add("removeFrames", tr("&Remove Frames"), QKeySequence("Shift+F5"), [ed] { ed->removeFrames(); });
    add("insertKeyframe", tr("Insert &Keyframe"), QKeySequence(Qt::Key_F6), [ed] { ed->insertKeyframe(false); });
    add("clearKeyframe", tr("Clear Keyframe"), QKeySequence("Shift+F6"), [ed] { ed->clearKeyframe(); });
    add("insertBlankKeyframe", tr("Insert &Blank Keyframe"), QKeySequence(Qt::Key_F7), [ed] { ed->insertKeyframe(true); });
    add("createClassicTween", tr("Create &Classic Tween"), QKeySequence("Ctrl+Alt+T"), [ed] { ed->createTween(TweenType::Classic); });
    add("createShapeTween", tr("Create S&hape Tween"), QKeySequence("Ctrl+Alt+Y"), [ed] { ed->createTween(TweenType::Shape); });
    add("removeTween", tr("Remove Tween"), {}, [ed] { ed->removeTween(); });
    add("addShapeHint", tr("Add Shape &Hint"), QKeySequence("Ctrl+Shift+H"), [ed] { ed->addShapeHint(); });
    add("removeShapeHints", tr("Remove All Hints"), {}, [ed] { ed->removeShapeHints(); });
    add("convertToKeyframes", tr("Convert to Keyframes"), {}, [ed] { ed->convertToKeyframes(false); });
    add("convertToBlankKeyframes", tr("Convert to Blank Keyframes"), {}, [ed] { ed->convertToKeyframes(true); });
    add("clearFrames", tr("Clear Frames"), QKeySequence("Alt+Backspace"), [ed] { ed->clearFrames(); });
    add("reverseFrames", tr("Reverse Frames"), {}, [ed] { ed->reverseFrames(); });
    add("cutFrames", tr("Cut Frames"), QKeySequence("Ctrl+Alt+X"), [ed] { ed->copyFrames(true); });
    add("copyFrames", tr("Copy Frames"), QKeySequence("Ctrl+Alt+C"), [ed] { ed->copyFrames(false); });
    add("pasteFrames", tr("Paste Frames"), QKeySequence("Ctrl+Alt+V"), [ed] { ed->pasteFrames(); });
    add("selectAllFrames", tr("Select All Frames"), QKeySequence("Ctrl+Alt+A"), [ed] {
        FrameSelection s;
        s.layerFrom = 0;
        s.layerTo = int(ed->timeline().layers.size()) - 1;
        s.frameFrom = 0;
        s.frameTo = ed->timeline().frameCount() - 1;
        ed->setFrameSelection(s);
    });

    // Modify
    add("convertToSymbol", tr("Convert to &Symbol…"), QKeySequence(Qt::Key_F8), [this] { convertToSymbol(); });
    add("breakApart", tr("&Break Apart"), QKeySequence("Ctrl+B"), [ed] { ed->breakApart(); });
    add("group", tr("&Group"), QKeySequence("Ctrl+G"), [ed] { ed->groupSelection(); });
    add("ungroup", tr("&Ungroup"), QKeySequence("Ctrl+Shift+G"), [ed] { ed->ungroupSelection(); });
    add("union", tr("Union"), {}, [ed] { ed->combineObjects(Editor::Combine::Union); });
    add("intersect", tr("Intersect"), {}, [ed] { ed->combineObjects(Editor::Combine::Intersect); });
    add("punch", tr("Punch"), {}, [ed] { ed->combineObjects(Editor::Combine::Punch); });
    add("crop", tr("Crop"), {}, [ed] { ed->combineObjects(Editor::Combine::Crop); });
    auto aroundCenter = [ed](const Affine& m) {
        const Rect b = ed->selectionBounds();
        if (!b.isEmpty()) ed->transformSelection(Affine::about(b.center(), m), QObject::tr("Transform"));
    };
    add("flipH", tr("Flip &Horizontal"), {}, [aroundCenter] { aroundCenter(Affine::scale(-1, 1)); }, "flip-h");
    add("flipV", tr("Flip &Vertical"), {}, [aroundCenter] { aroundCenter(Affine::scale(1, -1)); }, "flip-v");
    add("rotateCW", tr("Rotate 90° CW"), QKeySequence("Ctrl+Shift+9"), [aroundCenter] { aroundCenter(Affine::rotate(kPi / 2)); });
    add("rotateCCW", tr("Rotate 90° CCW"), QKeySequence("Ctrl+Shift+7"), [aroundCenter] { aroundCenter(Affine::rotate(-kPi / 2)); });
    add("bringToFront", tr("Bring to Front"), QKeySequence("Ctrl+Shift+Up"), [ed] { ed->arrange(Editor::Arrange::Front); });
    add("bringForward", tr("Bring Forward"), QKeySequence("Ctrl+Up"), [ed] { ed->arrange(Editor::Arrange::Forward); });
    add("sendBackward", tr("Send Backward"), QKeySequence("Ctrl+Down"), [ed] { ed->arrange(Editor::Arrange::Backward); });
    add("sendToBack", tr("Send to Back"), QKeySequence("Ctrl+Shift+Down"), [ed] { ed->arrange(Editor::Arrange::Back); });
    add("nudgeLeft", tr("Nudge Left"), QKeySequence(Qt::Key_Left), [ed] { ed->nudge(-1, 0); });
    add("nudgeRight", tr("Nudge Right"), QKeySequence(Qt::Key_Right), [ed] { ed->nudge(1, 0); });
    add("nudgeUp", tr("Nudge Up"), QKeySequence(Qt::Key_Up), [ed] { ed->nudge(0, -1); });
    add("nudgeDown", tr("Nudge Down"), QKeySequence(Qt::Key_Down), [ed] { ed->nudge(0, 1); });
    add("nudgeLeft10", tr("Nudge Left ×10"), QKeySequence("Shift+Left"), [ed] { ed->nudge(-10, 0); });
    add("nudgeRight10", tr("Nudge Right ×10"), QKeySequence("Shift+Right"), [ed] { ed->nudge(10, 0); });
    add("nudgeUp10", tr("Nudge Up ×10"), QKeySequence("Shift+Up"), [ed] { ed->nudge(0, -10); });
    add("nudgeDown10", tr("Nudge Down ×10"), QKeySequence("Shift+Down"), [ed] { ed->nudge(0, 10); });

    // Control
    add("play", tr("&Play / Stop"), QKeySequence(Qt::Key_Return), [this, ed] {
        if (Tool* t = m_stage->activeTool(); t && t->busy() && t->id() == ToolId::Pen) {
            QKeyEvent ev(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            t->keyPress(&ev);
            return;
        }
        ed->setPlaying(!ed->isPlaying());
    }, "play");
    add("firstFrame", tr("Go to First Frame"), QKeySequence("Shift+,"), [ed] { ed->setFrame(0); }, "first");
    add("lastFrame", tr("Go to Last Frame"), QKeySequence("Shift+."), [ed] { ed->setFrame(ed->timeline().frameCount() - 1); }, "last");
    add("prevFrame", tr("Previous Frame"), QKeySequence(Qt::Key_Comma), [ed] { ed->setFrame(ed->frame() - 1); }, "prev");
    add("nextFrame", tr("Next Frame"), QKeySequence(Qt::Key_Period), [ed] { ed->setFrame(ed->frame() + 1); }, "next");
    addCheck("loop", tr("Loop Playback"), {}, true, [ed](bool b) { ed->setLoopPlayback(b); });
    addCheck("simpleButtons", tr("Enable Simple &Buttons"), QKeySequence("Ctrl+Alt+B"), false, [ed](bool b) { ed->setSimpleButtons(b); });

    // Tools
    for (int i = 0; i < int(ToolId::Count); ++i) {
        const ToolId id = ToolId(i);
        add(QString("tool%1").arg(i), Editor::toolName(id), QKeySequence(ToolsPanel::shortcutFor(id)), [ed, id] { ed->setTool(id); },
            ToolsPanel::iconFor(id));
    }
    add("objectDrawing", tr("Object Drawing Mode"), QKeySequence(Qt::Key_J), [ed] {
        ed->settings().objectDrawing = !ed->settings().objectDrawing;
        ed->emitSettingsChanged();
    });
    add("swapColors", tr("Swap Colours"), QKeySequence(Qt::Key_X), [ed] {
        ToolSettings& s = ed->settings();
        const Color f = s.fill.mainColor(), st = s.stroke.paint.mainColor();
        s.fill = FillStyle::solid(st);
        s.stroke.paint = FillStyle::solid(f);
        ed->emitSettingsChanged();
    });
    add("defaultColors", tr("Default Colours"), QKeySequence(Qt::Key_D), [ed] {
        ed->settings().fill = FillStyle::solid(Color(0, 0, 0));
        ed->settings().stroke.paint = FillStyle::solid(Color(0, 0, 0));
        ed->settings().fillEnabled = ed->settings().strokeEnabled = true;
        ed->emitSettingsChanged();
    });
    auto sizeBy = [ed](double k) {
        ToolSettings& s = ed->settings();
        switch (ed->tool()) {
        case ToolId::Brush: s.brushSize = std::clamp(s.brushSize * k, 1.0, 400.0); break;
        case ToolId::Eraser: s.eraserSize = std::clamp(s.eraserSize * k, 1.0, 400.0); break;
        case ToolId::PaintBrush: s.paint.size = std::clamp(s.paint.size * k, 0.5, 1000.0); break;
        default: return;
        }
        ed->emitSettingsChanged();
    };
    add("sizeDown", tr("Smaller Brush"), QKeySequence(Qt::Key_BracketLeft), [sizeBy] { sizeBy(1 / 1.15); });
    add("sizeUp", tr("Bigger Brush"), QKeySequence(Qt::Key_BracketRight), [sizeBy] { sizeBy(1.15); });

    // Help
    add("hotkeys", tr("Keyboard Shortcuts"), QKeySequence(Qt::Key_F1), [this] { HotkeysDialog(m_actionOrder, this).exec(); });
    add("about", tr("About Vertexa"), {}, [this] {
        QMessageBox::about(this, tr("About Vertexa"),
                           tr("<h2>Vertexa %1</h2><p>Open-source 2D vector animation studio.</p>"
                              "<p>Exact vector geometry, Flash-style merge drawing, symbols, classic and shape tweens, "
                              "Krita-like texture brushes and full tablet support.</p>"
                              "<p>Licensed under the GNU GPL v3 or later. Inspired by Adobe Animate; references: "
                              "Krita, OpenToonz and Flare.</p>")
                               .arg(QApplication::applicationVersion()));
    });
}

void MainWindow::createMenus()
{
    auto a = [this](const char* n) { return m_actions.value(QString::fromLatin1(n)); };
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(a("new"));
    file->addAction(a("open"));
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    file->addAction(a("importFla"));
    file->addSeparator();
    file->addAction(a("save"));
    file->addAction(a("saveAs"));
    file->addSeparator();
    QMenu* exp = file->addMenu(tr("&Export"));
    exp->addAction(a("exportPng"));
    exp->addAction(a("exportVideo"));
    exp->addAction(a("exportSvg"));
    file->addSeparator();
    file->addAction(a("documentSettings"));
    file->addSeparator();
    file->addAction(a("quit"));

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    for (const char* n : {"undo", "redo"}) edit->addAction(a(n));
    edit->addSeparator();
    for (const char* n : {"cut", "copy", "paste", "pasteInPlace", "clear", "duplicate"}) edit->addAction(a(n));
    edit->addSeparator();
    for (const char* n : {"selectAll", "deselectAll"}) edit->addAction(a(n));
    edit->addSeparator();
    for (const char* n : {"cutFrames", "copyFrames", "pasteFrames", "clearFrames", "selectAllFrames"}) edit->addAction(a(n));
    edit->addSeparator();
    edit->addAction(a("editSymbols"));
    edit->addSeparator();
    edit->addAction(a("tablet"));

    QMenu* view = menuBar()->addMenu(tr("&View"));
    for (const char* n : {"zoomIn", "zoomOut", "zoom100", "fit"}) view->addAction(a(n));
    view->addSeparator();
    view->addAction(a("onionSkin"));
    view->addAction(a("onionOutline"));
    view->addSeparator();
    view->addAction(a("darkTheme"));

    QMenu* insert = menuBar()->addMenu(tr("&Insert"));
    insert->addAction(a("newSymbol"));
    QMenu* tl = insert->addMenu(tr("&Timeline"));
    for (const char* n : {"newLayer", "newFolder", "addMotionGuide"}) tl->addAction(a(n));
    tl->addSeparator();
    for (const char* n : {"insertFrame", "insertKeyframe", "insertBlankKeyframe"}) tl->addAction(a(n));
    insert->addSeparator();
    insert->addAction(a("createClassicTween"));
    insert->addAction(a("createShapeTween"));

    QMenu* modify = menuBar()->addMenu(tr("&Modify"));
    modify->addAction(a("documentSettings"));
    modify->addSeparator();
    for (const char* n : {"convertToSymbol", "breakApart"}) modify->addAction(a(n));
    QMenu* shape = modify->addMenu(tr("S&hape"));
    shape->addAction(a("addShapeHint"));
    shape->addAction(a("removeShapeHints"));
    QMenu* combine = modify->addMenu(tr("&Combine Objects"));
    for (const char* n : {"union", "intersect", "punch", "crop"}) combine->addAction(a(n));
    QMenu* tlm = modify->addMenu(tr("&Timeline"));
    for (const char* n : {"reverseFrames", "removeTween", "convertToKeyframes", "convertToBlankKeyframes", "clearKeyframe", "removeFrames"})
        tlm->addAction(a(n));
    QMenu* transform = modify->addMenu(tr("T&ransform"));
    for (const char* n : {"flipH", "flipV", "rotateCW", "rotateCCW"}) transform->addAction(a(n));
    QMenu* arrange = modify->addMenu(tr("&Arrange"));
    for (const char* n : {"bringToFront", "bringForward", "sendBackward", "sendToBack"}) arrange->addAction(a(n));
    modify->addSeparator();
    modify->addAction(a("group"));
    modify->addAction(a("ungroup"));

    QMenu* control = menuBar()->addMenu(tr("&Control"));
    for (const char* n : {"play", "firstFrame", "lastFrame", "prevFrame", "nextFrame", "loop"}) control->addAction(a(n));
    control->addSeparator();
    control->addAction(a("simpleButtons"));

    QMenu* tools = menuBar()->addMenu(tr("&Tools"));
    for (int i = 0; i < int(ToolId::Count); ++i) tools->addAction(m_actions.value(QString("tool%1").arg(i)));
    tools->addSeparator();
    for (const char* n : {"objectDrawing", "swapColors", "defaultColors", "sizeDown", "sizeUp"}) tools->addAction(a(n));

    m_windowMenu = menuBar()->addMenu(tr("&Window"));
    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(a("hotkeys"));
    help->addAction(a("about"));
}

void MainWindow::createDocks()
{
    auto lookup = [this](const QString& n) { return m_actions.value(n); };
    auto dock = [this](const QString& name, const QString& title, QWidget* w, Qt::DockWidgetArea area) {
        auto* d = new QDockWidget(title, this);
        d->setObjectName(name);
        d->setWidget(w);
        d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetFloatable);
        addDockWidget(area, d);
        m_windowMenu->addAction(d->toggleViewAction());
        return d;
    };
    QDockWidget* tools = dock("tools", tr("Tools"), new ToolsPanel(m_ed, this), Qt::LeftDockWidgetArea);
    tools->setTitleBarWidget(new QWidget(tools));
    tools->setFeatures(QDockWidget::DockWidgetMovable);
    QDockWidget* timeline = dock("timeline", tr("Timeline"), new TimelinePanel(m_ed, lookup, this), Qt::BottomDockWidgetArea);
    timeline->setTitleBarWidget(new QWidget(timeline));
    QDockWidget* props = dock("properties", tr("Properties"), new PropertiesPanel(m_ed, this), Qt::RightDockWidgetArea);
    QDockWidget* color = dock("color", tr("Color"), new ColorPanel(m_ed, this), Qt::RightDockWidgetArea);
    QDockWidget* lib = dock("library", tr("Library"), new LibraryPanel(m_ed, this), Qt::RightDockWidgetArea);
    QDockWidget* brushes = dock("brushes", tr("Brushes"), new BrushPanel(m_ed, this), Qt::RightDockWidgetArea);
    tabifyDockWidget(props, lib);
    tabifyDockWidget(color, brushes);
    props->raise();
    color->raise();
    // Panels carry their own expressive titles; the tab bar names them.
    for (QDockWidget* d : {props, color, lib, brushes}) d->setTitleBarWidget(new QWidget(d));
    setTabPosition(Qt::RightDockWidgetArea, QTabWidget::North);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    resizeDocks({props, color}, {520, 400}, Qt::Vertical);
    resizeDocks({props}, {320}, Qt::Horizontal);
    resizeDocks({timeline}, {250}, Qt::Vertical);
}

QWidget* MainWindow::createStageArea()
{
    auto* area = new QWidget(this);
    auto* lay = new QVBoxLayout(area);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto* bar = new QWidget(area);
    bar->setFixedHeight(40);
    bar->setAutoFillBackground(true);
    auto* bl = new QHBoxLayout(bar);
    bl->setContentsMargins(12, 4, 12, 4);
    m_breadcrumb = new QWidget(bar);
    auto* bcl = new QHBoxLayout(m_breadcrumb);
    bcl->setContentsMargins(0, 0, 0, 0);
    bcl->setSpacing(4);
    bl->addWidget(m_breadcrumb);
    bl->addStretch(1);
    m_zoomLabel = new QLabel("100%", bar);
    m_zoomLabel->setFont(Theme::ui(12, QFont::DemiBold));
    bl->addWidget(m_zoomLabel);
    auto* fit = new QToolButton(bar);
    fit->setText(tr("Fit"));
    fit->setToolTip(tr("Fit stage (Ctrl+2)"));
    bl->addWidget(fit);
    lay->addWidget(bar);
    m_stage = new StageView(m_ed, area);
    lay->addWidget(m_stage, 1);
    connect(fit, &QToolButton::clicked, m_stage, &StageView::fitStage);
    return area;
}

void MainWindow::updateBreadcrumb()
{
    auto* lay = static_cast<QHBoxLayout*>(m_breadcrumb->layout());
    while (QLayoutItem* it = lay->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    const ui::Palette& pal = Theme::p();
    auto crumb = [&](const QString& text, bool current, const QString& icon, int levelsUp) {
        auto* b = new QToolButton(m_breadcrumb);
        b->setText(text);
        b->setIcon(ui::icon(icon));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setIconSize(QSize(16, 16));
        b->setStyleSheet(QString("QToolButton { color: %1; font-weight: %2; font-size: 13px; padding: 4px 10px; border-radius: 8px; }"
                                 "QToolButton:hover { background: %3; }")
                             .arg(current ? pal.text.name() : pal.text2.name(), current ? "700" : "500", pal.bg3.name()));
        if (levelsUp > 0) connect(b, &QToolButton::clicked, this, [this, levelsUp] { m_ed->exitContext(levelsUp); });
        lay->addWidget(b);
    };
    const int n = int(m_ed->contextStack().size());
    crumb(QString::fromStdString(m_ed->doc().scenes[m_ed->scene()].name), n == 0, "layer", n);
    for (int i = 0; i < n; ++i) {
        auto* sep = new QLabel("›", m_breadcrumb);
        sep->setStyleSheet(QString("color: %1; font-size: 16px;").arg(pal.text3.name()));
        lay->addWidget(sep);
        const Symbol* s = m_ed->doc().symbol(m_ed->contextStack()[i].symbolId);
        const QString icon = !s ? "symbol" : s->type == SymbolType::MovieClip ? "movieclip" : s->type == SymbolType::Graphic ? "graphic" : "button";
        crumb(s ? QString::fromStdString(s->name) : QString("?"), i == n - 1, icon, n - 1 - i);
    }
    m_breadcrumb->parentWidget()->setStyleSheet(QString("background: %1;").arg(pal.bg0.name()));
}

void MainWindow::updateTitle()
{
    const QString name = m_ed->filePath().isEmpty() ? tr("Untitled") : QFileInfo(m_ed->filePath()).fileName();
    setWindowTitle(QString("%1%2 — Vertexa").arg(name, m_ed->isDirty() ? " •" : ""));
}

void MainWindow::rebuildRecentMenu()
{
    m_recentMenu->clear();
    const QStringList recent = QSettings().value("files/recent").toStringList();
    for (const QString& p : recent) m_recentMenu->addAction(QFileInfo(p).fileName(), this, [this, p] {
        if (maybeSave()) openFile(p);
    });
    m_recentMenu->setEnabled(!recent.isEmpty());
}

void MainWindow::addRecent(const QString& path)
{
    QSettings s;
    QStringList recent = s.value("files/recent").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 10) recent.removeLast();
    s.setValue("files/recent", recent);
    rebuildRecentMenu();
}

bool MainWindow::maybeSave()
{
    if (!m_ed->isDirty()) return true;
    const auto r = QMessageBox::question(this, tr("Unsaved changes"), tr("Save changes to the document before closing it?"),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return save();
    return true;
}

bool MainWindow::save()
{
    if (m_ed->filePath().isEmpty()) return saveAs();
    QString err;
    if (!saveDocument(m_ed->doc(), m_ed->filePath(), &err)) {
        QMessageBox::warning(this, tr("Save failed"), err);
        return false;
    }
    m_ed->markClean();
    addRecent(m_ed->filePath());
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(m_ed->filePath()).fileName()), 3000);
    return true;
}

bool MainWindow::saveAs()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Save Document"), m_ed->filePath(), tr(kFileFilter));
    if (path.isEmpty()) return false;
    if (QFileInfo(path).suffix().isEmpty()) path += ".vtx";
    m_ed->setFilePath(path);
    return save();
}

void MainWindow::newDocument()
{
    if (!maybeSave()) return;
    m_ed->setDocument(Document::createDefault(), {});
    m_stage->fitStage();
}

void MainWindow::open()
{
    if (!maybeSave()) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Document"), {}, tr(kOpenFilter));
    if (!path.isEmpty()) openFile(path);
}

void MainWindow::importFla()
{
    if (!maybeSave()) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Flash / Animate Document"), {}, tr(kFlaFilter));
    if (!path.isEmpty()) importFlaFile(path);
}

bool MainWindow::importFlaFile(const QString& path)
{
    Document d;
    io::ImportReport report;
    QString err;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = io::importFla(path, d, &report, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, tr("Import failed"), tr("%1\n\n%2").arg(path, err));
        return false;
    }
    // Imported documents are saved as .vtx, never over the original .fla.
    m_ed->setDocument(std::move(d), QString());
    addRecent(path);
    m_stage->fitStage();
    const QString summary = tr("%1: %2 symbols, %3 layers, %4 keyframes")
                                .arg(report.generator.isEmpty() ? QFileInfo(path).fileName() : report.generator)
                                .arg(report.symbols)
                                .arg(report.layers)
                                .arg(report.keyframes);
    m_stage->showToast(tr("Imported %1").arg(QFileInfo(path).fileName()));
    statusBar()->showMessage(summary, 8000);
    if (!report.warnings.isEmpty()) {
        QStringList unique;
        for (const QString& w : report.warnings)
            if (!unique.contains(w)) unique << w;
        auto* box = new QMessageBox(QMessageBox::Information, tr("Imported with notes"), summary, QMessageBox::Ok, this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setInformativeText(unique.mid(0, 12).join('\n'));
        box->open();
    }
    return true;
}

bool MainWindow::openFile(const QString& path)
{
    if (io::detectFla(path) != io::FlaFormat::Unknown) return importFlaFile(path);
    Document d;
    QString err;
    if (!loadDocument(path, d, &err)) {
        QMessageBox::warning(this, tr("Open failed"), tr("%1\n\n%2").arg(path, err));
        return false;
    }
    m_ed->setDocument(std::move(d), path);
    addRecent(path);
    m_stage->fitStage();
    return true;
}

void MainWindow::exportPngSequence()
{
    ExportDialog dlg(tr("Export PNG Sequence"), true, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const ExportOptions o = dlg.options();
    QString path = QFileDialog::getSaveFileName(this, tr("Export PNG"), {}, tr("PNG image (*.png)"));
    if (path.isEmpty()) return;
    if (path.endsWith(".png", Qt::CaseInsensitive)) path.chop(4);
    const Document& d = m_ed->doc();
    const Timeline& scene = d.scenes[m_ed->scene()];
    RenderOptions ro;
    ro.showGuides = false;
    ro.masksNeedLock = false;
    ro.outlineLayers = false;
    if (!o.allFrames) {
        Renderer::renderFrame(d, scene, m_ed->rootFrame(), o.scale, o.transparent, ro).save(path + ".png");
        return;
    }
    const int n = scene.frameCount();
    QProgressDialog progress(tr("Exporting frames…"), tr("Cancel"), 0, n, this);
    progress.setWindowModality(Qt::WindowModal);
    for (int f = 0; f < n; ++f) {
        progress.setValue(f);
        if (progress.wasCanceled()) break;
        RenderOptions fo = ro;
        fo.clipFrame = f;
        Renderer::renderFrame(d, scene, f, o.scale, o.transparent, fo).save(QString("%1_%2.png").arg(path).arg(f + 1, 4, 10, QChar('0')));
    }
    progress.setValue(n);
    statusBar()->showMessage(tr("Exported %n frame(s)", "", n), 4000);
}

void MainWindow::exportSvg()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Export SVG"), {}, tr("SVG image (*.svg)"));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += ".svg";
    QString err;
    const Document& d = m_ed->doc();
    if (!saveFrameSvg(d, d.scenes[m_ed->scene()], m_ed->rootFrame(), path, &err)) QMessageBox::warning(this, tr("Export failed"), err);
}

void MainWindow::exportVideo()
{
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        QMessageBox::information(this, tr("FFmpeg not found"),
                                 tr("Video export uses FFmpeg. Install it and make sure “ffmpeg” is on your PATH, "
                                    "or export a PNG sequence instead."));
        return;
    }
    ExportDialog dlg(tr("Export Video"), false, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const ExportOptions o = dlg.options();
    QString path = QFileDialog::getSaveFileName(this, tr("Export Video"), {}, tr("MP4 video (*.mp4);;QuickTime (*.mov);;WebM (*.webm)"));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += ".mp4";
    const Document& d = m_ed->doc();
    const Timeline& scene = d.scenes[m_ed->scene()];
    const int w = (int(std::lround(d.width * o.scale)) + 1) & ~1, h = (int(std::lround(d.height * o.scale)) + 1) & ~1;
    QProcess proc;
    QStringList args{"-y", "-f", "rawvideo", "-pix_fmt", "bgra", "-s", QString("%1x%2").arg(w).arg(h), "-r", QString::number(d.fps), "-i", "-"};
    if (path.endsWith(".webm", Qt::CaseInsensitive)) args << "-c:v" << "libvpx-vp9" << "-pix_fmt" << "yuv420p";
    else args << "-c:v" << "libx264" << "-pix_fmt" << "yuv420p" << "-crf" << "16";
    args << path;
    proc.start(ffmpeg, args);
    if (!proc.waitForStarted()) {
        QMessageBox::warning(this, tr("Export failed"), tr("Could not start FFmpeg."));
        return;
    }
    RenderOptions ro;
    ro.showGuides = false;
    ro.masksNeedLock = false;
    ro.outlineLayers = false;
    const int n = scene.frameCount();
    QProgressDialog progress(tr("Encoding video…"), tr("Cancel"), 0, n, this);
    progress.setWindowModality(Qt::WindowModal);
    for (int f = 0; f < n && !progress.wasCanceled(); ++f) {
        progress.setValue(f);
        RenderOptions fo = ro;
        fo.clipFrame = f;
        QImage img = Renderer::renderFrame(d, scene, f, o.scale, false, fo).convertToFormat(QImage::Format_ARGB32);
        if (img.width() != w || img.height() != h) img = img.copy(0, 0, w, h);
        for (int y = 0; y < h; ++y) proc.write(reinterpret_cast<const char*>(img.constScanLine(y)), qint64(w) * 4);
        proc.waitForBytesWritten(-1);
    }
    proc.closeWriteChannel();
    proc.waitForFinished(-1);
    progress.setValue(n);
    if (proc.exitCode() != 0) QMessageBox::warning(this, tr("Export failed"), QString::fromLocal8Bit(proc.readAllStandardError()).right(800));
    else statusBar()->showMessage(tr("Video exported to %1").arg(path), 5000);
}

void MainWindow::documentSettings()
{
    DocumentDialog dlg(m_ed->doc(), this);
    if (dlg.exec() == QDialog::Accepted) m_ed->setStageSettings(dlg.width(), dlg.height(), dlg.fps(), dlg.background());
}

void MainWindow::convertToSymbol()
{
    if (!m_ed->hasSelection()) {
        m_ed->notify(tr("Select something to convert to a symbol"));
        return;
    }
    ConvertToSymbolDialog dlg(QString::fromStdString(m_ed->doc().uniqueSymbolName("Symbol 1")), m_ed->doc().allLibraryFolders(), this);
    if (dlg.exec() == QDialog::Accepted)
        m_ed->convertSelectionToSymbol(dlg.name(), dlg.type(), dlg.registration(), dlg.folder(), dlg.scale9());
}

void MainWindow::toggleEditSymbol()
{
    if (m_ed->inSymbol()) {
        m_ed->exitToScene();
        return;
    }
    const auto els = m_ed->selectedElements();
    if (els.size() == 1 && els.front()->type() == ElementType::Instance) {
        const ElementRef r = m_ed->selection().front();
        m_ed->enterInstance(m_ed->timeline().layerIndex(r.layerId), r.index);
    }
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    QSettings s;
    s.setValue("ui/geometry", saveGeometry());
    s.setValue("ui/state", saveState(2));
    e->accept();
}

} // namespace vx::app
