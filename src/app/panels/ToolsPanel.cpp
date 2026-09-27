// SPDX-License-Identifier: GPL-3.0-or-later
#include "ToolsPanel.h"
#include "../Icons.h"
#include "../Theme.h"
#include "../Widgets.h"

#include "render/QtConvert.h"

#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QVariantAnimation>

namespace vx::app {

using ui::Theme;

namespace {

const std::vector<std::vector<ToolId>> kGroups = {
    {ToolId::Selection, ToolId::Subselection, ToolId::FreeTransform, ToolId::Lasso},
    {ToolId::Pen, ToolId::Line, ToolId::Rectangle, ToolId::Oval, ToolId::PolyStar},
    {ToolId::Pencil, ToolId::Brush, ToolId::PaintBrush, ToolId::Eraser},
    {ToolId::PaintBucket, ToolId::InkBottle, ToolId::Eyedropper},
    {ToolId::Hand, ToolId::Zoom},
};

constexpr double kItemH = 36.0;
constexpr double kTop = 10.0;

} // namespace

QString ToolsPanel::shortcutFor(ToolId id)
{
    switch (id) {
    case ToolId::Selection: return "V";
    case ToolId::Subselection: return "A";
    case ToolId::FreeTransform: return "Q";
    case ToolId::Lasso: return "L";
    case ToolId::Pen: return "P";
    case ToolId::Line: return "N";
    case ToolId::Rectangle: return "R";
    case ToolId::Oval: return "O";
    case ToolId::PolyStar: return "Shift+R";
    case ToolId::Pencil: return "Shift+Y";
    case ToolId::Brush: return "B";
    case ToolId::PaintBrush: return "Y";
    case ToolId::Eraser: return "E";
    case ToolId::PaintBucket: return "K";
    case ToolId::InkBottle: return "S";
    case ToolId::Eyedropper: return "I";
    case ToolId::Hand: return "H";
    case ToolId::Zoom: return "Z";
    case ToolId::Count: break;
    }
    return {};
}

QString ToolsPanel::iconFor(ToolId id)
{
    switch (id) {
    case ToolId::Selection: return "select";
    case ToolId::Subselection: return "subselect";
    case ToolId::FreeTransform: return "transform";
    case ToolId::Lasso: return "lasso";
    case ToolId::Pen: return "pen";
    case ToolId::Line: return "line";
    case ToolId::Rectangle: return "rect";
    case ToolId::Oval: return "oval";
    case ToolId::PolyStar: return "polystar";
    case ToolId::Pencil: return "pencil";
    case ToolId::Brush: return "brush";
    case ToolId::PaintBrush: return "paintbrush";
    case ToolId::Eraser: return "eraser";
    case ToolId::PaintBucket: return "bucket";
    case ToolId::InkBottle: return "inkbottle";
    case ToolId::Eyedropper: return "eyedropper";
    case ToolId::Hand: return "hand";
    case ToolId::Zoom: return "zoom";
    case ToolId::Count: break;
    }
    return {};
}

ToolsPanel::ToolsPanel(Editor* editor, QWidget* parent) : QWidget(parent), m_ed(editor)
{
    setMouseTracking(true);
    setFixedWidth(56);
    m_anim = new QVariantAnimation(this);
    m_anim->setDuration(260);
    m_anim->setEasingCurve(QEasingCurve::OutBack);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_indicatorY = v.toDouble();
        update();
    });
    m_stroke = new ColorSwatch(this);
    m_stroke->setStrokeLook(true);
    m_fill = new ColorSwatch(this);
    m_stroke->setToolTip(tr("Stroke colour"));
    m_fill->setToolTip(tr("Fill colour"));
    connect(m_fill, &ColorSwatch::clicked, this, [this]() {
        const QColor init = toQColor(m_ed->settings().fill.mainColor());
        popupColorPicker(m_fill, init, [this](const QColor& c, bool final) {
            FillStyle f = m_ed->settings().fill;
            if (f.kind == FillStyle::Kind::Solid) f.color = fromQColor(c);
            else if (!f.gradient.stops.empty()) f.gradient.stops.front().color = fromQColor(c);
            m_ed->settings().fillEnabled = true;
            if (final) m_ed->applyFillToSelection(f);
            else {
                m_ed->settings().fill = f;
                m_ed->emitSettingsChanged();
            }
        });
    });
    connect(m_stroke, &ColorSwatch::clicked, this, [this]() {
        const QColor init = toQColor(m_ed->settings().stroke.paint.mainColor());
        popupColorPicker(m_stroke, init, [this](const QColor& c, bool final) {
            StrokeStyle s = m_ed->settings().stroke;
            s.paint = FillStyle::solid(fromQColor(c));
            m_ed->settings().strokeEnabled = true;
            if (final) m_ed->applyStrokeToSelection(s);
            else {
                m_ed->settings().stroke = s;
                m_ed->emitSettingsChanged();
            }
        });
    });
    connect(m_ed, &Editor::toolChanged, this, [this](ToolId) {
        for (const Item& it : m_items)
            if (it.id == m_ed->tool()) {
                m_anim->stop();
                m_anim->setStartValue(m_indicatorY);
                m_anim->setEndValue(it.rect.top());
                m_anim->start();
            }
        update();
    });
    connect(m_ed, &Editor::settingsChanged, this, &ToolsPanel::syncSwatches);
    connect(Theme::instance(), &ui::Theme::changed, this, qOverload<>(&QWidget::update));
    layoutItems();
    syncSwatches();
}

