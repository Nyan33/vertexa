// SPDX-License-Identifier: GPL-3.0-or-later
#include "Cfb.h"

#include <QtEndian>

namespace vx::io {

namespace {

constexpr uint32_t kFreeSect = 0xffffffff;
constexpr uint32_t kEndOfChain = 0xfffffffe;
constexpr uint32_t kNoStream = 0xffffffff;
constexpr unsigned char kMagic[8] = {0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1};

uint32_t u32(const QByteArray& d, qsizetype at)
{
    if (at < 0 || at + 4 > d.size()) return kEndOfChain;
    return qFromLittleEndian<uint32_t>(d.constData() + at);
}

uint16_t u16(const QByteArray& d, qsizetype at)
{
    if (at < 0 || at + 2 > d.size()) return 0;
    return qFromLittleEndian<uint16_t>(d.constData() + at);
}

} // namespace

bool CompoundFile::isCompoundFile(const QByteArray& data)
{
    return data.size() >= 512 && memcmp(data.constData(), kMagic, 8) == 0;
}

bool CompoundFile::open(const QByteArray& data, QString* error)
{
    auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    if (!isCompoundFile(data)) return fail(QStringLiteral("Not a compound file"));
    m_data = data;
    const uint16_t sectorShift = u16(data, 30);
    const uint16_t miniShift = u16(data, 32);
    if (sectorShift < 7 || sectorShift > 16 || miniShift > sectorShift) return fail(QStringLiteral("Bad sector size"));
    m_sectorSize = 1u << sectorShift;
    m_miniSectorSize = 1u << miniShift;
    const uint32_t numFatSectors = u32(data, 44);
    const uint32_t firstDirSector = u32(data, 48);
    m_miniCutoff = u32(data, 56);
    const uint32_t firstMiniFat = u32(data, 60);
    const uint32_t numMiniFat = u32(data, 64);
    uint32_t difatSector = u32(data, 68);
    const uint32_t numDifat = u32(data, 72);

    auto sectorOffset = [&](uint32_t s) -> qsizetype { return qsizetype(s + 1) * m_sectorSize; };

    // DIFAT: 109 entries in the header, then chained DIFAT sectors.
    std::vector<uint32_t> fatSectors;
    for (int i = 0; i < 109 && fatSectors.size() < numFatSectors; ++i) {
        const uint32_t s = u32(data, 76 + i * 4);
        if (s == kFreeSect) break;
        fatSectors.push_back(s);
    }
    const uint32_t perSector = m_sectorSize / 4;
    for (uint32_t n = 0; n < numDifat && difatSector < kEndOfChain; ++n) {
        const qsizetype base = sectorOffset(difatSector);
        for (uint32_t i = 0; i + 1 < perSector && fatSectors.size() < numFatSectors; ++i) {
            const uint32_t s = u32(data, base + i * 4);
            if (s != kFreeSect) fatSectors.push_back(s);
        }
        difatSector = u32(data, base + (perSector - 1) * 4);
    }
    m_fat.clear();
    for (uint32_t s : fatSectors) {
        const qsizetype base = sectorOffset(s);
        if (base + m_sectorSize > data.size()) return fail(QStringLiteral("FAT sector out of range"));
        for (uint32_t i = 0; i < perSector; ++i) m_fat.push_back(u32(data, base + i * 4));
    }

    // Directory.
    const QByteArray dir = readChain(firstDirSector, 0, false);
    m_entries.clear();
    for (qsizetype at = 0; at + 128 <= dir.size(); at += 128) {
        Entry e;
        const uint16_t nameLen = u16(dir, at + 64);
        if (nameLen >= 2 && nameLen <= 64)
            e.name = QString::fromUtf16(reinterpret_cast<const char16_t*>(dir.constData() + at), nameLen / 2 - 1);
        e.type = uint8_t(dir[at + 66]);
        e.left = u32(dir, at + 68);
        e.right = u32(dir, at + 72);
        e.child = u32(dir, at + 76);
        e.start = u32(dir, at + 116);
        e.size = u32(dir, at + 120);
        if (m_sectorSize > 512) e.size |= uint64_t(u32(dir, at + 124)) << 32;
        m_entries.push_back(e);
    }
    if (m_entries.empty() || m_entries[0].type != 5) return fail(QStringLiteral("Missing root entry"));

    // Mini stream (held by the root entry) and the mini FAT.
    m_miniStream = readChain(m_entries[0].start, m_entries[0].size, false);
    m_miniFat.clear();
    if (numMiniFat > 0 && firstMiniFat < kEndOfChain) {
        const QByteArray mf = readChain(firstMiniFat, 0, false);
        for (qsizetype at = 0; at + 4 <= mf.size(); at += 4) m_miniFat.push_back(u32(mf, at));
    }

    m_paths.clear();
    collect(m_entries[0].child, QString(), 0);
    return true;
}

QByteArray CompoundFile::readChain(uint32_t start, uint64_t size, bool mini) const
{
    QByteArray out;
    const std::vector<uint32_t>& fat = mini ? m_miniFat : m_fat;
    const uint32_t sec = mini ? m_miniSectorSize : m_sectorSize;
    uint32_t s = start;
    size_t guard = 0;
    while (s < kEndOfChain && guard++ <= fat.size()) {
        if (mini) {
            const qsizetype at = qsizetype(s) * sec;
            if (at + sec > m_miniStream.size()) break;
            out.append(m_miniStream.constData() + at, sec);
        } else {
            const qsizetype at = qsizetype(s + 1) * sec;
            if (at >= m_data.size()) break;
            out.append(m_data.constData() + at, std::min<qsizetype>(sec, m_data.size() - at));
        }
        if (size > 0 && uint64_t(out.size()) >= size) break;
        if (s >= fat.size()) break;
        s = fat[s];
    }
    if (size > 0 && uint64_t(out.size()) > size) out.truncate(qsizetype(size));
    return out;
}

void CompoundFile::collect(uint32_t id, const QString& prefix, int depth)
{
    if (id == kNoStream || id >= m_entries.size() || depth > 64) return;
    const Entry& e = m_entries[id];
    collect(e.left, prefix, depth + 1);
    const QString path = prefix.isEmpty() ? e.name : prefix + '/' + e.name;
    if (e.type == 2) m_paths.emplace_back(path, id);
    else if (e.type == 1) collect(e.child, path, depth + 1);
    collect(e.right, prefix, depth + 1);
}

QStringList CompoundFile::streams() const
{
    QStringList out;
    for (const auto& [path, id] : m_paths) out << path;
    return out;
}

bool CompoundFile::hasStream(const QString& name) const
{
    for (const auto& [path, id] : m_paths)
        if (path == name) return true;
    return false;
}

QByteArray CompoundFile::stream(const QString& name) const
{
    for (const auto& [path, id] : m_paths) {
        if (path != name) continue;
        const Entry& e = m_entries[id];
        if (e.size == 0) return {};
        const bool mini = e.size < m_miniCutoff;
        return readChain(e.start, e.size, mini);
    }
    return {};
}

} // namespace vx::io
