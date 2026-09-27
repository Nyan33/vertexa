// SPDX-License-Identifier: GPL-3.0-or-later
#include "TimelinePanel.h"
#include "../Icons.h"
#include "../Theme.h"
#include "../Widgets.h"

#include "render/QtConvert.h"

#include <QAction>
#include <QUndoStack>
#include <QContextMenuEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace vx::app {

using ui::Theme;

// --- TimelineView ---------------------------------------------------------------------------

TimelineView::TimelineView(Editor* editor, ActionLookup actions, QWidget* parent)
    : QWidget(parent), m_ed(editor), m_actions(std::move(actions))
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(120);
    auto refresh = [this]() {
        rebuildRows();
        update();
        emit scrollRangeChanged();
    };
    connect(m_ed, &Editor::documentChanged, this, refresh);
    connect(m_ed, &Editor::contextChanged, this, refresh);
    connect(m_ed, &Editor::layerChanged, this, qOverload<>(&QWidget::update));
    connect(m_ed, &Editor::frameSelectionChanged, this, qOverload<>(&QWidget::update));
    connect(m_ed, &Editor::loopChanged, this, qOverload<>(&QWidget::update));
    connect(m_ed, &Editor::frameChanged, this, [this]() {
        if (m_ed->isPlaying()) ensurePlayheadVisible();
        update();
    });
    connect(Theme::instance(), &ui::Theme::changed, this, qOverload<>(&QWidget::update));
    rebuildRows();
}

void TimelineView::rebuildRows()
{
    m_rows.clear();
    const Timeline& tl = m_ed->timeline();
    for (int i = 0; i < int(tl.layers.size()); ++i)
        if (!tl.isCollapsed(i)) m_rows.push_back(i);
}

int TimelineView::contentWidth() const
{
    const int frames = std::max(m_ed->timeline().frameCount() + 60, int(width() / m_cellW) + 10);
    return int(frames * m_cellW);
}

int TimelineView::contentHeight() const { return int(m_rows.size()) * m_rowH + 8; }

void TimelineView::setScroll(int x, int y)
{
    m_scrollX = std::max(0, x);
    m_scrollY = std::max(0, y);
    update();
}

void TimelineView::setCellWidth(double w)
{
    m_cellW = std::clamp(w, 5.0, 48.0);
    update();
    emit scrollRangeChanged();
}

void TimelineView::ensurePlayheadVisible()
{
    const double x = m_ed->frame() * m_cellW;
    const int visible = width() - m_layersW;
    if (x < m_scrollX || x > m_scrollX + visible - m_cellW * 2)
        emit scrollRequested(int(std::max(0.0, x - visible * 0.25)), m_scrollY);
}

void TimelineView::resizeEvent(QResizeEvent*) { emit scrollRangeChanged(); }

int TimelineView::rowAt(double y) const
{
    if (y < m_rulerH) return -1;
    const int r = int((y - m_rulerH + m_scrollY) / m_rowH);
    return (r >= 0 && r < int(m_rows.size())) ? r : -1;
}

int TimelineView::frameAt(double x) const { return std::max(0, int(std::floor((x - m_layersW + m_scrollX) / m_cellW))); }
double TimelineView::frameX(int f) const { return m_layersW + f * m_cellW - m_scrollX; }
double TimelineView::rowY(int row) const { return m_rulerH + row * m_rowH - m_scrollY; }

TimelineView::Toggle TimelineView::toggleAt(QPointF p, int row) const
{
    if (row < 0 || p.x() > m_layersW) return Toggle::None;
    const Timeline& tl = m_ed->timeline();
    const int li = m_rows[row];
    const double y = rowY(row);
    const QRectF eye(m_layersW - 80, y + 5, 22, 20), lock(m_layersW - 56, y + 5, 22, 20), outl(m_layersW - 32, y + 5, 22, 20);
    if (eye.contains(p)) return Toggle::Visible;
    if (lock.contains(p)) return Toggle::Lock;
    if (outl.contains(p)) return Toggle::Outline;
    const double indent = tl.depth(li) * 14.0;
    if (tl.layers[li].type == LayerType::Folder && QRectF(4 + indent, y + 5, 18, 20).contains(p)) return Toggle::Expand;
    return Toggle::None;
}

void TimelineView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const ui::Palette& pal = Theme::p();
    p.fillRect(rect(), pal.bg1);
    paintGrid(p);
    paintRuler(p);
    paintLayerColumn(p);
}

