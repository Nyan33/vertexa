// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — minimal ZIP reader (stored and deflated entries) for XFL .fla
// files, with a self-contained RFC 1951 inflater.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace vx::io {

/// Decompresses raw DEFLATE data. Returns false on corrupt input.
bool inflate(const QByteArray& in, QByteArray& out, qsizetype expectedSize = -1);

class ZipReader {
public:
    bool open(const QByteArray& data, QString* error = nullptr);
    QStringList files() const;
    bool contains(const QString& name) const;
    /// Contents of an entry (names compare case-insensitively, '\\' == '/').
    QByteArray read(const QString& name) const;

private:
    struct Entry {
        QString name;
        int method = 0;
        uint32_t compressed = 0, size = 0, offset = 0;
    };
    const Entry* find(const QString& name) const;
    QByteArray m_data;
    std::vector<Entry> m_entries;
};

} // namespace vx::io
