// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — XFL import (Flash CS5 … Animate): DOMDocument.xml, LIBRARY/*.xml,
// either zipped (.fla) or as a folder (.xfl).
#include "FlaCommon.h"
#include "FlaImport.h"
#include "Zip.h"

#include "core/DocumentOps.h"
#include "core/ShapeOps.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QXmlStreamReader>

#include <cmath>
#include <map>
#include <memory>

namespace vx::io {

namespace {

// --- a small XML tree ------------------------------------------------------------------

struct XNode {
    QString name;
    QHash<QString, QString> attrs;
    std::vector<std::unique_ptr<XNode>> kids;

    const XNode* child(QStringView n) const
    {
        for (const auto& k : kids)
            if (k->name == n) return k.get();
        return nullptr;
    }
    std::vector<const XNode*> children(QStringView n = {}) const
    {
        std::vector<const XNode*> out;
        for (const auto& k : kids)
            if (n.isEmpty() || k->name == n) out.push_back(k.get());
        return out;
    }
    /// Children of a wrapper element (<fills><FillStyle/>…</fills>).
    std::vector<const XNode*> list(QStringView wrapper, QStringView n = {}) const
    {
        const XNode* w = child(wrapper);
        return w ? w->children(n) : std::vector<const XNode*>{};
    }
    QString attr(const QString& n, const QString& def = {}) const { return attrs.value(n, def); }
    bool has(const QString& n) const { return attrs.contains(n); }
    double num(const QString& n, double def = 0.0) const
    {
        bool ok = false;
        const double v = attrs.value(n).toDouble(&ok);
        return ok ? v : def;
    }
    bool flag(const QString& n, bool def = false) const
    {
        const auto it = attrs.find(n);
        return it == attrs.end() ? def : it.value() == QLatin1String("true");
    }
};

std::unique_ptr<XNode> parseXml(const QByteArray& data, QString* error)
{
    QXmlStreamReader xr(data);
    std::unique_ptr<XNode> root;
    std::vector<XNode*> stack;
    while (!xr.atEnd()) {
        switch (xr.readNext()) {
        case QXmlStreamReader::StartElement: {
            auto n = std::make_unique<XNode>();
            n->name = xr.name().toString();
            for (const QXmlStreamAttribute& a : xr.attributes()) n->attrs.insert(a.name().toString(), a.value().toString());
            XNode* raw = n.get();
            if (stack.empty()) root = std::move(n);
            else stack.back()->kids.push_back(std::move(n));
            stack.push_back(raw);
            break;
        }
        case QXmlStreamReader::EndElement:
            if (!stack.empty()) stack.pop_back();
            break;
        default: break;
        }
    }
    if (xr.hasError()) {
        if (error) *error = xr.errorString();
        return nullptr;
    }
    return root;
}

// --- values ----------------------------------------------------------------------------

Color parseColor(const XNode& n, const QString& colorAttr, const QString& alphaAttr, Color def)
{
    Color c = def;
    const QString s = n.attr(colorAttr);
    if (s.startsWith('#') && s.size() >= 7) {
        bool ok = false;
        const uint v = s.mid(1, 6).toUInt(&ok, 16);
        if (ok) c = Color(uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v));
    }
    if (n.has(alphaAttr)) c.a = uint8_t(std::clamp(std::lround(n.num(alphaAttr, 1.0) * 255.0), 0L, 255L));
    return c;
}

Affine parseMatrix(const XNode* owner)
{
    if (!owner) return {};
    const XNode* m = owner->child(u"matrix");
    const XNode* mm = m ? m->child(u"Matrix") : nullptr;
    if (!mm) return {};
    return {mm->num("a", 1), mm->num("b", 0), mm->num("c", 0), mm->num("d", 1), mm->num("tx", 0), mm->num("ty", 0)};
}

/// Edge coordinate: decimal twips or "#hex.frac" (two's complement when it
/// uses 6 or more hex digits).
bool parseCoord(QStringView tok, double& out)
{
    if (tok.isEmpty()) return false;
    if (tok.front() == '#') {
        QStringView hex = tok.mid(1);
        const qsizetype dot = hex.indexOf('.');
        QStringView ip = dot < 0 ? hex : hex.left(dot);
        QStringView fp = dot < 0 ? QStringView() : hex.mid(dot + 1);
        bool ok = true;
        qint64 iv = ip.isEmpty() ? 0 : ip.toLongLong(&ok, 16);
        if (!ok) return false;
        if (ip.size() >= 6) {
            const int bits = int(ip.size()) * 4;
            if (iv >= (qint64(1) << (bits - 1))) iv -= qint64(1) << bits;
        }
        double frac = 0.0;
        if (!fp.isEmpty()) {
            const qint64 fv = fp.toLongLong(&ok, 16);
            if (ok) frac = double(fv) / double(qint64(1) << (4 * fp.size()));
        }
        out = (iv >= 0 ? double(iv) + frac : double(iv) - frac) / kTwipsPerPixel;
        return true;
    }
    bool ok = false;
    const double v = tok.toDouble(&ok);
    out = v / kTwipsPerPixel;
    return ok;
}

FillStyle parseFill(const XNode* fs, QStringList& warnings)
{
    if (!fs) return FillStyle::solid(Color(0, 0, 0));
    for (const XNode* k : fs->children()) {
        if (k->name == QLatin1String("SolidColor")) return FillStyle::solid(parseColor(*k, "color", "alpha", Color(0, 0, 0)));
        if (k->name == QLatin1String("LinearGradient") || k->name == QLatin1String("RadialGradient")) {
            FillStyle f;
            f.kind = k->name == QLatin1String("LinearGradient") ? FillStyle::Kind::Linear : FillStyle::Kind::Radial;
            f.gradient.stops.clear();
            for (const XNode* e : k->children(u"GradientEntry"))
                f.gradient.stops.push_back({std::clamp(e->num("ratio", 0), 0.0, 1.0), parseColor(*e, "color", "alpha", Color(0, 0, 0))});
            if (f.gradient.stops.empty()) return FillStyle::solid(Color(0, 0, 0));
            f.gradient.matrix = parseMatrix(k) * Affine::scale(kGradientHalfSize);
            f.gradient.focal = std::clamp(k->num("focalPointRatio", 0), -1.0, 1.0);
            const QString spread = k->attr("spreadMethod");
            f.gradient.spread = spread == QLatin1String("reflect") ? SpreadMode::Reflect
                                : spread == QLatin1String("repeat") ? SpreadMode::Repeat
                                                                    : SpreadMode::Pad;
            f.gradient.linearRGB = k->attr("interpolationMethod") == QLatin1String("linearRGB");
            return f;
        }
        if (k->name == QLatin1String("BitmapFill")) {
            if (!warnings.contains(QStringLiteral("Bitmap fills are imported as grey")))
                warnings << QStringLiteral("Bitmap fills are imported as grey");
            return FillStyle::solid(Color(128, 128, 128));
        }
    }
    return FillStyle::solid(Color(0, 0, 0));
}

StrokeStyle parseStroke(const XNode* ss, QStringList& warnings)
{
    StrokeStyle s;
    if (!ss) return s;
    const XNode* k = ss->kids.empty() ? nullptr : ss->kids.front().get();
    if (!k) return s;
    s.width = k->num("weight", 1.0);
    const QString caps = k->attr("caps", "round"), joints = k->attr("joints", "round");
    s.cap = caps == QLatin1String("none") ? CapStyle::None : caps == QLatin1String("square") ? CapStyle::Square : CapStyle::Round;
    s.join = joints == QLatin1String("miter") ? JoinStyle::Miter : joints == QLatin1String("bevel") ? JoinStyle::Bevel : JoinStyle::Round;
    s.miterLimit = k->num("miterLimit", 3.0);
    s.scaleWithTransform = k->attr("scaleMode", "normal") != QLatin1String("none");
    if (k->attr("solidStyle") == QLatin1String("hairline")) s.pattern = StrokePattern::Hairline;
    if (k->name == QLatin1String("DashedStroke")) {
        s.pattern = StrokePattern::Dashed;
        s.dash = k->num("dash1", 6);
        s.gap = k->num("dash2", 4);
    } else if (k->name == QLatin1String("DottedStroke")) {
        s.pattern = StrokePattern::Dotted;
        s.gap = k->num("dotSpace", 3);
    }
    if (const XNode* fill = k->child(u"fill")) s.paint = parseFill(fill, warnings);
    return s;
}

/// Edges of a DOMShape (quadratic edges in twips, converted to cubics).
void parseEdges(const XNode& shape, ShapeGraph& g)
{
    for (const XNode* e : shape.list(u"edges", u"Edge")) {
        const QString data = e->attr("edges");
        if (data.isEmpty()) continue;
        const int f0 = e->has("fillStyle0") ? int(e->num("fillStyle0")) : 0;
        const int f1 = e->has("fillStyle1") ? int(e->num("fillStyle1")) : 0;
        const int st = e->has("strokeStyle") ? int(e->num("strokeStyle")) : 0;
        const int fl = f0 >= 0 && size_t(f0) <= g.fills.size() ? f0 : 0;
        const int fr = f1 >= 0 && size_t(f1) <= g.fills.size() ? f1 : 0;
        const int sk = st >= 0 && size_t(st) <= g.strokes.size() ? st : 0;
        if (fl == 0 && fr == 0 && sk == 0) continue;
        // Tokenise: commands are single characters, numbers are separated by spaces.
        Vec2 cur;
        qsizetype i = 0;
        const qsizetype n = data.size();
        auto skipSpace = [&]() {
            while (i < n && data[i].isSpace()) ++i;
        };
        auto token = [&]() -> QStringView {
            skipSpace();
            const qsizetype s = i;
            while (i < n && !data[i].isSpace() && !QStringView(u"!|/[]S").contains(data[i])) ++i;
            return QStringView(data).mid(s, i - s);
        };
        auto point = [&](Vec2& p) {
            double x, y;
            if (!parseCoord(token(), x) || !parseCoord(token(), y)) return false;
            p = {x, y};
            return true;
        };
        while (true) {
            skipSpace();
            if (i >= n) break;
            const QChar c = data[i++];
            if (c == '!') {
                if (!point(cur)) break;
            } else if (c == '|' || c == '/') {
                Vec2 p;
                if (!point(p)) break;
                if (!(p == cur)) g.edges.push_back({Cubic::line(cur, p), fl, fr, sk});
                cur = p;
            } else if (c == '[' || c == ']') {
                Vec2 q, p;
                if (!point(q) || !point(p)) break;
                g.edges.push_back({Cubic::fromQuad(cur, q, p), fl, fr, sk});
                cur = p;
            } else if (c == 'S') {
                while (i < n && data[i].isDigit()) ++i; // selection flags
            } else if (!c.isSpace()) {
                break;
            }
        }
    }
}

Filter parseFilter(const XNode& n, bool& ok)
{
    ok = true;
    const QString name = n.name;
    FilterType t;
    if (name == QLatin1String("DropShadowFilter")) t = FilterType::DropShadow;
    else if (name == QLatin1String("BlurFilter")) t = FilterType::Blur;
    else if (name == QLatin1String("GlowFilter")) t = FilterType::Glow;
    else if (name == QLatin1String("BevelFilter")) t = FilterType::Bevel;
    else if (name == QLatin1String("GradientGlowFilter")) t = FilterType::GradientGlow;
    else if (name == QLatin1String("GradientBevelFilter")) t = FilterType::GradientBevel;
    else if (name == QLatin1String("AdjustColorFilter")) t = FilterType::AdjustColor;
    else {
        ok = false;
        return {};
    }
    Filter f = Filter::defaults(t);
    f.enabled = n.flag("isEnabled", true);
    f.blurX = n.num("blurX", 5);
    f.blurY = n.num("blurY", 5);
    f.strength = n.num("strength", 1);
    f.quality = std::clamp(int(n.num("quality", 1)), 1, 3);
    f.angle = n.num("angle", 45);
    f.distance = n.num("distance", t == FilterType::Glow ? 0 : 5);
    f.inner = n.flag("inner");
    f.knockout = n.flag("knockout");
    f.hideObject = n.flag("hideObject");
    if (t == FilterType::Bevel) {
        f.color = parseColor(n, "shadowColor", "shadowAlpha", Color(0, 0, 0));
        f.highlight = parseColor(n, "highlightColor", "highlightAlpha", Color(255, 255, 255));
    } else {
        f.color = parseColor(n, "color", "alpha", f.color);
    }
    const QString type = n.attr("type", "inner");
    f.bevel = type == QLatin1String("outer") ? BevelKind::Outer : type == QLatin1String("full") ? BevelKind::Full : BevelKind::Inner;
    if (t == FilterType::GradientGlow || t == FilterType::GradientBevel) {
        f.gradient.stops.clear();
        for (const XNode* e : n.children(u"GradientEntry"))
            f.gradient.stops.push_back({std::clamp(e->num("ratio", 0), 0.0, 1.0), parseColor(*e, "color", "alpha", Color(0, 0, 0))});
        if (t == FilterType::GradientGlow && f.type == FilterType::GradientGlow && !n.has("type")) f.inner = false;
    }
    f.brightness = n.num("brightness", 0);
    f.contrast = n.num("contrast", 0);
    f.saturation = n.num("saturation", 0);
    f.hue = n.num("hue", 0);
    return f;
}

ColorEffect parseColorEffect(const XNode* owner)
{
    const XNode* cw = owner ? owner->child(u"color") : nullptr;
    const XNode* c = cw ? cw->child(u"Color") : nullptr;
    if (!c) return {};
    if (c->has("brightness")) {
        ColorEffect e;
        e.kind = ColorEffect::Kind::Brightness;
        e.brightness = std::clamp(c->num("brightness"), -1.0, 1.0);
        return e;
    }
    if (c->has("tintMultiplier")) {
        ColorEffect e;
        e.kind = ColorEffect::Kind::Tint;
        e.tintAmount = std::clamp(c->num("tintMultiplier"), 0.0, 1.0);
        e.tint = parseColor(*c, "tintColor", "__none", Color(0, 0, 0));
        return e;
    }
    ColorTransform t;
    t.rm = c->num("redMultiplier", 1);
    t.gm = c->num("greenMultiplier", 1);
    t.bm = c->num("blueMultiplier", 1);
    t.am = c->num("alphaMultiplier", 1);
    t.ro = c->num("redOffset", 0);
    t.go = c->num("greenOffset", 0);
    t.bo = c->num("blueOffset", 0);
    t.ao = c->num("alphaOffset", 0);
    return colorEffectFrom(t);
}

BlendMode parseBlend(const QString& s)
{
    static const QHash<QString, BlendMode> map = {
        {"layer", BlendMode::Layer},       {"darken", BlendMode::Darken},     {"multiply", BlendMode::Multiply},
        {"lighten", BlendMode::Lighten},   {"screen", BlendMode::Screen},     {"overlay", BlendMode::Overlay},
        {"hardlight", BlendMode::HardLight}, {"add", BlendMode::Add},          {"subtract", BlendMode::Subtract},
        {"difference", BlendMode::Difference}, {"invert", BlendMode::Invert}, {"alpha", BlendMode::Alpha},
        {"erase", BlendMode::Erase},
    };
    return map.value(s.toLower(), BlendMode::Normal);
}

SymbolType parseSymbolType(const QString& s)
{
    if (s == QLatin1String("graphic")) return SymbolType::Graphic;
    if (s == QLatin1String("button")) return SymbolType::Button;
    return SymbolType::MovieClip;
}

// --- the importer --------------------------------------------------------------------

class XflSource {
public:
    virtual ~XflSource() = default;
    virtual QByteArray read(const QString& path) const = 0;
};

class ZipSource : public XflSource {
public:
    ZipReader zip;
    QByteArray read(const QString& path) const override { return zip.read(path); }
};

class DirSource : public XflSource {
public:
    QDir dir;
    QByteArray read(const QString& path) const override
    {
        QFile f(dir.filePath(path));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }
};

class XflImporter {
public:
    XflImporter(const XflSource& src, Document& doc, ImportReport& report) : m_src(src), m_doc(doc), m_report(report) {}

