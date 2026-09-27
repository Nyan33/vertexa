// SPDX-License-Identifier: GPL-3.0-or-later
#include "Serialize.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <functional>
#include <map>
#include <set>

namespace vx {

namespace {

// --- primitives ---------------------------------------------------------------

QString colorToString(const Color& c)
{
    return QString::asprintf("#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
}

Color colorFromValue(const QJsonValue& v, Color def = Color(0, 0, 0))
{
    Color c;
    if (v.isString() && Color::parseHex(v.toString().toStdString(), c)) return c;
    return def;
}

QJsonArray matrixToJson(const Affine& m) { return {m.a, m.b, m.c, m.d, m.tx, m.ty}; }

Affine matrixFromJson(const QJsonValue& v)
{
    const QJsonArray a = v.toArray();
    if (a.size() != 6) return {};
    return {a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble(), a[4].toDouble(), a[5].toDouble()};
}

QJsonArray vecToJson(Vec2 p) { return {p.x, p.y}; }
Vec2 vecFromJson(const QJsonValue& v)
{
    const QJsonArray a = v.toArray();
    return a.size() == 2 ? Vec2{a[0].toDouble(), a[1].toDouble()} : Vec2{};
}

template <class E>
QString enumName(E v, std::initializer_list<const char*> names)
{
    int i = 0;
    for (const char* n : names)
        if (i++ == int(v)) return QString::fromLatin1(n);
    return QString::fromLatin1(*names.begin());
}

template <class E>
E enumFrom(const QJsonValue& v, std::initializer_list<const char*> names, E def)
{
    const QString s = v.toString();
    int i = 0;
    for (const char* n : names) {
        if (s == QLatin1String(n)) return E(i);
        ++i;
    }
    return def;
}

#define VX_NAMES(...) std::initializer_list<const char*>{__VA_ARGS__}
const auto kSpread = VX_NAMES("pad", "reflect", "repeat");
const auto kFillKind = VX_NAMES("solid", "linear", "radial");
const auto kCap = VX_NAMES("round", "square", "none");
const auto kJoin = VX_NAMES("round", "miter", "bevel");
const auto kPattern = VX_NAMES("solid", "dashed", "dotted", "hairline");
const auto kSymbolType = VX_NAMES("movieclip", "graphic", "button");
const auto kLoop = VX_NAMES("loop", "playonce", "singleframe", "loopreverse", "playoncereverse");
const auto kTween = VX_NAMES("none", "classic", "shape");
const auto kRotate = VX_NAMES("none", "auto", "cw", "ccw");
const auto kLabel = VX_NAMES("name", "comment", "anchor");
const auto kLayerType = VX_NAMES("normal", "guide", "mask", "folder");
const auto kColorEffect = VX_NAMES("none", "brightness", "tint", "alpha", "advanced");
const auto kTipType = VX_NAMES("auto", "image");
const auto kAutoShape = VX_NAMES("circle", "square");
const auto kTexMode = VX_NAMES("multiply", "subtract", "height");

// --- filters ----------------------------------------------------------------------

const auto kBevelKind = VX_NAMES("inner", "outer", "full");

QJsonObject filterToJson(const Filter& f)
{
    QJsonObject o;
    o["type"] = QString::fromUtf8(filterId(f.type).data(), int(filterId(f.type).size()));
    if (!f.enabled) o["enabled"] = false;
    if (f.type == FilterType::AdjustColor) {
        o["brightness"] = f.brightness;
        o["contrast"] = f.contrast;
        o["saturation"] = f.saturation;
        o["hue"] = f.hue;
        return o;
    }
    o["blurX"] = f.blurX;
    o["blurY"] = f.blurY;
    o["quality"] = f.quality;
    if (f.type == FilterType::Blur) return o;
    o["strength"] = f.strength;
    o["color"] = colorToString(f.color);
    o["inner"] = f.inner;
    o["knockout"] = f.knockout;
    if (f.type != FilterType::Glow && f.type != FilterType::GradientGlow) {
        o["angle"] = f.angle;
        o["distance"] = f.distance;
    }
    if (f.type == FilterType::DropShadow) o["hideObject"] = f.hideObject;
    if (f.type == FilterType::Bevel) o["highlight"] = colorToString(f.highlight);
    if (f.type == FilterType::Bevel || f.type == FilterType::GradientBevel) o["bevel"] = enumName(f.bevel, kBevelKind);
    if (f.type == FilterType::GradientGlow || f.type == FilterType::GradientBevel) {
        QJsonArray stops;
        for (const GradientStop& st : f.gradient.stops) stops.append(QJsonArray{st.pos, colorToString(st.color)});
        o["stops"] = stops;
    }
    return o;
}

Filter filterFromJson(const QJsonObject& o)
{
    Filter f = Filter::defaults(filterFromId(o["type"].toString().toStdString()));
    f.enabled = o["enabled"].toBool(true);
    f.blurX = o["blurX"].toDouble(f.blurX);
    f.blurY = o["blurY"].toDouble(f.blurY);
    f.quality = std::clamp(o["quality"].toInt(f.quality), 1, 3);
    f.strength = o["strength"].toDouble(f.strength);
    f.color = colorFromValue(o["color"], f.color);
    f.highlight = colorFromValue(o["highlight"], f.highlight);
    f.inner = o["inner"].toBool(false);
    f.knockout = o["knockout"].toBool(false);
    f.hideObject = o["hideObject"].toBool(false);
    f.angle = o["angle"].toDouble(f.angle);
    f.distance = o["distance"].toDouble(f.distance);
    f.bevel = enumFrom(o["bevel"], kBevelKind, BevelKind::Inner);
    f.brightness = o["brightness"].toDouble(0);
    f.contrast = o["contrast"].toDouble(0);
    f.saturation = o["saturation"].toDouble(0);
    f.hue = o["hue"].toDouble(0);
    if (o.contains("stops")) {
        f.gradient.stops.clear();
        for (const QJsonValue& v : o["stops"].toArray()) {
            const QJsonArray a = v.toArray();
            f.gradient.stops.push_back({a.at(0).toDouble(), colorFromValue(a.at(1))});
        }
    }
    return f;
}

// --- styles -----------------------------------------------------------------------

QJsonObject fillToJson(const FillStyle& f)
{
    QJsonObject o;
    o["kind"] = enumName(f.kind, kFillKind);
    if (f.kind == FillStyle::Kind::Solid) {
        o["color"] = colorToString(f.color);
        return o;
    }
    QJsonObject g;
    QJsonArray stops;
    for (const GradientStop& s : f.gradient.stops) stops.append(QJsonArray{s.pos, colorToString(s.color)});
    g["stops"] = stops;
    g["spread"] = enumName(f.gradient.spread, kSpread);
    g["focal"] = f.gradient.focal;
    g["linearRGB"] = f.gradient.linearRGB;
    g["matrix"] = matrixToJson(f.gradient.matrix);
    o["gradient"] = g;
    return o;
}

FillStyle fillFromJson(const QJsonObject& o)
{
    FillStyle f;
    f.kind = enumFrom(o["kind"], kFillKind, FillStyle::Kind::Solid);
    f.color = colorFromValue(o["color"]);
    if (f.kind != FillStyle::Kind::Solid) {
        const QJsonObject g = o["gradient"].toObject();
        f.gradient.stops.clear();
        for (const QJsonValue& v : g["stops"].toArray()) {
            const QJsonArray s = v.toArray();
            if (s.size() == 2) f.gradient.stops.push_back({s[0].toDouble(), colorFromValue(s[1])});
        }
        if (f.gradient.stops.empty()) f.gradient.stops = Gradient{}.stops;
        f.gradient.spread = enumFrom(g["spread"], kSpread, SpreadMode::Pad);
        f.gradient.focal = g["focal"].toDouble();
        f.gradient.linearRGB = g["linearRGB"].toBool();
        f.gradient.matrix = matrixFromJson(g["matrix"]);
    }
    return f;
}

QJsonObject strokeToJson(const StrokeStyle& s)
{
    QJsonObject o;
    o["paint"] = fillToJson(s.paint);
    o["width"] = s.width;
    o["cap"] = enumName(s.cap, kCap);
    o["join"] = enumName(s.join, kJoin);
    o["miter"] = s.miterLimit;
    o["pattern"] = enumName(s.pattern, kPattern);
    o["dash"] = s.dash;
    o["gap"] = s.gap;
    o["scale"] = s.scaleWithTransform;
    return o;
}

StrokeStyle strokeFromJson(const QJsonObject& o)
{
    StrokeStyle s;
    s.paint = fillFromJson(o["paint"].toObject());
    s.width = o["width"].toDouble(1.0);
    s.cap = enumFrom(o["cap"], kCap, CapStyle::Round);
    s.join = enumFrom(o["join"], kJoin, JoinStyle::Round);
    s.miterLimit = o["miter"].toDouble(3.0);
    s.pattern = enumFrom(o["pattern"], kPattern, StrokePattern::Solid);
    s.dash = o["dash"].toDouble(6.0);
    s.gap = o["gap"].toDouble(4.0);
    s.scaleWithTransform = o["scale"].toBool(true);
    return s;
}

QJsonObject easeToJson(const Ease& e)
{
    QJsonObject o;
    o["kind"] = QString::fromUtf8(easeId(e.kind).data(), int(easeId(e.kind).size()));
    if (e.kind == EaseKind::Classic) o["strength"] = e.strength;
    if (e.kind == EaseKind::Custom) {
        QJsonArray c;
        for (const Cubic& cu : e.curve) c.append(QJsonArray{cu.p0.x, cu.p0.y, cu.p1.x, cu.p1.y, cu.p2.x, cu.p2.y, cu.p3.x, cu.p3.y});
        o["curve"] = c;
    }
    return o;
}

Ease easeFromJson(const QJsonObject& o)
{
    Ease e;
    e.kind = easeFromId(o["kind"].toString().toStdString());
    e.strength = o["strength"].toInt();
    for (const QJsonValue& v : o["curve"].toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() == 8)
            e.curve.push_back({{a[0].toDouble(), a[1].toDouble()}, {a[2].toDouble(), a[3].toDouble()},
                               {a[4].toDouble(), a[5].toDouble()}, {a[6].toDouble(), a[7].toDouble()}});
    }
    return e;
}

QJsonObject colorEffectToJson(const ColorEffect& c)
{
    QJsonObject o;
    o["kind"] = enumName(c.kind, kColorEffect);
    switch (c.kind) {
    case ColorEffect::Kind::None: break;
    case ColorEffect::Kind::Brightness: o["brightness"] = c.brightness; break;
    case ColorEffect::Kind::Tint:
        o["tint"] = colorToString(c.tint);
        o["amount"] = c.tintAmount;
        break;
    case ColorEffect::Kind::Alpha: o["alpha"] = c.alpha; break;
    case ColorEffect::Kind::Advanced: {
        const ColorTransform& t = c.advanced;
        o["transform"] = QJsonArray{t.rm, t.gm, t.bm, t.am, t.ro, t.go, t.bo, t.ao};
        break;
    }
    }
    return o;
}

ColorEffect colorEffectFromJson(const QJsonObject& o)
{
    ColorEffect c;
    c.kind = enumFrom(o["kind"], kColorEffect, ColorEffect::Kind::None);
    c.brightness = o["brightness"].toDouble();
    c.tint = colorFromValue(o["tint"], Color(255, 255, 255));
    c.tintAmount = o["amount"].toDouble();
    c.alpha = o["alpha"].toDouble(1.0);
    const QJsonArray t = o["transform"].toArray();
    if (t.size() == 8)
        c.advanced = {t[0].toDouble(), t[1].toDouble(), t[2].toDouble(), t[3].toDouble(),
                      t[4].toDouble(), t[5].toDouble(), t[6].toDouble(), t[7].toDouble()};
    return c;
}

QJsonObject curveToJson(const ResponseCurve& c)
{
    QJsonArray pts;
    for (const Vec2& p : c.points) pts.append(QJsonArray{p.x, p.y});
    return QJsonObject{{"points", pts}};
}

ResponseCurve curveFromJson(const QJsonValue& v)
{
    ResponseCurve c;
    const QJsonArray pts = v.toObject()["points"].toArray();
    if (pts.size() >= 2) {
        c.points.clear();
        for (const QJsonValue& p : pts) c.points.push_back(vecFromJson(p));
    }
    return c;
}

// --- elements -----------------------------------------------------------------------

QJsonObject elementToJson(const Element& e);
ElementPtr elementFromJson(const QJsonObject& o);

/// Symbol types of the document being read: instances saved before
/// per-instance behaviour existed take their symbol's type.
thread_local const std::map<std::string, SymbolType>* t_symbolTypes = nullptr;

struct SymbolTypeScope {
    std::map<std::string, SymbolType> types;
    SymbolTypeScope(const QJsonArray& symbols, const Document* existing = nullptr)
    {
        if (existing)
            for (const Symbol& s : existing->symbols) types[s.id] = s.type;
        for (const QJsonValue& v : symbols) {
            const QJsonObject o = v.toObject();
            types[o["id"].toString().toStdString()] = enumFrom(o["type"], kSymbolType, SymbolType::MovieClip);
        }
        t_symbolTypes = &types;
    }
    ~SymbolTypeScope() { t_symbolTypes = nullptr; }
};

QJsonObject elementToJson(const Element& e)
{
    QJsonObject o;
    if (!e.matrix.isIdentity()) o["matrix"] = matrixToJson(e.matrix);
    o["pivot"] = vecToJson(e.pivot);
    if (!e.name.empty()) o["name"] = QString::fromStdString(e.name);
    switch (e.type()) {
    case ElementType::Shape: {
        const auto& s = static_cast<const ShapeElement&>(e);
        o["type"] = "shape";
        o["object"] = s.isObject;
        o["graph"] = shapeGraphToJson(*s.graph);
        break;
    }
    case ElementType::Instance: {
        const auto& in = static_cast<const InstanceElement&>(e);
        o["type"] = "instance";
        o["symbol"] = QString::fromStdString(in.symbolId);
        o["behavior"] = enumName(in.behavior, kSymbolType);
        if (in.color.kind != ColorEffect::Kind::None) o["color"] = colorEffectToJson(in.color);
        if (in.blend != BlendMode::Normal) o["blend"] = QString::fromUtf8(blendModeId(in.blend).data());
        o["loop"] = enumName(in.loop, kLoop);
        o["firstFrame"] = in.firstFrame;
        if (in.lastFrame >= 0) o["lastFrame"] = in.lastFrame;
        if (!in.visible) o["visible"] = false;
        if (!in.filters.empty()) {
            QJsonArray filters;
            for (const Filter& f : in.filters) filters.append(filterToJson(f));
            o["filters"] = filters;
        }
        break;
    }
    case ElementType::Group: {
        const auto& g = static_cast<const GroupElement&>(e);
        o["type"] = "group";
        QJsonArray children;
        for (const ElementPtr& c : g.children) children.append(elementToJson(*c));
        o["children"] = children;
        break;
    }
    case ElementType::Paint: {
        const auto& p = static_cast<const PaintElement&>(e);
        o["type"] = "paint";
        std::map<const BrushPreset*, int> brushIndex;
        QJsonArray brushes, strokes;
        for (const PaintStroke& s : p.strokes) {
            int bi = -1;
            if (s.brush) {
                auto it = brushIndex.find(s.brush.get());
                if (it == brushIndex.end()) {
                    bi = int(brushes.size());
                    brushIndex[s.brush.get()] = bi;
                    brushes.append(brushPresetToJson(*s.brush));
                } else {
                    bi = it->second;
                }
            }
            QJsonObject so;
            so["brush"] = bi;
            so["color"] = colorToString(s.color);
            so["seed"] = double(s.seed);
            if (s.erase) so["erase"] = true;
            QJsonArray samples;
            for (const PaintSample& ps : s.samples) {
                samples.append(ps.pos.x);
                samples.append(ps.pos.y);
                samples.append(double(ps.pressure));
                samples.append(double(ps.tiltX));
                samples.append(double(ps.tiltY));
                samples.append(double(ps.rotation));
            }
            so["samples"] = samples;
            strokes.append(so);
        }
        o["brushes"] = brushes;
        o["strokes"] = strokes;
        break;
    }
    case ElementType::Morph: o["type"] = "morph"; break;
    }
    return o;
}

ElementPtr elementFromJson(const QJsonObject& o)
{
    const QString type = o["type"].toString();
    std::shared_ptr<Element> e;
    if (type == "shape") {
        auto s = std::make_shared<ShapeElement>();
        s->isObject = o["object"].toBool();
        s->graph = std::make_shared<ShapeGraph>(shapeGraphFromJson(o["graph"].toObject()));
        e = s;
    } else if (type == "instance") {
        auto in = std::make_shared<InstanceElement>();
        in->symbolId = o["symbol"].toString().toStdString();
        if (o.contains("behavior")) {
            in->behavior = enumFrom(o["behavior"], kSymbolType, SymbolType::MovieClip);
        } else if (t_symbolTypes) {
            const auto it = t_symbolTypes->find(in->symbolId);
            if (it != t_symbolTypes->end()) in->behavior = it->second;
        }
        in->color = colorEffectFromJson(o["color"].toObject());
        in->blend = blendModeFromId(o["blend"].toString("normal").toStdString());
        in->loop = enumFrom(o["loop"], kLoop, LoopMode::Loop);
        in->firstFrame = o["firstFrame"].toInt();
        in->lastFrame = o["lastFrame"].toInt(-1);
        in->visible = o["visible"].toBool(true);
        for (const QJsonValue& v : o["filters"].toArray()) in->filters.push_back(filterFromJson(v.toObject()));
        e = in;
    } else if (type == "group") {
        auto g = std::make_shared<GroupElement>();
        for (const QJsonValue& v : o["children"].toArray())
            if (ElementPtr c = elementFromJson(v.toObject())) g->children.push_back(c);
        e = g;
    } else if (type == "paint") {
        auto p = std::make_shared<PaintElement>();
        std::vector<BrushPresetPtr> brushes;
        for (const QJsonValue& v : o["brushes"].toArray())
            brushes.push_back(std::make_shared<BrushPreset>(brushPresetFromJson(v.toObject())));
        for (const QJsonValue& v : o["strokes"].toArray()) {
            const QJsonObject so = v.toObject();
            PaintStroke s;
            const int bi = so["brush"].toInt(-1);
            if (bi >= 0 && bi < int(brushes.size())) s.brush = brushes[bi];
            else s.brush = std::make_shared<BrushPreset>(builtinBrushPresets().front());
            s.color = colorFromValue(so["color"]);
            s.seed = uint32_t(so["seed"].toDouble(1));
            s.erase = so["erase"].toBool();
            const QJsonArray a = so["samples"].toArray();
            for (int i = 0; i + 5 < a.size(); i += 6) {
                PaintSample ps;
                ps.pos = {a[i].toDouble(), a[i + 1].toDouble()};
                ps.pressure = float(a[i + 2].toDouble());
                ps.tiltX = float(a[i + 3].toDouble());
                ps.tiltY = float(a[i + 4].toDouble());
                ps.rotation = float(a[i + 5].toDouble());
                s.samples.push_back(ps);
            }
            p->strokes.push_back(std::move(s));
        }
        e = p;
    } else {
        return nullptr;
    }
    if (o.contains("matrix")) e->matrix = matrixFromJson(o["matrix"]);
    e->pivot = vecFromJson(o["pivot"]);
    e->name = o["name"].toString().toStdString();
    return e;
}

// --- timelines ------------------------------------------------------------------------

QJsonObject keyToJson(const Keyframe& k)
{
    QJsonObject o;
    o["start"] = k.start;
    o["duration"] = k.duration;
    if (k.tween != TweenType::None) o["tween"] = enumName(k.tween, kTween);
    if (k.tween == TweenType::Classic) {
        QJsonObject c;
        c["ease"] = easeToJson(k.classic.ease);
        c["rotate"] = enumName(k.classic.rotate, kRotate);
        c["rotations"] = k.classic.rotations;
        c["orientToPath"] = k.classic.orientToPath;
        c["sync"] = k.classic.sync;
        c["snap"] = k.classic.snap;
        c["scale"] = k.classic.scale;
        o["classic"] = c;
    }
    if (k.tween == TweenType::Shape) {
        QJsonObject s;
        s["ease"] = easeToJson(k.shape.ease);
        s["angular"] = k.shape.angular;
        o["shape"] = s;
    }
    if (!k.hints.empty()) {
        QJsonArray h;
        for (const ShapeHint& sh : k.hints) h.append(QJsonArray{sh.start.x, sh.start.y, sh.end.x, sh.end.y});
        o["hints"] = h;
    }
    if (!k.label.empty()) {
        o["label"] = QString::fromStdString(k.label);
        o["labelType"] = enumName(k.labelType, kLabel);
    }
    QJsonArray els;
    for (const ElementPtr& e : k.elements)
        if (e->type() != ElementType::Morph) els.append(elementToJson(*e));
    o["elements"] = els;
    return o;
}

Keyframe keyFromJson(const QJsonObject& o)
{
    Keyframe k;
    k.start = o["start"].toInt();
    k.duration = std::max(1, o["duration"].toInt(1));
    k.tween = enumFrom(o["tween"], kTween, TweenType::None);
    const QJsonObject c = o["classic"].toObject();
    k.classic.ease = easeFromJson(c["ease"].toObject());
    k.classic.rotate = enumFrom(c["rotate"], kRotate, RotateMode::Auto);
    k.classic.rotations = c["rotations"].toInt();
    k.classic.orientToPath = c["orientToPath"].toBool();
    k.classic.sync = c["sync"].toBool(true);
    k.classic.snap = c["snap"].toBool(true);
    k.classic.scale = c["scale"].toBool(true);
    const QJsonObject s = o["shape"].toObject();
    k.shape.ease = easeFromJson(s["ease"].toObject());
    k.shape.angular = s["angular"].toBool();
    for (const QJsonValue& v : o["hints"].toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() == 4) k.hints.push_back({{a[0].toDouble(), a[1].toDouble()}, {a[2].toDouble(), a[3].toDouble()}});
    }
    k.label = o["label"].toString().toStdString();
    k.labelType = enumFrom(o["labelType"], kLabel, LabelType::Name);
    for (const QJsonValue& v : o["elements"].toArray())
        if (ElementPtr e = elementFromJson(v.toObject())) k.elements.push_back(e);
    return k;
}

QJsonObject layerToJson(const Layer& l)
{
    QJsonObject o;
    o["id"] = double(l.id);
    o["name"] = QString::fromStdString(l.name);
    o["type"] = enumName(l.type, kLayerType);
    o["visible"] = l.visible;
    o["locked"] = l.locked;
    o["outline"] = l.outline;
    o["color"] = colorToString(l.color);
    if (l.parentId) o["parent"] = double(l.parentId);
    o["expanded"] = l.expanded;
    if (l.blend != BlendMode::Normal) o["blend"] = QString::fromUtf8(blendModeId(l.blend).data());
    if (l.opacity != 1.0) o["opacity"] = l.opacity;
    QJsonArray keys;
    for (const Keyframe& k : l.keys) keys.append(keyToJson(k));
    o["keys"] = keys;
    return o;
}

Layer layerFromJson(const QJsonObject& o)
{
    Layer l;
    l.id = uint32_t(o["id"].toDouble());
    l.name = o["name"].toString("Layer").toStdString();
    l.type = enumFrom(o["type"], kLayerType, LayerType::Normal);
    l.visible = o["visible"].toBool(true);
    l.locked = o["locked"].toBool(false);
    l.outline = o["outline"].toBool(false);
    l.color = colorFromValue(o["color"], Color(0x4f, 0x8c, 0xff));
    l.parentId = uint32_t(o["parent"].toDouble(0));
    l.expanded = o["expanded"].toBool(true);
    l.blend = blendModeFromId(o["blend"].toString("normal").toStdString());
    l.opacity = o["opacity"].toDouble(1.0);
    for (const QJsonValue& v : o["keys"].toArray()) l.keys.push_back(keyFromJson(v.toObject()));
    l.normalize();
    return l;
}

QJsonObject timelineToJson(const Timeline& t)
{
    QJsonObject o;
    o["name"] = QString::fromStdString(t.name);
    QJsonArray layers;
    for (const Layer& l : t.layers) layers.append(layerToJson(l));
    o["layers"] = layers;
    return o;
}

Timeline timelineFromJson(const QJsonObject& o)
{
    Timeline t;
    t.name = o["name"].toString("Scene 1").toStdString();
    for (const QJsonValue& v : o["layers"].toArray()) t.layers.push_back(layerFromJson(v.toObject()));
    return t;
}

QJsonObject symbolToJson(const Symbol& s)
{
    QJsonObject o;
    o["id"] = QString::fromStdString(s.id);
    o["name"] = QString::fromStdString(s.name);
    if (!s.folder.empty()) o["folder"] = QString::fromStdString(s.folder);
    o["type"] = enumName(s.type, kSymbolType);
    o["timeline"] = timelineToJson(s.timeline);
    return o;
}

Symbol symbolFromJson(const QJsonObject& o)
{
    Symbol s;
    s.id = o["id"].toString().toStdString();
    s.name = o["name"].toString().toStdString();
    s.folder = o["folder"].toString().toStdString();
    s.type = enumFrom(o["type"], kSymbolType, SymbolType::MovieClip);
    s.timeline = timelineFromJson(o["timeline"].toObject());
    return s;
}

QJsonObject imageToJson(const GrayImage& img)
{
    QJsonObject o;
    o["width"] = img.width;
    o["height"] = img.height;
    const QByteArray raw(reinterpret_cast<const char*>(img.pixels.data()), qsizetype(img.pixels.size()));
    o["data"] = QString::fromLatin1(qCompress(raw, 9).toBase64());
    return o;
}

GrayImagePtr imageFromJson(const QJsonObject& o)
{
    auto img = std::make_shared<GrayImage>();
    img->width = o["width"].toInt();
    img->height = o["height"].toInt();
    const QByteArray raw = qUncompress(QByteArray::fromBase64(o["data"].toString().toLatin1()));
    if (raw.size() != qsizetype(img->width) * img->height) return nullptr;
    img->pixels.assign(raw.begin(), raw.end());
    return img;
}

void collectSymbols(const Document& doc, const std::vector<ElementPtr>& els, std::set<std::string>& out)
{
    for (const ElementPtr& e : els) {
        if (const InstanceElement* in = asInstance(e)) {
            if (out.insert(in->symbolId).second)
                if (const Symbol* s = doc.symbol(in->symbolId))
                    for (const Layer& l : s->timeline.layers)
                        for (const Keyframe& k : l.keys) collectSymbols(doc, k.elements, out);
        } else if (const GroupElement* g = asGroup(e)) {
            collectSymbols(doc, g->children, out);
        }
    }
}

} // namespace

