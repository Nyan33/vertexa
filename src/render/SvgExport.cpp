// SPDX-License-Identifier: GPL-3.0-or-later
#include "SvgExport.h"
#include "DabEngine.h"

#include "core/Evaluate.h"

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QRegularExpression>
#include <QTextStream>

namespace vx {

namespace {

QString num(double v) { return QString::number(v, 'g', 10); }

QString matrixAttr(const Affine& m)
{
    if (m.isIdentity()) return {};
    return QString(" transform=\"matrix(%1 %2 %3 %4 %5 %6)\"").arg(num(m.a), num(m.b), num(m.c), num(m.d), num(m.tx), num(m.ty));
}

QString pathData(const std::vector<Cubic>& chain, bool close)
{
    if (chain.empty()) return {};
    QString d = QString("M%1 %2").arg(num(chain.front().p0.x), num(chain.front().p0.y));
    for (const Cubic& c : chain) {
        if (c.isStraight(1e-9)) d += QString("L%1 %2").arg(num(c.p3.x), num(c.p3.y));
        else
            d += QString("C%1 %2 %3 %4 %5 %6")
                     .arg(num(c.p1.x), num(c.p1.y), num(c.p2.x), num(c.p2.y), num(c.p3.x), num(c.p3.y));
    }
    if (close) d += "Z";
    return d;
}

QString colorAttr(const Color& c) { return QString("#%1").arg(QString::fromStdString(c.hex()).mid(1)); }

QString cssBlend(BlendMode m)
{
    switch (m) {
    case BlendMode::Multiply: return "multiply";
    case BlendMode::Screen: return "screen";
    case BlendMode::Overlay: return "overlay";
    case BlendMode::Darken: return "darken";
    case BlendMode::Lighten: return "lighten";
    case BlendMode::ColorDodge: return "color-dodge";
    case BlendMode::ColorBurn: return "color-burn";
    case BlendMode::HardLight: return "hard-light";
    case BlendMode::SoftLight: return "soft-light";
    case BlendMode::Difference: return "difference";
    case BlendMode::Exclusion: return "exclusion";
    case BlendMode::Hue: return "hue";
    case BlendMode::Saturation: return "saturation";
    case BlendMode::Color: return "color";
    case BlendMode::Luminosity: return "luminosity";
    default: return {};
    }
}

class Writer {
public:
    explicit Writer(const Document& d) : m_doc(d) {}

    QString defs;
    QString body;

    QString paint(const FillStyle& f, const ColorTransform& ct)
    {
        const FillStyle s = ct.isIdentity() ? f : f.withColorTransform(ct);
        if (s.kind == FillStyle::Kind::Solid)
            return QString("\"%1\"").arg(colorAttr(s.color));
        const QString id = QString("g%1").arg(++m_ids);
        const Affine& m = s.gradient.matrix;
        QString spread = s.gradient.spread == SpreadMode::Pad ? "pad" : s.gradient.spread == SpreadMode::Reflect ? "reflect" : "repeat";
        if (s.kind == FillStyle::Kind::Linear)
            defs += QString("<linearGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" x1=\"-1\" y1=\"0\" x2=\"1\" y2=\"0\" spreadMethod=\"%2\" "
                            "gradientTransform=\"matrix(%3 %4 %5 %6 %7 %8)\">")
                        .arg(id, spread, num(m.a), num(m.b), num(m.c), num(m.d), num(m.tx), num(m.ty));
        else
            defs += QString("<radialGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" cx=\"0\" cy=\"0\" r=\"1\" fx=\"%2\" fy=\"0\" spreadMethod=\"%3\" "
                            "gradientTransform=\"matrix(%4 %5 %6 %7 %8 %9)\">")
                        .arg(id, num(s.gradient.focal), spread, num(m.a), num(m.b), num(m.c), num(m.d), num(m.tx))
                        .arg(num(m.ty));
        for (const GradientStop& st : s.gradient.stops)
            defs += QString("<stop offset=\"%1\" stop-color=\"%2\" stop-opacity=\"%3\"/>").arg(num(st.pos), colorAttr(st.color), num(st.color.a / 255.0));
        defs += s.kind == FillStyle::Kind::Linear ? "</linearGradient>" : "</radialGradient>";
        return QString("\"url(#%1)\"").arg(id);
    }