    bool run(QString* error)
    {
        const QByteArray xml = m_src.read(QStringLiteral("DOMDocument.xml"));
        if (xml.isEmpty()) {
            if (error) *error = QStringLiteral("DOMDocument.xml is missing");
            return false;
        }
        std::unique_ptr<XNode> root = parseXml(xml, error);
        if (!root || root->name != QLatin1String("DOMDocument")) {
            if (error && error->isEmpty()) *error = QStringLiteral("Not an XFL document");
            return false;
        }
        m_doc.width = root->num("width", 550);
        m_doc.height = root->num("height", 400);
        m_doc.fps = root->num("frameRate", 24);
        m_doc.background = parseColor(*root, "backgroundColor", "__none", Color(255, 255, 255));
        m_report.generator = QStringLiteral("Animate / Flash (XFL %1)").arg(root->attr("xflVersion", "?"));

        // Library: register every symbol first, instances refer to them by name.
        std::vector<std::pair<std::unique_ptr<XNode>, size_t>> items;
        for (const XNode* inc : root->list(u"symbols", u"Include")) {
            const QString href = inc->attr("href");
            QString err;
            std::unique_ptr<XNode> item = parseXml(m_src.read(QStringLiteral("LIBRARY/") + href), &err);
            if (!item || item->name != QLatin1String("DOMSymbolItem")) {
                m_report.warnings << QStringLiteral("Library item %1 could not be read").arg(href);
                continue;
            }
            Symbol s;
            s.id = m_doc.newSymbolId();
            const QString full = item->attr("name", QFileInfo(href).completeBaseName());
            const qsizetype slash = full.lastIndexOf('/');
            s.name = (slash >= 0 ? full.mid(slash + 1) : full).toStdString();
            s.folder = slash >= 0 ? full.left(slash).toStdString() : std::string();
            s.type = parseSymbolType(item->attr("symbolType"));
            if (item->attr("scaleGridLeft").size()) {
                // 9-slice guides, in pixels of the symbol's space.
                const double l = item->attr("scaleGridLeft").toDouble(), r = item->attr("scaleGridRight").toDouble();
                const double t = item->attr("scaleGridTop").toDouble(), b = item->attr("scaleGridBottom").toDouble();
                if (r > l && b > t) s.scale9 = Rect(l, t, r, b);
            }
            m_symbols.insert(full, s.id);
            m_doc.symbols.push_back(std::move(s));
            items.emplace_back(std::move(item), m_doc.symbols.size() - 1);
        }
        for (auto& [item, index] : items) {
            const XNode* tlw = item->child(u"timeline");
            const XNode* tl = tlw ? tlw->child(u"DOMTimeline") : nullptr;
            if (tl) m_doc.symbols[index].timeline = timeline(*tl);
            ++m_report.symbols;
        }
        for (const XNode* tl : root->list(u"timelines", u"DOMTimeline")) {
            m_doc.scenes.push_back(timeline(*tl));
            ++m_report.scenes;
        }
        if (m_doc.scenes.empty()) {
            if (error) *error = QStringLiteral("The document has no scenes");
            return false;
        }
        if (!root->list(u"media").empty())
            m_report.warnings << QStringLiteral("Bitmaps, sounds and video are not imported yet");
        return true;
    }

private:
    Timeline timeline(const XNode& t)
    {
        Timeline tl;
        tl.name = t.attr("name", "Scene 1").toStdString();
        const std::vector<const XNode*> layers = t.list(u"layers", u"DOMLayer");
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < layers.size(); ++i) ids.push_back(m_doc.newLayerId());
        for (size_t i = 0; i < layers.size(); ++i) {
            const XNode& xl = *layers[i];
            Layer l;
            l.id = ids[i];
            l.name = xl.attr("name", "Layer").toStdString();
            const Color c = parseColor(xl, "color", "__none", Color(0x4f, 0xff, 0x4f));
            l.color = c;
            l.locked = xl.flag("locked");
            l.visible = xl.flag("visible", true);
            l.outline = xl.flag("outline");
            l.expanded = xl.flag("open", true);
            const QString type = xl.attr("layerType");
            if (type == QLatin1String("guide")) l.type = LayerType::Guide;
            else if (type == QLatin1String("mask")) l.type = LayerType::Mask;
            else if (type == QLatin1String("folder")) l.type = LayerType::Folder;
            if (xl.has("parentLayerIndex")) {
                const int p = int(xl.num("parentLayerIndex", -1));
                if (p >= 0 && size_t(p) < ids.size() && size_t(p) != i) l.parentId = ids[size_t(p)];
            }
            l.keys.clear();
            for (const XNode* f : xl.list(u"frames", u"DOMFrame")) {
                Keyframe k = keyframe(*f);
                // Fill gaps with blank keyframes so spans stay contiguous.
                const int end = l.keys.empty() ? 0 : l.keys.back().end();
                if (k.start > end) {
                    Keyframe blank;
                    blank.start = end;
                    blank.duration = k.start - end;
                    l.keys.push_back(std::move(blank));
                } else if (k.start < end) {
                    continue;
                }
                l.keys.push_back(std::move(k));
                ++m_report.keyframes;
            }
            l.normalize();
            tl.layers.push_back(std::move(l));
            ++m_report.layers;
        }
        return tl;
    }