QJsonObject shapeGraphToJson(const ShapeGraph& g)
{
    QJsonObject o;
    QJsonArray fills, strokes, edges;
    for (const FillStyle& f : g.fills) fills.append(fillToJson(f));
    for (const StrokeStyle& s : g.strokes) strokes.append(strokeToJson(s));
    for (const GEdge& e : g.edges)
        edges.append(QJsonArray{e.c.p0.x, e.c.p0.y, e.c.p1.x, e.c.p1.y, e.c.p2.x, e.c.p2.y, e.c.p3.x, e.c.p3.y,
                                e.fillL, e.fillR, e.stroke});
    o["fills"] = fills;
    o["strokes"] = strokes;
    o["edges"] = edges;
    return o;
}

ShapeGraph shapeGraphFromJson(const QJsonObject& o)
{
    ShapeGraph g;
    for (const QJsonValue& v : o["fills"].toArray()) g.fills.push_back(fillFromJson(v.toObject()));
    for (const QJsonValue& v : o["strokes"].toArray()) g.strokes.push_back(strokeFromJson(v.toObject()));
    const int nf = int(g.fills.size()), ns = int(g.strokes.size());
    for (const QJsonValue& v : o["edges"].toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() != 11) continue;
        GEdge e;
        e.c = {{a[0].toDouble(), a[1].toDouble()}, {a[2].toDouble(), a[3].toDouble()},
               {a[4].toDouble(), a[5].toDouble()}, {a[6].toDouble(), a[7].toDouble()}};
        e.fillL = a[8].toInt();
        e.fillR = a[9].toInt();
        e.stroke = a[10].toInt();
        if (e.fillL < 0 || e.fillL > nf) e.fillL = 0;
        if (e.fillR < 0 || e.fillR > nf) e.fillR = 0;
        if (e.stroke < 0 || e.stroke > ns) e.stroke = 0;
        g.edges.push_back(e);
    }
    return g;
}

