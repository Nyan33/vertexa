// SPDX-License-Identifier: GPL-3.0-or-later
#include "Editor.h"

#include "core/DocumentOps.h"
#include "core/Evaluate.h"
#include "core/TimelineOps.h"

#include <QTimer>
#include <QUndoCommand>
#include <QUndoStack>

namespace vx::app {

double ToolSettings::gapPixels() const
{
    switch (gap) {
    case GapSize::None: return 0.0;
    case GapSize::Small: return 4.0;
    case GapSize::Medium: return 10.0;
    case GapSize::Large: return 24.0;
    }
    return 0.0;
}

class SnapshotCommand : public QUndoCommand {
public:
    SnapshotCommand(Editor* e, Document before, Document after, Editor::State sb, Editor::State sa, const QString& label)
        : QUndoCommand(label), m_editor(e), m_before(std::move(before)), m_after(std::move(after)), m_sb(std::move(sb)),
          m_sa(std::move(sa))
    {
    }
    void undo() override { m_editor->restore(m_before, m_sb); }
    void redo() override
    {
        if (m_first) {
            m_first = false;
            return;
        }
        m_editor->restore(m_after, m_sa);
    }

private:
    Editor* m_editor;
    Document m_before, m_after;
    Editor::State m_sb, m_sa;
    bool m_first = true;
};

Editor::Editor(QObject* parent) : QObject(parent)
{
    m_undo = new QUndoStack(this);
    m_undo->setUndoLimit(300);
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &Editor::tick);
    connect(m_undo, &QUndoStack::cleanChanged, this, &Editor::pathChanged);
    m_doc = Document::createDefault();
    m_settings.stroke.width = 2.0;
    m_settings.stroke.paint = FillStyle::solid(Color(0x15, 0x15, 0x1A));
    m_settings.paint = builtinBrushPresets()[1];
}

Editor::~Editor() = default;

void Editor::setPreview(std::optional<Document> d)
{
    m_preview = std::move(d);
    emit previewChanged();
}

void Editor::setDocument(Document d, const QString& path)
{
    setPlaying(false);
    m_doc = std::move(d);
    m_preview.reset();
    m_path = path;
    m_undo->clear();
    m_scene = 0;
    m_stack.clear();
    m_frame = 0;
    m_layer = 0;
    m_frameSel = {};
    m_selection.clear();
    m_shapePick = {};
    validateState();
    emit documentChanged();
    emit contextChanged();
    emit frameChanged(m_frame);
    emit layerChanged(m_layer);
    emit selectionChanged();
    emit pathChanged();
}

void Editor::setFilePath(const QString& p)
{
    m_path = p;
    emit pathChanged();
}

bool Editor::isDirty() const { return !m_undo->isClean(); }
void Editor::markClean()
{
    m_undo->setClean();
    emit pathChanged();
}

bool Editor::edit(const QString& label, const std::function<bool(Document&)>& fn)
{
    const bool hadPreview = m_preview.has_value();
    m_preview.reset();
    Document d = m_doc;
    const State sb = state();
    if (!fn(d)) {
        if (hadPreview) emit previewChanged();
        return false;
    }
    Document before = std::move(m_doc);
    m_doc = std::move(d);
    validateState();
    m_undo->push(new SnapshotCommand(this, std::move(before), m_doc, sb, state(), label));
    emit documentChanged();
    return true;
}

Editor::State Editor::state() const { return {m_scene, m_stack, m_frame, m_layer}; }

void Editor::restore(const Document& d, const State& s)
{
    m_doc = d;
    m_preview.reset();
    const bool ctxChanged = s.stack.size() != m_stack.size() || s.scene != m_scene;
    m_scene = s.scene;
    m_stack = s.stack;
    m_frame = s.frame;
    m_layer = s.layer;
    m_selection.clear();
    m_shapePick = {};
    validateState();
    emit documentChanged();
    if (ctxChanged) emit contextChanged();
    emit frameChanged(m_frame);
    emit layerChanged(m_layer);
    emit selectionChanged();
}