void TimelineView::paintRuler(QPainter& p)
{
    const ui::Palette& pal = Theme::p();
    p.save();
    p.setClipRect(QRect(m_layersW, 0, width() - m_layersW, m_rulerH));
    p.fillRect(QRect(m_layersW, 0, width(), m_rulerH), pal.bg1);
    const int first = frameAt(m_layersW), last = frameAt(width()) + 1;
    // A button symbol's first four frames are its states, as in Animate.
    const Symbol* sym = m_ed->inSymbol() ? m_ed->doc().symbol(m_ed->contextStack().back().symbolId) : nullptr;
    const bool button = sym && sym->type == SymbolType::Button;
    const QString states[4] = {tr("Up"), tr("Over"), tr("Down"), tr("Hit")};
    p.setFont(Theme::ui(10, QFont::DemiBold));
    for (int f = first; f <= last; ++f) {
        const double x = frameX(f);
        const bool major = (f + 1) % 5 == 0 || f == 0;
        p.setPen(QPen(major ? pal.text3 : pal.line, 1));
        p.drawLine(QPointF(x + 0.5, m_rulerH - (major ? 8 : 4)), QPointF(x + 0.5, m_rulerH));
        if (button && f >= 0 && f < 4) {
            const QString& full = states[f];
            const bool fits = QFontMetrics(p.font()).horizontalAdvance(full) + 2 <= m_cellW;
            p.setPen(pal.accent);
            p.drawText(QRectF(x, 2, m_cellW, m_rulerH - 10), Qt::AlignHCenter | Qt::AlignVCenter, fits ? full : full.left(1));
            continue;
        }
        const int step = m_cellW < 9 ? 10 : 5;
        if ((f + 1) % step == 0 || f == 0) {
            p.setPen(pal.text2);
            p.drawText(QRectF(x - 10, 2, m_cellW + 20, m_rulerH - 10), Qt::AlignHCenter | Qt::AlignVCenter, QString::number(f + 1));
        }
    }
    // Loop range: a bracket along the top of the ruler (dragged by its ends
    // or its middle).
    if (m_ed->loopPlayback()) {
        const double x0 = frameX(m_ed->loopStart()), x1 = frameX(m_ed->loopEnd()) + m_cellW;
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(ui::withAlpha(pal.accent, 40));
        p.drawRect(QRectF(x0, 0, x1 - x0, m_rulerH));
        p.setBrush(pal.accent);
        p.drawRoundedRect(QRectF(x0, 0, x1 - x0, 5), 2, 2);
        p.drawRect(QRectF(x0, 0, 3, m_rulerH - 4));
        p.drawRect(QRectF(x1 - 3, 0, 3, m_rulerH - 4));
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    // Playhead pill.
    const int f = m_ed->frame();
    const double cx = frameX(f) + m_cellW / 2;
    const QString label = QString::number(f + 1);
    const QFont font = Theme::ui(11, QFont::Bold);
    p.setFont(font);
    const double w = std::max(24.0, QFontMetrics(font).horizontalAdvance(label) + 14.0);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(pal.accent);
    p.drawRoundedRect(QRectF(cx - w / 2, 3, w, m_rulerH - 8), 7, 7);
    p.setPen(pal.accentInk);
    p.drawText(QRectF(cx - w / 2, 3, w, m_rulerH - 8), Qt::AlignCenter, label);
    p.restore();
    p.setPen(QPen(pal.line, 1));
    p.drawLine(QPointF(0, m_rulerH - 0.5), QPointF(width(), m_rulerH - 0.5));
}

void TimelineView::paintGrid(QPainter& p)
{
    const ui::Palette& pal = Theme::p();
    const Timeline& tl = m_ed->timeline();
    p.save();
    p.setClipRect(QRect(m_layersW, m_rulerH, width() - m_layersW, height() - m_rulerH));
    const int first = frameAt(m_layersW), last = frameAt(width()) + 1;
    const FrameSelection& sel = m_ed->frameSelection();
    for (int row = 0; row < int(m_rows.size()); ++row) {
        const double y = rowY(row);
        if (y > height() || y + m_rowH < m_rulerH) continue;
        const int li = m_rows[row];
        const Layer& l = tl.layers[li];
        const QRectF rowRect(m_layersW, y, width() - m_layersW, m_rowH);
        if (li == m_ed->layerIndex()) p.fillRect(rowRect, ui::withAlpha(pal.accent, 14));
        // Frame cells.
        for (int f = first; f <= last; ++f) {
            const double x = frameX(f);
            if ((f + 1) % 5 == 0) p.fillRect(QRectF(x, y, m_cellW, m_rowH), ui::withAlpha(pal.text, 6));
        }
        if (l.type == LayerType::Folder) {
            p.fillRect(QRectF(m_layersW, y + m_rowH / 2.0 - 1, width(), 2), ui::withAlpha(pal.text3, 60));
            p.setPen(QPen(pal.line, 1));
            p.drawLine(QPointF(m_layersW, y + m_rowH - 0.5), QPointF(width(), y + m_rowH - 0.5));
            continue;
        }
        p.setRenderHint(QPainter::Antialiasing);
        for (size_t ki = 0; ki < l.keys.size(); ++ki) {
            const Keyframe& k = l.keys[ki];
            if (k.end() < first || k.start > last) continue;
            const QRectF span(frameX(k.start) + 1, y + 3, k.duration * m_cellW - 2, m_rowH - 6);
            QColor fill = k.isEmpty() ? ui::withAlpha(pal.text, 10) : ui::mix(pal.bg2, pal.bg3, 0.6);
            const Keyframe* next = ki + 1 < l.keys.size() ? &l.keys[ki + 1] : nullptr;
            const bool validTween = next && !next->isEmpty() && !k.isEmpty();
            if (k.tween == TweenType::Classic) fill = ui::withAlpha(pal.violet, validTween ? 70 : 38);
            if (k.tween == TweenType::Shape) fill = ui::withAlpha(pal.mint, validTween ? 70 : 38);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(span, 5, 5);
            const double cy = y + m_rowH / 2.0;
            // Tween arrow.
            if (k.tween != TweenType::None && k.duration > 1) {
                const QColor tc = k.tween == TweenType::Classic ? pal.violet : pal.mint;
                const double x0 = frameX(k.start) + m_cellW, x1 = frameX(k.end()) - 3;
                QPen pen(tc, 1.4);
                if (!validTween) pen.setStyle(Qt::DashLine);
                p.setPen(pen);
                p.drawLine(QPointF(x0, cy), QPointF(x1, cy));
                if (validTween) {
                    p.setBrush(tc);
                    p.setPen(Qt::NoPen);
                    QPainterPath head;
                    head.moveTo(x1, cy);
                    head.lineTo(x1 - 6, cy - 4);
                    head.lineTo(x1 - 6, cy + 4);
                    head.closeSubpath();
                    p.drawPath(head);
                }
            }
            // Keyframe dot.
            const QPointF kc(frameX(k.start) + m_cellW / 2, cy);
            const double r = std::min(4.5, m_cellW * 0.32);
            if (k.isEmpty()) {
                p.setPen(QPen(pal.text2, 1.4));
                p.setBrush(Qt::NoBrush);
            } else {
                p.setPen(Qt::NoPen);
                p.setBrush(pal.text);
            }
            p.drawEllipse(kc, r, r);
            // Span end marker.
            if (k.duration > 1 && k.tween == TweenType::None) {
                const double ex = frameX(k.end() - 1) + m_cellW / 2;
                p.setPen(Qt::NoPen);
                p.setBrush(pal.text3);
                p.drawRoundedRect(QRectF(ex - 1.5, cy - 5, 3, 10), 1.5, 1.5);
            }
            // Label.
            if (!k.label.empty() && k.duration * m_cellW > 24) {
                p.setPen(Qt::NoPen);
                p.setBrush(k.labelType == LabelType::Comment ? pal.mint : pal.yellow);
                const double lx = frameX(k.start) + m_cellW + 2;
                QPainterPath flag;
                flag.moveTo(lx, y + 7);
                flag.lineTo(lx + 8, y + 10);
                flag.lineTo(lx, y + 13);
                flag.closeSubpath();
                p.drawPath(flag);
                p.setPen(pal.text);
                p.setFont(Theme::ui(10.5, QFont::DemiBold));
                p.drawText(QRectF(lx + 11, y + 3, k.duration * m_cellW - m_cellW - 16, m_rowH - 6),
                           Qt::AlignVCenter | Qt::AlignLeft, QString::fromStdString(k.label));
            }
            if (!k.hints.empty()) {
                p.setPen(Qt::NoPen);
                p.setBrush(pal.yellow);
                p.drawEllipse(QPointF(frameX(k.start) + m_cellW / 2, y + 6), 2.5, 2.5);
            }
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(pal.line, 1));
        p.drawLine(QPointF(m_layersW, y + m_rowH - 0.5), QPointF(width(), y + m_rowH - 0.5));
        // Frame selection.
        if (sel.valid() && li >= sel.layerFrom && li <= sel.layerTo) {
            const QRectF sr(frameX(sel.frameFrom), y + 1, (sel.frameTo - sel.frameFrom + 1) * m_cellW, m_rowH - 2);
            p.fillRect(sr, ui::withAlpha(pal.accent, 60));
            p.setPen(QPen(pal.accent, 1));
            p.drawRect(sr.adjusted(0, 0, -1, -1));
        }
    }
    // Drag ghost for frame moves.
    if (m_drag == Drag::MoveFrames && m_moveDelta != 0 && m_pressRow >= 0) {
        const FrameSelection& s = m_ed->frameSelection();
        const int from = s.valid() ? s.frameFrom : m_pressFrame, to = s.valid() ? s.frameTo : m_pressFrame;
        const QRectF g(frameX(from + m_moveDelta), rowY(m_pressRow) + 2, (to - from + 1) * m_cellW, m_rowH - 4);
        p.setPen(QPen(pal.accent, 1.5, Qt::DashLine));
        p.setBrush(ui::withAlpha(pal.accent, 30));
        p.drawRect(g);
    }
    // Playhead line.
    const double px = frameX(m_ed->frame()) + m_cellW / 2;
    p.fillRect(QRectF(frameX(m_ed->frame()), m_rulerH, m_cellW, height()), ui::withAlpha(pal.accent, 18));
    p.setPen(QPen(pal.accent, 1.5));
    p.drawLine(QPointF(px, m_rulerH), QPointF(px, height()));
    // Onion skin range markers.
    if (m_ed->onionSkin) {
        p.setRenderHint(QPainter::Antialiasing);
        const double a = frameX(std::max(0, m_ed->frame() - m_ed->onionBefore)), b = frameX(m_ed->frame() + m_ed->onionAfter + 1);
        p.setPen(Qt::NoPen);
        p.setBrush(ui::withAlpha(pal.selection, 60));
        p.drawRect(QRectF(a, m_rulerH, px - a, 3));
        p.setBrush(ui::withAlpha(pal.mint, 60));
        p.drawRect(QRectF(px, m_rulerH, b - px, 3));
    }
    p.restore();
}

void TimelineView::paintLayerColumn(QPainter& p)
{
    const ui::Palette& pal = Theme::p();
    const Timeline& tl = m_ed->timeline();
    p.save();
    p.fillRect(QRect(0, 0, m_layersW, height()), pal.bg1);
    // Column header.
    p.setFont(Theme::ui(10, QFont::Bold));
    p.setPen(pal.text3);
    p.drawText(QRect(14, 0, 120, m_rulerH), Qt::AlignVCenter, m_ed->inSymbol() ? tr("SYMBOL LAYERS") : tr("LAYERS"));
    ui::paintIcon(p, "eye", QRectF(m_layersW - 77, 6, 16, 16), pal.text3);
    ui::paintIcon(p, "lock", QRectF(m_layersW - 53, 6, 16, 16), pal.text3);
    ui::paintIcon(p, "outline", QRectF(m_layersW - 29, 6, 16, 16), pal.text3);
    p.setClipRect(QRect(0, m_rulerH, m_layersW, height() - m_rulerH));
    p.setRenderHint(QPainter::Antialiasing);
    for (int row = 0; row < int(m_rows.size()); ++row) {
        const double y = rowY(row);
        if (y > height() || y + m_rowH < m_rulerH) continue;
        const int li = m_rows[row];
        const Layer& l = tl.layers[li];
        const bool current = li == m_ed->layerIndex();
        const QRectF r(0, y, m_layersW, m_rowH);
        if (current) {
            p.setPen(Qt::NoPen);
            p.setBrush(ui::withAlpha(pal.accent, 34));
            p.drawRoundedRect(r.adjusted(4, 2, -4, -2), 8, 8);
            p.setBrush(pal.accent);
            p.drawRoundedRect(QRectF(1, y + 7, 3.5, m_rowH - 14), 2, 2);
        } else if (row == m_hoverRow) {
            p.setPen(Qt::NoPen);
            p.setBrush(pal.bg3);
            p.drawRoundedRect(r.adjusted(4, 2, -4, -2), 8, 8);
        }
        const double indent = tl.depth(li) * 14.0;
        QString icon = "layer";
        if (l.type == LayerType::Folder) icon = "folder";
        else if (l.type == LayerType::Mask) icon = "mask";
        else if (l.type == LayerType::Guide) icon = "guide";
        double x = 10 + indent;
        if (l.type == LayerType::Folder) {
            ui::paintIcon(p, l.expanded ? "chevron-down" : "chevron-right", QRectF(x - 4, y + 7, 16, 16), pal.text2);
            x += 12;
        }
        // Layer colour chip + type icon.
        const QColor lc = toQColor(l.color);
        ui::paintIcon(p, icon, QRectF(x, y + 7, 16, 16), current ? pal.text : pal.text2);
        p.setFont(Theme::ui(12.5, current ? QFont::DemiBold : QFont::Normal));
        p.setPen(l.visible ? (current ? pal.text : pal.text2) : pal.text3);
        const QRectF nameRect(x + 24, y, m_layersW - 84 - (x + 24), m_rowH);
        p.drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(p.font()).elidedText(QString::fromStdString(l.name), Qt::ElideRight, int(nameRect.width())));
        // Toggles.
        ui::paintIcon(p, l.visible ? "eye" : "eyeoff", QRectF(m_layersW - 78, y + 6, 18, 18), l.visible ? pal.text3 : pal.danger);
        ui::paintIcon(p, l.locked ? "lock" : "unlock", QRectF(m_layersW - 54, y + 6, 18, 18), l.locked ? pal.accent : pal.text3);
        p.setPen(QPen(lc, 1.6));
        p.setBrush(l.outline ? Qt::transparent : lc);
        p.drawRoundedRect(QRectF(m_layersW - 27, y + 9, 12, 12), 3, 3);
        if (m_drag == Drag::MoveLayer && row == m_dropRow) {
            p.setPen(QPen(pal.accent, 2));
            p.drawLine(QPointF(8, y), QPointF(m_layersW - 8, y));
        }
    }
    if (m_drag == Drag::MoveLayer && m_dropRow == int(m_rows.size())) {
        const double y = rowY(m_dropRow);
        p.setPen(QPen(pal.accent, 2));
        p.drawLine(QPointF(8, y), QPointF(m_layersW - 8, y));
    }
    p.restore();
    p.setPen(QPen(pal.line, 1));
    p.drawLine(QPointF(m_layersW - 0.5, 0), QPointF(m_layersW - 0.5, height()));
}

