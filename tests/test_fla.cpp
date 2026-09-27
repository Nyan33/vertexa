// SPDX-License-Identifier: GPL-3.0-or-later
// Binary FLA import. The test builds a small Flash CS4 style document byte by
// byte (OLE2 container + MFC archive streams) following docs/FLA_FORMAT.md, so
// the importer is exercised without shipping third-party .fla files. Real
// files can be checked too: set VERTEXA_FLA_SAMPLES to a folder of .fla files.
#include "TestMain.h"

#include "core/Evaluate.h"
#include "core/Serialize.h"
#include "core/io/Cfb.h"
#include "core/io/FlaImport.h"

#include <QDir>
#include <QtEndian>

#include <cstring>

using namespace vx;

namespace {

// --- little writer ------------------------------------------------------------------

struct W {
    QByteArray b;
    W& u8(int v) { b.append(char(uint8_t(v))); return *this; }
    W& u16(int v) { u8(v & 0xff); return u8((v >> 8) & 0xff); }
    W& u32(uint32_t v) { u16(int(v & 0xffff)); return u16(int(v >> 16)); }
    W& s32(int32_t v) { return u32(uint32_t(v)); }
    W& s16(int v) { return u16(v & 0xffff); }
    W& f32(float f) { uint32_t v; std::memcpy(&v, &f, 4); return u32(v); }
    W& zeros(int n) { b.append(QByteArray(n, '\0')); return *this; }
    W& str(const QString& s) // MFC unicode CString
    {
        u8(0xff).u16(0xfffe).u8(int(s.size()));
        for (QChar c : s) u16(c.unicode());
        return *this;
    }
    W& newClass(const char* name)
    {
        u16(0xffff).u16(1).u16(int(strlen(name)));
        b.append(name);
        return *this;
    }
    W& matrix(double a, double bb, double c, double d, double tx, double ty)
    {
        return s32(int32_t(a * 65536)).s32(int32_t(bb * 65536)).s32(int32_t(c * 65536)).s32(int32_t(d * 65536))
            .s32(int32_t(tx * 20)).s32(int32_t(ty * 20));
    }
    W& color(int r, int g, int bb, int a = 255) { return u8(r).u8(g).u8(bb).u8(a); }
};

// CPicObj base with no children: schema 5, flags, NULL tag, INT_MIN point, 2 bytes.
void emptyBase(W& w) { w.u8(5).u8(0).u16(0).s32(INT32_MIN).s32(INT32_MIN).u8(0).u8(0); }

/// Shape tail of a frame: a square (straight edges) or a curved blob.
void shapeTail(W& w, bool curved, int fillColor)
{
    w.u8(6).matrix(1, 0, 0, 1, 0, 0);
    w.u8(5).u32(curved ? 3 : 4);                                     // data schema, edge count hint
    w.u16(1).color(fillColor, 40, 80).u8(0).u8(0);                   // one solid fill
    w.u16(0);                                                        // no line styles
    const int unit = 256 * 20;                                       // 1/256 twip per unit
    if (!curved) {
        // Square 10,10 .. 110,110 drawn clockwise on screen; fill index in
        // the third (left) slot. First record: style change + move.
        w.u8(0x80 | 0x40 | 0x10 | 0x02).u8(0).u8(0).u8(1).s32(10 * unit).s32(10 * unit).s16(0).s16(0);
        w.u8(0);                                                     // straight edges carry an extra byte (CS4)
        // The record above moved to (10,10) with a zero-length edge; now the
        // four sides (type 2 deltas: s32 pairs).
        const int d[4][2] = {{100, 0}, {0, 100}, {-100, 0}, {0, -100}};
        for (auto& e : d) w.u8(0x20).s32(e[0] * unit).s32(e[1] * unit).u8(0);
    } else {
        // Triangle-ish blob with one quadratic edge.
        w.u8(0x80 | 0x40 | 0x20 | 0x0 | 0x02).u8(0).u8(0).u8(1).s32(0).s32(0).s32(50 * unit).s32(0).u8(0);
        w.u8(0x20 | 0x08).s32(25 * unit).s32(40 * unit).s32(0).s32(50 * unit); // quadratic edge
        w.u8(0x20).s32(-50 * unit).s32(-50 * unit).u8(0);
    }
    w.u8(0);  // end of edges
    w.s32(0); // no cubic records
}

/// CPicFrame tail (frame schema 29).
void frameTail(W& w, int span, int flags, const QString& label)
{
    w.u8(29).u16(span).u16(flags).u16(0).u16(0).u16(0);
    w.u16(1).u8(3).u32(0).s32(0x3fffffff).u16(0xffff);
    w.str(label);
    w.u32(5).u32(1).u32(0x1234).u32(0).u32(0).str({});               // timeline sub-object
    w.u32(0).u32(0).u32(0).u16(0).u32(0).u16(0).str({}).u32(1).u32(0).u32(0).u32(1).u32(0);
}

void layerTail(W& w, const QString& name)
{
    w.u16(0).s32(INT32_MIN).s32(INT32_MIN).u8(0).u8(0); // end of frames + point
    w.u8(13).str(name).u8(1).u8(0).u8(0);
    w.u32(0xffffffff).color(0x4f, 0xff, 0x4f).u32(0).u32(1);
    w.u8(0).u16(0).u8(1).u8(1).u8(0);                    // mode, parent (none), editor bytes
}

void pageEnd(W& w) { w.u16(0).s32(INT32_MIN).s32(INT32_MIN).u8(0).u8(0).u8(7).zeros(12); }

QByteArray symbolStream()
{
    // One layer, keyframe 1 (2 frames, classic tween) + keyframe 2 (3 frames, label).
    W w;
    w.u8(1).newClass("CPicPage").u8(5).u8(0);
    w.newClass("CPicLayer").u8(5).u8(0);
    w.newClass("CPicFrame");
    emptyBase(w);
    shapeTail(w, false, 200);
    frameTail(w, 2, 0x2601, {});
    w.u16(0x8005); // CPicFrame class back-reference (index 5)
    emptyBase(w);
    shapeTail(w, true, 30);
    frameTail(w, 3, 0x2600, QStringLiteral("Hello"));
    layerTail(w, QStringLiteral("Art"));
    pageEnd(w);
    return w.b;
}

QByteArray sceneStream()
{
    W w;
    w.u8(1).newClass("CPicPage").u8(5).u8(0);
    w.newClass("CPicLayer").u8(5).u8(0);
    w.newClass("CPicFrame").u8(5).u8(0);
    // Frame children: one graphic instance of symbol 1.
    w.newClass("CPicSymbol").u8(5).u8(0).u16(0).s32(4000).s32(1000).u8(0).u8(0);
    w.u8(22).matrix(1, 0, 0, 1, 200, 50);
    w.u16(1).u16(2);                                     // first frame 1, single frame
    w.u8(1).u16(0x0100).u16(0).u16(0x0100).u16(0).u16(0x0100).u16(0).u16(0x0080).u16(0); // alpha 50 %
    w.u16(4).u16(50).u32(0xff000000);
    w.str(QStringLiteral("box1")).u32(1);
    w.zeros(3).u8(0).u8(1).u16(0);                       // no filters, blend normal
    for (int i = 0; i < 16; ++i) w.f32(i % 5 == 0 ? 1.0f : 0.0f);
    w.zeros(24).s32(INT32_MIN).s32(INT32_MIN).zeros(6);
    w.u16(0).s32(INT32_MIN).s32(INT32_MIN).u8(0).u8(0);  // end of frame children
    shapeTail(w, false, 90);
    frameTail(w, 4, 0x2600, {});
    layerTail(w, QStringLiteral("Stage"));
    pageEnd(w);
    return w.b;
}

QByteArray contentsStream()
{
    W w;
    w.u32(0x147).zeros(24);
    auto page = [&](const QString& stream, const QString& name, int id, int type) {
        w.u16(0x8001).u8(25).u8(int(stream.size()));
        for (QChar c : stream) w.u16(c.unicode());
        w.str(name).u32(uint32_t(id)).u8(type).zeros(40);
    };
    page(QStringLiteral("P 1 1"), QStringLiteral("Scene 1"), 0, 0);
    page(QStringLiteral("S 1 1"), QStringLiteral("Box"), 1, 0);
    // Stage rectangle, background (+53) and 8.8 frame rate (+62).
    W st;
    st.s32(0).s32(550 * 20).s32(0).s32(400 * 20);
    st.zeros(53 - 16).color(20, 30, 40).zeros(62 - 57).u16(30 * 256).zeros(8);
    w.b.append(st.b);
    w.str(QStringLiteral("PublishHtmlProperties::VersionInfo")).str(QStringLiteral("10,0,2,0;9,0,124,0"));
    return w.b;
}

/// Minimal [MS-CFB] v3 writer: small streams live in the mini stream.
QByteArray compoundFile(const std::vector<std::pair<QString, QByteArray>>& streams)
{
    constexpr uint32_t kEnd = 0xfffffffe, kFree = 0xffffffff, kFatSect = 0xfffffffd;
    QByteArray mini;
    std::vector<uint32_t> miniFat;
    struct Placed {
        QString name;
        uint32_t start;
        uint32_t size;
    };
    std::vector<Placed> placed;
    for (const auto& [name, data] : streams) {
        const uint32_t first = uint32_t(mini.size() / 64);
        const int blocks = int((data.size() + 63) / 64);
        for (int i = 0; i < blocks; ++i) miniFat.push_back(i + 1 < blocks ? first + i + 1 : kEnd);
        mini.append(data);
        mini.append(QByteArray(blocks * 64 - data.size(), '\0'));
        placed.push_back({name, first, uint32_t(data.size())});
    }
    auto sectors = [](qsizetype bytes) { return uint32_t((bytes + 511) / 512); };
    const uint32_t dirSectors = sectors((placed.size() + 1) * 128);
    const uint32_t miniFatSectors = sectors(qsizetype(miniFat.size()) * 4);
    const uint32_t miniSectors = sectors(mini.size());
    const uint32_t fatSector = 0, dirStart = 1, miniFatStart = dirStart + dirSectors,
                   miniStart = miniFatStart + miniFatSectors;
    const uint32_t total = miniStart + miniSectors;
    std::vector<uint32_t> fat(128, kFree);
    fat[fatSector] = kFatSect;
    auto chain = [&](uint32_t start, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) fat[start + i] = i + 1 < n ? start + i + 1 : kEnd;
    };
    chain(dirStart, dirSectors);
    chain(miniFatStart, miniFatSectors);
    chain(miniStart, miniSectors);

