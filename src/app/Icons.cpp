// SPDX-License-Identifier: GPL-3.0-or-later
#include "Icons.h"
#include "Theme.h"

#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <cmath>
#include <functional>

namespace vx::ui {

namespace {

using Draw = std::function<void(QPainter&, const QColor&)>;

QPen linePen(const QColor& c, double w = 1.8)
{
    QPen pen(c, w);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

void stroke(QPainter& p, const QPainterPath& path, const QColor& c, double w = 1.8)
{
    p.setPen(linePen(c, w));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void fill(QPainter& p, const QPainterPath& path, const QColor& c)
{
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawPath(path);
}

QPainterPath poly(std::initializer_list<QPointF> pts, bool close = false)
{
    QPainterPath path;
    bool first = true;
    for (const QPointF& pt : pts) {
        if (first) path.moveTo(pt);
        else path.lineTo(pt);
        first = false;
    }
    if (close) path.closeSubpath();
    return path;
}

QPainterPath circle(double x, double y, double r)
{
    QPainterPath path;
    path.addEllipse(QPointF(x, y), r, r);
    return path;
}

QPainterPath rrect(double x, double y, double w, double h, double r)
{
    QPainterPath path;
    path.addRoundedRect(QRectF(x, y, w, h), r, r);
    return path;
}

const QHash<QString, Draw>& icons()
{
    static const QHash<QString, Draw> map = [] {
        QHash<QString, Draw> m;
        // --- tools --------------------------------------------------------
        m["select"] = [](QPainter& p, const QColor& c) {
            const auto a = poly({{6, 3.5}, {18.5, 12.5}, {12.6, 13.6}, {15.8, 20}, {13.3, 21.2}, {10.1, 14.8}, {6, 18.6}}, true);
            fill(p, a, c);
        };
        m["subselect"] = [](QPainter& p, const QColor& c) {
            const auto a = poly({{6, 3.5}, {18.5, 12.5}, {12.6, 13.6}, {15.8, 20}, {13.3, 21.2}, {10.1, 14.8}, {6, 18.6}}, true);
            stroke(p, a, c, 1.6);
        };
        m["transform"] = [](QPainter& p, const QColor& c) {
            QPainterPath r;
            r.addRect(QRectF(6, 6, 12, 12));
            stroke(p, r, c, 1.5);
            for (QPointF q : {QPointF(6, 6), QPointF(18, 6), QPointF(6, 18), QPointF(18, 18)})
                fill(p, rrect(q.x() - 2.2, q.y() - 2.2, 4.4, 4.4, 1), c);
            fill(p, circle(12, 12, 1.6), c);
        };
        m["lasso"] = [](QPainter& p, const QColor& c) {
            QPainterPath path;
            path.moveTo(9, 18);
            path.cubicTo(2, 16, 2, 5, 12, 5);
            path.cubicTo(21, 5, 22, 14, 13, 15);
            path.cubicTo(9, 15.4, 8, 18.5, 10.5, 21);
            stroke(p, path, c);
        };
        m["pen"] = [](QPainter& p, const QColor& c) {
            const auto nib = poly({{12, 3}, {18, 12}, {15, 20}, {9, 20}, {6, 12}}, true);
            stroke(p, nib, c);
            stroke(p, poly({{12, 3}, {12, 11}}), c);
            fill(p, circle(12, 12.5, 1.6), c);
        };
        m["line"] = [](QPainter& p, const QColor& c) {
            stroke(p, poly({{5, 19}, {19, 5}}), c, 2.0);
        };
        m["rect"] = [](QPainter& p, const QColor& c) { stroke(p, rrect(4.5, 6, 15, 12, 1.5), c); };
        m["oval"] = [](QPainter& p, const QColor& c) {
            QPainterPath e;
            e.addEllipse(QRectF(4, 6, 16, 12));
            stroke(p, e, c);
        };
        m["polystar"] = [](QPainter& p, const QColor& c) {
            QPainterPath s;
            for (int i = 0; i < 10; ++i) {
                const double a = -M_PI / 2 + i * M_PI / 5;
                const double r = (i % 2) ? 3.8 : 8.5;
                const QPointF q(12 + r * std::cos(a), 12.8 + r * std::sin(a));
                if (i == 0) s.moveTo(q);
                else s.lineTo(q);
            }
            s.closeSubpath();
            stroke(p, s, c, 1.6);
        };
        m["pencil"] = [](QPainter& p, const QColor& c) {
            stroke(p, poly({{5, 19}, {6, 15}, {16, 5}, {19, 8}, {9, 18}}, true), c, 1.6);
            stroke(p, poly({{14, 7}, {17, 10}}), c, 1.4);
        };
        m["brush"] = [](QPainter& p, const QColor& c) {
            QPainterPath handle = poly({{20, 4}, {12, 13}});
            stroke(p, handle, c, 2.2);
            QPainterPath blob;
            blob.moveTo(11.5, 12);
            blob.cubicTo(14.5, 14, 13.5, 18, 10, 19.5);
            blob.cubicTo(7.5, 20.5, 5, 20, 4, 20.5);
            blob.cubicTo(5, 18, 5, 15, 7.5, 13.2);
            blob.cubicTo(8.8, 12.2, 10.3, 11.5, 11.5, 12);
            fill(p, blob, c);
        };
        m["paintbrush"] = [](QPainter& p, const QColor& c) {
            stroke(p, poly({{20, 4}, {13, 11}}), c, 2.2);
            QPainterPath ferrule = poly({{12, 10}, {15, 13}, {12.5, 15.5}, {9.5, 12.5}}, true);
            fill(p, ferrule, c);
            for (int i = 0; i < 7; ++i) {
                const double t = i / 6.0;
                const QPointF from(9.5 + t * 3.0, 12.5 + t * 3.0);
                const QPointF to(4.5 + t * 3.5 - (i % 2) * 0.6, 17 + t * 3.0 + (i % 2) * 0.6);
                stroke(p, poly({from, to}), c, 1.0);
            }
        };
        m["eraser"] = [](QPainter& p, const QColor& c) {
            stroke(p, poly({{4, 15}, {13, 6}, {19.5, 12.5}, {12, 20}, {8.5, 20}}, true), c);
            stroke(p, poly({{8.5, 10.5}, {15, 17}}), c);
            stroke(p, poly({{12, 20}, {20, 20}}), c);
        };
        m["bucket"] = [](QPainter& p, const QColor& c) {
            stroke(p, poly({{5, 11}, {11, 5}, {18, 12}, {12, 18}}, true), c);
            QPainterPath drop;
            drop.moveTo(19.5, 14);
            drop.cubicTo(18, 17, 18, 19, 19.5, 19.5);
            drop.cubicTo(21, 19, 21, 17, 19.5, 14);
            fill(p, drop, c);
            stroke(p, poly({{8, 2.5}, {11, 5}}), c, 1.5);
        };
        m["inkbottle"] = [](QPainter& p, const QColor& c) {
            stroke(p, rrect(6, 10, 12, 10, 3), c);
            stroke(p, poly({{9, 10}, {9, 6}, {15, 6}, {15, 10}}), c);
            stroke(p, poly({{10.5, 3.5}, {13.5, 3.5}}), c);
        };
        m["eyedropper"] = [](QPainter& p, const QColor& c) {
            stroke(p, poly({{4.5, 19.5}, {6, 15}, {13, 8}, {16, 11}, {9, 18}}, true), c, 1.6);
            QPainterPath bulb = poly({{13.5, 6.5}, {16.5, 3.5}, {20.5, 7.5}, {17.5, 10.5}}, true);
            fill(p, bulb, c);
        };
        m["hand"] = [](QPainter& p, const QColor& c) {
            QPainterPath h;
            h.moveTo(8, 13);
            h.lineTo(8, 6.5);
            h.cubicTo(8, 5, 10.2, 5, 10.2, 6.5);
            h.lineTo(10.2, 11);
            h.lineTo(10.2, 4.8);
            h.cubicTo(10.2, 3.3, 12.4, 3.3, 12.4, 4.8);
            h.lineTo(12.4, 11);
            h.lineTo(12.4, 5.8);
            h.cubicTo(12.4, 4.3, 14.6, 4.3, 14.6, 5.8);
            h.lineTo(14.6, 11.5);
            h.lineTo(14.6, 8);
            h.cubicTo(14.6, 6.5, 16.8, 6.5, 16.8, 8);
            h.lineTo(16.8, 14.5);
            h.cubicTo(16.8, 18.5, 14.5, 21, 11.5, 21);
            h.cubicTo(9, 21, 7.5, 19.5, 6, 17);
            h.lineTo(4.5, 14);
            h.cubicTo(3.8, 12.6, 5.5, 11.5, 6.5, 12.6);
            h.lineTo(8, 14.5);
            stroke(p, h, c, 1.5);
        };
        m["zoom"] = [](QPainter& p, const QColor& c) {
            stroke(p, circle(10.5, 10.5, 6), c);
            stroke(p, poly({{15, 15}, {20, 20}}), c, 2.2);
        };
        // --- timeline / layers -------------------------------------------
        m["eye"] = [](QPainter& p, const QColor& c) {
            QPainterPath e;
            e.moveTo(3, 12);
            e.cubicTo(6, 6.5, 18, 6.5, 21, 12);
            e.cubicTo(18, 17.5, 6, 17.5, 3, 12);
            stroke(p, e, c, 1.6);
            fill(p, circle(12, 12, 2.6), c);
        };
        m["eyeoff"] = [](QPainter& p, const QColor& c) {
            QPainterPath e;
            e.moveTo(3, 12);
            e.cubicTo(6, 6.5, 18, 6.5, 21, 12);
            e.cubicTo(18, 17.5, 6, 17.5, 3, 12);
            stroke(p, e, withAlpha(c, 110), 1.6);
            stroke(p, poly({{4, 20}, {20, 4}}), c, 1.8);
        };
        m["lock"] = [](QPainter& p, const QColor& c) {
            fill(p, rrect(6, 11, 12, 9, 2), c);
            QPainterPath sh;
            sh.moveTo(8.5, 11);
            sh.lineTo(8.5, 8);
            sh.cubicTo(8.5, 3.5, 15.5, 3.5, 15.5, 8);
            sh.lineTo(15.5, 11);
            stroke(p, sh, c, 1.8);
        };
        m["unlock"] = [](QPainter& p, const QColor& c) {
            stroke(p, rrect(6, 11, 12, 9, 2), c, 1.5);
            QPainterPath sh;
            sh.moveTo(8.5, 11);
            sh.lineTo(8.5, 8);
            sh.cubicTo(8.5, 3.5, 15.5, 3.5, 15.5, 7);
            stroke(p, sh, c, 1.5);
        };
        m["outline"] = [](QPainter& p, const QColor& c) { stroke(p, rrect(6, 6, 12, 12, 1.5), c, 1.8); };
        m["layer"] = [](QPainter& p, const QColor& c) {
            stroke(p, poly({{12, 4}, {21, 9}, {12, 14}, {3, 9}}, true), c, 1.6);
            stroke(p, poly({{3, 13.5}, {12, 18.5}, {21, 13.5}}), c, 1.6);
        };
        m["folder"] = [](QPainter& p, const QColor& c) {
            stroke(p, poly({{3.5, 7}, {9.5, 7}, {11.5, 9}, {20.5, 9}, {20.5, 18.5}, {3.5, 18.5}}, true), c, 1.6);
        };
        m["mask"] = [](QPainter& p, const QColor& c) {
            stroke(p, rrect(4, 4, 16, 16, 3), c, 1.5);
            fill(p, circle(12, 12, 4.5), c);
        };
        m["guide"] = [](QPainter& p, const QColor& c) {
            QPainterPath path;
            path.moveTo(4, 19);
            path.cubicTo(8, 4, 16, 20, 20, 5);
            QPen pen = linePen(c, 1.6);
            pen.setDashPattern({2, 2});
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
            fill(p, circle(20, 5, 2), c);
        };
        m["plus"] = [](QPainter& p, const QColor& c) {
            stroke(p, poly({{12, 5}, {12, 19}}), c, 2.0);
            stroke(p, poly({{5, 12}, {19, 12}}), c, 2.0);
        };
        m["minus"] = [](QPainter& p, const QColor& c) { stroke(p, poly({{5, 12}, {19, 12}}), c, 2.0); };
        m["trash"] = [](QPainter& p, const QColor& c) {
            stroke(p, poly({{6, 8}, {7.3, 20}, {16.7, 20}, {18, 8}}), c, 1.6);
            stroke(p, poly({{4, 7}, {20, 7}}), c, 1.6);
            stroke(p, poly({{9.5, 7}, {10, 4}, {14, 4}, {14.5, 7}}), c, 1.6);
        };
        m["play"] = [](QPainter& p, const QColor& c) { fill(p, poly({{7.5, 5}, {19, 12}, {7.5, 19}}, true), c); };
        m["pause"] = [](QPainter& p, const QColor& c) {
            fill(p, rrect(6.5, 5, 4, 14, 1.2), c);
            fill(p, rrect(13.5, 5, 4, 14, 1.2), c);
        };
        m["first"] = [](QPainter& p, const QColor& c) {
            fill(p, rrect(5, 6, 2.4, 12, 1), c);
            fill(p, poly({{19, 6}, {9, 12}, {19, 18}}, true), c);
        };
        m["last"] = [](QPainter& p, const QColor& c) {
            fill(p, rrect(16.6, 6, 2.4, 12, 1), c);
            fill(p, poly({{5, 6}, {15, 12}, {5, 18}}, true), c);
        };
        m["prev"] = [](QPainter& p, const QColor& c) { fill(p, poly({{16, 6}, {8, 12}, {16, 18}}, true), c); };
        m["next"] = [](QPainter& p, const QColor& c) { fill(p, poly({{8, 6}, {16, 12}, {8, 18}}, true), c); };
        m["loop"] = [](QPainter& p, const QColor& c) {
            QPainterPath path;
            path.moveTo(6, 13);
            path.cubicTo(6, 8, 9, 7, 12, 7);
            path.lineTo(18, 7);
            stroke(p, path, c, 1.6);
            stroke(p, poly({{15.5, 4.5}, {18, 7}, {15.5, 9.5}}), c, 1.6);
            QPainterPath back;
            back.moveTo(18, 11);
            back.cubicTo(18, 16, 15, 17, 12, 17);
            back.lineTo(6, 17);
            stroke(p, back, c, 1.6);
            stroke(p, poly({{8.5, 14.5}, {6, 17}, {8.5, 19.5}}), c, 1.6);
        };
        m["onion"] = [](QPainter& p, const QColor& c) {
            stroke(p, circle(9.5, 12, 5.5), withAlpha(c, 120), 1.5);
            fill(p, circle(14.5, 12, 5.5), c);
        };
        m["onionoutline"] = [](QPainter& p, const QColor& c) {
            stroke(p, circle(9.5, 12, 5.5), withAlpha(c, 120), 1.5);
            stroke(p, circle(14.5, 12, 5.5), c, 1.5);
        };
        m["swap"] = [](QPainter& p, const QColor& c) {
            QPainterPath path;
            path.moveTo(6, 9);
            path.cubicTo(6, 5, 10, 5, 14, 5);
            stroke(p, path, c, 1.6);
            stroke(p, poly({{12, 3}, {14.5, 5}, {12, 7}}), c, 1.6);
            QPainterPath b;
            b.moveTo(18, 15);
            b.cubicTo(18, 19, 14, 19, 10, 19);
            stroke(p, b, c, 1.6);
            stroke(p, poly({{12, 17}, {9.5, 19}, {12, 21}}), c, 1.6);
        };
        m["none"] = [](QPainter& p, const QColor& c) {
            stroke(p, circle(12, 12, 7), c, 1.6);
            stroke(p, poly({{7, 17}, {17, 7}}), c, 1.6);
        };
        m["symbol"] = [](QPainter& p, const QColor& c) {
            stroke(p, circle(12, 12, 7.5), c, 1.6);
            stroke(p, poly({{12, 7}, {12, 17}}), c, 1.4);
            stroke(p, poly({{7, 12}, {17, 12}}), c, 1.4);
        };
        m["movieclip"] = [](QPainter& p, const QColor& c) {
            stroke(p, rrect(4, 6, 16, 12, 2), c, 1.5);
            for (double x : {7.0, 11.0, 15.0}) {
                fill(p, rrect(x, 7.5, 2, 1.8, 0.4), c);
                fill(p, rrect(x, 14.7, 2, 1.8, 0.4), c);
            }
        };
        m["graphic"] = [](QPainter& p, const QColor& c) {
            fill(p, circle(9, 10, 4.5), withAlpha(c, 150));
            fill(p, poly({{12, 19}, {16, 9}, {20.5, 19}}, true), c);
        };
        m["button"] = [](QPainter& p, const QColor& c) {
            stroke(p, rrect(3.5, 7, 17, 10, 5), c, 1.5);
            fill(p, circle(12, 12, 2), c);
        };
        m["gear"] = [](QPainter& p, const QColor& c) {
            stroke(p, circle(12, 12, 3), c, 1.6);
            for (int i = 0; i < 8; ++i) {
                const double a = i * M_PI / 4;
                stroke(p, poly({{12 + 5.5 * std::cos(a), 12 + 5.5 * std::sin(a)}, {12 + 8 * std::cos(a), 12 + 8 * std::sin(a)}}), c, 2.2);
            }
            stroke(p, circle(12, 12, 5.8), c, 1.6);
        };
        m["chevron-down"] = [](QPainter& p, const QColor& c) { stroke(p, poly({{7, 10}, {12, 15}, {17, 10}}), c, 1.8); };
        m["chevron-right"] = [](QPainter& p, const QColor& c) { stroke(p, poly({{10, 7}, {15, 12}, {10, 17}}), c, 1.8); };
        m["close"] = [](QPainter& p, const QColor& c) {
            stroke(p, poly({{7, 7}, {17, 17}}), c, 1.8);
            stroke(p, poly({{17, 7}, {7, 17}}), c, 1.8);
        };
        m["object"] = [](QPainter& p, const QColor& c) {
            QPen pen = linePen(c, 1.6);
            pen.setDashPattern({2.2, 1.8});
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(4.5, 4.5, 15, 15), 2, 2);
            fill(p, circle(12, 12, 3.5), c);
        };
        m["pressure"] = [](QPainter& p, const QColor& c) {
            QPainterPath path;
            path.moveTo(4, 17);
            path.cubicTo(8, 17, 9, 7, 12, 7);
            path.cubicTo(15, 7, 16, 17, 20, 17);
            stroke(p, path, c, 1.6);
            fill(p, circle(12, 7, 1.8), c);
        };
        m["tilt"] = [](QPainter& p, const QColor& c) {
            stroke(p, poly({{6, 19}, {17, 5}}), c, 2.0);
            QPainterPath arc;
            arc.moveTo(6, 12);
            arc.cubicTo(8, 12, 10, 13.5, 10.5, 15.5);
            stroke(p, arc, c, 1.4);
            stroke(p, poly({{4, 19}, {20, 19}}), c, 1.4);
        };
        m["snap"] = [](QPainter& p, const QColor& c) {
            QPainterPath u;
            u.moveTo(6, 5);
            u.lineTo(6, 12);
            u.cubicTo(6, 20, 18, 20, 18, 12);
            u.lineTo(18, 5);
            stroke(p, u, c, 2.0);
            stroke(p, poly({{6, 8.5}, {9.5, 8.5}}), c, 2.0);
            stroke(p, poly({{14.5, 8.5}, {18, 8.5}}), c, 2.0);
        };
        m["library"] = [](QPainter& p, const QColor& c) {
            stroke(p, rrect(4, 5, 4, 14, 1), c, 1.5);
            stroke(p, rrect(10, 5, 4, 14, 1), c, 1.5);
            stroke(p, poly({{16, 6}, {19.5, 5.2}, {21.5, 18.2}, {18, 19}}, true), c, 1.5);
        };
        m["palette"] = [](QPainter& p, const QColor& c) {
            QPainterPath pal;
            pal.moveTo(12, 3.5);
            pal.cubicTo(5, 3.5, 3.5, 9, 3.5, 12);
            pal.cubicTo(3.5, 17, 7, 20.5, 12, 20.5);
            pal.cubicTo(14, 20.5, 14, 18, 13, 17);
            pal.cubicTo(12, 15.5, 13, 14, 15, 14);
            pal.lineTo(17, 14);
            pal.cubicTo(19.5, 14, 20.5, 12.5, 20.5, 10.5);
            pal.cubicTo(20.5, 6, 17, 3.5, 12, 3.5);
            stroke(p, pal, c, 1.5);
            fill(p, circle(8, 10, 1.4), c);
            fill(p, circle(11.5, 7, 1.4), c);
            fill(p, circle(15.5, 8.5, 1.4), c);
        };
        m["sliders"] = [](QPainter& p, const QColor& c) {
            for (double y : {7.0, 12.0, 17.0}) stroke(p, poly({{4, y}, {20, y}}), c, 1.5);
            fill(p, circle(9, 7, 2.2), c);
            fill(p, circle(15, 12, 2.2), c);
            fill(p, circle(7, 17, 2.2), c);
        };
        m["keyframe"] = [](QPainter& p, const QColor& c) { fill(p, poly({{12, 5}, {19, 12}, {12, 19}, {5, 12}}, true), c); };
        m["hint"] = [](QPainter& p, const QColor& c) {
            fill(p, circle(12, 12, 7), c);
            p.setPen(Theme::p().accentInk);
            QFont f = Theme::ui(10, QFont::Bold);
            p.setFont(f);
            p.drawText(QRectF(5, 5, 14, 14), Qt::AlignCenter, "a");
        };
        m["flip-h"] = [](QPainter& p, const QColor& c) {
            fill(p, poly({{11, 5}, {11, 19}, {4, 19}}, true), c);
            stroke(p, poly({{13, 5}, {13, 19}, {20, 19}}, true), c, 1.5);
        };
        m["flip-v"] = [](QPainter& p, const QColor& c) {
            fill(p, poly({{5, 11}, {19, 11}, {19, 4}}, true), c);
            stroke(p, poly({{5, 13}, {19, 13}, {19, 20}}, true), c, 1.5);
        };
        m["rotate"] = [](QPainter& p, const QColor& c) {
            QPainterPath arc;
            arc.arcMoveTo(QRectF(5, 5, 14, 14), 90);
            arc.arcTo(QRectF(5, 5, 14, 14), 90, -270);
            stroke(p, arc, c, 1.6);
            stroke(p, poly({{9, 3}, {12, 5}, {9.5, 8}}), c, 1.6);
        };
        m["tablet"] = [](QPainter& p, const QColor& c) {
            stroke(p, rrect(3, 6, 18, 13, 2.5), c, 1.5);
            stroke(p, poly({{14, 15}, {20, 3}}), c, 2.0);
        };
        return m;
    }();
    return map;
}

class VxIconEngine : public QIconEngine {
public:
    explicit VxIconEngine(QString name) : m_name(std::move(name)) {}
    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        const Palette& pal = Theme::p();
        QColor c = pal.text2;
        if (mode == QIcon::Disabled) c = pal.text3;
        else if (state == QIcon::On) c = pal.accent;
        else if (mode == QIcon::Active || mode == QIcon::Selected) c = pal.text;
        paintIcon(*p, m_name, rect, c);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap pm(size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap pm(size * scale);
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }
    QIconEngine* clone() const override { return new VxIconEngine(m_name); }

private:
    QString m_name;
};

} // namespace

void paintIcon(QPainter& p, const QString& name, const QRectF& r, const QColor& c)
{
    auto it = icons().find(name);
    if (it == icons().end()) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(r.topLeft());
    p.scale(r.width() / 24.0, r.height() / 24.0);
    it.value()(p, c);
    p.restore();
}

QIcon icon(const QString& name) { return QIcon(new VxIconEngine(name)); }

QPixmap iconPixmap(const QString& name, int size, const QColor& c, qreal dpr)
{
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    paintIcon(p, name, QRectF(0, 0, size, size), c);
    return pm;
}

} // namespace vx::ui
