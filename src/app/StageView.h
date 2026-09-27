// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — the stage: renders the document, hosts the tools, handles
// zoom/pan, tablets (pressure, tilt, rotation, eraser tip) and onion skins.
#pragma once

#include "Editor.h"
#include "tools/Tool.h"

#include <QElapsedTimer>
#include <QImage>
#include <QWidget>

#include <map>
#include <memory>

class QVariantAnimation;

namespace vx::app {

class StageView : public QWidget {
    Q_OBJECT
public:
    explicit StageView(Editor* editor, QWidget* parent = nullptr);
    ~StageView() override;

    Editor* editor() const { return m_ed; }
    double zoom() const { return m_zoom; }
    void setZoom(double z, QPointF anchor, bool animated = true);
    void zoomBy(double factor);
    void zoomTo100();
    void fitStage();
    void centerStage();

    /// Stage (scene) coordinates -> widget coordinates.
    Affine stageToWidget() const;
    /// Timeline being edited -> widget coordinates.
    Affine timelineToWidget() const;
    Vec2 widgetToTimeline(QPointF p) const;
    /// Timeline units per widget pixel (for hit tolerances).
    double unitsPerPixel() const;

    Tool* activeTool() const { return m_active; }
    Tool* toolFor(ToolId id) const;
    void showToast(const QString& text);
    void invalidate();
    void refreshCursor();
    /// Colour of the rendered stage under a widget position.
    QColor colorAt(QPointF widgetPos) const;
    /// Custom cursors: "bend", "corner", "rotate".
    QCursor toolCursor(const QString& kind) const;

signals:
    void zoomChanged(double zoom);
    void pointerMoved(double x, double y);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void tabletEvent(QTabletEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void leaveEvent(QEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
    bool event(QEvent*) override;

private:
    void renderCache();
    void drawSelection(QPainter& p);
    void drawToast(QPainter& p);
    void activateTool(ToolId id);
    ToolEvent makeEvent(QPointF widgetPos, Qt::KeyboardModifiers mods) const;
    Tool* strokeTool() const { return m_strokeTool ? m_strokeTool : m_active; }

    Editor* m_ed;
    std::map<ToolId, std::unique_ptr<Tool>> m_tools;
    Tool* m_active = nullptr;
    Tool* m_strokeTool = nullptr; ///< eraser-tip override for the current stroke
    double m_zoom = 1.0;
    QPointF m_pan{0, 0};  ///< widget position of the stage origin
    bool m_placed = false;
    QImage m_cache;
    bool m_cacheValid = false;
    bool m_spaceDown = false;
    bool m_panning = false;
    QPointF m_panAnchor;
    QPointF m_panStart;
    bool m_tabletActive = false;
    bool m_tabletDown = false;
    QElapsedTimer m_clock;
    QString m_toast;
    double m_toastT = 0.0;
    QVariantAnimation* m_toastAnim = nullptr;
    QVariantAnimation* m_zoomAnim = nullptr;
    QPointF m_zoomAnchor;
    QPointF m_lastWidget;
    bool m_hasPointer = false;
    /// Simple buttons: the button under the pointer and whether it is down.
    const Element* m_hotButton = nullptr;
    bool m_buttonDown = false;
    bool updateHotButton(Vec2 pos);
    /// 9-slice guides of the symbol being edited: 0/1 left/right, 2/3 top/bottom.
    const Symbol* sliceSymbol() const;
    int guideAt(Vec2 pos) const;
    void drawSliceGuides(QPainter& p);
    int m_guideDrag = -1;
    int m_guideHover = -1;
    Rect m_guideGrid;
};

} // namespace vx::app
