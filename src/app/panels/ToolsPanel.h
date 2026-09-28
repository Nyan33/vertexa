// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — the tool strip: grouped tools with a sliding highlight, fill and
// stroke swatches, object drawing toggle. On short screens the tools go in
// two columns, and whatever still doesn't fit scrolls with the wheel.
#pragma once

#include "../Editor.h"

#include <QWidget>

class QVariantAnimation;

namespace vx::app {

class ColorSwatch;

class ToolsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ToolsPanel(Editor* editor, QWidget* parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return {sizeHint().width(), 120}; }
    static QString shortcutFor(ToolId id);
    static QString iconFor(ToolId id);
    int columns() const { return m_cols; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    bool event(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    struct Item {
        ToolId id;
        QRectF rect; ///< content coordinates (before scrolling)
    };
    /// Height of everything laid out in `cols` columns.
    static double contentHeight(int cols);
    void layoutItems();
    void setScroll(double y);
    /// Scrolls just enough to show the current tool.
    void revealTool();
    /// Widget point -> content point.
    QPointF content(QPointF p) const { return p + QPointF(0, m_scroll); }
    int itemAt(QPointF p) const;
    void syncSwatches();
    Editor* m_ed;
    std::vector<Item> m_items;
    std::vector<double> m_separators;
    int m_cols = 1;
    double m_contentH = 0;
    double m_scroll = 0;
    int m_hover = -1;
    QPointF m_indicator; ///< top left of the sliding highlight
    QVariantAnimation* m_anim = nullptr;
    ColorSwatch* m_stroke = nullptr;
    ColorSwatch* m_fill = nullptr;
    QPointF m_strokePos, m_fillPos; ///< swatch positions (content coordinates)
    QRectF m_objectToggle, m_swapButton;
};

} // namespace vx::app
