// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — binary (Flash 5 … CS4) .fla import. The layouts decoded here are
// documented, with the evidence for each field, in docs/FLA_FORMAT.md.
#include "Cfb.h"
#include "FlaCommon.h"
#include "FlaImport.h"

#include <QtEndian>

#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>

namespace vx::io {

namespace {

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// --- byte reader ------------------------------------------------------------------

class Reader {
public:
    explicit Reader(const QByteArray& d) : m_d(d) {}

    qsizetype pos() const { return m_pos; }
    void seek(qsizetype p) { m_pos = p; }
    qsizetype size() const { return m_d.size(); }
    bool atEnd() const { return m_pos >= m_d.size(); }

    uint8_t u8() { need(1); return uint8_t(m_d[m_pos++]); }
    uint16_t u16() { need(2); const auto v = qFromLittleEndian<uint16_t>(m_d.constData() + m_pos); m_pos += 2; return v; }
    int16_t s16() { return int16_t(u16()); }
    uint32_t u32() { need(4); const auto v = qFromLittleEndian<uint32_t>(m_d.constData() + m_pos); m_pos += 4; return v; }
    int32_t s32() { return int32_t(u32()); }
    float f32()
    {
        const uint32_t v = u32();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    void skip(qsizetype n) { need(n); m_pos += n; }
    QByteArray bytes(qsizetype n)
    {
        need(n);
        const QByteArray b = m_d.mid(m_pos, n);
        m_pos += n;
        return b;
    }

    /// MFC CString: u8 length, or FF + FFFE + (u8|FF u16) UTF-16 length, or
    /// FF + FFFF + u32 length.
    QString string()
    {
        const uint8_t b = u8();
        if (b < 0xff) return QString::fromLatin1(bytes(b));
        const uint16_t ext = u16();
        if (ext == 0xfffe) {
            uint32_t n = u8();
            if (n == 0xff) n = u16();
            const QByteArray raw = bytes(qsizetype(n) * 2);
            return QString::fromUtf16(reinterpret_cast<const char16_t*>(raw.constData()), n);
        }
        if (ext == 0xffff) return QString::fromLatin1(bytes(u32()));
        return QString::fromLatin1(bytes(ext));
    }

private:
    void need(qsizetype n) const
    {
        if (n < 0 || m_pos + n > m_d.size()) throw ParseError("unexpected end of stream");
    }
    QByteArray m_d;
    qsizetype m_pos = 0;
};

// --- decoded objects --------------------------------------------------------------

struct PObj {
    virtual ~PObj() = default;
    std::string cls;
    std::vector<std::shared_ptr<PObj>> children;
    int32_t px = INT32_MIN, py = INT32_MIN; ///< CPicObj point (twips)
};
using PObjPtr = std::shared_ptr<PObj>;

struct PShape : PObj {
    ShapeGraph graph;
    Affine matrix;
};

struct PFrame : PShape {
    int span = 1;
    int flags = 0;
    int ease = 0;
    QString label;
};

struct PSymbol : PObj {
    bool sprite = false;
    Affine matrix;
    int firstFrame = 0;
    int loop = 0;
    ColorTransform ct;
    QString name;
    uint32_t ref = 0;
    FilterList filters;
    int blend = 0;
};

struct PLayer : PObj {
    QString name;
    bool locked = false, hidden = false;
    Color color = Color(0x4f, 0xff, 0x4f);
    int mode = 0;
    PObjPtr parent;
};

struct PPage : PObj {};

Affine readMatrix(Reader& r)
{
    const double a = r.s32() / 65536.0, b = r.s32() / 65536.0, c = r.s32() / 65536.0, d = r.s32() / 65536.0;
    const double tx = r.s32() / kTwipsPerPixel, ty = r.s32() / kTwipsPerPixel;
    return {a, b, c, d, tx, ty};
}

Color readColor(Reader& r)
{
    const uint32_t v = r.u32();
    return Color(uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24));
}

// --- the archive ------------------------------------------------------------------

class Archive {
public:
    Archive(const QByteArray& data, QStringList& warnings) : r(data), m_warnings(warnings) {}

