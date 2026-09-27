// SPDX-License-Identifier: GPL-3.0-or-later
#include "PropertiesPanel.h"
#include "../Icons.h"
#include "../Theme.h"
#include "../Widgets.h"

#include "core/DocumentOps.h"
#include "core/Evaluate.h"
#include "render/QtConvert.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace vx::app {

using ui::Theme;

namespace {

HotNumber* number(QWidget* parent, double v, double lo, double hi, int decimals, double step, const QString& suffix,
                  std::function<void(double)> commit, std::function<void(double)> live = {})
{
    auto* n = new HotNumber(parent);
    n->setRange(lo, hi);
    n->setDecimals(decimals);
    n->setStep(step);
    n->setSuffix(suffix);
    n->setValue(v);
    QObject::connect(n, &HotNumber::valueCommitted, parent, [commit](double x) { commit(x); });
    if (live) QObject::connect(n, &HotNumber::valueChanged, parent, [live](double x) { live(x); });
    return n;
}

QComboBox* combo(QWidget* parent, const QStringList& items, int current, std::function<void(int)> fn)
{
    auto* c = new QComboBox(parent);
    c->addItems(items);
    c->setCurrentIndex(current);
    QObject::connect(c, qOverload<int>(&QComboBox::activated), parent, [fn](int i) { fn(i); });
    return c;
}

QCheckBox* check(QWidget* parent, const QString& text, bool on, std::function<void(bool)> fn)
{
    auto* c = new QCheckBox(text, parent);
    c->setChecked(on);
    QObject::connect(c, &QCheckBox::toggled, parent, [fn](bool b) { fn(b); });
    return c;
}

QWidget* hbox(QWidget* parent, const QList<QWidget*>& ws)
{
    auto* w = new QWidget(parent);
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(10);
    for (QWidget* x : ws) l->addWidget(x);
    l->addStretch(1);
    return w;
}

QStringList easeNames()
{
    QStringList out;
    for (int i = 0; i < int(EaseKind::Count); ++i) {
        const auto l = easeLabel(EaseKind(i));
        out << QString::fromUtf8(l.data(), int(l.size()));
    }
    return out;
}

/// Blend mode picker grouped like Animate's menu (Normal | Layer | darken
/// group | lighten group | contrast group | Add, Subtract, Difference |
/// Invert, Alpha, Erase), followed by the extra Krita-style modes.
QComboBox* blendCombo(QWidget* parent, BlendMode current, std::function<void(BlendMode)> fn)
{
    auto* c = new QComboBox(parent);
    auto add = [c](BlendMode m) {
        const auto label = blendModeLabel(m);
        c->addItem(QString::fromUtf8(label.data(), int(label.size())), int(m));
    };
    const std::vector<std::vector<BlendMode>> groups = {
        {BlendMode::Normal},
        {BlendMode::Layer},
        {BlendMode::Darken, BlendMode::Multiply},
        {BlendMode::Lighten, BlendMode::Screen},
        {BlendMode::Overlay, BlendMode::HardLight},
        {BlendMode::Add, BlendMode::Subtract, BlendMode::Difference},
        {BlendMode::Invert, BlendMode::Alpha, BlendMode::Erase},
    };
    for (size_t g = 0; g < groups.size(); ++g) {
        if (g > 0) c->insertSeparator(c->count());
        for (BlendMode m : groups[g]) add(m);
    }
    c->insertSeparator(c->count());
    for (const auto& b : kBlendModes)
        if (!b.animate) add(b.mode);
    c->setCurrentIndex(std::max(0, c->findData(int(current))));
    QObject::connect(c, qOverload<int>(&QComboBox::activated), parent, [c, fn](int i) {
        const QVariant v = c->itemData(i);
        if (v.isValid()) fn(BlendMode(v.toInt()));
    });
    return c;
}

} // namespace

PropertiesPanel::PropertiesPanel(Editor* editor, QWidget* parent) : QScrollArea(parent), m_ed(editor)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (auto sig : {&Editor::selectionChanged, &Editor::contextChanged, &Editor::documentChanged, &Editor::frameSelectionChanged})
        connect(m_ed, sig, this, &PropertiesPanel::scheduleRebuild);
    connect(m_ed, &Editor::layerChanged, this, &PropertiesPanel::scheduleRebuild);
    connect(m_ed, &Editor::toolChanged, this, &PropertiesPanel::scheduleRebuild);
    connect(m_ed, &Editor::frameChanged, this, [this]() {
        if (!m_ed->isPlaying()) scheduleRebuild();
    });
    connect(Theme::instance(), &ui::Theme::changed, this, &PropertiesPanel::scheduleRebuild);
    rebuild();
}

void PropertiesPanel::scheduleRebuild()
{
    if (m_pending) return;
    m_pending = true;
    QTimer::singleShot(0, this, [this]() {
        m_pending = false;
        rebuild();
    });
}

QGridLayout* PropertiesPanel::section(const QString& title, const QString& subtitle)
{
    auto* t = new SectionTitle(title, m_content);
    t->setSubtitle(subtitle);
    m_layout->addWidget(t);
    auto* box = new QWidget(m_content);
    auto* g = new QGridLayout(box);
    g->setContentsMargins(15, 6, 4, 14);
    g->setHorizontalSpacing(12);
    g->setVerticalSpacing(8);
    g->setColumnStretch(1, 1);
    m_layout->addWidget(box);
    return g;
}

void PropertiesPanel::row(QGridLayout* g, const QString& label, QWidget* w)
{
    const int r = g->rowCount();
    if (!label.isEmpty()) {
        auto* l = new QLabel(label, m_content);
        l->setStyleSheet(QString("color: %1; font-size: 11px;").arg(Theme::p().text2.name()));
        g->addWidget(l, r, 0, Qt::AlignLeft | Qt::AlignVCenter);
        g->addWidget(w, r, 1);
    } else {
        g->addWidget(w, r, 0, 1, 2);
    }
}

void PropertiesPanel::rebuild()
{
    QWidget* old = takeWidget();
    if (old) old->deleteLater();
    m_content = new QWidget();
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(10, 8, 12, 12);
    m_layout->setSpacing(0);

    buildToolOptions();
    if (m_ed->hasSelection()) buildSelection();
    else {
        buildFrame();
        buildLayer();
        buildDocument();
    }
    m_layout->addStretch(1);
    setWidget(m_content);
}