void Editor::validateState()
{
    if (m_doc.scenes.empty()) {
        Timeline t;
        t.layers.push_back(m_doc.makeLayer("Layer 1"));
        m_doc.scenes.push_back(t);
    }
    m_scene = std::clamp(m_scene, 0, int(m_doc.scenes.size()) - 1);
    while (!m_stack.empty() && !m_doc.symbol(m_stack.back().symbolId)) m_stack.pop_back();
    const Timeline& tl = timeline();
    m_layer = std::clamp(m_layer, 0, std::max(0, int(tl.layers.size()) - 1));
    m_frame = std::max(0, m_frame);
}

const Timeline& Editor::timelineOf(const Document& d) const
{
    if (!m_stack.empty())
        if (const Symbol* s = d.symbol(m_stack.back().symbolId)) return s->timeline;
    return d.scenes[std::clamp(m_scene, 0, int(d.scenes.size()) - 1)];
}

const Timeline& Editor::timeline() const { return timelineOf(m_doc); }

Timeline& Editor::mutableTimeline(Document& d) const
{
    if (!m_stack.empty())
        if (Symbol* s = d.symbol(m_stack.back().symbolId)) return s->timeline;
    return d.scenes[std::clamp(m_scene, 0, int(d.scenes.size()) - 1)];
}

Affine Editor::contextMatrix() const
{
    Affine m;
    for (const ContextEntry& e : m_stack) m = m * e.matrix;
    return m;
}

std::vector<std::pair<uint32_t, int>> Editor::focusPath() const
{
    std::vector<std::pair<uint32_t, int>> path;
    for (const ContextEntry& e : m_stack) {
        if (!e.inPlace) return {};
        path.push_back({e.parentLayerId, e.elementIndex});
    }
    return path;
}

int Editor::rootFrame() const { return m_stack.empty() ? m_frame : m_stack.front().parentFrame; }

void Editor::enterSymbol(const std::string& symbolId)
{
    const Symbol* s = m_doc.symbol(symbolId);
    if (!s) return;
    setPlaying(false);
    ContextEntry e;
    e.symbolId = symbolId;
    e.inPlace = false;
    e.parentFrame = m_frame;
    e.parentLayer = m_layer;
    // Isolation mode: the registration point sits in the middle of the stage.
    e.matrix = Affine::translate(m_doc.width * 0.5, m_doc.height * 0.5) * contextMatrix().inverted();
    m_stack.push_back(e);
    m_frame = 0;
    m_layer = 0;
    m_selection.clear();
    m_shapePick = {};
    m_frameSel = {};
    validateState();
    emit contextChanged();
    emit frameChanged(m_frame);
    emit layerChanged(m_layer);
    emit selectionChanged();
}

bool Editor::enterInstance(int layerIndex, int elementIndex)
{
    const Timeline& tl = timeline();
    if (layerIndex < 0 || layerIndex >= int(tl.layers.size())) return false;
    const auto items = evaluateLayer(m_doc, tl, layerIndex, m_frame);
    if (elementIndex < 0 || elementIndex >= int(items.size())) return false;
    const InstanceElement* in = asInstance(items[elementIndex].element);
    if (!in || !m_doc.symbol(in->symbolId)) return false;
    setPlaying(false);
    ContextEntry e;
    e.symbolId = in->symbolId;
    e.inPlace = true;
    e.parentLayerId = tl.layers[layerIndex].id;
    e.elementIndex = elementIndex;
    e.parentFrame = m_frame;
    e.parentLayer = m_layer;
    e.matrix = in->matrix;
    const Symbol* s = m_doc.symbol(in->symbolId);
    const int startFrame = s && in->behavior == SymbolType::Graphic ? instanceSymbolFrame(m_doc, *in, items[elementIndex].localFrame) : 0;
    m_stack.push_back(e);
    m_frame = startFrame;
    m_layer = 0;
    m_selection.clear();
    m_shapePick = {};
    m_frameSel = {};
    validateState();
    emit contextChanged();
    emit frameChanged(m_frame);
    emit layerChanged(m_layer);
    emit selectionChanged();
    return true;
}

