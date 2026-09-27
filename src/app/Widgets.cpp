// SPDX-License-Identifier: GPL-3.0-or-later
#include "Widgets.h"
#include "Icons.h"
#include "Theme.h"

#include "render/QtConvert.h"

#include <QApplication>
#include <QEnterEvent>
#include <QFrame>
#include <QHelpEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QToolTip>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <cmath>

namespace vx::app {

using ui::Theme;

namespace {

void drawChecker(QPainter& p, const QRectF& r, double cell = 5.0)
{
    p.save();
    p.setClipRect(r);
    p.fillRect(r, QColor(235, 235, 240));
    const QColor dark(200, 200, 208);
    for (double y = r.top(); y < r.bottom(); y += cell)
        for (double x = r.left(); x < r.right(); x += cell)
            if ((int((x - r.left()) / cell) + int((y - r.top()) / cell)) % 2) p.fillRect(QRectF(x, y, cell, cell), dark);
    p.restore();
}

QBrush fillBrush(const FillStyle& f, const QRectF& r)
{
    if (f.kind == FillStyle::Kind::Solid) return QBrush(toQColor(f.color));
    QGradientStops stops;
    for (const GradientStop& s : f.gradient.stops) stops.push_back({s.pos, toQColor(s.color)});
    if (f.kind == FillStyle::Kind::Linear) {
        QLinearGradient g(r.topLeft(), r.topRight());
        g.setStops(stops);
        return QBrush(g);
    }
    QRadialGradient g(r.center(), r.width() * 0.5);
    g.setStops(stops);
    return QBrush(g);
}

} // namespace

// --- SectionTitle ------------------------------------------------------------------------

SectionTitle::SectionTitle(const QString& text, QWidget* parent) : QWidget(parent), m_text(text)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    connect(Theme::instance(), &ui::Theme::changed, this, qOverload<>(&QWidget::update));
}

void SectionTitle::setText(const QString& t)
{
    m_text = t;
    update();
}

void SectionTitle::setSubtitle(const QString& t)
{
    m_sub = t;
    update();
}

QSize SectionTitle::sizeHint() const { return {160, 38}; }

void SectionTitle::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ui::Palette& pal = Theme::p();
    p.setPen(Qt::NoPen);
    p.setBrush(pal.accent);
    p.drawEllipse(QPointF(5, height() - 13), 3.5, 3.5);
    p.setFont(Theme::display(21));
    p.setPen(pal.text);
    const QFontMetrics fm(p.font());
    p.drawText(QPointF(15, height() - 6), m_text);
    if (!m_sub.isEmpty()) {
        p.setFont(Theme::ui(11, QFont::DemiBold));
        p.setPen(pal.text3);
        p.drawText(QPointF(15 + fm.horizontalAdvance(m_text) + 10, height() - 8), m_sub.toUpper());
    }
}

// --- HotNumber ------------------------------------------------------------------------------

HotNumber::HotNumber(QWidget* parent) : QWidget(parent)
{
    setCursor(Qt::SizeHorCursor);
    setFocusPolicy(Qt::ClickFocus);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_anim = new QVariantAnimation(this);
    m_anim->setDuration(160);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_glow = v.toDouble();
        update();
    });
}

void HotNumber::setRange(double lo, double hi)
{
    m_lo = lo;
    m_hi = hi;
    setValue(m_value);
}
void HotNumber::setDecimals(int d)
{
    m_decimals = d;
    update();
}
void HotNumber::setStep(double s) { m_step = s; }
void HotNumber::setSuffix(const QString& s)
{
    m_suffix = s;
    update();
}
void HotNumber::setLabel(const QString& l)
{
    m_label = l;
    updateGeometry();
    update();
}

void HotNumber::setValue(double v, bool notify)
{
    v = std::clamp(v, m_lo, m_hi);
    const double q = std::pow(10.0, m_decimals);
    v = std::round(v * q) / q;
    if (v == m_value) return;
    m_value = v;
    update();
    if (notify) emit valueChanged(v);
}

QString HotNumber::text() const { return QString::number(m_value, 'f', m_decimals) + m_suffix; }