QSize ToolsPanel::sizeHint() const { return {56, 760}; }

void ToolsPanel::layoutItems()
{
    m_items.clear();
    m_separators.clear();
    double y = kTop;
    for (size_t g = 0; g < kGroups.size(); ++g) {
        if (g > 0) {
            m_separators.push_back(y + 5);
            y += 11;
        }
        for (ToolId id : kGroups[g]) {
            m_items.push_back({id, QRectF(6, y, width() - 12, kItemH)});
            y += kItemH + 2;
        }
    }
    for (const Item& it : m_items)
        if (it.id == m_ed->tool()) m_indicatorY = it.rect.top();
    const double sy = y + 18;
    m_stroke->move(8, int(sy));
    m_fill->move(18, int(sy + 12));
    m_swapButton = QRectF(8, sy + 46, 18, 18);
    m_objectToggle = QRectF(8, sy + 72, width() - 16, 34);
}

void ToolsPanel::resizeEvent(QResizeEvent*) { layoutItems(); }

void ToolsPanel::syncSwatches()
{
    const ToolSettings& s = m_ed->settings();
    m_fill->setFill(s.fill, s.fillEnabled);
    m_stroke->setFill(s.stroke.paint, s.strokeEnabled);
    update();
}

int ToolsPanel::itemAt(QPointF p) const
{
    for (int i = 0; i < int(m_items.size()); ++i)
        if (m_items[i].rect.contains(p)) return i;
    return -1;
}

void ToolsPanel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ui::Palette& pal = Theme::p();
    p.fillRect(rect(), pal.bg1);
    // Sliding highlight.
    const QRectF ind(6, m_indicatorY, width() - 12, kItemH);
    p.setPen(Qt::NoPen);
    p.setBrush(ui::withAlpha(pal.accent, 44));
    p.drawRoundedRect(ind, 9, 9);
    p.setBrush(pal.accent);
    p.drawRoundedRect(QRectF(1, ind.top() + 8, 3.5, ind.height() - 16), 2, 2);
    for (int i = 0; i < int(m_items.size()); ++i) {
        const Item& it = m_items[i];
        const bool active = it.id == m_ed->tool();
        if (i == m_hover && !active) {
            p.setBrush(pal.bg3);
            p.drawRoundedRect(it.rect, 9, 9);
        }
        const QColor c = active ? pal.accent : (i == m_hover ? pal.text : pal.text2);
        const QPointF ctr = it.rect.center();
        ui::paintIcon(p, iconFor(it.id), QRectF(ctr.x() - 11, ctr.y() - 11, 22, 22), c);
    }
    p.setPen(QPen(pal.line, 1));
    for (double y : m_separators) p.drawLine(QPointF(14, y), QPointF(width() - 14, y));
    // Swap colours.
    ui::paintIcon(p, "swap", m_swapButton, pal.text3);
    // Object drawing toggle (J).
    const bool obj = m_ed->settings().objectDrawing;
    p.setPen(Qt::NoPen);
    p.setBrush(obj ? ui::withAlpha(pal.accent, 44) : Qt::transparent);
    p.drawRoundedRect(m_objectToggle, 9, 9);
    const QPointF oc = m_objectToggle.center();
    ui::paintIcon(p, "object", QRectF(oc.x() - 10, oc.y() - 10, 20, 20), obj ? pal.accent : pal.text3);
}

void ToolsPanel::mousePressEvent(QMouseEvent* e)
{
    const int i = itemAt(e->position());
    if (i >= 0) {
        m_ed->setTool(m_items[i].id);
        return;
    }
    if (m_swapButton.adjusted(-4, -4, 4, 4).contains(e->position())) {
        ToolSettings& s = m_ed->settings();
        const Color f = s.fill.mainColor(), st = s.stroke.paint.mainColor();
        s.fill = FillStyle::solid(st);
        s.stroke.paint = FillStyle::solid(f);
        m_ed->emitSettingsChanged();
        return;
    }
    if (m_objectToggle.contains(e->position())) {
        m_ed->settings().objectDrawing = !m_ed->settings().objectDrawing;
        m_ed->emitSettingsChanged();
        update();
    }
}

void ToolsPanel::mouseMoveEvent(QMouseEvent* e)
{
    const int h = itemAt(e->position());
    if (h != m_hover) {
        m_hover = h;
        update();
    }
}

void ToolsPanel::leaveEvent(QEvent*)
{
    m_hover = -1;
    update();
}

bool ToolsPanel::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        const int i = itemAt(he->pos());
        if (i >= 0) {
            const ToolId id = m_items[i].id;
            QToolTip::showText(he->globalPos(), QString("<b>%1</b>&nbsp;&nbsp;<span style='color:%2'>%3</span>")
                                                     .arg(Editor::toolName(id), Theme::p().text3.name(), shortcutFor(id)),
                               this);
        } else if (m_objectToggle.contains(he->pos())) {
            QToolTip::showText(he->globalPos(), tr("<b>Object Drawing</b>&nbsp;&nbsp;J"), this);
        } else if (m_swapButton.adjusted(-4, -4, 4, 4).contains(he->pos())) {
            QToolTip::showText(he->globalPos(), tr("<b>Swap Colours</b>&nbsp;&nbsp;X"), this);
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QWidget::event(e);
}

} // namespace vx::app