void TimelineView::mousePressEvent(QMouseEvent* e)
{
    const QPointF pos = e->position();
    m_pressPos = pos;
    if (m_rename) m_rename->clearFocus();
    if (e->button() != Qt::LeftButton) return;
    if (m_ed->isPlaying()) m_ed->setPlaying(false);
    if (pos.y() < m_rulerH) {
        if (pos.x() > m_layersW) {
            m_drag = loopPartAt(pos);
            if (m_drag == Drag::LoopMove) m_loopGrab = frameAt(pos.x()) - m_ed->loopStart();
            if (m_drag != Drag::None) return;
            m_drag = Drag::Scrub;
            m_ed->setFrame(frameAt(pos.x()));
        }
        return;
    }
    const int row = rowAt(pos.y());
    if (row < 0) return;
    const int li = m_rows[row];
    const Timeline& tl = m_ed->timeline();
    if (pos.x() < m_layersW) {
        const bool alt = e->modifiers() & Qt::AltModifier;
        switch (toggleAt(pos, row)) {
        case Toggle::Visible:
            if (alt) m_ed->toggleOthersHidden(li);
            else m_ed->setLayerProperty(li, [](Layer& l) { l.visible = !l.visible; }, tr("Show/Hide Layer"));
            return;
        case Toggle::Lock:
            if (alt) m_ed->toggleOthersLocked(li);
            else m_ed->setLayerProperty(li, [](Layer& l) { l.locked = !l.locked; }, tr("Lock Layer"));
            return;
        case Toggle::Outline:
            m_ed->setLayerProperty(li, [](Layer& l) { l.outline = !l.outline; }, tr("Outline Layer"));
            return;
        case Toggle::Expand:
            m_ed->setLayerProperty(li, [](Layer& l) { l.expanded = !l.expanded; }, tr("Expand Folder"));
            return;
        case Toggle::None: break;
        }
        m_ed->setLayerIndex(li);
        m_ed->setFrameSelection({});
        m_drag = Drag::MoveLayer;
        m_pressRow = row;
        m_dropRow = -1;
        return;
    }
    const int f = frameAt(pos.x());
    m_ed->setLayerIndex(li);
    const FrameSelection& sel = m_ed->frameSelection();
    if (e->modifiers() & Qt::ShiftModifier) {
        const int anchorL = sel.valid() ? sel.layerFrom : m_ed->layerIndex();
        const int anchorF = sel.valid() ? sel.frameFrom : m_ed->frame();
        FrameSelection s;
        s.layerFrom = std::min(anchorL, li);
        s.layerTo = std::max(anchorL, li);
        s.frameFrom = std::min(anchorF, f);
        s.frameTo = std::max(anchorF, f);
        m_ed->setFrameSelection(s);
        m_ed->setFrame(f);
        return;
    }
    m_pressRow = row;
    m_pressFrame = f;
    m_moveDelta = 0;
    const bool onKey = tl.layers[li].isKeyStart(f) && f < tl.layers[li].length();
    if (sel.contains(li, f) || onKey) {
        m_drag = Drag::MoveFrames;
        if (!sel.contains(li, f)) {
            FrameSelection s;
            s.layerFrom = s.layerTo = li;
            s.frameFrom = s.frameTo = f;
            m_ed->setFrameSelection(s);
        }
    } else {
        m_drag = Drag::Select;
        FrameSelection s;
        s.layerFrom = s.layerTo = li;
        s.frameFrom = s.frameTo = f;
        m_ed->setFrameSelection(s);
    }
    m_ed->setFrame(f);
}