QSize HotNumber::sizeHint() const
{
    const QFontMetrics fm(Theme::ui(13, QFont::DemiBold));
    const QFontMetrics lf(Theme::ui(11));
    return {fm.horizontalAdvance(text()) + (m_label.isEmpty() ? 0 : lf.horizontalAdvance(m_label) + 8) + 12, 26};
}

void HotNumber::paintEvent(QPaintEvent*)
{
    if (m_edit) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ui::Palette& pal = Theme::p();
    int x = 2;
    if (!m_label.isEmpty()) {
        p.setFont(Theme::ui(11));
        p.setPen(pal.text2);
        p.drawText(QRect(x, 0, width(), height()), Qt::AlignVCenter | Qt::AlignLeft, m_label);
        x += QFontMetrics(p.font()).horizontalAdvance(m_label) + 8;
    }
    p.setFont(Theme::ui(13, QFont::DemiBold));
    const QColor c = ui::mix(pal.accent, pal.text, isEnabled() ? 0.0 : 0.6);
    const QFontMetrics fm(p.font());
    const int tw = fm.horizontalAdvance(text());
    if (m_glow > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(ui::withAlpha(pal.accent, int(40 * m_glow)));
        p.drawRoundedRect(QRectF(x - 4, 2, tw + 8, height() - 4), 6, 6);
    }
    p.setPen(c);
    p.drawText(QRect(x, 0, width() - x, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
    QPen dash(ui::withAlpha(c, 160), 1, Qt::DotLine);
    p.setPen(dash);
    p.drawLine(QPointF(x, height() - 5.5), QPointF(x + tw, height() - 5.5));
}

void HotNumber::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    m_pressed = true;
    m_dragged = false;
    m_pressPos = e->globalPosition();
    m_pressValue = m_value;
}

void HotNumber::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_pressed) return;
    const double dx = e->globalPosition().x() - m_pressPos.x();
    if (!m_dragged && std::abs(dx) < 3) return;
    m_dragged = true;
    double k = m_step;
    if (e->modifiers() & Qt::ShiftModifier) k *= 10;
    if (e->modifiers() & Qt::AltModifier) k *= 0.1;
    setValue(m_pressValue + dx * k, true);
}

void HotNumber::mouseReleaseEvent(QMouseEvent*)
{
    if (!m_pressed) return;
    m_pressed = false;
    if (m_dragged) emit valueCommitted(m_value);
    else startEdit();
}

void HotNumber::wheelEvent(QWheelEvent* e)
{
    if (!hasFocus()) {
        e->ignore();
        return;
    }
    setValue(m_value + (e->angleDelta().y() > 0 ? m_step : -m_step), true);
    emit valueCommitted(m_value);
}

void HotNumber::enterEvent(QEnterEvent*)
{
    m_hover = true;
    m_anim->setStartValue(m_glow);
    m_anim->setEndValue(1.0);
    m_anim->start();
}

void HotNumber::leaveEvent(QEvent*)
{
    m_hover = false;
    m_anim->setStartValue(m_glow);
    m_anim->setEndValue(0.0);
    m_anim->start();
}

void HotNumber::startEdit()
{
    if (m_edit) return;
    m_edit = new QLineEdit(this);
    m_edit->setText(QString::number(m_value, 'f', m_decimals));
    m_edit->setGeometry(rect());
    m_edit->setFont(Theme::ui(13, QFont::DemiBold));
    m_edit->selectAll();
    m_edit->show();
    m_edit->setFocus();
    auto finish = [this]() {
        if (!m_edit) return;
        bool ok = false;
        const double v = m_edit->text().toDouble(&ok);
        QLineEdit* ed = m_edit;
        m_edit = nullptr;
        ed->deleteLater();
        if (ok) {
            setValue(v, true);
            emit valueCommitted(m_value);
        }
        update();
    };
    connect(m_edit, &QLineEdit::editingFinished, this, finish);
}

// --- ColorSwatch ------------------------------------------------------------------------------

ColorSwatch::ColorSwatch(QWidget* parent) : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFixedSize(30, 30);
}

void ColorSwatch::setFill(const FillStyle& f, bool enabled)
{
    m_fill = f;
    m_enabled = enabled;
    update();
}

