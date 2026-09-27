// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — timeline: transport bar with an expressive frame counter, layer
// list (folders, masks, guides, visibility/lock/outline toggles) and the
// frame grid (keyframes, spans, tweens, labels, onion markers).
#pragma once

#include "../Editor.h"

#include <QWidget>

#include <functional>

class QAction;
class QLineEdit;
class QScrollBar;
class QToolButton;
class QLabel;

namespace vx::app {

class HotNumber;

using ActionLookup = std::function<QAction*(const QString&)>;

class TimelineView : public QWidget {
    Q_OBJECT
public:
    TimelineView(Editor* editor, ActionLookup actions, QWidget* parent = nullptr);
    void setScroll(int x, int y);
    int contentWidth() const;
    int contentHeight() const;
    int layerColumnWidth() const { return m_layersW; }
    void ensurePlayheadVisible();
    double cellWidth() const { return m_cellW; }
    void setCellWidth(double w);

signals:
    void scrollRangeChanged();
    void scrollRequested(int x, int y);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    enum class Drag { None, Scrub, Select, MoveFrames, MoveLayer, LoopStart, LoopEnd, LoopMove };
    /// Part of the loop bracket on the ruler under `pos`, or Drag::None.
    Drag loopPartAt(QPointF pos) const;
    enum class Toggle { None, Visible, Lock, Outline, Expand };
    void rebuildRows();
    int rowAt(double y) const;          ///< index into m_rows, -1 if none
    int frameAt(double x) const;
    double frameX(int f) const;
    double rowY(int row) const;
    Toggle toggleAt(QPointF p, int row) const;
    void paintLayerColumn(QPainter& p);
    void paintRuler(QPainter& p);
    void paintGrid(QPainter& p);
    void startRename(int layerIndex);
    void layerMenu(const QPoint& global, int layerIndex);
    void frameMenu(const QPoint& global);

    Editor* m_ed;
    ActionLookup m_actions;
    std::vector<int> m_rows; ///< displayed layer indices
    double m_cellW = 13.0;
    int m_rowH = 30;
    int m_rulerH = 28;
    int m_layersW = 236;
    int m_scrollX = 0, m_scrollY = 0;
    Drag m_drag = Drag::None;
    int m_loopGrab = 0; ///< frame offset of the pointer in the loop range when moving it
    QPointF m_pressPos;
    int m_pressRow = -1, m_pressFrame = -1;
    int m_hoverRow = -1;
    int m_moveDelta = 0;
    int m_dropRow = -1;
    QLineEdit* m_rename = nullptr;
};

class TimelinePanel : public QWidget {
    Q_OBJECT
public:
    TimelinePanel(Editor* editor, ActionLookup actions, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void syncScrollBars();
    void syncHeader();
    Editor* m_ed;
    TimelineView* m_view;
    QScrollBar* m_h;
    QScrollBar* m_v;
    QWidget* m_counter;
    QToolButton* m_play;
    QToolButton* m_loop;
    QToolButton* m_onion;
    QToolButton* m_onionOutline;
    HotNumber* m_fps;
    QLabel* m_time;
};

} // namespace vx::app
