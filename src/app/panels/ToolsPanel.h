// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — the tool strip: grouped tools with a sliding highlight, fill and
// stroke swatches, object drawing toggle.
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
    QSize minimumSizeHint() const override { return {56, 120}; }
    static QString shortcutFor(ToolId id);
    static QString iconFor(ToolId id);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    bool event(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    struct Item {
        ToolId id;
        QRectF rect;
    };
    void layoutItems();
    int itemAt(QPointF p) const;
    void syncSwatches();
    Editor* m_ed;
    std::vector<Item> m_items;
    std::vector<double> m_separators;
    int m_hover = -1;
    double m_indicatorY = 0;
    QVariantAnimation* m_anim = nullptr;
    ColorSwatch* m_stroke = nullptr;
    ColorSwatch* m_fill = nullptr;
    QRectF m_objectToggle, m_swapButton;
};

} // namespace vx::app