    Keyframe keyframe(const XNode& f)
    {
        Keyframe k;
        k.start = std::max(0, int(f.num("index", 0)));
        k.duration = std::max(1, int(f.num("duration", 1)));
        k.label = f.attr("name").toStdString();
        const QString lt = f.attr("labelType");
        k.labelType = lt == QLatin1String("comment") ? LabelType::Comment : lt == QLatin1String("anchor") ? LabelType::Anchor : LabelType::Name;
        const QString tween = f.attr("tweenType");
        if (tween == QLatin1String("motion")) {
            k.tween = TweenType::Classic;
            const QString rot = f.attr("motionTweenRotate", "auto");
            k.classic.rotate = rot == QLatin1String("none") ? RotateMode::None
                               : rot == QLatin1String("clockwise") ? RotateMode::Clockwise
                               : rot == QLatin1String("counter-clockwise") ? RotateMode::CounterClockwise
                                                                           : RotateMode::Auto;
            k.classic.rotations = int(f.num("motionTweenRotateTimes", 0));
            k.classic.orientToPath = f.flag("motionTweenOrientToPath");
            k.classic.scale = f.flag("motionTweenScale", true);
            k.classic.snap = f.flag("motionTweenSnap", true);
            k.classic.sync = f.flag("motionTweenSync", true);
            k.classic.ease = ease(f);
        } else if (tween == QLatin1String("shape")) {
            k.tween = TweenType::Shape;
            k.shape.angular = f.attr("shapeTweenBlend") == QLatin1String("angular");
            k.shape.ease = ease(f);
        } else if (!tween.isEmpty()) {
            if (!m_report.warnings.contains(QStringLiteral("Motion tweens (object based) are imported as keyframes")))
                m_report.warnings << QStringLiteral("Motion tweens (object based) are imported as keyframes");
        }
        for (const XNode* e : f.list(u"elements"))
            if (ElementPtr el = element(*e)) k.elements.push_back(el);
        return k;
    }