TimelineView::Drag TimelineView::loopPartAt(QPointF pos) const
{
    if (!m_ed->loopPlayback() || pos.y() >= m_rulerH || pos.x() <= m_layersW) return Drag::None;
    const double x0 = frameX(m_ed->loopStart()), x1 = frameX(m_ed->loopEnd()) + m_cellW;
    if (std::abs(pos.x() - x0) <= 5) return Drag::LoopStart;
    if (std::abs(pos.x() - x1) <= 5) return Drag::LoopEnd;
    if (pos.y() <= 9 && pos.x() > x0 && pos.x() < x1) return Drag::LoopMove;
    return Drag::None;
}

void TimelineView::mouseMoveEvent(QMouseEvent* e)
{
    const QPointF pos = e->position();
    if (m_drag == Drag::None) {
        const Drag part = loopPartAt(pos);
        setCursor(part == Drag::LoopStart || part == Drag::LoopEnd ? Qt::SizeHorCursor
                  : part == Drag::LoopMove                         ? Qt::OpenHandCursor
                                                                   : Qt::ArrowCursor);
    }
    const int hr = pos.x() < m_layersW ? rowAt(pos.y()) : -1;
    if (hr != m_hoverRow) {
        m_hoverRow = hr;
        update();
    }
    switch (m_drag) {
    case Drag::Scrub: m_ed->setFrame(frameAt(pos.x())); break;
    case Drag::LoopStart: m_ed->setLoopRange(std::min(frameAt(pos.x()), m_ed->loopEnd()), m_ed->loopEnd()); break;
    case Drag::LoopEnd: m_ed->setLoopRange(m_ed->loopStart(), std::max(frameAt(pos.x()), m_ed->loopStart())); break;
    case Drag::LoopMove: {
        const int len = m_ed->loopEnd() - m_ed->loopStart();
        const int last = std::max(0, m_ed->timeline().frameCount() - 1);
        const int from = std::clamp(frameAt(pos.x()) - m_loopGrab, 0, std::max(0, last - len));
        m_ed->setLoopRange(from, from + len);
        break;
    }
    case Drag::Select: {
        const int row = std::clamp(int((pos.y() - m_rulerH + m_scrollY) / m_rowH), 0, std::max(0, int(m_rows.size()) - 1));
        if (m_rows.empty() || m_pressRow < 0) break;
        const int a = m_rows[m_pressRow], b = m_rows[row];
        const int f = frameAt(pos.x());
        FrameSelection s;
        s.layerFrom = std::min(a, b);
        s.layerTo = std::max(a, b);
        s.frameFrom = std::min(m_pressFrame, f);
        s.frameTo = std::max(m_pressFrame, f);
        m_ed->setFrameSelection(s);
        m_ed->setFrame(f);
        break;
    }
    case Drag::MoveFrames: {
        const int d = frameAt(pos.x()) - m_pressFrame;
        if (d != m_moveDelta) {
            m_moveDelta = d;
            update();
        }
        break;
    }
    case Drag::MoveLayer: {
        if (std::abs(pos.y() - m_pressPos.y()) < 4) break;
        const int r = std::clamp(int((pos.y() - m_rulerH + m_scrollY + m_rowH / 2) / m_rowH), 0, int(m_rows.size()));
        if (r != m_dropRow) {
            m_dropRow = r;
            update();
        }
        break;
    }
    case Drag::None: break;
    }
}