void ColorSwatch::setActive(bool a)
{
    m_active = a;
    update();
}

void ColorSwatch::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ui::Palette& pal = Theme::p();
    const QRectF r = QRectF(rect()).adjusted(3, 3, -3, -3);
    QPainterPath shape;
    shape.addRoundedRect(r, 7, 7);
    if (m_stroke) {
        QPainterPath inner;
        inner.addRoundedRect(r.adjusted(7, 7, -7, -7), 3, 3);
        shape = shape.subtracted(inner);
    }
    p.save();
    p.setClipPath(shape);
    drawChecker(p, r);
    if (m_enabled) p.fillRect(r, fillBrush(m_fill, r));
    p.restore();
    if (!m_enabled) {
        p.setPen(QPen(pal.danger, 2));
        p.drawLine(r.bottomLeft() + QPointF(4, -4), r.topRight() + QPointF(-4, 4));
    }
    p.setPen(QPen(m_active ? pal.accent : (m_hover ? pal.text2 : pal.line), m_active ? 2.0 : 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 7, 7);
}

void ColorSwatch::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) emit clicked();
}

void ColorSwatch::enterEvent(QEnterEvent*)
{
    m_hover = true;
    update();
}

void ColorSwatch::leaveEvent(QEvent*)
{
    m_hover = false;
    update();
}

// --- ColorPicker -----------------------------------------------------------------------------

ColorPicker::ColorPicker(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(200, 200);
    m_hex = new QLineEdit(this);
    m_hex->setFont(Theme::ui(12, QFont::DemiBold));
    connect(m_hex, &QLineEdit::editingFinished, this, [this]() {
        QString t = m_hex->text().trimmed();
        if (!t.startsWith('#')) t.prepend('#');
        QColor c(t);
        if (c.isValid()) {
            c.setAlphaF(float(m_a));
            setColor(c);
            emit colorChanged(color());
            emit colorCommitted(color());
        }
    });
}

QColor ColorPicker::color() const { return QColor::fromHsvF(float(m_h), float(m_s), float(m_v), float(m_a)); }

void ColorPicker::setColor(const QColor& c)
{
    float h, s, v, a;
    c.getHsvF(&h, &s, &v, &a);
    if (h >= 0) m_h = h;
    m_s = s;
    m_v = v;
    m_a = a;
    m_hex->setText(c.name(QColor::HexRgb).toUpper());
    update();
}

QRectF ColorPicker::svRect() const { return QRectF(0, 0, width(), height() - 78); }
QRectF ColorPicker::hueRect() const { return QRectF(0, height() - 70, width(), 14); }
QRectF ColorPicker::alphaRect() const { return QRectF(0, height() - 50, width(), 14); }

void ColorPicker::resizeEvent(QResizeEvent*)
{
    m_hex->setGeometry(0, height() - 30, 110, 28);
    m_svHue = -1;
}

