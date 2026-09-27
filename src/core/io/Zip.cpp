// SPDX-License-Identifier: GPL-3.0-or-later
#include "Zip.h"

#include <QtEndian>

#include <array>

namespace vx::io {

namespace {

// --- inflate (RFC 1951) -------------------------------------------------------------

struct BitReader {
    const uint8_t* p;
    qsizetype n;
    qsizetype pos = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    bool overrun = false;

    int bits(int need)
    {
        while (bitCnt < need) {
            uint32_t byte = 0;
            if (pos < n) byte = p[pos++];
            else overrun = true;
            bitBuf |= byte << bitCnt;
            bitCnt += 8;
        }
        const int v = int(bitBuf & ((1u << need) - 1));
        bitBuf >>= need;
        bitCnt -= need;
        return v;
    }
    void alignByte()
    {
        bitBuf = 0;
        bitCnt = 0;
    }
};

/// Canonical Huffman decoding table (counts per length + symbols by code).
struct Huffman {
    std::array<uint16_t, 16> count{};
    std::array<uint16_t, 320> symbol{};

    bool build(const uint8_t* lengths, int n)
    {
        count.fill(0);
        for (int i = 0; i < n; ++i) count[lengths[i]]++;
        count[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        std::array<uint16_t, 16> offs{};
        for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + count[len];
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offs[lengths[i]]++] = uint16_t(i);
        return true;
    }