void TimelineView::mouseReleaseEvent(QMouseEvent*)
{
    const Drag d = m_drag;
    m_drag = Drag::None;
    if (d == Drag::MoveFrames && m_moveDelta != 0 && m_pressRow >= 0) {
        const FrameSelection s = m_ed->frameSelection();
        const int from = s.valid() ? s.frameFrom : m_pressFrame, to = s.valid() ? s.frameTo : m_pressFrame;
        const int delta = std::max(-from, m_moveDelta);
        const int li = m_rows[m_pressRow];
        if (li < 0) return;
        const int layerFrom = s.valid() ? s.layerFrom : li, layerTo = s.valid() ? s.layerTo : li;
        m_ed->undoStack()->beginMacro(tr("Move Frames"));
        for (int l = layerFrom; l <= layerTo; ++l) m_ed->moveFrames(l, from, to, delta);
        m_ed->undoStack()->endMacro();
        FrameSelection ns = s;
        ns.frameFrom = from + delta;
        ns.frameTo = to + delta;
        m_ed->setFrameSelection(ns);
        m_ed->setFrame(from + delta);
    }
    if (d == Drag::MoveLayer && m_dropRow >= 0 && m_pressRow >= 0) {
        const int from = m_rows[m_pressRow];
        const int before = m_dropRow < int(m_rows.size()) ? m_rows[m_dropRow] : int(m_ed->timeline().layers.size());
        m_ed->moveLayer(from, before);
    }
    m_moveDelta = 0;
    m_dropRow = -1;
    update();
}