    W out;
    out.b.append("\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8);
    out.zeros(16).u16(0x3e).u16(3).u16(0xfffe).u16(9).u16(6).zeros(6);
    out.u32(0).u32(1).u32(dirStart).u32(0).u32(4096).u32(miniFatStart).u32(miniFatSectors).u32(kEnd).u32(0);
    out.u32(fatSector);
    for (int i = 1; i < 109; ++i) out.u32(kFree);
    // FAT sector.
    for (uint32_t v : fat) out.u32(v);
    // Directory.
    W dir;
    auto entry = [&](const QString& name, int type, uint32_t child, uint32_t right, uint32_t start, uint32_t size) {
        W e;
        for (QChar c : name) e.u16(c.unicode());
        e.u16(0);
        e.zeros(64 - int(e.b.size()));
        e.u16(int((name.size() + 1) * 2)).u8(type).u8(1).u32(kFree).u32(right).u32(child);
        e.zeros(16 + 4 + 16).u32(start).u32(size).u32(0);
        dir.b.append(e.b);
    };
    entry(QStringLiteral("Root Entry"), 5, 1, kFree, miniStart, uint32_t(mini.size()));
    for (size_t i = 0; i < placed.size(); ++i)
        entry(placed[i].name, 2, kFree, i + 1 < placed.size() ? uint32_t(i + 2) : kFree, placed[i].start, placed[i].size);
    dir.zeros(int(dirSectors * 512 - dir.b.size()));
    out.b.append(dir.b);
    W mf;
    for (uint32_t v : miniFat) mf.u32(v);
    mf.b.append(QByteArray(int(miniFatSectors * 512 - mf.b.size()), char(0xff)));
    out.b.append(mf.b);
    out.b.append(mini);
    out.zeros(int(total * 512 + 512 - out.b.size()));
    return out.b;
}

QByteArray sampleFla()
{
    return compoundFile({{QStringLiteral("Contents"), contentsStream()},
                         {QStringLiteral("P 1 1"), sceneStream()},
                         {QStringLiteral("S 1 1"), symbolStream()}});
}

} // namespace