QJsonObject brushPresetToJson(const BrushPreset& p)
{
    QJsonObject o;
    o["id"] = QString::fromStdString(p.id);
    o["name"] = QString::fromStdString(p.name);
    o["category"] = QString::fromStdString(p.category);
    o["tipType"] = enumName(p.tipType, kTipType);
    o["autoShape"] = enumName(p.autoShape, kAutoShape);
    o["hardness"] = p.hardness;
    o["roundness"] = p.roundness;
    o["angle"] = p.angle;
    o["tipImage"] = QString::fromStdString(p.tipImage);
    o["tipDensity"] = p.tipDensity;
    o["size"] = p.size;
    o["spacing"] = p.spacing;
    o["opacity"] = p.opacity;
    o["flow"] = p.flow;
    o["buildUp"] = p.buildUp;
    o["pressureSize"] = p.pressureSize;
    o["sizeCurve"] = curveToJson(p.sizeCurve);
    o["minSize"] = p.minSize;
    o["pressureOpacity"] = p.pressureOpacity;
    o["opacityCurve"] = curveToJson(p.opacityCurve);
    o["pressureFlow"] = p.pressureFlow;
    o["flowCurve"] = curveToJson(p.flowCurve);
    o["tiltAngle"] = p.tiltAngle;
    o["followDirection"] = p.followDirection;
    o["rotationJitter"] = p.rotationJitter;
    o["sizeJitter"] = p.sizeJitter;
    o["opacityJitter"] = p.opacityJitter;
    o["scatter"] = p.scatter;
    o["count"] = p.count;
    o["textureEnabled"] = p.textureEnabled;
    o["texture"] = QString::fromStdString(p.texture);
    o["textureScale"] = p.textureScale;
    o["textureStrength"] = p.textureStrength;
    o["textureBrightness"] = p.textureBrightness;
    o["textureContrast"] = p.textureContrast;
    o["textureInvert"] = p.textureInvert;
    o["textureMode"] = enumName(p.textureMode, kTexMode);
    o["blend"] = QString::fromUtf8(blendModeId(p.blend).data());
    o["smoothing"] = p.smoothing;
    return o;
}