void TimelineView::mouseDoubleClickEvent(QMouseEvent* e)
{
    const QPointF pos = e->position();
    const int row = rowAt(pos.y());
    if (row < 0) return;
    const int li = m_rows[row];
    if (pos.x() < m_layersW) {
        if (toggleAt(pos, row) == Toggle::None) startRename(li);
        return;
    }
    const Layer& l = m_ed->timeline().layers[li];
    const Keyframe* k = l.keyAt(frameAt(pos.x()));
    if (!k) return;
    FrameSelection s;
    s.layerFrom = s.layerTo = li;
    s.frameFrom = k->start;
    s.frameTo = k->end() - 1;
    m_ed->setFrameSelection(s);
}

void TimelineView::startRename(int li)
{
    const int row = int(std::find(m_rows.begin(), m_rows.end(), li) - m_rows.begin());
    if (row >= int(m_rows.size())) return;
    delete m_rename;
    m_rename = new QLineEdit(this);
    m_rename->setText(QString::fromStdString(m_ed->timeline().layers[li].name));
    const double indent = m_ed->timeline().depth(li) * 14.0;
    m_rename->setGeometry(int(30 + indent), int(rowY(row) + 3), int(m_layersW - 112 - indent), m_rowH - 6);
    m_rename->selectAll();
    m_rename->show();
    m_rename->setFocus();
    connect(m_rename, &QLineEdit::editingFinished, this, [this, li]() {
        if (!m_rename) return;
        const QString name = m_rename->text();
        m_rename->deleteLater();
        m_rename = nullptr;
        m_ed->renameLayer(li, name);
    });
}

void TimelineView::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        setCellWidth(m_cellW * (e->angleDelta().y() > 0 ? 1.15 : 1 / 1.15));
        return;
    }
    const QPoint d = e->angleDelta();
    if (e->modifiers() & Qt::ShiftModifier || std::abs(d.x()) > std::abs(d.y()))
        emit scrollRequested(m_scrollX - (d.x() != 0 ? d.x() : d.y()) / 2, m_scrollY);
    else emit scrollRequested(m_scrollX, m_scrollY - d.y() / 3);
}

void TimelineView::contextMenuEvent(QContextMenuEvent* e)
{
    const int row = rowAt(e->pos().y());
    if (row < 0) return;
    const int li = m_rows[row];
    m_ed->setLayerIndex(li);
    if (e->pos().x() < m_layersW) {
        layerMenu(e->globalPos(), li);
        return;
    }
    const int f = frameAt(e->pos().x());
    if (!m_ed->frameSelection().contains(li, f)) {
        FrameSelection s;
        s.layerFrom = s.layerTo = li;
        s.frameFrom = s.frameTo = f;
        m_ed->setFrameSelection(s);
    }
    m_ed->setFrame(f);
    frameMenu(e->globalPos());
}