    static Ease ease(const XNode& f)
    {
        Ease e;
        if (f.has("acceleration") && f.num("acceleration") != 0) {
            e.kind = EaseKind::Classic;
            e.strength = std::clamp(int(f.num("acceleration")), -100, 100);
        }
        for (const XNode* t : f.list(u"tweens")) {
            if (t->name == QLatin1String("Ease")) {
                const EaseKind k = easeFromId(t->attr("method").toStdString());
                if (k != EaseKind::None) {
                    e.kind = k;
                    if (k == EaseKind::Classic) e.strength = std::clamp(int(t->num("intensity", e.strength)), -100, 100);
                }
            } else if (t->name == QLatin1String("CustomEase")) {
                std::vector<Vec2> pts;
                for (const XNode* p : t->children(u"Point")) pts.push_back({p->num("x"), p->num("y")});
                if (pts.size() >= 4 && (pts.size() - 1) % 3 == 0) {
                    e.kind = EaseKind::Custom;
                    e.curve.clear();
                    for (size_t i = 0; i + 3 < pts.size(); i += 3) e.curve.push_back({pts[i], pts[i + 1], pts[i + 2], pts[i + 3]});
                }
            }
        }
        return e;
    }

    ElementPtr element(const XNode& e)
    {
        const QString n = e.name;
        if (n == QLatin1String("DOMShape")) {
            ShapeGraph g;
            for (const XNode* fs : e.list(u"fills", u"FillStyle")) {
                const int idx = int(fs->num("index", double(g.fills.size() + 1)));
                if (idx > int(g.fills.size())) g.fills.resize(size_t(idx), FillStyle::solid(Color(0, 0, 0)));
                if (idx > 0) g.fills[size_t(idx - 1)] = parseFill(fs, m_report.warnings);
            }
            for (const XNode* ss : e.list(u"strokes", u"StrokeStyle")) {
                const int idx = int(ss->num("index", double(g.strokes.size() + 1)));
                if (idx > int(g.strokes.size())) g.strokes.resize(size_t(idx));
                if (idx > 0) g.strokes[size_t(idx - 1)] = parseStroke(ss, m_report.warnings);
            }
            parseEdges(e, g);
            if (g.isEmpty()) return nullptr;
            fixFillSides(g);
            ++m_report.shapes;
            const Affine m = parseMatrix(&e);
            const bool object = e.flag("isDrawingObject");
            if (!object && !(m == Affine())) g = g.transformed(m);
            return makeShapeElement(std::move(g), object, object ? m : Affine());
        }
        if (n == QLatin1String("DOMSymbolInstance")) {
            const QString lib = e.attr("libraryItemName");
            const auto it = m_symbols.find(lib);
            if (it == m_symbols.end()) {
                m_report.warnings << QStringLiteral("Missing library item %1").arg(lib);
                return nullptr;
            }
            auto in = std::make_shared<InstanceElement>();
            in->symbolId = it.value();
            in->behavior = parseSymbolType(e.attr("symbolType"));
            in->matrix = parseMatrix(&e);
            if (const XNode* tp = e.child(u"transformationPoint"))
                if (const XNode* p = tp->child(u"Point")) in->pivot = {p->num("x"), p->num("y")};
            in->name = e.attr("name").toStdString();
            const QString loop = e.attr("loop", "loop");
            in->loop = loop == QLatin1String("play once") ? LoopMode::PlayOnce
                       : loop == QLatin1String("single frame") ? LoopMode::SingleFrame
                       : loop == QLatin1String("loop reverse") ? LoopMode::LoopReverse
                       : loop == QLatin1String("play once reverse") ? LoopMode::PlayOnceReverse
                                                                   : LoopMode::Loop;
            in->firstFrame = std::max(0, int(e.num("firstFrame", 0)));
            in->lastFrame = e.has("lastFrame") ? int(e.num("lastFrame")) : -1;
            in->blend = parseBlend(e.attr("blendMode"));
            in->visible = e.flag("isVisible", true);
            in->color = parseColorEffect(&e);
            for (const XNode* fx : e.list(u"filters")) {
                bool ok = false;
                const Filter f = parseFilter(*fx, ok);
                if (ok) in->filters.push_back(f);
            }
            ++m_report.instances;
            return in;
        }
        if (n == QLatin1String("DOMGroup")) {
            auto g = std::make_shared<GroupElement>();
            for (const XNode* m : e.list(u"members"))
                if (ElementPtr c = element(*m)) g->children.push_back(c);
            if (g->children.empty()) return nullptr;
            return g;
        }
        if (n == QLatin1String("DOMRectangleObject") || n == QLatin1String("DOMOvalObject")) return primitive(e);
        const QString what = n.startsWith(QLatin1String("DOM")) ? n.mid(3) : n;
        const QString msg = QStringLiteral("%1 elements are not imported yet").arg(what);
        if (!m_report.warnings.contains(msg)) m_report.warnings << msg;
        return nullptr;
    }