    void shape(const ShapeRenderData& rd, const Affine& m, const ColorTransform& ct)
    {
        body += QString("<g%1>").arg(matrixAttr(m));
        for (const auto& fp : rd.fills) {
            QString d;
            for (const Contour& c : fp.contours) d += pathData(c, true);
            const FillStyle s = ct.isIdentity() ? fp.style : fp.style.withColorTransform(ct);
            const double op = s.kind == FillStyle::Kind::Solid ? s.color.a / 255.0 : 1.0;
            body += QString("<path d=\"%1\" fill=%2%3 fill-rule=\"nonzero\"/>")
                        .arg(d, paint(fp.style, ct), op < 1.0 ? QString(" fill-opacity=\"%1\"").arg(num(op)) : QString());
        }
        for (const auto& sp : rd.strokes) {
            QString d;
            for (size_t i = 0; i < sp.chains.size(); ++i) d += pathData(sp.chains[i], sp.closed[i]);
            const StrokeStyle& st = sp.style;
            const FillStyle s = ct.isIdentity() ? st.paint : st.paint.withColorTransform(ct);
            const double op = s.kind == FillStyle::Kind::Solid ? s.color.a / 255.0 : 1.0;
            QString extra;
            if (st.pattern == StrokePattern::Dashed) extra += QString(" stroke-dasharray=\"%1 %2\"").arg(num(st.dash), num(st.gap));
            if (st.pattern == StrokePattern::Dotted) extra += QString(" stroke-dasharray=\"0 %1\"").arg(num(st.gap + st.width));
            if (!st.scaleWithTransform || st.pattern == StrokePattern::Hairline) extra += " vector-effect=\"non-scaling-stroke\"";
            if (op < 1.0) extra += QString(" stroke-opacity=\"%1\"").arg(num(op));
            const char* cap = st.cap == CapStyle::Round ? "round" : st.cap == CapStyle::Square ? "square" : "butt";
            const char* join = st.join == JoinStyle::Round ? "round" : st.join == JoinStyle::Miter ? "miter" : "bevel";
            body += QString("<path d=\"%1\" fill=\"none\" stroke=%2 stroke-width=\"%3\" stroke-linecap=\"%4\" stroke-linejoin=\"%5\"%6/>")
                        .arg(d, paint(st.paint, ct), num(st.pattern == StrokePattern::Hairline ? 1.0 : st.width), cap, join, extra);
        }
        body += "</g>";
    }

    void paintElement(const PaintElement& p, const Affine& m, const ColorTransform& ct)
    {
        const Rect b = p.localBounds();
        if (b.isEmpty()) return;
        const double scale = 2.0;
        const int w = std::max(1, int(std::ceil(b.width() * scale))), h = std::max(1, int(std::ceil(b.height() * scale)));
        if (double(w) * h > 64e6) return;
        QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
        img.fill(0);
        DabContext ctx;
        ctx.toDevice = Affine::scale(scale) * Affine::translate(-b.x0, -b.y0);
        ctx.color = ct;
        ctx.doc = &m_doc;
        vx::paintElement(img, p, ctx);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        body += QString("<g%1><image x=\"%2\" y=\"%3\" width=\"%4\" height=\"%5\" href=\"data:image/png;base64,%6\"/></g>")
                    .arg(matrixAttr(m), num(b.x0), num(b.y0), num(w / scale), num(h / scale), QString::fromLatin1(png.toBase64()));
    }

    /// SVG equivalent of Animate filters (outer shadows and glows, blur, adjust
    /// colour); returns the filter id or an empty string.
    QString svgFilter(const FilterList& filters)
    {
        QString prims;
        QString in = QStringLiteral("SourceGraphic");
        int n = 0;
        auto sigma = [](const Filter& f, double blur) { return std::sqrt(std::max(1, f.quality) * std::max(0.0, blur * blur - 1) / 12.0); };
        for (const Filter& f : filters) {
            if (!f.enabled) continue;
            const QString out = QString("r%1").arg(++n);
            switch (f.type) {
            case FilterType::Blur:
                prims += QString("<feGaussianBlur in=\"%1\" stdDeviation=\"%2 %3\" result=\"%4\"/>")
                             .arg(in, num(sigma(f, f.blurX)), num(sigma(f, f.blurY)), out);
                break;
            case FilterType::DropShadow:
            case FilterType::Glow: {
                if (f.inner || f.knockout || f.hideObject) continue;
                const double a = f.angle * kPi / 180.0;
                const double d = f.type == FilterType::Glow ? 0.0 : f.distance;
                prims += QString("<feDropShadow in=\"%1\" dx=\"%2\" dy=\"%3\" stdDeviation=\"%4 %5\" flood-color=\"#%6\" "
                                 "flood-opacity=\"%7\" result=\"%8\"/>")
                             .arg(in, num(std::cos(a) * d), num(std::sin(a) * d), num(sigma(f, f.blurX)), num(sigma(f, f.blurY)),
                                  QString::asprintf("%02X%02X%02X", f.color.r, f.color.g, f.color.b),
                                  num(std::min(1.0, f.color.a / 255.0 * f.strength)), out);
                break;
            }
            case FilterType::AdjustColor: {
                const auto mat = adjustColorMatrix(f);
                QStringList values;
                for (int i = 0; i < 20; ++i) values << num(i % 5 == 4 ? mat[i] / 255.0 : mat[i]);
                prims += QString("<feColorMatrix in=\"%1\" type=\"matrix\" values=\"%2\" result=\"%3\"/>")
                             .arg(in, values.join(' '), out);
                break;
            }
            default: continue;
            }
            in = out;
        }
        if (prims.isEmpty()) return {};
        const QString id = QString("f%1").arg(++m_ids);
        defs += QString("<filter id=\"%1\" x=\"-50%\" y=\"-50%\" width=\"200%\" height=\"200%\">%2</filter>").arg(id, prims);
        return id;
    }