void PropertiesPanel::buildFillStroke(bool applyToSelection)
{
    QGridLayout* g = section(tr("Fill & Stroke"));
    const ToolSettings& s = m_ed->settings();
    auto* fill = new ColorSwatch(m_content);
    fill->setFill(s.fill, s.fillEnabled);
    auto* stroke = new ColorSwatch(m_content);
    stroke->setStrokeLook(true);
    stroke->setFill(s.stroke.paint, s.strokeEnabled);
    connect(fill, &ColorSwatch::clicked, this, [this, fill, applyToSelection]() {
        popupColorPicker(fill, toQColor(m_ed->settings().fill.mainColor()), [this, fill, applyToSelection](const QColor& c, bool final) {
            FillStyle f = m_ed->settings().fill;
            if (f.kind == FillStyle::Kind::Solid) f.color = fromQColor(c);
            else if (!f.gradient.stops.empty()) f.gradient.stops.front().color = fromQColor(c);
            m_ed->settings().fillEnabled = true;
            fill->setFill(f, true);
            if (final && applyToSelection) m_ed->applyFillToSelection(f);
            else {
                m_ed->settings().fill = f;
                m_ed->emitSettingsChanged();
            }
        });
    });
    connect(stroke, &ColorSwatch::clicked, this, [this, stroke, applyToSelection]() {
        popupColorPicker(stroke, toQColor(m_ed->settings().stroke.paint.mainColor()),
                         [this, stroke, applyToSelection](const QColor& c, bool final) {
                             StrokeStyle st = m_ed->settings().stroke;
                             st.paint = FillStyle::solid(fromQColor(c));
                             m_ed->settings().strokeEnabled = true;
                             stroke->setFill(st.paint, true);
                             if (final && applyToSelection) m_ed->applyStrokeToSelection(st);
                             else {
                                 m_ed->settings().stroke = st;
                                 m_ed->emitSettingsChanged();
                             }
                         });
    });
    auto* noFill = new QPushButton(m_content);
    noFill->setIcon(ui::icon("none"));
    noFill->setToolTip(tr("No fill"));
    noFill->setFixedSize(30, 30);
    connect(noFill, &QPushButton::clicked, this, [this, fill]() {
        m_ed->settings().fillEnabled = !m_ed->settings().fillEnabled;
        fill->setFill(m_ed->settings().fill, m_ed->settings().fillEnabled);
        m_ed->emitSettingsChanged();
    });
    auto* noStroke = new QPushButton(m_content);
    noStroke->setIcon(ui::icon("none"));
    noStroke->setToolTip(tr("No stroke"));
    noStroke->setFixedSize(30, 30);
    connect(noStroke, &QPushButton::clicked, this, [this, stroke]() {
        m_ed->settings().strokeEnabled = !m_ed->settings().strokeEnabled;
        stroke->setFill(m_ed->settings().stroke.paint, m_ed->settings().strokeEnabled);
        m_ed->emitSettingsChanged();
    });
    row(g, tr("Fill"), hbox(m_content, {fill, noFill}));
    auto* kind = combo(m_content, {tr("Solid"), tr("Linear gradient"), tr("Radial gradient")}, int(s.fill.kind), [this](int i) {
        FillStyle f = m_ed->settings().fill;
        if (FillStyle::Kind(i) != FillStyle::Kind::Solid && f.kind == FillStyle::Kind::Solid) {
            f.gradient.stops = {{0.0, f.color}, {1.0, Color(255, 255, 255)}};
        }
        f.kind = FillStyle::Kind(i);
        if (f.kind == FillStyle::Kind::Solid && !f.gradient.stops.empty()) f.color = f.gradient.stops.front().color;
        m_ed->settings().fill = f;
        m_ed->emitSettingsChanged();
        scheduleRebuild();
    });
    row(g, tr("Type"), kind);
    if (s.fill.isGradient() && s.fill.gradient.stops.size() >= 2) {
        auto* end = new ColorSwatch(m_content);
        end->setFill(FillStyle::solid(s.fill.gradient.stops.back().color));
        connect(end, &ColorSwatch::clicked, this, [this, end]() {
            popupColorPicker(end, toQColor(m_ed->settings().fill.gradient.stops.back().color), [this, end](const QColor& c, bool) {
                m_ed->settings().fill.gradient.stops.back().color = fromQColor(c);
                end->setFill(FillStyle::solid(fromQColor(c)));
                m_ed->emitSettingsChanged();
            });
        });
        row(g, tr("Gradient end"), hbox(m_content, {end}));
    }
    row(g, tr("Stroke"), hbox(m_content, {stroke, noStroke}));
    row(g, tr("Width"), number(m_content, s.stroke.width, 0.1, 200, 2, 0.1, " px", [this, applyToSelection](double v) {
            StrokeStyle st = m_ed->settings().stroke;
            st.width = v;
            if (applyToSelection) m_ed->applyStrokeToSelection(st);
            else m_ed->settings().stroke = st;
        }));
    row(g, tr("Style"), combo(m_content, {tr("Solid"), tr("Dashed"), tr("Dotted"), tr("Hairline")}, int(s.stroke.pattern),
                              [this, applyToSelection](int i) {
                                  StrokeStyle st = m_ed->settings().stroke;
                                  st.pattern = StrokePattern(i);
                                  if (applyToSelection) m_ed->applyStrokeToSelection(st);
                                  else m_ed->settings().stroke = st;
                              }));
    row(g, tr("Cap / Join"),
        hbox(m_content, {combo(m_content, {tr("Round"), tr("Square"), tr("None")}, int(s.stroke.cap),
                               [this](int i) { m_ed->settings().stroke.cap = CapStyle(i); }),
                         combo(m_content, {tr("Round"), tr("Miter"), tr("Bevel")}, int(s.stroke.join),
                               [this](int i) { m_ed->settings().stroke.join = JoinStyle(i); })}));
}