void Editor::exitContext(int levels)
{
    if (m_stack.empty()) return;
    setPlaying(false);
    ContextEntry last;
    for (int i = 0; i < levels && !m_stack.empty(); ++i) {
        last = m_stack.back();
        m_stack.pop_back();
    }
    m_frame = last.parentFrame;
    m_layer = last.parentLayer;
    m_selection.clear();
    m_shapePick = {};
    m_frameSel = {};
    validateState();
    if (last.inPlace) {
        const int li = timeline().layerIndex(last.parentLayerId);
        if (li >= 0) m_selection.push_back({last.parentLayerId, last.elementIndex});
    }
    emit contextChanged();
    emit frameChanged(m_frame);
    emit layerChanged(m_layer);
    emit selectionChanged();
}

void Editor::exitToScene() { exitContext(int(m_stack.size())); }

QString Editor::contextName() const
{
    if (m_stack.empty()) return QString::fromStdString(m_doc.scenes[m_scene].name);
    const Symbol* s = m_doc.symbol(m_stack.back().symbolId);
    return s ? QString::fromStdString(s->name) : QString();
}

void Editor::setFrame(int f)
{
    f = std::max(0, f);
    if (f == m_frame) return;
    m_frame = f;
    if (!m_playing) {
        m_selection.clear();
        m_shapePick = {};
        emit selectionChanged();
    }
    emit frameChanged(m_frame);
}

void Editor::setLayerIndex(int i)
{
    i = std::clamp(i, 0, std::max(0, int(timeline().layers.size()) - 1));
    if (i == m_layer) return;
    m_layer = i;
    emit layerChanged(m_layer);
}

const Layer* Editor::currentLayer() const
{
    const Timeline& tl = timeline();
    return (m_layer >= 0 && m_layer < int(tl.layers.size())) ? &tl.layers[m_layer] : nullptr;
}

void Editor::setFrameSelection(const FrameSelection& s)
{
    m_frameSel = s;
    emit frameSelectionChanged();
}

void Editor::setPlaying(bool play)
{
    if (play == m_playing) return;
    m_playing = play;
    if (play) {
        m_selection.clear();
        m_shapePick = {};
        emit selectionChanged();
        m_timer->start(int(std::lround(1000.0 / std::max(1.0, m_doc.fps))));
    } else {
        m_timer->stop();
    }
    emit playingChanged(play);
}

void Editor::setLoopPlayback(bool on) { m_loop = on; }

void Editor::setOnion(bool on, bool outline)
{
    onionSkin = on;
    onionOutline = outline;
    emit onionChanged();
}

void Editor::tick()
{
    const int count = timeline().frameCount();
    int next = m_frame + 1;
    if (next >= count) {
        if (!m_loop) {
            setPlaying(false);
            return;
        }
        next = 0;
    }
    m_frame = next;
    emit frameChanged(m_frame);
}

void Editor::setSelection(std::vector<ElementRef> s)
{
    m_selection = std::move(s);
    emit selectionChanged();
}

void Editor::setShapePick(ShapePick p)
{
    m_shapePick = std::move(p);
    emit selectionChanged();
}

void Editor::clearSelection()
{
    if (m_selection.empty() && !m_shapePick.valid()) return;
    m_selection.clear();
    m_shapePick = {};
    emit selectionChanged();
}

std::vector<ElementPtr> Editor::selectedElements() const
{
    std::vector<ElementPtr> out;
    const Timeline& tl = timeline();
    for (const ElementRef& r : m_selection) {
        const Layer* l = tl.layerById(r.layerId);
        if (!l) continue;
        const Keyframe* k = l->keyAt(m_frame);
        if (!k || r.index < 0 || r.index >= int(k->elements.size())) continue;
        out.push_back(k->elements[r.index]);
    }
    return out;
}