    Reader r;

    PObjPtr readObject()
    {
        const qsizetype at = r.pos();
        const uint16_t tag = r.u16();
        if (tag == 0) return nullptr;
        std::string cls;
        if (tag == 0xffff) {
            r.u16(); // schema
            const uint16_t n = r.u16();
            if (n > 64) throw ParseError("bad class name");
            cls = r.bytes(n).toStdString();
            m_table.push_back({true, cls, nullptr});
        } else if (tag == 0x7fff) {
            const uint32_t i = r.u32();
            if (!(i & 0x80000000u)) return objectAt(i & 0x7fffffffu, at);
            cls = classAt(i & 0x7fffffffu, at);
        } else if (tag & 0x8000) {
            cls = classAt(tag & 0x7fffu, at);
        } else {
            return objectAt(tag, at);
        }
        PObjPtr o = create(cls);
        o->cls = cls;
        m_table.push_back({false, cls, o});
        parse(*o);
        return o;
    }

private:
    struct Entry {
        bool isClass;
        std::string cls;
        PObjPtr obj;
    };

    std::string classAt(uint32_t i, qsizetype at) const
    {
        if (i == 0 || i > m_table.size() || !m_table[i - 1].isClass)
            throw ParseError("bad class reference at " + std::to_string(at));
        return m_table[i - 1].cls;
    }
    PObjPtr objectAt(uint32_t i, qsizetype at) const
    {
        if (i == 0 || i > m_table.size() || m_table[i - 1].isClass)
            throw ParseError("bad object reference at " + std::to_string(at));
        return m_table[i - 1].obj;
    }

    static PObjPtr create(const std::string& cls)
    {
        if (cls == "CPicPage") return std::make_shared<PPage>();
        if (cls == "CPicLayer") return std::make_shared<PLayer>();
        if (cls == "CPicFrame") return std::make_shared<PFrame>();
        if (cls == "CPicShape") return std::make_shared<PShape>();
        if (cls == "CPicSymbol" || cls == "CPicSprite" || cls == "CPicButton") return std::make_shared<PSymbol>();
        throw ParseError("unsupported object " + cls);
    }

    void parse(PObj& o);
    void base(PObj& o);
    void shapeTail(PShape& s);
    void shapeData(ShapeGraph& g, int shapeSchema);
    FillStyle fillStyle(bool caps, int dataSchema);
    StrokeStyle lineStyle(bool caps, int dataSchema);
    void frameTail(PFrame& f);
    void timelineSub();
    void symbol(PSymbol& s);
    Filter filter();

