// SPDX-License-Identifier: GPL-3.0-or-later
#include "ColorPanel.h"
#include "../Theme.h"
#include "../Widgets.h"

#include "render/QtConvert.h"

#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

namespace vx::app {

using ui::Theme;

namespace {

constexpr int kCell = 20;
constexpr int kGap = 4;

} // namespace

SwatchGrid::SwatchGrid(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    // Greys, then a vivid curated palette (hue sweep in three values).
    for (int i = 0; i <= 10; ++i) m_colors.push_back(QColor::fromHsvF(0, 0, float(i / 10.0)));
    const double hues[] = {0.0, 0.03, 0.07, 0.11, 0.15, 0.22, 0.33, 0.42, 0.5, 0.55, 0.6, 0.66, 0.73, 0.8, 0.88, 0.94};
    for (double v : {1.0, 0.8, 0.55})
        for (double h : hues) m_colors.push_back(QColor::fromHsvF(float(h), float(v == 1.0 ? 0.55 : 0.85), float(v)));
    for (const char* c : {"#FF5B2E", "#FFC42E", "#2BD9A8", "#3D8BFF", "#8B6CFF", "#FF4FA3", "#14141A", "#F3F3F6"})
        m_colors.push_back(QColor(c));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void SwatchGrid::addRecent(const QColor& c)
{
    m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), c), m_recent.end());
    m_recent.insert(m_recent.begin(), c);
    if (m_recent.size() > 10) m_recent.pop_back();
    updateGeometry();
    update();
}

QSize SwatchGrid::sizeHint() const { return {240, heightForWidth(240)}; }

int SwatchGrid::heightForWidth(int w) const
{
    const int cols = std::max(1, (w + kGap) / (kCell + kGap));
    const int rows = int((m_colors.size() + cols - 1) / cols) + (m_recent.empty() ? 0 : 1);
    return rows * (kCell + kGap) + 22;
}

QRectF SwatchGrid::cellRect(int i) const
{
    const int cols = std::max(1, (width() + kGap) / (kCell + kGap));
    int row, col;
    double top = 0;
    if (!m_recent.empty()) {
        if (i < int(m_recent.size())) {
            return QRectF(i * (kCell + kGap), 18, kCell, kCell);
        }
        i -= int(m_recent.size());
        top = kCell + kGap + 18;
    } else {
        top = 18;
    }
    row = i / cols;
    col = i % cols;
    return QRectF(col * (kCell + kGap), top + row * (kCell + kGap), kCell, kCell);
}

int SwatchGrid::indexAt(QPointF p) const
{
    const int n = int(m_recent.size() + m_colors.size());
    for (int i = 0; i < n; ++i)
        if (cellRect(i).contains(p)) return i;
    return -1;
}

void SwatchGrid::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ui::Palette& pal = Theme::p();
    p.setFont(Theme::ui(10, QFont::Bold));
    p.setPen(pal.text3);
    p.drawText(QPointF(0, 11), m_recent.empty() ? tr("SWATCHES") : tr("RECENT"));
    const int n = int(m_recent.size() + m_colors.size());
    for (int i = 0; i < n; ++i) {
        const QColor c = i < int(m_recent.size()) ? m_recent[i] : m_colors[i - m_recent.size()];
        const QRectF r = cellRect(i);
        p.setPen(i == m_hover ? QPen(pal.text, 2) : QPen(pal.line, 1));
        p.setBrush(c);
        p.drawRoundedRect(i == m_hover ? r.adjusted(-1.5, -1.5, 1.5, 1.5) : r, 6, 6);
    }
}

void SwatchGrid::mousePressEvent(QMouseEvent* e)
{
    const int i = indexAt(e->position());
    if (i < 0) return;
    emit picked(i < int(m_recent.size()) ? m_recent[i] : m_colors[i - m_recent.size()]);
}

void SwatchGrid::mouseMoveEvent(QMouseEvent* e)
{
    const int h = indexAt(e->position());
    if (h != m_hover) {
        m_hover = h;
        update();
    }
}

void SwatchGrid::leaveEvent(QEvent*)
{
    m_hover = -1;
    update();
}

ColorPanel::ColorPanel(Editor* editor, QWidget* parent) : QScrollArea(parent), m_ed(editor)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget();
    auto* lay = new QVBoxLayout(content);
    lay->setContentsMargins(10, 8, 14, 10);
    lay->setSpacing(10);
    lay->addWidget(new SectionTitle(tr("Color"), content));
    m_target = new Segmented(content);
    m_target->addSegment(tr("Fill"), "bucket");
    m_target->addSegment(tr("Stroke"), "inkbottle");
    lay->addWidget(m_target);
    m_picker = new ColorPicker(content);
    m_picker->setFixedHeight(236);
    lay->addWidget(m_picker);
    m_swatches = new SwatchGrid(content);
    lay->addWidget(m_swatches);
    lay->addStretch(1);
    setWidget(content);
    connect(m_target, &Segmented::changed, this, &ColorPanel::sync);
    connect(m_picker, &ColorPicker::colorChanged, this, [this](const QColor& c) { apply(c, false); });
    connect(m_picker, &ColorPicker::colorCommitted, this, [this](const QColor& c) { apply(c, true); });
    connect(m_swatches, &SwatchGrid::picked, this, [this](const QColor& c) {
        m_picker->setColor(c);
        apply(c, true);
    });
    connect(m_ed, &Editor::settingsChanged, this, &ColorPanel::sync);
    sync();
}

void ColorPanel::sync()
{
    if (m_syncing) return;
    const ToolSettings& s = m_ed->settings();
    const Color c = m_target->current() == 0 ? s.fill.mainColor() : s.stroke.paint.mainColor();
    m_syncing = true;
    m_picker->setColor(toQColor(c));
    m_syncing = false;
}

void ColorPanel::apply(const QColor& c, bool final)
{
    if (m_syncing) return;
    m_syncing = true;
    if (m_target->current() == 0) {
        FillStyle f = m_ed->settings().fill;
        if (f.kind == FillStyle::Kind::Solid) f.color = fromQColor(c);
        else if (!f.gradient.stops.empty()) f.gradient.stops.front().color = fromQColor(c);
        m_ed->settings().fillEnabled = true;
        if (final) m_ed->applyFillToSelection(f);
        else {
            m_ed->settings().fill = f;
            m_ed->emitSettingsChanged();
        }
    } else {
        StrokeStyle s = m_ed->settings().stroke;
        s.paint = FillStyle::solid(fromQColor(c));
        m_ed->settings().strokeEnabled = true;
        if (final) m_ed->applyStrokeToSelection(s);
        else {
            m_ed->settings().stroke = s;
            m_ed->emitSettingsChanged();
        }
    }
    if (final) m_swatches->addRecent(c);
    m_syncing = false;
}

} // namespace vx::app