void PropertiesPanel::buildToolOptions()
{
    ToolSettings& s = m_ed->settings();
    const ToolId tool = m_ed->tool();
    QGridLayout* g = section(Editor::toolName(tool), tr("tool"));
    auto objectToggle = [&]() {
        row(g, {}, check(m_content, tr("Object drawing (J)"), s.objectDrawing, [this](bool b) {
                m_ed->settings().objectDrawing = b;
                m_ed->emitSettingsChanged();
            }));
    };
    switch (tool) {
    case ToolId::Brush: {
        row(g, tr("Size"), number(m_content, s.brushSize, 1, 400, 1, 0.5, " px", [this](double v) {
                m_ed->settings().brushSize = v;
                m_ed->emitSettingsChanged();
            }, [this](double v) {
                m_ed->settings().brushSize = v;
                m_ed->emitSettingsChanged();
            }));
        auto* shape = new Segmented(m_content);
        for (const char* t : {"●", "◆", "■", "▬", "╱", "╲", "—", "│"}) shape->addSegment(QString::fromUtf8(t));
        static const TipShape shapes[] = {TipShape::Round, TipShape::Ellipse, TipShape::Square, TipShape::Rectangle,
                                          TipShape::Slash, TipShape::Backslash, TipShape::Horizontal, TipShape::Vertical};
        for (int i = 0; i < 8; ++i)
            if (shapes[i] == s.brushShape) shape->setCurrent(i);
        connect(shape, &Segmented::changed, this, [this](int i) {
            static const TipShape sh[] = {TipShape::Round, TipShape::Ellipse, TipShape::Square, TipShape::Rectangle,
                                          TipShape::Slash, TipShape::Backslash, TipShape::Horizontal, TipShape::Vertical};
            m_ed->settings().brushShape = sh[i];
            m_ed->emitSettingsChanged();
        });
        row(g, tr("Shape"), shape);
        row(g, tr("Angle"), number(m_content, s.brushAngle, -180, 180, 0, 1, "°", [this](double v) { m_ed->settings().brushAngle = v; }));
        row(g, tr("Mode"), combo(m_content, {tr("Paint Normal"), tr("Paint Fills"), tr("Paint Behind"), tr("Paint Selection"), tr("Paint Inside")},
                                 int(s.brushMode), [this](int i) { m_ed->settings().brushMode = PaintMode(i); }));
        row(g, tr("Smoothing"), number(m_content, s.brushSmoothing, 0, 100, 0, 1, "", [this](double v) { m_ed->settings().brushSmoothing = v; }));
        row(g, {}, check(m_content, tr("Pressure → size"), s.brushPressure, [this](bool b) { m_ed->settings().brushPressure = b; }));
        row(g, tr("Min size"), number(m_content, s.brushMinSize * 100, 1, 100, 0, 1, "%", [this](double v) { m_ed->settings().brushMinSize = v / 100.0; }));
        row(g, {}, check(m_content, tr("Tilt → angle"), s.brushTilt, [this](bool b) { m_ed->settings().brushTilt = b; }));
        objectToggle();
        break;
    }
    case ToolId::Eraser: {
        row(g, tr("Mode"), combo(m_content, {tr("Erase Normal"), tr("Erase Fills"), tr("Erase Lines"), tr("Erase Selected Fills"), tr("Erase Inside")},
                                 int(s.eraseMode), [this](int i) { m_ed->settings().eraseMode = EraseMode(i); }));
        row(g, tr("Size"), number(m_content, s.eraserSize, 1, 400, 1, 0.5, " px", [this](double v) {
                m_ed->settings().eraserSize = v;
                m_ed->emitSettingsChanged();
            }));
        auto* shape = new Segmented(m_content);
        shape->addSegment("●");
        shape->addSegment("■");
        shape->setCurrent(s.eraserShape == TipShape::Square ? 1 : 0);
        connect(shape, &Segmented::changed, this, [this](int i) { m_ed->settings().eraserShape = i ? TipShape::Square : TipShape::Round; });
        row(g, tr("Shape"), shape);
        row(g, {}, check(m_content, tr("Faucet (delete a whole fill or line)"), s.faucet, [this](bool b) {
                m_ed->settings().faucet = b;
                m_ed->emitSettingsChanged();
            }));
        row(g, {}, check(m_content, tr("Pressure → size"), s.eraserPressure, [this](bool b) { m_ed->settings().eraserPressure = b; }));
        break;
    }
    case ToolId::Pencil: {
        auto* mode = new Segmented(m_content);
        mode->addSegment(tr("Straighten"));
        mode->addSegment(tr("Smooth"));
        mode->addSegment(tr("Ink"));
        mode->setCurrent(int(s.pencilMode));
        connect(mode, &Segmented::changed, this, [this](int i) { m_ed->settings().pencilMode = PencilMode(i); });
        row(g, {}, mode);
        row(g, tr("Smoothing"), number(m_content, s.pencilSmoothing, 0, 100, 0, 1, "", [this](double v) { m_ed->settings().pencilSmoothing = v; }));
        objectToggle();
        break;
    }
    case ToolId::PaintBrush: {
        auto* name = new QLabel(QString::fromStdString(s.paint.name), m_content);
        name->setFont(Theme::ui(13, QFont::DemiBold));
        row(g, tr("Preset"), name);
        row(g, tr("Size"), number(m_content, s.paintSizeScale * 100, 1, 1000, 0, 1, "%", [this](double v) {
                m_ed->settings().paintSizeScale = v / 100.0;
                m_ed->emitSettingsChanged();
            }));
        row(g, tr("Opacity"), number(m_content, s.paint.opacity * 100, 1, 100, 0, 1, "%", [this](double v) { m_ed->settings().paint.opacity = v / 100.0; }));
        row(g, tr("Flow"), number(m_content, s.paint.flow * 100, 1, 100, 0, 1, "%", [this](double v) { m_ed->settings().paint.flow = v / 100.0; }));
        row(g, tr("Smoothing"), number(m_content, s.paint.smoothing, 0, 100, 0, 1, "", [this](double v) { m_ed->settings().paint.smoothing = v; }));
        row(g, {}, check(m_content, tr("Erase with this brush"), s.paintErase, [this](bool b) { m_ed->settings().paintErase = b; }));
        auto* hint = new QLabel(tr("Pick presets and edit the engine in the Brushes panel."), m_content);
        hint->setWordWrap(true);
        hint->setStyleSheet(QString("color: %1; font-size: 11px;").arg(Theme::p().text3.name()));
        row(g, {}, hint);
        break;
    }
    case ToolId::PaintBucket:
        row(g, tr("Gap size"), combo(m_content, {tr("Don't close gaps"), tr("Close small gaps"), tr("Close medium gaps"), tr("Close large gaps")},
                                     int(s.gap), [this](int i) { m_ed->settings().gap = GapSize(i); }));
        break;
    case ToolId::Rectangle:
        row(g, tr("Corner radius"), number(m_content, s.rectRadius, 0, 2000, 1, 0.5, " px", [this](double v) { m_ed->settings().rectRadius = v; }));
        objectToggle();
        break;
    case ToolId::PolyStar:
        row(g, tr("Sides"), number(m_content, s.polySides, 3, 32, 0, 0.2, "", [this](double v) { m_ed->settings().polySides = int(v); }));
        row(g, {}, check(m_content, tr("Star"), s.polyStar, [this](bool b) { m_ed->settings().polyStar = b; }));
        row(g, tr("Point size"), number(m_content, s.starDepth, 0.05, 1, 2, 0.01, "", [this](double v) { m_ed->settings().starDepth = v; }));
        objectToggle();
        break;
    case ToolId::Line:
    case ToolId::Oval:
    case ToolId::Pen: objectToggle(); break;
    default: {
        auto* hint = new QLabel(m_content);
        hint->setWordWrap(true);
        hint->setStyleSheet(QString("color: %1; font-size: 11px;").arg(Theme::p().text3.name()));
        if (tool == ToolId::Selection)
            hint->setText(tr("Click fills and lines to select them. Drag an edge to bend it, drag a corner to move it. "
                             "Double-click a symbol to edit it in place."));
        else if (tool == ToolId::Subselection) hint->setText(tr("Drag points and Bezier handles. Alt breaks a smooth point."));
        else if (tool == ToolId::FreeTransform)
            hint->setText(tr("Corners scale (Shift: proportional, Alt: from the pivot), outside rotates, edges skew."));
        else hint->setText(tr("Space + drag pans, Ctrl + wheel zooms."));
        row(g, {}, hint);
        break;
    }
    }
    const bool drawing = tool == ToolId::Brush || tool == ToolId::Pencil || tool == ToolId::Line || tool == ToolId::Rectangle ||
                         tool == ToolId::Oval || tool == ToolId::PolyStar || tool == ToolId::Pen || tool == ToolId::PaintBucket ||
                         tool == ToolId::InkBottle || tool == ToolId::PaintBrush;
    if (drawing && !m_ed->hasSelection()) buildFillStroke(false);
}

