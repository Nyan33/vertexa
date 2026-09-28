// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — tool interface. Tools receive events in the coordinate space of
// the timeline being edited (which may be a symbol edited in place).
#pragma once

#include "../Editor.h"

#include <QCursor>
#include <QPointF>

class QKeyEvent;
class QPainter;

namespace vx::app {

class StageView;

struct ToolEvent {
    Vec2 pos;              ///< timeline space
    QPointF widget;        ///< widget coordinates
    double pressure = 1.0; ///< after the pressure curve
    double rawPressure = 1.0;
    double tiltX = 0.0, tiltY = 0.0, rotation = 0.0, tangential = 0.0;
    bool tablet = false;
    bool eraserTip = false;
    Qt::KeyboardModifiers mods;
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons;
    double time = 0.0; ///< seconds
};

class Tool {
public:
    Tool(Editor* editor, StageView* view) : ed(editor), view(view) {}
    virtual ~Tool() = default;

    virtual ToolId id() const = 0;
    virtual void press(const ToolEvent&) {}
    virtual void move(const ToolEvent&) {}
    virtual void release(const ToolEvent&) {}
    virtual void hover(const ToolEvent&) {}
    virtual void doubleClick(const ToolEvent&) {}
    virtual bool keyPress(QKeyEvent*) { return false; }
    virtual void cancel() {}
    virtual void activate() {}
    virtual void deactivate() { cancel(); }
    /// Overlay in widget coordinates.
    virtual void paint(QPainter&) {}
    virtual QCursor cursor() const { return Qt::CrossCursor; }
    /// True while a drag/stroke is in progress.
    virtual bool busy() const { return false; }
    /// True while finished work is still being computed in the background.
    virtual bool hasPendingWork() const { return false; }

protected:
    QPointF toWidget(Vec2 p) const;
    double unitsPerPixel() const;
    void update() const;
    /// Repaints only part of the stage (widget coordinates): what a stroke
    /// or a brush cursor changed.
    void update(const QRectF& widgetRect) const;
    Editor* ed;
    StageView* view;
};

} // namespace vx::app