VX_TEST(cfb_reader)
{
    io::CompoundFile cf;
    QString err;
    CHECK(cf.open(sampleFla(), &err));
    CHECK(cf.streams().size() == 3);
    CHECK(cf.hasStream(QStringLiteral("S 1 1")));
    CHECK(cf.stream(QStringLiteral("S 1 1")) == symbolStream());
    CHECK(cf.stream(QStringLiteral("Contents")) == contentsStream());
    CHECK(!io::CompoundFile::isCompoundFile(QByteArray("PK\x03\x04", 4)));
}

VX_TEST(binary_fla_import)
{
    Document doc;
    io::ImportReport report;
    QString err;
    CHECK(io::importBinaryFla(sampleFla(), doc, &report, &err));
    for (const QString& w : report.warnings) std::printf("  warning: %s\n", qPrintable(w));
    CHECK(report.warnings.isEmpty());
    CHECK(report.generator == QStringLiteral("Flash CS4"));
    CHECK_NEAR(doc.width, 550.0, 1e-9);
    CHECK_NEAR(doc.height, 400.0, 1e-9);
    CHECK_NEAR(doc.fps, 30.0, 1e-9);
    CHECK(doc.background == Color(20, 30, 40));
    CHECK(doc.scenes.size() == 1 && doc.symbols.size() == 1);

    const Symbol& box = doc.symbols[0];
    CHECK(box.name == "Box" && box.type == SymbolType::Graphic);
    CHECK(box.timeline.layers.size() == 1);
    const Layer& art = box.timeline.layers[0];
    CHECK(art.name == "Art");
    CHECK(art.keys.size() == 2);
    CHECK(art.keys[0].duration == 2 && art.keys[0].tween == TweenType::Classic);
    CHECK(art.keys[1].start == 2 && art.keys[1].duration == 3 && art.keys[1].label == "Hello");

    // The square: exact geometry and the fill on the inside (left slot).
    const auto* sq = dynamic_cast<const ShapeElement*>(art.keys[0].elements.at(0).get());
    CHECK(sq != nullptr);
    CHECK(sq->graph->edges.size() == 4);
    CHECK(sq->graph->fillAt({60, 60}) == 1);
    CHECK(sq->graph->fillAt({200, 200}) == 0);
    CHECK_NEAR(sq->graph->fillRegion().area(), 10000.0, 1e-6);
    CHECK(sq->graph->fills[0].color == Color(200, 40, 80));

    // The blob keeps its quadratic edge as an exact cubic.
    const auto* blob = dynamic_cast<const ShapeElement*>(art.keys[1].elements.at(0).get());
    CHECK(blob != nullptr && blob->graph->edges.size() == 3);
    bool curved = false;
    for (const GEdge& e : blob->graph->edges) curved |= !e.c.isStraight(1e-9);
    CHECK(curved);

    // Scene: the merge shape at the bottom, then the instance.
    const Layer& stage = doc.scenes[0].layers[0];
    CHECK(stage.name == "Stage" && stage.keys.size() == 1 && stage.keys[0].duration == 4);
    CHECK(stage.keys[0].elements.size() == 2);
    const auto* inst = dynamic_cast<const InstanceElement*>(stage.keys[0].elements[1].get());
    CHECK(inst != nullptr);
    CHECK(inst->symbolId == box.id);
    CHECK(inst->name == "box1");
    CHECK_NEAR(inst->matrix.tx, 200.0, 1e-9);
    CHECK_NEAR(inst->matrix.ty, 50.0, 1e-9);
    CHECK(inst->firstFrame == 1 && inst->loop == LoopMode::SingleFrame);
    CHECK(inst->color.kind == ColorEffect::Kind::Alpha);
    CHECK_NEAR(inst->color.alpha, 0.5, 1e-9);
    CHECK_NEAR(inst->pivot.x, 0.0, 1e-9); // point (200,50) px = tx,ty
    CHECK_NEAR(inst->pivot.y, 0.0, 1e-9);
}