void TimelineView::frameMenu(const QPoint& global)
{
    QMenu menu(this);
    auto add = [&](const char* name) {
        if (QAction* a = m_actions(QString::fromLatin1(name))) menu.addAction(a);
    };
    add("createClassicTween");
    add("createShapeTween");
    add("removeTween");
    menu.addSeparator();
    add("insertFrame");
    add("removeFrames");
    add("insertKeyframe");
    add("insertBlankKeyframe");
    add("clearKeyframe");
    add("convertToKeyframes");
    add("convertToBlankKeyframes");
    menu.addSeparator();
    add("cutFrames");
    add("copyFrames");
    add("pasteFrames");
    add("clearFrames");
    add("selectAllFrames");
    menu.addSeparator();
    add("reverseFrames");
    add("addShapeHint");
    menu.exec(global);
}

void TimelineView::layerMenu(const QPoint& global, int li)
{
    const Layer l = m_ed->timeline().layers[li];
    QMenu menu(this);
    menu.addAction(tr("Rename"), this, [this, li]() { startRename(li); });
    menu.addSeparator();
    auto typeAction = [&](const QString& text, LayerType t) {
        QAction* a = menu.addAction(text, this, [this, li, t]() {
            m_ed->setLayerProperty(li, [t](Layer& x) { x.type = t; }, tr("Layer Type"));
        });
        a->setCheckable(true);
        a->setChecked(l.type == t);
    };
    typeAction(tr("Normal"), LayerType::Normal);
    typeAction(tr("Guide"), LayerType::Guide);
    typeAction(tr("Mask"), LayerType::Mask);
    if (QAction* a = m_actions("addMotionGuide")) menu.addAction(a);
    menu.addSeparator();
    menu.addAction(tr("Lock Others"), this, [this, li]() { m_ed->toggleOthersLocked(li); });
    menu.addAction(tr("Hide Others"), this, [this, li]() { m_ed->toggleOthersHidden(li); });
    if (l.parentId) {
        menu.addAction(tr("Move Out of Parent"), this, [this, li]() {
            m_ed->setLayerProperty(li, [this](Layer& x) {
                const Layer* p = m_ed->timeline().layerById(x.parentId);
                x.parentId = p ? p->parentId : 0;
            }, tr("Release Layer"));
        });
    }
    if (li > 0) {
        const Layer& above = m_ed->timeline().layers[li - 1];
        if (above.type == LayerType::Folder || above.type == LayerType::Mask || above.type == LayerType::Guide) {
            const uint32_t pid = above.id;
            menu.addAction(tr("Put Inside “%1”").arg(QString::fromStdString(above.name)), this, [this, li, pid]() {
                m_ed->setLayerProperty(li, [pid](Layer& x) { x.parentId = pid; }, tr("Nest Layer"));
            });
        }
    }
    menu.addSeparator();
    if (QAction* a = m_actions("newLayer")) menu.addAction(a);
    if (QAction* a = m_actions("newFolder")) menu.addAction(a);
    if (QAction* a = m_actions("deleteLayer")) menu.addAction(a);
    menu.exec(global);
}

// --- TimelinePanel -----------------------------------------------------------------------------------

namespace {

class FrameCounter : public QWidget {
public:
    explicit FrameCounter(Editor* ed, QWidget* parent) : QWidget(parent), m_ed(ed)
    {
        setFixedSize(150, 52);
        connect(m_ed, &Editor::frameChanged, this, qOverload<>(&QWidget::update));
        connect(m_ed, &Editor::documentChanged, this, qOverload<>(&QWidget::update));
        connect(m_ed, &Editor::contextChanged, this, qOverload<>(&QWidget::update));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const ui::Palette& pal = Theme::p();
        const int total = m_ed->timeline().frameCount();
        const QString num = QString("%1").arg(m_ed->frame() + 1, 3, 10, QChar('0'));
        p.setFont(Theme::display(36));
        p.setPen(pal.text);
        p.drawText(QRectF(0, 0, width(), height()), Qt::AlignVCenter | Qt::AlignLeft, num);
        const int w = QFontMetrics(p.font()).horizontalAdvance(num);
        p.setFont(Theme::ui(10.5, QFont::Bold));
        p.setPen(pal.accent);
        p.drawText(QPointF(w + 6, 22), tr("FRAME"));
        p.setPen(pal.text3);
        p.drawText(QPointF(w + 6, 38), QString("/ %1").arg(total));
    }

private:
    Editor* m_ed;
};

QToolButton* transportButton(const QString& icon, const QString& tip, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setIcon(ui::icon(icon));
    b->setIconSize(QSize(18, 18));
    b->setFixedSize(32, 32);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    return b;
}

} // namespace