    /// Rectangle and oval primitives become drawing objects.
    ElementPtr primitive(const XNode& e)
    {
        const double x = e.num("x"), y = e.num("y"), w = e.num("objectWidth"), h = e.num("objectHeight");
        if (w <= 0 || h <= 0) return nullptr;
        Region r = e.name == QLatin1String("DOMOvalObject") ? Region::ellipse({x + w / 2, y + h / 2}, w / 2, h / 2)
                                                             : Region::roundedRect({x, y, x + w, y + h}, e.num("topLeftRadius"));
        FillStyle fill = FillStyle::solid(Color(0, 0, 0));
        bool hasFill = false;
        if (const XNode* fw = e.child(u"fill")) {
            fill = parseFill(fw, m_report.warnings);
            hasFill = true;
        }
        StrokeStyle stroke;
        bool hasStroke = false;
        if (const XNode* sw = e.child(u"stroke")) {
            stroke = parseStroke(sw, m_report.warnings);
            hasStroke = true;
        }
        ShapeGraph g = graphFromShape(r, hasFill ? &fill : nullptr, hasStroke ? &stroke : nullptr);
        if (g.isEmpty()) return nullptr;
        ++m_report.shapes;
        return makeShapeElement(std::move(g), true, parseMatrix(&e));
    }