void PropertiesPanel::buildSelection()
{
    const auto els = m_ed->selectedElements();
    const Document& doc = m_ed->doc();
    const ShapePick& pick = m_ed->shapePick();
    QString what;
    if (els.size() == 1 && !pick.valid()) {
        switch (els.front()->type()) {
        case ElementType::Instance: what = tr("Symbol Instance"); break;
        case ElementType::Group: what = tr("Group"); break;
        case ElementType::Paint: what = tr("Texture Paint"); break;
        case ElementType::Shape: what = asShape(els.front())->isObject ? tr("Drawing Object") : tr("Shape"); break;
        case ElementType::Morph: what = tr("Shape"); break;
        }
    } else if (els.empty() && pick.valid()) {
        what = tr("Shape");
    } else {
        what = tr("%1 objects").arg(els.size() + (pick.valid() ? 1 : 0));
    }
    QGridLayout* g = section(what, tr("selection"));
    // Position & size of the selection bounds.
    const Rect b = m_ed->selectionBounds();
    if (!b.isEmpty()) {
        auto moveTo = [this](double nx, double ny) {
            const Rect cur = m_ed->selectionBounds();
            m_ed->transformSelection(Affine::translate(nx - cur.x0, ny - cur.y0), tr("Move"));
        };
        row(g, tr("X / Y"),
            hbox(m_content, {number(m_content, b.x0, -1e6, 1e6, 1, 1, "", [this, moveTo](double v) { moveTo(v, m_ed->selectionBounds().y0); }),
                             number(m_content, b.y0, -1e6, 1e6, 1, 1, "", [this, moveTo](double v) { moveTo(m_ed->selectionBounds().x0, v); })}));
        auto resize = [this](double w, double h) {
            const Rect cur = m_ed->selectionBounds();
            if (cur.width() <= 0 || cur.height() <= 0) return;
            m_ed->transformSelection(Affine::about(cur.topLeft(), Affine::scale(w / cur.width(), h / cur.height())), tr("Resize"));
        };
        row(g, tr("W / H"),
            hbox(m_content, {number(m_content, b.width(), 0.1, 1e6, 1, 1, "", [this, resize](double v) { resize(v, m_ed->selectionBounds().height()); }),
                             number(m_content, b.height(), 0.1, 1e6, 1, 1, "", [this, resize](double v) { resize(m_ed->selectionBounds().width(), v); })}));
    }
    if (els.size() == 1 && !pick.valid() && els.front()->type() != ElementType::Shape) {
        const ElementPtr e = els.front();
        const AffineParts parts = AffineParts::decompose(e->matrix);
        row(g, tr("Rotation"), number(m_content, parts.rotation() * 180.0 / kPi, -360, 360, 1, 1, "°", [this, e](double deg) {
                AffineParts p = AffineParts::decompose(e->matrix);
                const double delta = deg * kPi / 180.0 - p.rotation();
                const Vec2 pivot = e->matrix.map(e->pivot);
                m_ed->transformSelection(Affine::about(pivot, Affine::rotate(delta)), tr("Rotate"));
            }));
    }

    // Symbol instance.
    if (els.size() == 1 && els.front()->type() == ElementType::Instance && !pick.valid()) {
        const auto* in = static_cast<const InstanceElement*>(els.front().get());
        const Symbol* sym = doc.symbol(in->symbolId);
        QGridLayout* ig = section(sym ? QString::fromStdString(sym->name) : tr("Missing symbol"), tr("instance"));
        if (sym) {
            const std::string id = sym->id;
            // Like Animate: the behaviour belongs to the instance, the symbol's
            // own type is set in the Library.
            row(ig, tr("Behavior"), combo(m_content, {tr("Movie Clip"), tr("Graphic"), tr("Button")}, int(in->behavior), [this](int i) {
                    m_ed->setInstanceProperty([i](InstanceElement& x) { x.behavior = SymbolType(i); }, tr("Instance Behavior"));
                }));
            QStringList names;
            int cur = 0;
            for (int i = 0; i < int(doc.symbols.size()); ++i) {
                names << QString::fromStdString(doc.symbols[i].name);
                if (doc.symbols[i].id == id) cur = i;
            }
            row(ig, tr("Swap"), combo(m_content, names, cur, [this](int i) {
                    const std::string nid = m_ed->doc().symbols[i].id;
                    if (m_ed->inSymbol() && m_ed->doc().symbolContains(nid, m_ed->contextStack().back().symbolId)) {
                        m_ed->notify(tr("A symbol can't contain itself"));
                        return;
                    }
                    m_ed->setInstanceProperty([nid](InstanceElement& x) { x.symbolId = nid; }, tr("Swap Symbol"));
                }));
        }
        auto* nameEdit = new QLineEdit(QString::fromStdString(in->name), m_content);
        nameEdit->setPlaceholderText(tr("<Instance name>"));
        connect(nameEdit, &QLineEdit::editingFinished, this, [this, nameEdit]() {
            const std::string n = nameEdit->text().toStdString();
            m_ed->setInstanceProperty([n](InstanceElement& x) { x.name = n; }, tr("Instance Name"));
        });
        row(ig, tr("Name"), nameEdit);

        // Colour effect.
        const ColorEffect ce = in->color;
        row(ig, tr("Color effect"), combo(m_content, {tr("None"), tr("Brightness"), tr("Tint"), tr("Alpha"), tr("Advanced")}, int(ce.kind), [this](int i) {
                m_ed->setInstanceProperty([i](InstanceElement& x) {
                    x.color.kind = ColorEffect::Kind(i);
                    if (x.color.kind == ColorEffect::Kind::Tint && x.color.tintAmount == 0) x.color.tintAmount = 0.5;
                    if (x.color.kind == ColorEffect::Kind::Alpha && x.color.alpha == 1) x.color.alpha = 0.5;
                }, tr("Color Effect"));
            }));
        auto previewFx = [this](std::function<void(InstanceElement&)> fn) {
            Document d = m_ed->doc();
            Timeline& tl = m_ed->mutableTimeline(d);
            for (const ElementRef& r : m_ed->selection()) {
                const int li = tl.layerIndex(r.layerId);
                Keyframe* k = li >= 0 ? tl.layers[li].keyAt(m_ed->frame()) : nullptr;
                if (!k || r.index >= int(k->elements.size())) continue;
                if (const InstanceElement* x = asInstance(k->elements[r.index])) {
                    auto c = x->cloneAs<InstanceElement>();
                    fn(*c);
                    k->elements[r.index] = c;
                }
            }
            m_ed->setPreview(std::move(d));
        };
        switch (ce.kind) {
        case ColorEffect::Kind::Brightness: {
            auto fn = [](double v) { return [v](InstanceElement& x) { x.color.brightness = v / 100.0; }; };
            row(ig, tr("Brightness"), number(m_content, ce.brightness * 100, -100, 100, 0, 1, "%",
                                             [this, fn](double v) { m_ed->setInstanceProperty(fn(v), tr("Brightness")); },
                                             [previewFx, fn](double v) { previewFx(fn(v)); }));
            break;
        }
        case ColorEffect::Kind::Tint: {
            auto* sw = new ColorSwatch(m_content);
            sw->setFill(FillStyle::solid(ce.tint));
            connect(sw, &ColorSwatch::clicked, this, [this, sw, ce]() {
                popupColorPicker(sw, toQColor(ce.tint), [this, sw](const QColor& c, bool final) {
                    sw->setFill(FillStyle::solid(fromQColor(c)));
                    if (final) m_ed->setInstanceProperty([c](InstanceElement& x) { x.color.tint = fromQColor(c); }, tr("Tint"));
                });
            });
            auto fn = [](double v) { return [v](InstanceElement& x) { x.color.tintAmount = v / 100.0; }; };
            row(ig, tr("Tint"), hbox(m_content, {sw, number(m_content, ce.tintAmount * 100, 0, 100, 0, 1, "%",
                                                          [this, fn](double v) { m_ed->setInstanceProperty(fn(v), tr("Tint")); },
                                                          [previewFx, fn](double v) { previewFx(fn(v)); })}));
            break;
        }
        case ColorEffect::Kind::Alpha: {
            auto fn = [](double v) { return [v](InstanceElement& x) { x.color.alpha = v / 100.0; }; };
            row(ig, tr("Alpha"), number(m_content, ce.alpha * 100, 0, 100, 0, 1, "%",
                                        [this, fn](double v) { m_ed->setInstanceProperty(fn(v), tr("Alpha")); },
                                        [previewFx, fn](double v) { previewFx(fn(v)); }));
            break;
        }
        case ColorEffect::Kind::Advanced: {
            const ColorTransform& t = ce.advanced;
            const char* labels[4] = {"R", "G", "B", "A"};
            const double mult[4] = {t.rm, t.gm, t.bm, t.am}, off[4] = {t.ro, t.go, t.bo, t.ao};
            for (int ch = 0; ch < 4; ++ch) {
                auto fm = [ch](double v) {
                    return [ch, v](InstanceElement& x) {
                        double* m[4] = {&x.color.advanced.rm, &x.color.advanced.gm, &x.color.advanced.bm, &x.color.advanced.am};
                        *m[ch] = v / 100.0;
                    };
                };
                auto fo = [ch](double v) {
                    return [ch, v](InstanceElement& x) {
                        double* o[4] = {&x.color.advanced.ro, &x.color.advanced.go, &x.color.advanced.bo, &x.color.advanced.ao};
                        *o[ch] = v;
                    };
                };
                row(ig, QString(labels[ch]),
                    hbox(m_content, {number(m_content, mult[ch] * 100, -100, 100, 0, 1, "%",
                                            [this, fm](double v) { m_ed->setInstanceProperty(fm(v), tr("Advanced Color")); },
                                            [previewFx, fm](double v) { previewFx(fm(v)); }),
                                     number(m_content, off[ch], -255, 255, 0, 1, "",
                                            [this, fo](double v) { m_ed->setInstanceProperty(fo(v), tr("Advanced Color")); },
                                            [previewFx, fo](double v) { previewFx(fo(v)); })}));
            }
            break;
        }
        case ColorEffect::Kind::None: break;
        }
        // Blending (movie clips and buttons, like Animate).
        if (sym && in->behavior != SymbolType::Graphic) {
            auto* bc = blendCombo(m_content, in->blend, [this](BlendMode b) {
                m_ed->setInstanceProperty([b](InstanceElement& x) { x.blend = b; }, tr("Blending"));
            });
            row(ig, tr("Blending"), bc);
            row(ig, {}, check(m_content, tr("Visible"), in->visible, [this](bool b) {
                    m_ed->setInstanceProperty([b](InstanceElement& x) { x.visible = b; }, tr("Visible"));
                }));
        }
        // Looping (graphic symbols).
        if (sym && in->behavior == SymbolType::Graphic) {
            row(ig, tr("Looping"), combo(m_content, {tr("Loop"), tr("Play Once"), tr("Single Frame"), tr("Loop Reverse"), tr("Play Once Reverse")},
                                         int(in->loop), [this](int i) {
                                             m_ed->setInstanceProperty([i](InstanceElement& x) { x.loop = LoopMode(i); }, tr("Looping"));
                                         }));
            const int len = sym->timeline.frameCount();
            row(ig, tr("First / Last"),
                hbox(m_content, {number(m_content, in->firstFrame + 1, 1, len, 0, 0.2, "", [this](double v) {
                                     m_ed->setInstanceProperty([v](InstanceElement& x) { x.firstFrame = int(v) - 1; }, tr("First Frame"));
                                 }),
                                 number(m_content, in->lastFrame < 0 ? len : in->lastFrame + 1, 1, len, 0, 0.2, "", [this, len](double v) {
                                     m_ed->setInstanceProperty([v, len](InstanceElement& x) { x.lastFrame = int(v) >= len ? -1 : int(v) - 1; },
                                                               tr("Last Frame"));
                                 })}));
        }
        auto* edit = new QPushButton(tr("Edit Symbol"), m_content);
        edit->setProperty("accent", true);
        if (sym && in->behavior != SymbolType::Graphic) buildFilters(*in, previewFx);
        connect(edit, &QPushButton::clicked, this, [this]() {
            const ElementRef r = m_ed->selection().front();
            const int li = m_ed->timeline().layerIndex(r.layerId);
            m_ed->enterInstance(li, r.index);
        });
        row(ig, {}, edit);
    }
    // Shapes: fill and stroke apply to the selection.
    bool hasShape = pick.valid();
    for (const ElementPtr& e : els)
        if (e->type() == ElementType::Shape) hasShape = true;
    if (hasShape) buildFillStroke(true);
}