    void element(const EvalItem& it, const Affine& parent, const ColorTransform& ct, int depth)
    {
        const Element& e = *it.element;
        const Affine m = parent * e.matrix;
        switch (e.type()) {
        case ElementType::Shape:
            shape(static_cast<const ShapeElement&>(e).graph->renderData(), m, ct);
            break;
        case ElementType::Morph:
            if (const auto& d = static_cast<const MorphElement&>(e).data) shape(*d, m, ct);
            break;
        case ElementType::Group:
            for (const ElementPtr& c : static_cast<const GroupElement&>(e).children) element({c, it.localFrame}, m, ct, depth);
            break;
        case ElementType::Paint: paintElement(static_cast<const PaintElement&>(e), m, ct); break;
        case ElementType::Instance: {
            const auto& in = static_cast<const InstanceElement&>(e);
            const Symbol* s = m_doc.symbol(in.symbolId);
            if (!s || !in.visible || depth > 32) return;
            const QString blend = s->type != SymbolType::Graphic ? cssBlend(in.blend) : QString();
            const QString filter = s->type != SymbolType::Graphic ? svgFilter(in.filters) : QString();
            if (!blend.isEmpty()) body += QString("<g style=\"mix-blend-mode:%1\">").arg(blend);
            if (!filter.isEmpty()) body += QString("<g filter=\"url(#%1)\">").arg(filter);
            timeline(s->timeline, instanceSymbolFrame(m_doc, in, it.localFrame), m, ct * in.color.toTransform(), depth + 1);
            if (!filter.isEmpty()) body += "</g>";
            if (!blend.isEmpty()) body += "</g>";
            break;
        }
        }
    }

    void layerItems(const Timeline& tl, int li, int frame, const Affine& m, const ColorTransform& ct, int depth)
    {
        for (const EvalItem& it : evaluateLayer(m_doc, tl, li, frame)) element(it, m, ct, depth);
    }

    void timeline(const Timeline& tl, int frame, const Affine& m, const ColorTransform& ct, int depth)
    {
        const int n = int(tl.layers.size());
        for (int i = n - 1; i >= 0; --i) {
            const Layer& l = tl.layers[i];
            if (!l.visible || l.type == LayerType::Folder || l.type == LayerType::Guide) continue;
            if (tl.maskOf(i)) continue; // drawn with their mask
            if (l.type == LayerType::Mask) {
                const QString id = QString("m%1").arg(++m_ids);
                // Clip path from the mask content.
                QString saved = body;
                body.clear();
                layerItems(tl, i, frame, m, {}, depth);
                QString clip = body;
                clip.replace(QRegularExpression(" fill=\"url\\(#[^)]*\\)\"| fill=\"#[0-9A-Fa-f]{6}\""), "");
                body = saved;
                defs += QString("<clipPath id=\"%1\">%2</clipPath>").arg(id, clip);
                body += QString("<g clip-path=\"url(#%1)\">").arg(id);
                for (int j = n - 1; j >= 0; --j)
                    if (tl.layers[j].parentId == l.id && tl.layers[j].visible) layerItems(tl, j, frame, m, ct, depth);
                body += "</g>";
                continue;
            }
            const bool group = l.opacity < 1.0 || l.blend != BlendMode::Normal;
            if (group) {
                QString style;
                if (!cssBlend(l.blend).isEmpty()) style = QString(" style=\"mix-blend-mode:%1\"").arg(cssBlend(l.blend));
                body += QString("<g opacity=\"%1\"%2>").arg(num(l.opacity), style);
            }
            layerItems(tl, i, frame, m, ct, depth);
            if (group) body += "</g>";
        }
    }

private:
    const Document& m_doc;
    int m_ids = 0;
};

} // namespace

QString frameToSvg(const Document& doc, const Timeline& tl, int frame)
{
    Writer w(doc);
    w.timeline(tl, frame, Affine{}, ColorTransform{}, 0);
    QString out;
    out += QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%1\" height=\"%2\" "
                   "viewBox=\"0 0 %1 %2\">\n")
               .arg(num(doc.width), num(doc.height));
    out += "<!-- Exported by Vertexa -->\n";
    if (!w.defs.isEmpty()) out += "<defs>" + w.defs + "</defs>\n";
    out += QString("<rect width=\"100%\" height=\"100%\" fill=\"%1\"/>\n").arg(colorAttr(doc.background));
    out += w.body + "\n</svg>\n";
    return out;
}

bool saveFrameSvg(const Document& doc, const Timeline& tl, int frame, const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(frameToSvg(doc, tl, frame).toUtf8());
    return true;
}

} // namespace vx