    /// Flash stores the fill on each side of an edge; decide once per document
    /// which of our sides fillStyle0 maps to, by checking that the faces
    /// labelled by the topology cover the same area as the fill loops.
    void fixFillSides(ShapeGraph& g)
    {
        if (m_sidesDecided >= 3) {
            if (m_swapSides) swapSides(g);
            return;
        }
        double render = 0.0;
        for (const auto& fp : g.renderData().fills) {
            Region r;
            r.contours = fp.contours;
            render += std::abs(normalizeRegion(r).area());
        }
        if (render <= 1e-6) {
            if (m_swapSides) swapSides(g);
            return;
        }
        const double topo = std::abs(g.fillRegion().area());
        ShapeGraph swapped = g;
        swapSides(swapped);
        const double topoSwapped = std::abs(swapped.fillRegion().area());
        const bool swap = std::abs(topoSwapped - render) < std::abs(topo - render);
        if (m_sidesDecided == 0) m_swapSides = swap;
        ++m_sidesDecided;
        if (swap) g = swapped;
    }

    static void swapSides(ShapeGraph& g)
    {
        for (GEdge& e : g.edges) std::swap(e.fillL, e.fillR);
        g.invalidate();
    }

    const XflSource& m_src;
    Document& m_doc;
    ImportReport& m_report;
    QHash<QString, std::string> m_symbols;
    bool m_swapSides = false;
    int m_sidesDecided = 0;
};

} // namespace