void ColorPicker::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF sv = svRect();
    if (m_svHue != m_h || m_svCache.size() != sv.size().toSize()) {
        const int w = std::max(1, int(sv.width())), h = std::max(1, int(sv.height()));
        m_svCache = QImage(w, h, QImage::Format_RGB32);
        for (int y = 0; y < h; ++y) {
            auto* line = reinterpret_cast<QRgb*>(m_svCache.scanLine(y));
            for (int x = 0; x < w; ++x)
                line[x] = QColor::fromHsvF(float(m_h), float(x / double(w - 1)), float(1.0 - y / double(h - 1))).rgb();
        }
        m_svHue = m_h;
    }
    QPainterPath clip;
    clip.addRoundedRect(sv, 8, 8);
    p.save();
    p.setClipPath(clip);
    p.drawImage(sv.topLeft(), m_svCache);
    p.restore();
    const QPointF knob(sv.left() + m_s * sv.width(), sv.top() + (1 - m_v) * sv.height());
    p.setPen(QPen(Qt::white, 2.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(knob, 7, 7);
    p.setPen(QPen(QColor(0, 0, 0, 90), 1));
    p.drawEllipse(knob, 8.5, 8.5);

    const QRectF hr = hueRect();
    QLinearGradient hg(hr.topLeft(), hr.topRight());
    for (int i = 0; i <= 6; ++i) hg.setColorAt(i / 6.0, QColor::fromHsvF(float(i / 6.0 >= 1 ? 0.999 : i / 6.0), 1, 1));
    p.setPen(Qt::NoPen);
    p.setBrush(hg);
    p.drawRoundedRect(hr, 7, 7);
    const QRectF ar = alphaRect();
    QPainterPath ap;
    ap.addRoundedRect(ar, 7, 7);
    p.save();
    p.setClipPath(ap);
    drawChecker(p, ar, 7);
    QLinearGradient ag(ar.topLeft(), ar.topRight());
    QColor c0 = color(), c1 = color();
    c0.setAlpha(0);
    c1.setAlpha(255);
    ag.setColorAt(0, c0);
    ag.setColorAt(1, c1);
    p.fillRect(ar, ag);
    p.restore();
    auto marker = [&](const QRectF& r, double t) {
        const QPointF c(r.left() + t * r.width(), r.center().y());
        p.setPen(QPen(Qt::white, 2.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, 7.5, 7.5);
    };
    marker(hr, m_h);
    marker(ar, m_a);
    // Current colour chip next to the hex field.
    const QRectF chip(width() - 64, height() - 30, 64, 28);
    QPainterPath cp;
    cp.addRoundedRect(chip, 7, 7);
    p.save();
    p.setClipPath(cp);
    drawChecker(p, chip);
    p.fillRect(chip, color());
    p.restore();
}

void ColorPicker::pick(QPointF pos)
{
    switch (m_drag) {
    case Part::SV: {
        const QRectF sv = svRect();
        m_s = std::clamp((pos.x() - sv.left()) / sv.width(), 0.0, 1.0);
        m_v = std::clamp(1.0 - (pos.y() - sv.top()) / sv.height(), 0.0, 1.0);
        break;
    }
    case Part::Hue: m_h = std::clamp((pos.x() - hueRect().left()) / hueRect().width(), 0.0, 0.999); break;
    case Part::Alpha: m_a = std::clamp((pos.x() - alphaRect().left()) / alphaRect().width(), 0.0, 1.0); break;
    case Part::None: return;
    }
    m_hex->setText(color().name(QColor::HexRgb).toUpper());
    update();
    emit colorChanged(color());
}

void ColorPicker::mousePressEvent(QMouseEvent* e)
{
    const QPointF p = e->position();
    if (svRect().contains(p)) m_drag = Part::SV;
    else if (hueRect().adjusted(0, -4, 0, 4).contains(p)) m_drag = Part::Hue;
    else if (alphaRect().adjusted(0, -4, 0, 4).contains(p)) m_drag = Part::Alpha;
    pick(p);
}

void ColorPicker::mouseMoveEvent(QMouseEvent* e) { pick(e->position()); }

void ColorPicker::mouseReleaseEvent(QMouseEvent*)
{
    if (m_drag != Part::None) emit colorCommitted(color());
    m_drag = Part::None;
}

void popupColorPicker(QWidget* anchor, const QColor& initial, const std::function<void(const QColor&, bool)>& onChange)
{
    auto* popup = new QFrame(anchor, Qt::Popup);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setStyleSheet(QString("QFrame { background: %1; border: 1px solid %2; border-radius: 12px; }")
                             .arg(Theme::p().bg2.name(), Theme::p().line.name()));
    auto* lay = new QVBoxLayout(popup);
    lay->setContentsMargins(12, 12, 12, 12);
    auto* picker = new ColorPicker(popup);
    picker->setFixedSize(236, 250);
    picker->setColor(initial);
    lay->addWidget(picker);
    QObject::connect(picker, &ColorPicker::colorChanged, popup, [onChange](const QColor& c) { onChange(c, false); });
    QObject::connect(picker, &ColorPicker::colorCommitted, popup, [onChange](const QColor& c) { onChange(c, true); });
    popup->adjustSize();
    QPoint pos = anchor->mapToGlobal(QPoint(anchor->width() + 6, 0));
    if (QScreen* s = anchor->screen()) {
        const QRect avail = s->availableGeometry();
        if (pos.x() + popup->width() > avail.right()) pos.setX(anchor->mapToGlobal(QPoint(0, 0)).x() - popup->width() - 6);
        if (pos.y() + popup->height() > avail.bottom()) pos.setY(avail.bottom() - popup->height());
    }
    popup->move(pos);
    popup->show();
}

// --- CurveEditor ---------------------------------------------------------------------------------

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent) { setMinimumSize(140, 110); }

void CurveEditor::setCurve(const ResponseCurve& c)
{
    m_curve = c;
    update();
}

void CurveEditor::setMarker(double x)
{
    m_marker = x;
    update();
}

QRectF CurveEditor::area() const { return QRectF(rect()).adjusted(8, 8, -8, -8); }
QPointF CurveEditor::toWidget(Vec2 p) const
{
    const QRectF a = area();
    return {a.left() + p.x * a.width(), a.bottom() - p.y * a.height()};
}
Vec2 CurveEditor::fromWidget(QPointF p) const
{
    const QRectF a = area();
    return {std::clamp((p.x() - a.left()) / a.width(), 0.0, 1.0), std::clamp((a.bottom() - p.y()) / a.height(), 0.0, 1.0)};
}

void CurveEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ui::Palette& pal = Theme::p();
    const QRectF a = area();
    p.setPen(Qt::NoPen);
    p.setBrush(pal.bg2);
    p.drawRoundedRect(a.adjusted(-6, -6, 6, 6), 10, 10);
    p.setPen(QPen(pal.line, 1));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(QPointF(a.left() + a.width() * i / 4, a.top()), QPointF(a.left() + a.width() * i / 4, a.bottom()));
        p.drawLine(QPointF(a.left(), a.top() + a.height() * i / 4), QPointF(a.right(), a.top() + a.height() * i / 4));
    }
    QPainterPath path;
    QPainterPath fill;
    fill.moveTo(toWidget({0, 0}));
    for (int i = 0; i <= 64; ++i) {
        const double x = i / 64.0;
        const QPointF w = toWidget({x, m_curve.eval(x)});
        if (i == 0) path.moveTo(w);
        else path.lineTo(w);
        fill.lineTo(w);
    }
    fill.lineTo(toWidget({1, 0}));
    fill.closeSubpath();
    p.fillPath(fill, ui::withAlpha(pal.accent, 40));
    p.setPen(QPen(pal.accent, 2.2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    for (const Vec2& v : m_curve.points) {
        p.setPen(QPen(pal.bg1, 2));
        p.setBrush(pal.text);
        p.drawEllipse(toWidget(v), 5, 5);
    }
    if (m_marker >= 0) {
        const QPointF m = toWidget({m_marker, m_curve.eval(m_marker)});
        p.setPen(Qt::NoPen);
        p.setBrush(pal.mint);
        p.drawEllipse(m, 5, 5);
    }
}

void CurveEditor::mousePressEvent(QMouseEvent* e)
{
    m_drag = -1;
    for (int i = 0; i < int(m_curve.points.size()); ++i)
        if (QLineF(toWidget(m_curve.points[i]), e->position()).length() < 9) m_drag = i;
    if (e->button() == Qt::RightButton && m_drag > 0 && m_drag < int(m_curve.points.size()) - 1) {
        m_curve.points.erase(m_curve.points.begin() + m_drag);
        m_drag = -1;
        update();
        emit curveChanged(m_curve);
    }
}

void CurveEditor::mouseMoveEvent(QMouseEvent* e)
{
    if (m_drag < 0) return;
    Vec2 v = fromWidget(e->position());
    auto& pts = m_curve.points;
    const double eps = 0.01;
    if (m_drag == 0) v.x = 0;
    else if (m_drag == int(pts.size()) - 1) v.x = 1;
    else v.x = std::clamp(v.x, pts[m_drag - 1].x + eps, pts[m_drag + 1].x - eps);
    pts[m_drag] = v;
    update();
    emit curveChanged(m_curve);
}

void CurveEditor::mouseReleaseEvent(QMouseEvent*) { m_drag = -1; }

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* e)
{
    const Vec2 v = fromWidget(e->position());
    auto& pts = m_curve.points;
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        if (v.x > pts[i].x + 0.01 && v.x < pts[i + 1].x - 0.01) {
            pts.insert(pts.begin() + long(i) + 1, v);
            break;
        }
    update();
    emit curveChanged(m_curve);
}