void PropertiesPanel::buildFilters(const InstanceElement& in, const std::function<void(std::function<void(InstanceElement&)>)>& previewFx)
{
    QGridLayout* fg = section(tr("Filters"), in.filters.empty() ? tr("none") : tr("%1").arg(in.filters.size()));
    auto* add = new QToolButton(m_content);
    add->setText(tr("+ Add filter"));
    add->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(add);
    for (int t = 0; t < int(FilterType::Count); ++t) {
        const FilterType type = FilterType(t);
        menu->addAction(QString::fromUtf8(filterLabel(type).data()), this, [this, type]() {
            m_ed->setInstanceProperty([type](InstanceElement& x) { x.filters.push_back(Filter::defaults(type)); }, tr("Add Filter"));
        });
    }
    add->setMenu(menu);
    row(fg, {}, add);

    for (int i = 0; i < int(in.filters.size()); ++i) {
        const Filter& f = in.filters[i];
        auto commit = [this, i](std::function<void(Filter&)> fn, const QString& label) {
            m_ed->setInstanceProperty([i, fn](InstanceElement& x) {
                if (i < int(x.filters.size())) fn(x.filters[i]);
            }, label);
        };
        auto preview = [previewFx, i](std::function<void(Filter&)> fn) {
            previewFx([i, fn](InstanceElement& x) {
                if (i < int(x.filters.size())) fn(x.filters[i]);
            });
        };
        auto num = [&](double value, double lo, double hi, int decimals, double step, const QString& suffix,
                       std::function<void(Filter&, double)> set, const QString& label) {
            return number(m_content, value, lo, hi, decimals, step, suffix,
                          [commit, set, label](double v) { commit([set, v](Filter& x) { set(x, v); }, label); },
                          [preview, set](double v) { preview([set, v](Filter& x) { set(x, v); }); });
        };
        auto swatch = [&](const Color& c, std::function<void(Filter&, Color)> set) {
            auto* sw = new ColorSwatch(m_content);
            sw->setFill(FillStyle::solid(c));
            connect(sw, &ColorSwatch::clicked, this, [this, sw, c, set, commit]() {
                popupColorPicker(sw, toQColor(c), [sw, set, commit](const QColor& q, bool final) {
                    sw->setFill(FillStyle::solid(fromQColor(q)));
                    if (final) commit([set, q](Filter& x) { set(x, fromQColor(q)); }, QObject::tr("Filter Color"));
                });
            });
            return sw;
        };

        // Header: enable, name, remove.
        auto* remove = new QToolButton(m_content);
        remove->setIcon(ui::icon("trash"));
        remove->setToolTip(tr("Remove filter"));
        connect(remove, &QToolButton::clicked, this, [this, i]() {
            m_ed->setInstanceProperty([i](InstanceElement& x) {
                if (i < int(x.filters.size())) x.filters.erase(x.filters.begin() + i);
            }, tr("Remove Filter"));
        });
        row(fg, {}, hbox(m_content, {check(m_content, QString::fromUtf8(filterLabel(f.type).data()), f.enabled,
                                           [commit](bool b) { commit([b](Filter& x) { x.enabled = b; }, QObject::tr("Enable Filter")); }),
                                     remove}));

        if (f.type == FilterType::AdjustColor) {
            row(fg, tr("Brightness"), num(f.brightness, -100, 100, 0, 1, {}, [](Filter& x, double v) { x.brightness = v; }, tr("Brightness")));
            row(fg, tr("Contrast"), num(f.contrast, -100, 100, 0, 1, {}, [](Filter& x, double v) { x.contrast = v; }, tr("Contrast")));
            row(fg, tr("Saturation"), num(f.saturation, -100, 100, 0, 1, {}, [](Filter& x, double v) { x.saturation = v; }, tr("Saturation")));
            row(fg, tr("Hue"), num(f.hue, -180, 180, 0, 1, QStringLiteral("°"), [](Filter& x, double v) { x.hue = v; }, tr("Hue")));
            continue;
        }
        // Blur X / Y with Animate's link lock: while linked both change together.
        auto linked = std::make_shared<bool>(f.blurX == f.blurY);
        auto* lock = new QToolButton(m_content);
        lock->setCheckable(true);
        lock->setChecked(*linked);
        lock->setIcon(ui::icon(*linked ? "lock" : "unlock"));
        lock->setToolTip(tr("Link blur X and Y"));
        connect(lock, &QToolButton::toggled, lock, [lock, linked](bool on) {
            *linked = on;
            lock->setIcon(ui::icon(on ? "lock" : "unlock"));
        });
        auto blurSet = [linked](bool isX) {
            return [linked, isX](Filter& x, double v) {
                if (*linked || isX) x.blurX = v;
                if (*linked || !isX) x.blurY = v;
            };
        };
        row(fg, tr("Blur X / Y"),
            hbox(m_content, {num(f.blurX, 0, 255, 1, 0.5, QStringLiteral(" px"), blurSet(true), tr("Blur")), lock,
                             num(f.blurY, 0, 255, 1, 0.5, QStringLiteral(" px"), blurSet(false), tr("Blur"))}));
        row(fg, tr("Quality"), combo(m_content, {tr("Low"), tr("Medium"), tr("High")}, std::clamp(f.quality, 1, 3) - 1,
                                     [commit](int q) { commit([q](Filter& x) { x.quality = q + 1; }, QObject::tr("Filter Quality")); }));
        if (f.type == FilterType::Blur) continue;
        row(fg, tr("Strength"), num(f.strength * 100, 0, 25500, 0, 5, QStringLiteral("%"), [](Filter& x, double v) { x.strength = v / 100.0; }, tr("Strength")));
        const bool angled = f.type == FilterType::DropShadow || f.type == FilterType::Bevel ||
                            f.type == FilterType::GradientBevel || f.type == FilterType::GradientGlow;
        if (angled) {
            row(fg, tr("Angle / Distance"),
                hbox(m_content, {num(f.angle, -360, 360, 0, 1, QStringLiteral("°"), [](Filter& x, double v) { x.angle = v; }, tr("Angle")),
                                 num(f.distance, -255, 255, 1, 0.5, QStringLiteral(" px"), [](Filter& x, double v) { x.distance = v; }, tr("Distance"))}));
        }
        if (f.type == FilterType::Bevel) {
            row(fg, tr("Shadow / Highlight"),
                hbox(m_content, {swatch(f.color, [](Filter& x, Color c) { x.color = c; }),
                                 swatch(f.highlight, [](Filter& x, Color c) { x.highlight = c; })}));
        } else if (f.type == FilterType::GradientGlow || f.type == FilterType::GradientBevel) {
            if (!f.gradient.stops.empty()) {
                const int last = int(f.gradient.stops.size()) - 1;
                row(fg, tr("Gradient"),
                    hbox(m_content, {swatch(f.gradient.stops.front().color, [](Filter& x, Color c) { x.gradient.stops.front().color = c; }),
                                     swatch(f.gradient.stops[last].color, [last](Filter& x, Color c) {
                                         if (last < int(x.gradient.stops.size())) x.gradient.stops[last].color = c;
                                     })}));
            }
        } else {
            row(fg, tr("Color"), swatch(f.color, [](Filter& x, Color c) { x.color = c; }));
        }
        if (f.type == FilterType::Bevel || f.type == FilterType::GradientBevel) {
            row(fg, tr("Type"), combo(m_content, {tr("Inner"), tr("Outer"), tr("Full")}, int(f.bevel), [commit](int k) {
                    commit([k](Filter& x) { x.bevel = BevelKind(k); }, QObject::tr("Bevel Type"));
                }));
        }
        QList<QWidget*> flags{check(m_content, tr("Knockout"), f.knockout,
                                    [commit](bool b) { commit([b](Filter& x) { x.knockout = b; }, QObject::tr("Knockout")); })};
        if (f.type == FilterType::DropShadow || f.type == FilterType::Glow || f.type == FilterType::GradientGlow)
            flags << check(m_content, tr("Inner"), f.inner, [commit](bool b) { commit([b](Filter& x) { x.inner = b; }, QObject::tr("Inner")); });
        if (f.type == FilterType::DropShadow)
            flags << check(m_content, tr("Hide object"), f.hideObject,
                           [commit](bool b) { commit([b](Filter& x) { x.hideObject = b; }, QObject::tr("Hide Object")); });
        row(fg, {}, hbox(m_content, flags));
    }
}