bool importXfl(const QString& path, Document& doc, ImportReport& report, QString* error)
{
    std::unique_ptr<XflSource> src;
    const QFileInfo fi(path);
    if (fi.isDir() || fi.fileName().compare(QLatin1String("DOMDocument.xml"), Qt::CaseInsensitive) == 0 ||
        fi.suffix().compare(QLatin1String("xfl"), Qt::CaseInsensitive) == 0) {
        auto d = std::make_unique<DirSource>();
        d->dir = fi.isDir() ? QDir(path) : fi.dir();
        src = std::move(d);
    } else {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error) *error = f.errorString();
            return false;
        }
        auto z = std::make_unique<ZipSource>();
        if (!z->zip.open(f.readAll(), error)) return false;
        src = std::move(z);
    }
    Document out;
    out.scenes.clear();
    out.symbols.clear();
    XflImporter imp(*src, out, report);
    if (!imp.run(error)) return false;
    for (Timeline& tl : out.scenes)
        if (tl.layers.empty()) tl.layers.push_back(out.makeLayer("Layer 1"));
    for (Symbol& s : out.symbols)
        if (s.timeline.layers.empty()) s.timeline.layers.push_back(out.makeLayer("Layer 1"));
    doc = std::move(out);
    return true;
}

bool importXflZip(const QByteArray& zipData, Document& doc, ImportReport* reportOut, QString* error)
{
    ImportReport local;
    ImportReport& report = reportOut ? *reportOut : local;
    report.format = FlaFormat::XflZip;
    auto z = std::make_unique<ZipSource>();
    if (!z->zip.open(zipData, error)) return false;
    Document out;
    out.scenes.clear();
    out.symbols.clear();
    XflImporter imp(*z, out, report);
    if (!imp.run(error)) return false;
    for (Timeline& tl : out.scenes)
        if (tl.layers.empty()) tl.layers.push_back(out.makeLayer("Layer 1"));
    for (Symbol& s : out.symbols)
        if (s.timeline.layers.empty()) s.timeline.layers.push_back(out.makeLayer("Layer 1"));
    doc = std::move(out);
    return true;
}

} // namespace vx::io