BrushPreset brushPresetFromJson(const QJsonObject& o)
{
    BrushPreset p;
    p.id = o["id"].toString().toStdString();
    p.name = o["name"].toString("Brush").toStdString();
    p.category = o["category"].toString().toStdString();
    p.tipType = enumFrom(o["tipType"], kTipType, TipType::Auto);
    p.autoShape = enumFrom(o["autoShape"], kAutoShape, AutoTipShape::Circle);
    p.hardness = o["hardness"].toDouble(p.hardness);
    p.roundness = o["roundness"].toDouble(p.roundness);
    p.angle = o["angle"].toDouble(p.angle);
    p.tipImage = o["tipImage"].toString().toStdString();
    p.tipDensity = o["tipDensity"].toDouble(1.0);
    p.size = o["size"].toDouble(p.size);
    p.spacing = o["spacing"].toDouble(p.spacing);
    p.opacity = o["opacity"].toDouble(p.opacity);
    p.flow = o["flow"].toDouble(p.flow);
    p.buildUp = o["buildUp"].toBool(p.buildUp);
    p.pressureSize = o["pressureSize"].toBool(p.pressureSize);
    p.sizeCurve = curveFromJson(o["sizeCurve"]);
    p.minSize = o["minSize"].toDouble(p.minSize);
    p.pressureOpacity = o["pressureOpacity"].toBool(p.pressureOpacity);
    p.opacityCurve = curveFromJson(o["opacityCurve"]);
    p.pressureFlow = o["pressureFlow"].toBool(p.pressureFlow);
    p.flowCurve = curveFromJson(o["flowCurve"]);
    p.tiltAngle = o["tiltAngle"].toBool(p.tiltAngle);
    p.followDirection = o["followDirection"].toBool(p.followDirection);
    p.rotationJitter = o["rotationJitter"].toDouble(p.rotationJitter);
    p.sizeJitter = o["sizeJitter"].toDouble(p.sizeJitter);
    p.opacityJitter = o["opacityJitter"].toDouble(p.opacityJitter);
    p.scatter = o["scatter"].toDouble(p.scatter);
    p.count = std::max(1, o["count"].toInt(p.count));
    p.textureEnabled = o["textureEnabled"].toBool(p.textureEnabled);
    p.texture = o["texture"].toString(QString::fromStdString(p.texture)).toStdString();
    p.textureScale = o["textureScale"].toDouble(p.textureScale);
    p.textureStrength = o["textureStrength"].toDouble(p.textureStrength);
    p.textureBrightness = o["textureBrightness"].toDouble(p.textureBrightness);
    p.textureContrast = o["textureContrast"].toDouble(p.textureContrast);
    p.textureInvert = o["textureInvert"].toBool(p.textureInvert);
    p.textureMode = enumFrom(o["textureMode"], kTexMode, TextureMode::Multiply);
    p.blend = blendModeFromId(o["blend"].toString("normal").toStdString());
    p.smoothing = o["smoothing"].toDouble(p.smoothing);
    return p;
}