VX_TEST(filters_roundtrip)
{
    Document doc;
    QString err;
    CHECK(io::importBinaryFla(sampleFla(), doc, nullptr, &err));
    auto& key = doc.scenes[0].layers[0].keys[0];
    auto inst = key.elements[1]->cloneAs<InstanceElement>();
    Filter glow = Filter::defaults(FilterType::Glow);
    glow.color = Color(255, 200, 40, 180);
    glow.blurX = 12;
    glow.quality = 3;
    Filter adjust = Filter::defaults(FilterType::AdjustColor);
    adjust.hue = 45;
    adjust.saturation = -30;
    inst->filters = {glow, adjust};
    key.elements[1] = inst;
    Document back;
    CHECK(deserializeDocument(serializeDocument(doc), back));
    const auto* b = dynamic_cast<const InstanceElement*>(back.scenes[0].layers[0].keys[0].elements[1].get());
    CHECK(b != nullptr && b->filters.size() == 2);
    CHECK(b->filters[0] == glow);
    CHECK(b->filters[1] == adjust);
    // Interpolation keeps types and blends values.
    Filter glow2 = glow;
    glow2.blurX = 20;
    const FilterList mid = lerpFilters({glow}, {glow2}, 0.5);
    CHECK_NEAR(mid[0].blurX, 16.0, 1e-9);
}

VX_TEST(real_fla_samples)
{
    // Optional: every .fla in $VERTEXA_FLA_SAMPLES must import cleanly.
    const QByteArray dir = qgetenv("VERTEXA_FLA_SAMPLES");
    if (dir.isEmpty()) return;
    const QStringList files = QDir(QString::fromLocal8Bit(dir)).entryList({QStringLiteral("*.fla")}, QDir::Files);
    for (const QString& f : files) {
        Document doc;
        io::ImportReport report;
        QString err;
        const bool ok = io::importFla(QDir(QString::fromLocal8Bit(dir)).filePath(f), doc, &report, &err);
        std::printf("  %s: %s, %d symbols, %d layers, %d keyframes, %d warnings\n", qPrintable(f),
                    ok ? "ok" : qPrintable(err), report.symbols, report.layers, report.keyframes,
                    int(report.warnings.size()));
        for (const QString& w : report.warnings) std::printf("    %s\n", qPrintable(w));
        CHECK(ok);
        CHECK(report.warnings.isEmpty());
        for (const Timeline& tl : doc.scenes) CHECK(!timelineBounds(doc, tl, 0).isEmpty());
    }
}

VX_TEST_MAIN()