// --- Segmented --------------------------------------------------------------------------------------

Segmented::Segmented(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_anim = new QVariantAnimation(this);
    m_anim->setDuration(220);
    m_anim->setEasingCurve(QEasingCurve::OutBack);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_pos = v.toDouble();
        update();
    });
}

void Segmented::addSegment(const QString& text, const QString& icon, const QString& tip)
{
    m_segs.push_back({text, icon, tip});
    updateGeometry();
    update();
}

void Segmented::setCurrent(int i)
{
    if (i < 0 || i >= int(m_segs.size())) return;
    if (i == m_current && m_anim->state() != QAbstractAnimation::Running) {
        m_pos = i;
        update();
        return;
    }
    m_current = i;
    m_anim->stop();
    m_anim->setStartValue(m_pos);
    m_anim->setEndValue(double(i));
    m_anim->start();
}

QSize Segmented::sizeHint() const
{
    const QFontMetrics fm(Theme::ui(12, QFont::DemiBold));
    int w = 0;
    for (const Seg& s : m_segs) w += std::max(34, (s.text.isEmpty() ? 0 : fm.horizontalAdvance(s.text) + 18) + (s.icon.isEmpty() ? 0 : 22));
    return {w + 6, 32};
}

QRectF Segmented::segRect(int i) const
{
    const double w = (width() - 6.0) / std::max<size_t>(1, m_segs.size());
    return QRectF(3 + i * w, 3, w, height() - 6);
}

