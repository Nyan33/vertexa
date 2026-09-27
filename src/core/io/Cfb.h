// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — reader for Microsoft Compound File Binary containers ([MS-CFB],
// "OLE2 structured storage"), the container of pre-CS5 .fla files.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace vx::io {

class CompoundFile {
public:
    /// Parses the container. Returns false (with a message) on a malformed file.
    bool open(const QByteArray& data, QString* error = nullptr);

    /// Names of all streams (paths joined with '/').
    QStringList streams() const;
    bool hasStream(const QString& name) const;
    /// Contents of a stream, or an empty array when missing.
    QByteArray stream(const QString& name) const;

    static bool isCompoundFile(const QByteArray& data);

private:
    struct Entry {
        QString name;
        int type = 0; ///< 1 storage, 2 stream, 5 root
        uint32_t left = 0xffffffff, right = 0xffffffff, child = 0xffffffff;
        uint32_t start = 0;
        uint64_t size = 0;
    };

    QByteArray readChain(uint32_t start, uint64_t size, bool mini) const;
    void collect(uint32_t id, const QString& prefix, int depth);

    QByteArray m_data;
    uint32_t m_sectorSize = 512;
    uint32_t m_miniSectorSize = 64;
    uint32_t m_miniCutoff = 4096;
    std::vector<uint32_t> m_fat;
    std::vector<uint32_t> m_miniFat;
    QByteArray m_miniStream;
    std::vector<Entry> m_entries;
    std::vector<std::pair<QString, uint32_t>> m_paths;
};

} // namespace vx::io
