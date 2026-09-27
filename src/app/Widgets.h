// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — small reusable widgets with the Vertexa look: hot-text numbers
// (drag to scrub, click to type — like Animate), colour swatches and picker,
// response curve editor, segmented toggles and expressive section titles.
#pragma once

#include "core/VectorBrush.h"
#include "core/Style.h"

#include <QColor>
#include <QLabel>
#include <QWidget>

#include <functional>

class QLineEdit;
class QVariantAnimation;
class QButtonGroup;

namespace vx::app {

/// Big, heavy section title with an accent dot.
class SectionTitle : public QWidget {
    Q_OBJECT
public:
    explicit SectionTitle(const QString& text, QWidget* parent = nullptr);
    void setText(const QString& t);
    void setSubtitle(const QString& t);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QString m_text, m_sub;
};

/// Animate-style "hot text": drag horizontally to scrub, click to edit.
class HotNumber : public QWidget {
    Q_OBJECT
public:
    explicit HotNumber(QWidget* parent = nullptr);
    void setRange(double lo, double hi);
    void setDecimals(int d);
    void setStep(double s);
    void setSuffix(const QString& s);
    void setLabel(const QString& l);
    double value() const { return m_value; }
    void setValue(double v, bool notify = false);
    QSize sizeHint() const override;

signals:
    void valueChanged(double v);   ///< while scrubbing
    void valueCommitted(double v); ///< mouse released / edit finished

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    QString text() const;
    void startEdit();
    double m_value = 0, m_lo = -1e9, m_hi = 1e9, m_step = 1;
    int m_decimals = 0;
    QString m_suffix, m_label;
    bool m_pressed = false, m_dragged = false, m_hover = false;
    QPointF m_pressPos;
    double m_pressValue = 0;
    QLineEdit* m_edit = nullptr;
    double m_glow = 0;
    QVariantAnimation* m_anim = nullptr;
};

/// Colour swatch: solid, gradient or "none".
class ColorSwatch : public QWidget {
    Q_OBJECT
public:
    explicit ColorSwatch(QWidget* parent = nullptr);
    void setFill(const FillStyle& f, bool enabled = true);
    void setStrokeLook(bool s) { m_stroke = s; update(); }
    void setActive(bool a);
    QSize sizeHint() const override { return {30, 30}; }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    FillStyle m_fill;
    bool m_enabled = true, m_stroke = false, m_active = false, m_hover = false;
};

/// HSV colour picker: saturation/value square, hue strip, alpha strip, hex.
class ColorPicker : public QWidget {
    Q_OBJECT
public:
    explicit ColorPicker(QWidget* parent = nullptr);
    QColor color() const;
    void setColor(const QColor& c);

signals:
    void colorChanged(const QColor& c);
    void colorCommitted(const QColor& c);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    enum class Part { None, SV, Hue, Alpha };
    QRectF svRect() const;
    QRectF hueRect() const;
    QRectF alphaRect() const;
    void pick(QPointF p);
    double m_h = 0.6, m_s = 0.8, m_v = 0.9, m_a = 1.0;
    Part m_drag = Part::None;
    QLineEdit* m_hex = nullptr;
    QImage m_svCache;
    double m_svHue = -1;
};

/// Editable monotone response curve (pressure curves, sensor curves).
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget* parent = nullptr);
    void setCurve(const ResponseCurve& c);
    const ResponseCurve& curve() const { return m_curve; }
    void setMarker(double x); ///< live pressure indicator (-1 hides)
    QSize sizeHint() const override { return {200, 150}; }

signals:
    void curveChanged(const ResponseCurve& c);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    QRectF area() const;
    QPointF toWidget(Vec2 p) const;
    Vec2 fromWidget(QPointF p) const;
    ResponseCurve m_curve;
    int m_drag = -1;
    double m_marker = -1;
};

/// Row of mutually exclusive icon/text toggles.
class Segmented : public QWidget {
    Q_OBJECT
public:
    explicit Segmented(QWidget* parent = nullptr);
    void addSegment(const QString& text, const QString& icon = {}, const QString& tip = {});
    void setCurrent(int i);
    int current() const { return m_current; }
    QSize sizeHint() const override;

signals:
    void changed(int index);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    bool event(QEvent*) override;

private:
    struct Seg {
        QString text, icon, tip;
    };
    QRectF segRect(int i) const;
    int segAt(QPointF p) const;
    std::vector<Seg> m_segs;
    int m_current = 0;
    int m_hover = -1;
    double m_pos = 0; ///< animated indicator position
    QVariantAnimation* m_anim = nullptr;
};

/// Opens a floating colour picker next to `anchor`.
void popupColorPicker(QWidget* anchor, const QColor& initial, const std::function<void(const QColor&, bool final)>& onChange);

} // namespace vx::app