void PropertiesPanel::buildFrame()
{
    const Layer* l = m_ed->currentLayer();
    if (!l || l->type == LayerType::Folder) return;
    const Keyframe* k = l->keyAt(m_ed->frame());
    if (!k) return;
    QGridLayout* g = section(tr("Frame"), tr("%1 · frames %2–%3").arg(QString::fromStdString(l->name)).arg(k->start + 1).arg(k->end()));
    auto* label = new QLineEdit(QString::fromStdString(k->label), m_content);
    label->setPlaceholderText(tr("Label"));
    connect(label, &QLineEdit::editingFinished, this, [this, label]() {
        const std::string t = label->text().toStdString();
        m_ed->setKeyframeProperty([t](Keyframe& k) { k.label = t; }, tr("Frame Label"));
    });
    row(g, tr("Label"), label);
    row(g, tr("Label type"), combo(m_content, {tr("Name"), tr("Comment"), tr("Anchor")}, int(k->labelType), [this](int i) {
            m_ed->setKeyframeProperty([i](Keyframe& k) { k.labelType = LabelType(i); }, tr("Label Type"));
        }));
    auto* tween = new Segmented(m_content);
    tween->addSegment(tr("None"));
    tween->addSegment(tr("Classic"));
    tween->addSegment(tr("Shape"));
    tween->setCurrent(int(k->tween));
    connect(tween, &Segmented::changed, this, [this](int i) {
        if (i == 0) m_ed->removeTween();
        else m_ed->createTween(TweenType(i));
    });
    row(g, tr("Tween"), tween);
    if (k->tween == TweenType::None) return;

    const Ease ease = k->tween == TweenType::Classic ? k->classic.ease : k->shape.ease;
    const bool classic = k->tween == TweenType::Classic;
    row(g, tr("Ease"), combo(m_content, easeNames(), int(ease.kind), [this, classic](int i) {
            m_ed->setKeyframeProperty([i, classic](Keyframe& k) {
                Ease& e = classic ? k.classic.ease : k.shape.ease;
                e.kind = EaseKind(i);
                if (e.kind == EaseKind::Custom && e.curve.empty()) e.curve = {Cubic{{0, 0}, {0.42, 0}, {0.58, 1}, {1, 1}}};
            }, tr("Ease"));
        }));
    if (ease.kind == EaseKind::Classic)
        row(g, tr("Strength"), number(m_content, ease.strength, -100, 100, 0, 1, "", [this, classic](double v) {
                m_ed->setKeyframeProperty([v, classic](Keyframe& k) { (classic ? k.classic.ease : k.shape.ease).strength = int(v); }, tr("Ease"));
            }));
    if (classic) {
        const ClassicTweenSettings& c = k->classic;
        row(g, tr("Rotate"), hbox(m_content, {combo(m_content, {tr("None"), tr("Auto"), tr("CW"), tr("CCW")}, int(c.rotate), [this](int i) {
                                                  m_ed->setKeyframeProperty([i](Keyframe& k) { k.classic.rotate = RotateMode(i); }, tr("Rotate"));
                                              }),
                                              number(m_content, c.rotations, 0, 100, 0, 0.1, "×", [this](double v) {
                                                  m_ed->setKeyframeProperty([v](Keyframe& k) { k.classic.rotations = int(v); }, tr("Rotations"));
                                              })}));
        row(g, {}, check(m_content, tr("Orient to path"), c.orientToPath, [this](bool b) {
                m_ed->setKeyframeProperty([b](Keyframe& k) { k.classic.orientToPath = b; }, tr("Orient to Path"));
            }));
        row(g, {}, check(m_content, tr("Sync graphic symbols"), c.sync, [this](bool b) {
                m_ed->setKeyframeProperty([b](Keyframe& k) { k.classic.sync = b; }, tr("Sync"));
            }));
        row(g, {}, check(m_content, tr("Scale"), c.scale, [this](bool b) {
                m_ed->setKeyframeProperty([b](Keyframe& k) { k.classic.scale = b; }, tr("Scale"));
            }));
    } else {
        auto* blend = new Segmented(m_content);
        blend->addSegment(tr("Distributive"));
        blend->addSegment(tr("Angular"));
        blend->setCurrent(k->shape.angular ? 1 : 0);
        connect(blend, &Segmented::changed, this, [this](int i) {
            m_ed->setKeyframeProperty([i](Keyframe& k) { k.shape.angular = i == 1; }, tr("Shape Blend"));
        });
        row(g, tr("Blend"), blend);
        auto* add = new QPushButton(tr("Add Shape Hint"), m_content);
        connect(add, &QPushButton::clicked, m_ed, &Editor::addShapeHint);
        auto* clear = new QPushButton(tr("Remove All"), m_content);
        connect(clear, &QPushButton::clicked, m_ed, &Editor::removeShapeHints);
        row(g, tr("Hints (%1)").arg(k->hints.size()), hbox(m_content, {add, clear}));
    }
}