TimelinePanel::TimelinePanel(Editor* editor, ActionLookup actions, QWidget* parent) : QWidget(parent), m_ed(editor)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 6, 6, 4);
    lay->setSpacing(4);

    auto* header = new QHBoxLayout();
    header->setSpacing(4);
    auto* title = new SectionTitle(tr("Timeline"), this);
    title->setFixedWidth(128);
    header->addWidget(title);
    auto addAction = [&](const QString& name, const QString& icon) {
        QAction* a = actions(name);
        auto* b = transportButton(icon, a ? a->toolTip() : name, this);
        if (a) connect(b, &QToolButton::clicked, a, &QAction::trigger);
        header->addWidget(b);
        return b;
    };
    addAction("newLayer", "plus");
    addAction("newFolder", "folder");
    addAction("deleteLayer", "trash");
    header->addSpacing(18);
    m_counter = new FrameCounter(m_ed, this);
    header->addWidget(m_counter);
    header->addSpacing(8);
    addAction("firstFrame", "first");
    addAction("prevFrame", "prev");
    m_play = transportButton("play", tr("Play / Stop (Enter)"), this);
    m_play->setFixedSize(40, 36);
    m_play->setIconSize(QSize(22, 22));
    connect(m_play, &QToolButton::clicked, this, [this]() { m_ed->setPlaying(!m_ed->isPlaying()); });
    header->addWidget(m_play);
    addAction("nextFrame", "next");
    addAction("lastFrame", "last");
    m_loop = transportButton("loop", tr("Loop playback"), this);
    m_loop->setCheckable(true);
    m_loop->setChecked(m_ed->loopPlayback());
    m_loop->setToolTip(tr("Loop playback (Alt+Shift+L): drag the bracket on the ruler to choose the frames; "
                          "with frames selected, the loop takes the selection"));
    connect(m_loop, &QToolButton::toggled, m_ed, &Editor::setLoopPlayback);
    connect(m_ed, &Editor::loopChanged, this, [this]() {
        const QSignalBlocker block(m_loop);
        m_loop->setChecked(m_ed->loopPlayback());
    });
    header->addWidget(m_loop);
    header->addSpacing(14);
    m_onion = transportButton("onion", tr("Onion skin (Alt+Shift+O)"), this);
    m_onion->setCheckable(true);
    m_onionOutline = transportButton("onionoutline", tr("Onion skin outlines"), this);
    m_onionOutline->setCheckable(true);
    connect(m_onion, &QToolButton::toggled, this, [this](bool on) { m_ed->setOnion(on, m_onionOutline->isChecked()); });
    connect(m_onionOutline, &QToolButton::toggled, this, [this](bool on) {
        m_ed->setOnion(on || m_onion->isChecked(), on);
    });
    header->addWidget(m_onion);
    header->addWidget(m_onionOutline);
    header->addStretch(1);
    m_time = new QLabel(this);
    m_time->setFont(Theme::ui(12, QFont::DemiBold));
    header->addWidget(m_time);
    header->addSpacing(12);
    m_fps = new HotNumber(this);
    m_fps->setLabel(tr("FPS"));
    m_fps->setRange(1, 120);
    m_fps->setDecimals(2);
    m_fps->setStep(0.25);
    connect(m_fps, &HotNumber::valueCommitted, this, [this](double v) {
        const Document& d = m_ed->doc();
        m_ed->setStageSettings(d.width, d.height, v, d.background);
    });
    header->addWidget(m_fps);
    lay->addLayout(header);

    auto* body = new QGridLayout();
    body->setSpacing(0);
    m_view = new TimelineView(m_ed, std::move(actions), this);
    m_h = new QScrollBar(Qt::Horizontal, this);
    m_v = new QScrollBar(Qt::Vertical, this);
    body->addWidget(m_view, 0, 0);
    body->addWidget(m_v, 0, 1);
    body->addWidget(m_h, 1, 0);
    lay->addLayout(body, 1);

    connect(m_h, &QScrollBar::valueChanged, this, [this](int x) { m_view->setScroll(x, m_v->value()); });
    connect(m_v, &QScrollBar::valueChanged, this, [this](int y) { m_view->setScroll(m_h->value(), y); });
    connect(m_view, &TimelineView::scrollRangeChanged, this, &TimelinePanel::syncScrollBars);
    connect(m_view, &TimelineView::scrollRequested, this, [this](int x, int y) {
        m_h->setValue(x);
        m_v->setValue(y);
    });
    connect(m_ed, &Editor::playingChanged, this, [this](bool p) { m_play->setIcon(ui::icon(p ? "pause" : "play")); });
    connect(m_ed, &Editor::frameChanged, this, &TimelinePanel::syncHeader);
    connect(m_ed, &Editor::documentChanged, this, &TimelinePanel::syncHeader);
    connect(m_ed, &Editor::onionChanged, this, [this]() {
        const QSignalBlocker b1(m_onion), b2(m_onionOutline);
        m_onion->setChecked(m_ed->onionSkin);
        m_onionOutline->setChecked(m_ed->onionOutline);
    });
    syncHeader();
    syncScrollBars();
}

void TimelinePanel::syncScrollBars()
{
    const int visW = std::max(0, m_view->width() - m_view->layerColumnWidth());
    m_h->setRange(0, std::max(0, m_view->contentWidth() - visW));
    m_h->setPageStep(visW);
    m_h->setSingleStep(int(m_view->cellWidth() * 3));
    const int visH = std::max(0, m_view->height() - 28);
    m_v->setRange(0, std::max(0, m_view->contentHeight() - visH));
    m_v->setPageStep(visH);
    m_v->setSingleStep(30);
}

void TimelinePanel::syncHeader()
{
    const Document& d = m_ed->doc();
    m_fps->setValue(d.fps);
    m_time->setText(QString("%1s").arg(m_ed->frame() / std::max(1.0, d.fps), 0, 'f', 2));
    m_time->setStyleSheet(QString("color: %1").arg(Theme::p().text2.name()));
    m_counter->update();
}

void TimelinePanel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Theme::p().bg1);
}

} // namespace vx::app