    int decode(BitReader& br) const
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= br.bits(1);
            const int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflateBlock(BitReader& br, QByteArray& out, const Huffman& lit, const Huffman& dist)
{
    for (;;) {
        int sym = lit.decode(br);
        if (sym < 0 || br.overrun) return false;
        if (sym < 256) {
            out.append(char(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            const int len = kLenBase[sym] + br.bits(kLenExtra[sym]);
            const int ds = dist.decode(br);
            if (ds < 0 || ds >= 30) return false;
            const qsizetype d = kDistBase[ds] + br.bits(kDistExtra[ds]);
            if (d > out.size()) return false;
            const qsizetype from = out.size() - d;
            for (int i = 0; i < len; ++i) out.append(out.at(from + i));
        }
    }
}

} // namespace

bool inflate(const QByteArray& in, QByteArray& out, qsizetype expectedSize)
{
    out.clear();
    if (expectedSize > 0) out.reserve(expectedSize);
    BitReader br{reinterpret_cast<const uint8_t*>(in.constData()), in.size()};
    Huffman fixedLit, fixedDist;
    {
        uint8_t l[288];
        for (int i = 0; i < 144; ++i) l[i] = 8;
        for (int i = 144; i < 256; ++i) l[i] = 9;
        for (int i = 256; i < 280; ++i) l[i] = 7;
        for (int i = 280; i < 288; ++i) l[i] = 8;
        fixedLit.build(l, 288);
        uint8_t d[30];
        for (uint8_t& x : d) x = 5;
        fixedDist.build(d, 30);
    }
    bool last = false;
    while (!last) {
        last = br.bits(1);
        const int type = br.bits(2);
        if (type == 0) {
            br.alignByte();
            if (br.pos + 4 > br.n) return false;
            const uint16_t len = qFromLittleEndian<uint16_t>(br.p + br.pos);
            const uint16_t nlen = qFromLittleEndian<uint16_t>(br.p + br.pos + 2);
            br.pos += 4;
            if (uint16_t(~nlen) != len || br.pos + len > br.n) return false;
            out.append(reinterpret_cast<const char*>(br.p + br.pos), len);
            br.pos += len;
        } else if (type == 1) {
            if (!inflateBlock(br, out, fixedLit, fixedDist)) return false;
        } else if (type == 2) {
            const int hlit = br.bits(5) + 257, hdist = br.bits(5) + 1, hclen = br.bits(4) + 4;
            static constexpr int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {};
            for (int i = 0; i < hclen; ++i) cl[order[i]] = uint8_t(br.bits(3));
            Huffman clh;
            if (!clh.build(cl, 19)) return false;
            uint8_t lengths[320] = {};
            int i = 0;
            while (i < hlit + hdist) {
                const int sym = clh.decode(br);
                if (sym < 0 || br.overrun) return false;
                if (sym < 16) {
                    lengths[i++] = uint8_t(sym);
                } else {
                    int rep = 0;
                    uint8_t val = 0;
                    if (sym == 16) {
                        if (i == 0) return false;
                        val = lengths[i - 1];
                        rep = 3 + br.bits(2);
                    } else if (sym == 17) {
                        rep = 3 + br.bits(3);
                    } else {
                        rep = 11 + br.bits(7);
                    }
                    if (i + rep > hlit + hdist) return false;
                    while (rep--) lengths[i++] = val;
                }
            }
            Huffman lit, dist;
            if (!lit.build(lengths, hlit) || !dist.build(lengths + hlit, hdist)) return false;
            if (!inflateBlock(br, out, lit, dist)) return false;
        } else {
            return false;
        }
        if (br.overrun) return false;
    }
    return expectedSize < 0 || out.size() == expectedSize;
}

// --- ZIP ------------------------------------------------------------------------------

bool ZipReader::open(const QByteArray& data, QString* error)
{
    auto fail = [&](const QString& m) {
        if (error) *error = m;
        return false;
    };
    m_data = data;
    m_entries.clear();
    // End of central directory: search backwards (comment up to 64 KB).
    qsizetype eocd = -1;
    for (qsizetype i = data.size() - 22; i >= 0 && i >= data.size() - 22 - 65535; --i) {
        if (qFromLittleEndian<uint32_t>(data.constData() + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) return fail(QStringLiteral("Not a ZIP archive"));
    const uint16_t count = qFromLittleEndian<uint16_t>(data.constData() + eocd + 10);
    qsizetype at = qFromLittleEndian<uint32_t>(data.constData() + eocd + 16);
    for (int i = 0; i < count; ++i) {
        if (at + 46 > data.size() || qFromLittleEndian<uint32_t>(data.constData() + at) != 0x02014b50)
            return fail(QStringLiteral("Corrupt ZIP directory"));
        Entry e;
        const uint16_t flags = qFromLittleEndian<uint16_t>(data.constData() + at + 8);
        e.method = qFromLittleEndian<uint16_t>(data.constData() + at + 10);
        e.compressed = qFromLittleEndian<uint32_t>(data.constData() + at + 20);
        e.size = qFromLittleEndian<uint32_t>(data.constData() + at + 24);
        const uint16_t nameLen = qFromLittleEndian<uint16_t>(data.constData() + at + 28);
        const uint16_t extraLen = qFromLittleEndian<uint16_t>(data.constData() + at + 30);
        const uint16_t commentLen = qFromLittleEndian<uint16_t>(data.constData() + at + 32);
        e.offset = qFromLittleEndian<uint32_t>(data.constData() + at + 42);
        if (at + 46 + nameLen > data.size()) return fail(QStringLiteral("Corrupt ZIP directory"));
        const QByteArray raw = data.mid(at + 46, nameLen);
        e.name = (flags & 0x800) ? QString::fromUtf8(raw) : QString::fromLocal8Bit(raw);
        e.name.replace('\\', '/');
        m_entries.push_back(e);
        at += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

QStringList ZipReader::files() const
{
    QStringList out;
    for (const Entry& e : m_entries) out << e.name;
    return out;
}

const ZipReader::Entry* ZipReader::find(const QString& name) const
{
    QString n = name;
    n.replace('\\', '/');
    for (const Entry& e : m_entries)
        if (e.name.compare(n, Qt::CaseInsensitive) == 0) return &e;
    return nullptr;
}

bool ZipReader::contains(const QString& name) const { return find(name) != nullptr; }

QByteArray ZipReader::read(const QString& name) const
{
    const Entry* e = find(name);
    if (!e) return {};
    const qsizetype lh = e->offset;
    if (lh + 30 > m_data.size() || qFromLittleEndian<uint32_t>(m_data.constData() + lh) != 0x04034b50) return {};
    const uint16_t nameLen = qFromLittleEndian<uint16_t>(m_data.constData() + lh + 26);
    const uint16_t extraLen = qFromLittleEndian<uint16_t>(m_data.constData() + lh + 28);
    const qsizetype start = lh + 30 + nameLen + extraLen;
    if (start + e->compressed > m_data.size()) return {};
    const QByteArray raw = m_data.mid(start, e->compressed);
    if (e->method == 0) return raw;
    if (e->method != 8) return {};
    QByteArray out;
    if (!inflate(raw, out, e->size)) return {};
    return out;
}

} // namespace vx::io