void PropertiesPanel::buildLayer()
{
    const int li = m_ed->layerIndex();
    const Layer* l = m_ed->currentLayer();
    if (!l) return;
    QGridLayout* g = section(tr("Layer"));
    auto* name = new QLineEdit(QString::fromStdString(l->name), m_content);
    connect(name, &QLineEdit::editingFinished, this, [this, li, name]() { m_ed->renameLayer(li, name->text()); });
    row(g, tr("Name"), name);
    row(g, tr("Type"), combo(m_content, {tr("Normal"), tr("Guide"), tr("Mask"), tr("Folder")}, int(l->type), [this, li](int i) {
            m_ed->setLayerProperty(li, [i](Layer& x) { x.type = LayerType(i); }, tr("Layer Type"));
        }));
    if (l->type != LayerType::Folder) {
        row(g, tr("Blending"), blendCombo(m_content, l->blend, [this, li](BlendMode b) {
                m_ed->setLayerProperty(li, [b](Layer& x) { x.blend = b; }, tr("Layer Blending"));
            }));
        row(g, tr("Opacity"), number(m_content, l->opacity * 100, 0, 100, 0, 1, "%", [this, li](double v) {
                m_ed->setLayerProperty(li, [v](Layer& x) { x.opacity = v / 100.0; }, tr("Layer Opacity"));
            }));
    }
}