QByteArray serializeDocument(const Document& doc, bool pretty)
{
    QJsonObject root;
    root["format"] = "vertexa";
    root["version"] = kFormatVersion;
    QJsonObject stage;
    stage["width"] = doc.width;
    stage["height"] = doc.height;
    stage["fps"] = doc.fps;
    stage["background"] = colorToString(doc.background);
    root["stage"] = stage;
    root["nextLayerId"] = double(doc.nextLayerId);
    root["nextSymbolSerial"] = double(doc.nextSymbolSerial);
    QJsonArray scenes, symbols;
    for (const Timeline& t : doc.scenes) scenes.append(timelineToJson(t));
    for (const Symbol& s : doc.symbols) symbols.append(symbolToJson(s));
    root["scenes"] = scenes;
    root["symbols"] = symbols;
    QJsonObject images;
    for (const auto& [id, img] : doc.images)
        if (img) images[QString::fromStdString(id)] = imageToJson(*img);
    root["images"] = images;
    return QJsonDocument(root).toJson(pretty ? QJsonDocument::Indented : QJsonDocument::Compact);
}

bool deserializeDocument(const QByteArray& data, Document& doc, QString* error)
{
    QJsonParseError pe;
    const QJsonDocument jd = QJsonDocument::fromJson(data, &pe);
    if (jd.isNull() || !jd.isObject()) {
        if (error) *error = pe.errorString();
        return false;
    }
    const QJsonObject root = jd.object();
    if (root["format"].toString() != "vertexa") {
        if (error) *error = QStringLiteral("Not a Vertexa document");
        return false;
    }
    if (root["version"].toInt() > kFormatVersion) {
        if (error) *error = QStringLiteral("The document was saved by a newer version of Vertexa");
        return false;
    }
    Document d;
    const QJsonObject stage = root["stage"].toObject();
    d.width = stage["width"].toDouble(1280);
    d.height = stage["height"].toDouble(720);
    d.fps = stage["fps"].toDouble(24);
    d.background = colorFromValue(stage["background"], Color(255, 255, 255));
    const SymbolTypeScope typeScope(root["symbols"].toArray());
    for (const QJsonValue& v : root["scenes"].toArray()) d.scenes.push_back(timelineFromJson(v.toObject()));
    for (const QJsonValue& v : root["symbols"].toArray()) d.symbols.push_back(symbolFromJson(v.toObject()));
    const QJsonObject images = root["images"].toObject();
    for (auto it = images.begin(); it != images.end(); ++it)
        if (GrayImagePtr img = imageFromJson(it.value().toObject())) d.images[it.key().toStdString()] = img;
    // Keep id counters ahead of anything in the file.
    uint32_t maxLayer = 0;
    auto scan = [&](const Timeline& t) {
        for (const Layer& l : t.layers) maxLayer = std::max(maxLayer, l.id);
    };
    for (const Timeline& t : d.scenes) scan(t);
    for (const Symbol& s : d.symbols) scan(s.timeline);
    d.nextLayerId = std::max<uint32_t>(uint32_t(root["nextLayerId"].toDouble(1)), maxLayer + 1);
    d.nextSymbolSerial = std::max<uint64_t>(uint64_t(root["nextSymbolSerial"].toDouble(1)), d.symbols.size() + 1);
    if (d.scenes.empty()) {
        Timeline t;
        t.layers.push_back(d.makeLayer("Layer 1"));
        d.scenes.push_back(std::move(t));
    }
    for (Timeline& t : d.scenes)
        if (t.layers.empty()) t.layers.push_back(d.makeLayer("Layer 1"));
    doc = std::move(d);
    return true;
}