    std::vector<Entry> m_table;
    QStringList& m_warnings;
};

void Archive::parse(PObj& o)
{
    if (auto* f = dynamic_cast<PFrame*>(&o)) {
        base(*f);
        shapeTail(*f);
        frameTail(*f);
    } else if (auto* s = dynamic_cast<PShape*>(&o)) {
        base(*s);
        shapeTail(*s);
    } else if (auto* sym = dynamic_cast<PSymbol*>(&o)) {
        symbol(*sym);
    } else if (auto* l = dynamic_cast<PLayer*>(&o)) {
        base(*l);
        const int ls = r.u8();
        l->name = r.string();
        if (ls <= 3) r.u8();
        if (ls >= 4 && ls <= 30) {
            r.u8(); // "current layer" flag of the editor
            l->locked = r.u8() != 0;
            l->hidden = r.u8() != 0;
        }
        if (ls >= 5 && ls <= 30) r.u32();
        if (ls >= 6 && ls <= 30) {
            l->color = readColor(r);
            r.u32();
        }
        if (ls >= 8 && ls <= 30) r.u32();
        l->mode = r.u8();
        l->parent = readObject();
        if (ls >= 7 && ls < 9) readObject();
        if (ls >= 2 && ls < 6) r.u8();
        if (ls >= 3 && ls < 9) r.u8();
        if (ls >= 9) r.u8();
        if (ls >= 10) r.u8();
        if (ls >= 11) r.u8();
    } else if (dynamic_cast<PPage*>(&o)) {
        base(o);
        // Page tail: editor state (current frame, current layer …) until the end.
    }
}

void Archive::base(PObj& o)
{
    const int schema = r.u8();
    r.u8(); // flags
    for (;;) {
        PObjPtr c = readObject();
        if (!c) break;
        o.children.push_back(c);
    }
    if (schema >= 1) {
        o.px = r.s32();
        o.py = r.s32();
    }
    if (schema >= 3) r.u8();
    if (schema >= 4) r.u8();
}

void Archive::shapeTail(PShape& s)
{
    const int shapeSchema = r.u8();
    s.matrix = readMatrix(r);
    shapeData(s.graph, shapeSchema);
    if (!(s.matrix == Affine())) s.graph = s.graph.transformed(s.matrix);
}

FillStyle Archive::fillStyle(bool caps, int dataSchema)
{
    const Color color = readColor(r);
    const uint8_t sub = r.u8();
    r.u8(); // flags
    const int sel = sub & 0x70;
    if (sel & 0x10) {
        FillStyle f;
        f.kind = (sub & 0x03) ? FillStyle::Kind::Radial : FillStyle::Kind::Linear;
        const Affine m = readMatrix(r);
        int n = r.u8();
        if (caps) {
            r.u16();
            r.u8();
        }
        if (dataSchema >= 5) r.skip(5);
        f.gradient.stops.clear();
        for (int i = 0; i < n; ++i) {
            const double pos = r.u8() / 255.0;
            f.gradient.stops.push_back({pos, readColor(r)});
        }
        if (f.gradient.stops.empty()) return FillStyle::solid(color);
        f.gradient.matrix = m * Affine::scale(kGradientHalfSize);
        return f;
    }
    if (sel & 0x40) {
        readMatrix(r);
        r.u32();
        if (!m_warnings.contains(QStringLiteral("Bitmap fills are imported as solid colour")))
            m_warnings << QStringLiteral("Bitmap fills are imported as solid colour");
        return FillStyle::solid(color);
    }
    if (sel & 0x20) {
        readMatrix(r);
        r.u32();
        r.skip(8);
    }
    return FillStyle::solid(color);
}

StrokeStyle Archive::lineStyle(bool caps, int dataSchema)
{
    StrokeStyle s;
    s.paint = FillStyle::solid(readColor(r));
    const int width = r.u16();
    r.skip(4); // compact inline fill
    if (width == 0) s.pattern = StrokePattern::Hairline;
    s.width = width == 0 ? 1.0 : width / kTwipsPerPixel;
    if (caps) {
        const int startCap = r.u8();
        r.u8(); // end cap
        const int join = r.u8();
        r.u8();
        s.miterLimit = std::max(1, int(r.u16()));
        const FillStyle paint = fillStyle(caps, dataSchema);
        if (paint.isGradient()) s.paint = paint;
        s.cap = startCap == 1 ? CapStyle::None : startCap == 2 ? CapStyle::Square : CapStyle::Round;
        s.join = join == 1 ? JoinStyle::Bevel : join == 2 ? JoinStyle::Miter : JoinStyle::Round;
    }
    return s;
}

void Archive::shapeData(ShapeGraph& g, int shapeSchema)
{
    const bool caps = shapeSchema > 2;
    const int ds = r.u8();
    const uint32_t edgeHint = r.u32();
    const int nf = r.u16();
    for (int i = 0; i < nf; ++i) g.fills.push_back(fillStyle(caps, ds));
    const int nl = r.u16();
    for (int i = 0; i < nl; ++i) g.strokes.push_back(lineStyle(caps, ds));

    // Edge stream: quadratic edges in 1/256 twip (1/128 before Flash 8).
    const double unit = 1.0 / (kTwipsPerPixel * (shapeSchema > 2 ? 256.0 : 128.0));
    const bool lineExtraByte = shapeSchema >= 6;
    int64_t x = 0, y = 0;
    int line = 0, fillL = 0, fillR = 0;
    auto delta = [&](int type, int64_t& dx, int64_t& dy) {
        switch (type) {
        case 0: dx = dy = 0; break;
        case 1: dx = r.s16(); dy = r.s16(); break;
        case 2: dx = r.s32(); dy = r.s32(); break;
        default: dx = int64_t(r.s16()) * 128; dy = int64_t(r.s16()) * 128; break;
        }
    };
    auto clampStyle = [](int v, size_t n) { return v >= 0 && size_t(v) <= n ? v : 0; };
    g.edges.reserve(edgeHint < 1000000 ? edgeHint : 0);
    for (;;) {
        const uint8_t fl = r.u8();
        if (fl == 0) break;
        if (fl & 0x40) {
            if (fl & 0x80) {
                line = r.u8();
                fillR = r.u8();
                fillL = r.u8();
            } else {
                line = r.u16() & 0x7fff;
                fillR = r.u16() & 0x7fff;
                fillL = r.u16() & 0x7fff;
            }
        }
        int64_t mx, my, cx, cy, ax, ay;
        delta(fl & 3, mx, my);
        delta((fl >> 2) & 3, cx, cy);
        delta((fl >> 4) & 3, ax, ay);
        const bool straight = (fl & 0x0c) == 0;
        if (straight && lineExtraByte) r.u8();
        const int64_t fx = x + mx, fy = y + my;
        const int64_t tx = fx + ax, ty = fy + ay;
        const Vec2 p0(fx * unit, fy * unit), p3(tx * unit, ty * unit);
        Cubic c;
        if (straight) {
            c = Cubic::line(p0, p3);
        } else {
            c = Cubic::fromQuad(p0, Vec2((fx + cx) * unit, (fy + cy) * unit), p3);
        }
        x = tx;
        y = ty;
        const int s = clampStyle(line, g.strokes.size());
        const int l = clampStyle(fillL, g.fills.size()), rr = clampStyle(fillR, g.fills.size());
        if ((l == 0 && rr == 0 && s == 0) || (p0 == p3 && straight)) continue;
        g.edges.push_back({c, l, rr, s});
    }

    // Original cubic segments kept for editing in Flash; the quadratic edges
    // above are what Flash renders, so these are skipped.
    if (ds > 4) {
        const int32_t n = r.s32();
        if (n < 0 || n > 10000000) throw ParseError("bad cubic count");
        for (int32_t i = 0; i < n; ++i) {
            if (shapeSchema >= 6) {
                r.skip(32);
                const int k = r.u8();
                r.skip(qsizetype(k) * 10);
                const uint8_t flags = r.u8();
                if (flags & 1) r.skip(8);
                if (flags & 2) r.skip(8);
            } else {
                r.skip(32);
            }
        }
    }
    g.invalidate();
}

void Archive::timelineSub()
{
    const uint32_t type = r.u32();
    const uint32_t format = r.u32();
    if (type >= 1) {
        r.u32();
        const uint32_t n = r.u32();
        if (n > 100000) throw ParseError("bad timeline id count");
        r.skip(qsizetype(n) * 4);
    }
    if (format == 1 && type >= 4) {
        if (type >= 5) r.u32();
        r.string();
    } else if (format == 0) {
        r.u32();
        const uint32_t n = r.u32();
        if (n > 100000) throw ParseError("bad per-frame count");
        r.skip(qsizetype(n) * 4);
    }
}

void Archive::frameTail(PFrame& f)
{
    const int fs = r.u8();
    f.span = r.u16();
    f.flags = fs > 2 ? r.u16() : r.u8();
    f.ease = fs > 1 ? r.s16() : 0;
    if (fs > 4) r.u16(); // sound
    if (fs > 5) {
        const int n = r.u16();
        r.skip(qsizetype(n) * 8);
    }
    if (fs > 6) {
        r.u16();
        r.u8();
        r.u32();
        r.s32();
    }
    if (fs > 7) r.u16();
    if (fs > 8) {
        if (fs >= 23) f.label = r.string();
        if (fs >= 19) timelineSub();
        else throw ParseError("frame schema " + std::to_string(fs) + " is not supported yet");
        if (fs > 10) {
            r.u32();
            r.u32();
            if (fs > 11) r.u32();
            if (fs > 12 && readObject()) {
                if (!m_warnings.contains(QStringLiteral("Shape tweens are imported as keyframes")))
                    m_warnings << QStringLiteral("Shape tweens are imported as keyframes");
            }
            if (fs > 13) r.u32();
            if (fs > 14) readObject();
            if (fs > 15 && fs >= 23) r.string();
            if (fs > 19) r.u32();
            if (fs > 20) r.u32();
            if (fs >= 22) r.u32();
            if (fs >= 24) {
                r.u32();
                r.u32();
            }
        }
    }
    if (f.span > 100000) throw ParseError("bad keyframe span");
}

Filter Archive::filter()
{
    // 48-byte generic filter record; the type uses SWF filter numbering.
    Filter f;
    const int type = r.u8();
    r.skip(7);
    const Color color = readColor(r);
    f.distance = r.f32();
    f.blurX = r.f32();
    f.blurY = r.f32();
    f.angle = r.f32() * 180.0 / kPi;
    f.inner = r.u32() != 0;
    f.knockout = r.u32() != 0;
    f.quality = std::clamp(int(r.u32()), 1, 3);
    f.strength = r.u32() / 100.0;
    f.hideObject = r.u32() != 0;
    switch (type) {
    case 0: f.type = FilterType::DropShadow; break;
    case 1: f.type = FilterType::Blur; break;
    case 2: f.type = FilterType::Glow; break;
    case 3: f.type = FilterType::Bevel; break;
    case 4: f.type = FilterType::GradientGlow; break;
    case 6: f.type = FilterType::AdjustColor; break;
    case 7: f.type = FilterType::GradientBevel; break;
    default:
        f.type = FilterType::Blur;
        if (!m_warnings.contains(QStringLiteral("Unknown filter types are imported as blur")))
            m_warnings << QStringLiteral("Unknown filter types are imported as blur");
        break;
    }
    f.color = color;
    return f;
}

void Archive::symbol(PSymbol& s)
{
    base(s);
    const int schema = r.u8();
    s.matrix = readMatrix(r);
    if (schema < 22) throw ParseError("symbol instance schema " + std::to_string(schema) + " is not supported yet");
    s.firstFrame = r.u16();
    s.loop = r.u16();
    if (r.u8()) {
        const double rm = r.s16() / 256.0, ro = r.s16(), gm = r.s16() / 256.0, go = r.s16();
        const double bm = r.s16() / 256.0, bo = r.s16(), am = r.s16() / 256.0, ao = r.s16();
        s.ct = {rm, gm, bm, am, ro, go, bo, ao};
    }
    r.u16(); // colour effect mode shown in the Properties panel
    r.u16(); // effect percentage
    r.u32(); // effect colour
    s.name = r.string();
    s.ref = r.u32();
    r.skip(3);
    if (r.u8()) {
        const uint32_t n = r.u32();
        if (n > 64) throw ParseError("bad filter count");
        for (uint32_t i = 0; i < n; ++i) s.filters.push_back(filter());
    }
    s.blend = r.u8();
    r.u16();
    r.skip(64); // 3D matrix (CS4)
    r.skip(24);
    r.s32(); // 3D centre
    r.s32();
    r.skip(6);
    if (s.cls == "CPicSprite" || s.cls == "CPicButton") {
        s.sprite = true;
        r.u8(); // sprite schema
        timelineSub();
        r.string();
        r.u32();
        r.skip(18);
        r.string(); // component metadata (XML)
    }
}

// --- library (Contents stream) -----------------------------------------------------

struct PageInfo {
    QString stream;
    QString name;
    uint32_t id = 0;
    int type = 0; ///< 0 graphic, 1 button, 2 movie clip
};

QByteArray utf16(const QString& s)
{
    return QByteArray(reinterpret_cast<const char*>(s.utf16()), s.size() * 2);
}

std::vector<PageInfo> readPages(const QByteArray& contents, const QStringList& streams)
{
    std::vector<std::pair<qsizetype, PageInfo>> found;
    for (const QString& st : streams) {
        if (!st.startsWith(QLatin1String("P ")) && !st.startsWith(QLatin1String("S ")) &&
            !st.startsWith(QLatin1String("Page ")) && !st.startsWith(QLatin1String("Symbol ")))
            continue;
        QByteArray key;
        key.append(char(st.size()));
        key.append(utf16(st));
        const qsizetype at = contents.indexOf(key);
        if (at < 0) continue;
        PageInfo info;
        info.stream = st;
        try {
            Reader r(contents);
            r.seek(at + key.size());
            info.name = r.string();
            info.id = r.u32();
            info.type = r.u8();
        } catch (const ParseError&) {
            info.name = st;
        }
        found.emplace_back(at, info);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<PageInfo> out;
    for (auto& [at, p] : found) out.push_back(p);
    return out;
}

void readStageSettings(const QByteArray& c, Document& doc)
{
    // Stage rectangle {0, width, 0, height} in twips, then (53 bytes later) the
    // background colour and (62 bytes later) the 8.8 frame rate.
    for (qsizetype at = 0; at + 64 <= c.size(); ++at) {
        const int32_t x0 = qFromLittleEndian<int32_t>(c.constData() + at);
        const int32_t x1 = qFromLittleEndian<int32_t>(c.constData() + at + 4);
        const int32_t y0 = qFromLittleEndian<int32_t>(c.constData() + at + 8);
        const int32_t y1 = qFromLittleEndian<int32_t>(c.constData() + at + 12);
        if (x0 != 0 || y0 != 0 || x1 < 20 || y1 < 20 || x1 > 20 * 8192 || y1 > 20 * 8192) continue;
        if (x1 % 20 || y1 % 20) continue;
        if (uint8_t(c[at + 53 + 3]) != 0xff) continue;
        const uint16_t fps = qFromLittleEndian<uint16_t>(c.constData() + at + 62);
        if (fps < 0x0100 || fps > 0x7800) continue;
        doc.width = x1 / kTwipsPerPixel;
        doc.height = y1 / kTwipsPerPixel;
        doc.background = Color(uint8_t(c[at + 53]), uint8_t(c[at + 54]), uint8_t(c[at + 55]));
        doc.fps = fps / 256.0;
        return;
    }
}

QString generatorOf(const QByteArray& contents)
{
    // Publish settings: the newest player version the authoring tool knew.
    const QByteArray key = utf16(QStringLiteral("PublishHtmlProperties::VersionInfo"));
    const qsizetype at = contents.indexOf(key);
    if (at < 0) return QStringLiteral("Flash");
    try {
        Reader r(contents);
        r.seek(at + key.size());
        const QString v = r.string();
        const int major = v.section(',', 0, 0).toInt();
        switch (major) {
        case 10: return QStringLiteral("Flash CS4");
        case 9: return QStringLiteral("Flash CS3");
        case 8: return QStringLiteral("Flash 8");
        case 7: return QStringLiteral("Flash MX 2004");
        case 6: return QStringLiteral("Flash MX");
        default: return major > 0 ? QStringLiteral("Flash (player %1)").arg(major) : QStringLiteral("Flash");
        }
    } catch (const ParseError&) {
        return QStringLiteral("Flash");
    }
}

// --- building the document ---------------------------------------------------------

class Builder {
public:
    Builder(Document& doc, ImportReport& report) : m_doc(doc), m_report(report) {}

    std::map<uint32_t, std::string> symbolIds; ///< Flash symbol number -> id

    Timeline timeline(const PObjPtr& page, const QString& name)
    {
        Timeline tl;
        tl.name = name.toStdString();
        if (!page) return tl;
        // Layers are stored bottom first; masks may be serialised inside the
        // layers they mask, so collect unique layers in stacking order.
        std::vector<PLayer*> layers;
        for (const PObjPtr& c : page->children)
            if (auto* l = dynamic_cast<PLayer*>(c.get()))
                if (std::find(layers.begin(), layers.end(), l) == layers.end()) layers.push_back(l);
        std::reverse(layers.begin(), layers.end());
        std::map<const PLayer*, uint32_t> ids;
        for (PLayer* pl : layers) ids[pl] = m_doc.newLayerId();
        for (PLayer* pl : layers) {
            Layer l;
            l.id = ids[pl];
            l.name = pl->name.toStdString();
            l.locked = pl->locked;
            l.color = Color(pl->color.r, pl->color.g, pl->color.b);
            switch (pl->mode) {
            case 1: l.type = LayerType::Guide; break;
            case 3: l.type = LayerType::Mask; break;
            case 5: l.type = LayerType::Folder; break;
            default: l.type = LayerType::Normal; break;
            }
            if (auto* parent = dynamic_cast<PLayer*>(pl->parent.get()))
                if (ids.count(parent)) l.parentId = ids[parent];
            int start = 0;
            for (const PObjPtr& c : pl->children) {
                auto* f = dynamic_cast<PFrame*>(c.get());
                if (!f || f->span <= 0) continue; // zero-length keyframes occupy no frames
                Keyframe k;
                k.start = start;
                k.duration = f->span;
                k.label = f->label.toStdString();
                if (f->flags & 1) {
                    k.tween = TweenType::Classic;
                    k.classic.ease.kind = f->ease != 0 ? EaseKind::Classic : EaseKind::None;
                    k.classic.ease.strength = std::clamp(f->ease, -100, 100);
                }
                k.elements = elements(*f);
                start += f->span;
                l.keys.push_back(std::move(k));
                ++m_report.keyframes;
            }
            l.normalize();
            tl.layers.push_back(std::move(l));
            ++m_report.layers;
        }
        return tl;
    }

private:
    std::vector<ElementPtr> elements(const PShape& s)
    {
        std::vector<ElementPtr> out;
        if (!s.graph.isEmpty()) {
            out.push_back(makeShapeElement(s.graph, false));
            ++m_report.shapes;
        }
        for (const PObjPtr& c : s.children) {
            if (auto* sym = dynamic_cast<PSymbol*>(c.get())) {
                if (ElementPtr e = instance(*sym)) out.push_back(e);
            } else if (auto* sh = dynamic_cast<PShape*>(c.get())) {
                std::vector<ElementPtr> inner = elements(*sh);
                if (inner.size() == 1 && inner[0]->type() == ElementType::Shape) {
                    auto obj = inner[0]->cloneAs<ShapeElement>();
                    obj->isObject = true;
                    out.push_back(obj);
                } else if (!inner.empty()) {
                    auto g = std::make_shared<GroupElement>();
                    g->children = std::move(inner);
                    out.push_back(g);
                }
            }
        }
        return out;
    }

    ElementPtr instance(const PSymbol& s)
    {
        auto it = symbolIds.find(s.ref);
        if (it == symbolIds.end()) {
            m_report.warnings << QStringLiteral("An instance refers to missing symbol %1").arg(s.ref);
            return nullptr;
        }
        auto e = std::make_shared<InstanceElement>();
        e->symbolId = it->second;
        e->behavior = s.cls == "CPicSprite" ? SymbolType::MovieClip : s.cls == "CPicButton" ? SymbolType::Button : SymbolType::Graphic;
        e->matrix = s.matrix;
        if (s.px != INT32_MIN && s.py != INT32_MIN) {
            const Vec2 p(s.px / kTwipsPerPixel, s.py / kTwipsPerPixel);
            if (std::abs(s.matrix.det()) > 1e-12) e->pivot = s.matrix.inverted().map(p);
        }
        e->name = s.name.toStdString();
        e->firstFrame = std::max(0, s.firstFrame);
        e->loop = s.loop == 1 ? LoopMode::PlayOnce : s.loop == 2 ? LoopMode::SingleFrame : LoopMode::Loop;
        e->color = colorEffectFrom(s.ct);
        e->blend = blendFromFlash(s.blend);
        e->filters = s.filters;
        ++m_report.instances;
        return e;
    }

    Document& m_doc;
    ImportReport& m_report;
};

PObjPtr parseStream(const QByteArray& data, const QString& name, QStringList& warnings)
{
    if (data.isEmpty()) return nullptr;
    Archive ar(data, warnings);
    PObjPtr root;
    try {
        ar.r.u8(); // stream version
        root = ar.readObject();
    } catch (const ParseError& e) {
        warnings << QStringLiteral("%1: %2 (imported partially)").arg(name, QString::fromStdString(e.what()));
    }
    return root;
}

} // namespace

bool importBinaryFla(const QByteArray& data, Document& doc, ImportReport* reportOut, QString* error)
{
    ImportReport local;
    ImportReport& report = reportOut ? *reportOut : local;
    report.format = FlaFormat::Binary;
    CompoundFile cf;
    if (!cf.open(data, error)) return false;
    const QByteArray contents = cf.stream(QStringLiteral("Contents"));
    if (contents.isEmpty()) {
        if (error) *error = QStringLiteral("Not a Flash document (no Contents stream)");
        return false;
    }
    report.generator = generatorOf(contents);

    Document out;
    out.scenes.clear();
    out.symbols.clear();
    readStageSettings(contents, out);

    const std::vector<PageInfo> pages = readPages(contents, cf.streams());
    Builder builder(out, report);
    // Register symbols first: instances refer to them by number.
    for (const PageInfo& p : pages) {
        if (!p.stream.startsWith('S')) continue;
        Symbol s;
        s.id = out.newSymbolId();
        s.name = p.name.toStdString();
        s.type = p.type == 2 ? SymbolType::MovieClip : p.type == 1 ? SymbolType::Button : SymbolType::Graphic;
        builder.symbolIds[p.id] = s.id;
        out.symbols.push_back(std::move(s));
    }
    for (const PageInfo& p : pages) {
        if (p.stream.startsWith('S')) {
            const PObjPtr root = parseStream(cf.stream(p.stream), p.stream, report.warnings);
            Symbol* s = out.symbol(builder.symbolIds[p.id]);
            if (s) s->timeline = builder.timeline(root, p.name);
            ++report.symbols;
        }
    }
    for (const PageInfo& p : pages) {
        if (p.stream.startsWith('P')) {
            const PObjPtr root = parseStream(cf.stream(p.stream), p.stream, report.warnings);
            out.scenes.push_back(builder.timeline(root, p.name));
            ++report.scenes;
        }
    }
    if (out.scenes.empty()) {
        if (error) *error = QStringLiteral("The document has no scenes");
        return false;
    }
    for (Timeline& tl : out.scenes)
        if (tl.layers.empty()) tl.layers.push_back(out.makeLayer("Layer 1"));
    for (Symbol& s : out.symbols)
        if (s.timeline.layers.empty()) s.timeline.layers.push_back(out.makeLayer("Layer 1"));
    for (const QString& st : cf.streams())
        if (st.startsWith(QLatin1String("M ")) || st.startsWith(QLatin1String("Media "))) {
            report.warnings << QStringLiteral("Bitmaps and sounds are not imported yet");
            break;
        }
    doc = std::move(out);
    return true;
}

} // namespace vx::io