Rect Editor::elementsBounds(const std::vector<ElementPtr>& els) const
{
    Rect r;
    for (const ElementPtr& e : els) r.include(elementBounds(m_doc, *e));
    return r;
}

Rect Editor::selectionBounds() const
{
    Rect r = elementsBounds(selectedElements());
    if (m_shapePick.valid()) {
        if (m_shapePick.region) {
            ShapeGraph rest, lifted;
            cutByRegion(*m_shapePick.graph, *m_shapePick.region, rest, lifted);
            r.include(lifted.bounds(true));
        } else {
            ShapeGraph rest, lifted;
            liftSelection(*m_shapePick.graph, m_shapePick.sel, rest, lifted);
            r.include(lifted.bounds(true));
        }
    }
    return r;
}

ShapeGraphPtr Editor::mergeShape(int layerIndex) const
{
    const Timeline& tl = timeline();
    if (layerIndex < 0 || layerIndex >= int(tl.layers.size())) return nullptr;
    const Keyframe* k = tl.layers[layerIndex].keyAt(m_frame);
    if (!k || k->elements.empty()) return nullptr;
    const ShapeElement* s = asShape(k->elements.front());
    return (s && !s->isObject) ? s->graph : nullptr;
}

bool Editor::canEdit(int layerIndex, QString* why) const
{
    const Timeline& tl = timeline();
    if (layerIndex < 0 || layerIndex >= int(tl.layers.size())) {
        if (why) *why = tr("No layer selected");
        return false;
    }
    const Layer& l = tl.layers[layerIndex];
    if (l.type == LayerType::Folder) {
        if (why) *why = tr("Folders can't hold artwork — pick a layer");
        return false;
    }
    if (l.locked) {
        if (why) *why = tr("Layer “%1” is locked").arg(QString::fromStdString(l.name));
        return false;
    }
    if (!l.visible) {
        if (why) *why = tr("Layer “%1” is hidden").arg(QString::fromStdString(l.name));
        return false;
    }
    return true;
}

Keyframe* Editor::editableKey(Document& d, int layerIndex, QString* why) const
{
    if (!canEdit(layerIndex, why)) return nullptr;
    Timeline& tl = mutableTimeline(d);
    Layer& l = tl.layers[layerIndex];
    if (m_frame >= l.length()) vx::insertKeyframe(d, tl, layerIndex, m_frame, true);
    return tl.layers[layerIndex].keyAt(m_frame);
}

void Editor::notify(const QString& msg) { emit message(msg); }

void Editor::setTool(ToolId t)
{
    if (t == m_tool) return;
    m_tool = t;
    emit toolChanged(t);
}

QString Editor::toolName(ToolId t)
{
    switch (t) {
    case ToolId::Selection: return tr("Selection");
    case ToolId::Subselection: return tr("Subselection");
    case ToolId::FreeTransform: return tr("Free Transform");
    case ToolId::Lasso: return tr("Lasso");
    case ToolId::Pen: return tr("Pen");
    case ToolId::Line: return tr("Line");
    case ToolId::Rectangle: return tr("Rectangle");
    case ToolId::Oval: return tr("Oval");
    case ToolId::PolyStar: return tr("PolyStar");
    case ToolId::Pencil: return tr("Pencil");
    case ToolId::Brush: return tr("Brush");
    case ToolId::PaintBrush: return tr("Paint Brush");
    case ToolId::Eraser: return tr("Eraser");
    case ToolId::PaintBucket: return tr("Paint Bucket");
    case ToolId::InkBottle: return tr("Ink Bottle");
    case ToolId::Eyedropper: return tr("Eyedropper");
    case ToolId::Hand: return tr("Hand");
    case ToolId::Zoom: return tr("Zoom");
    case ToolId::Count: break;
    }
    return {};
}

} // namespace vx::app