int Segmented::segAt(QPointF p) const
{
    for (int i = 0; i < int(m_segs.size()); ++i)
        if (segRect(i).contains(p)) return i;
    return -1;
}

void Segmented::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ui::Palette& pal = Theme::p();
    p.setPen(Qt::NoPen);
    p.setBrush(pal.bg2);
    p.drawRoundedRect(QRectF(rect()), 9, 9);
    if (!m_segs.empty()) {
        const double w = (width() - 6.0) / m_segs.size();
        p.setBrush(pal.accent);
        p.drawRoundedRect(QRectF(3 + m_pos * w, 3, w, height() - 6), 7, 7);
    }
    p.setFont(Theme::ui(12, QFont::DemiBold));
    for (int i = 0; i < int(m_segs.size()); ++i) {
        const QRectF r = segRect(i);
        const bool cur = i == m_current;
        if (!cur && i == m_hover) {
            p.setBrush(pal.bg3);
            p.drawRoundedRect(r, 7, 7);
        }
        const QColor c = cur ? pal.accentInk : pal.text2;
        const Seg& s = m_segs[i];
        if (!s.icon.isEmpty() && s.text.isEmpty()) {
            ui::paintIcon(p, s.icon, QRectF(r.center().x() - 9, r.center().y() - 9, 18, 18), c);
        } else if (!s.icon.isEmpty()) {
            ui::paintIcon(p, s.icon, QRectF(r.left() + 6, r.center().y() - 8, 16, 16), c);
            p.setPen(c);
            p.drawText(r.adjusted(24, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, s.text);
        } else {
            p.setPen(c);
            p.drawText(r, Qt::AlignCenter, s.text);
        }
    }
}

void Segmented::mousePressEvent(QMouseEvent* e)
{
    const int i = segAt(e->position());
    if (i < 0 || i == m_current) return;
    setCurrent(i);
    emit changed(i);
}

void Segmented::mouseMoveEvent(QMouseEvent* e)
{
    const int h = segAt(e->position());
    if (h != m_hover) {
        m_hover = h;
        update();
    }
}

void Segmented::leaveEvent(QEvent*)
{
    m_hover = -1;
    update();
}

bool Segmented::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        const int i = segAt(he->pos());
        if (i >= 0 && !m_segs[i].tip.isEmpty()) QToolTip::showText(he->globalPos(), m_segs[i].tip, this);
        else QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

} // namespace vx::app