bool saveDocument(const Document& doc, const QString& path, QString* error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(serializeDocument(doc));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

bool loadDocument(const QString& path, Document& doc, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    return deserializeDocument(f.readAll(), doc, error);
}

QByteArray serializeClipboard(const Document& doc, const std::vector<ElementPtr>& elements)
{
    QJsonObject root;
    root["format"] = "vertexa-clipboard";
    QJsonArray els, syms;
    for (const ElementPtr& e : elements) els.append(elementToJson(*e));
    std::set<std::string> used;
    collectSymbols(doc, elements, used);
    for (const std::string& id : used)
        if (const Symbol* s = doc.symbol(id)) syms.append(symbolToJson(*s));
    root["elements"] = els;
    root["symbols"] = syms;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::vector<ElementPtr> deserializeClipboard(const QByteArray& data, Document& doc)
{
    std::vector<ElementPtr> out;
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    if (root["format"].toString() != "vertexa-clipboard") return out;
    const SymbolTypeScope typeScope(root["symbols"].toArray(), &doc);
    for (const QJsonValue& v : root["symbols"].toArray()) {
        Symbol s = symbolFromJson(v.toObject());
        if (!doc.symbol(s.id)) {
            // Layer ids must stay unique inside the document.
            std::map<uint32_t, uint32_t> remap;
            for (Layer& l : s.timeline.layers) remap[l.id] = doc.newLayerId();
            for (Layer& l : s.timeline.layers) {
                l.id = remap[l.id];
                if (l.parentId) l.parentId = remap.count(l.parentId) ? remap[l.parentId] : 0;
            }
            doc.symbols.push_back(std::move(s));
        }
    }
    for (const QJsonValue& v : root["elements"].toArray())
        if (ElementPtr e = elementFromJson(v.toObject())) out.push_back(e);
    return out;
}

} // namespace vx
