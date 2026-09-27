// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — Color panel: fill/stroke target, HSV picker, swatches.
#pragma once

#include "../Editor.h"

#include <QScrollArea>
#include <QWidget>

namespace vx::app {

class ColorPicker;
class Segmented;

class SwatchGrid : public QWidget {
    Q_OBJECT
public:
    explicit SwatchGrid(QWidget* parent = nullptr);
    void addRecent(const QColor& c);
    QSize sizeHint() const override;
    int heightForWidth(int w) const override;
    bool hasHeightForWidth() const override { return true; }

signals:
    void picked(const QColor& c);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    int indexAt(QPointF p) const;
    QRectF cellRect(int i) const;
    std::vector<QColor> m_colors;
    std::vector<QColor> m_recent;
    int m_hover = -1;
};

class ColorPanel : public QScrollArea {
    Q_OBJECT
public:
    explicit ColorPanel(Editor* editor, QWidget* parent = nullptr);

private:
    void sync();
    void apply(const QColor& c, bool final);
    Editor* m_ed;
    Segmented* m_target;
    ColorPicker* m_picker;
    SwatchGrid* m_swatches;
    bool m_syncing = false;
};

} // namespace vx::app