void PropertiesPanel::buildDocument()
{
    const Document& d = m_ed->doc();
    QGridLayout* g = section(tr("Document"));
    auto set = [this](double w, double h, double fps, Color bg) { m_ed->setStageSettings(w, h, fps, bg); };
    row(g, tr("Stage"),
        hbox(m_content, {number(m_content, d.width, 1, 16384, 0, 1, "", [this, set](double v) {
                             const Document& x = m_ed->doc();
                             set(v, x.height, x.fps, x.background);
                         }),
                         number(m_content, d.height, 1, 16384, 0, 1, "", [this, set](double v) {
                             const Document& x = m_ed->doc();
                             set(x.width, v, x.fps, x.background);
                         })}));
    row(g, tr("FPS"), number(m_content, d.fps, 1, 120, 2, 0.25, "", [this, set](double v) {
            const Document& x = m_ed->doc();
            set(x.width, x.height, v, x.background);
        }));
    auto* bg = new ColorSwatch(m_content);
    bg->setFill(FillStyle::solid(d.background));
    connect(bg, &ColorSwatch::clicked, this, [this, bg]() {
        popupColorPicker(bg, toQColor(m_ed->doc().background), [this, bg](const QColor& c, bool final) {
            bg->setFill(FillStyle::solid(fromQColor(c)));
            if (final) {
                const Document& x = m_ed->doc();
                m_ed->setStageSettings(x.width, x.height, x.fps, fromQColor(c));
            }
        });
    });
    row(g, tr("Background"), hbox(m_content, {bg}));
}

} // namespace vx::app
