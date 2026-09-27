// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — importing Adobe Flash / Animate documents.
//
//  * Binary .fla (Flash 5 … CS4): an OLE2 container whose streams hold MFC
//    CArchive object trees (CPicPage → CPicLayer → CPicFrame → shapes and
//    symbol instances). The format is undocumented; see docs/FLA_FORMAT.md for
//    what Vertexa decodes and how it was verified.
//  * XFL (Flash CS5 and later, Animate): a ZIP (.fla) or a folder (.xfl) with
//    DOMDocument.xml, LIBRARY/*.xml and bin/ media.
#pragma once

#include "core/Document.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace vx::io {

enum class FlaFormat { Unknown, Binary, XflZip, XflFolder };

struct ImportReport {
    FlaFormat format = FlaFormat::Unknown;
    QString generator;      ///< e.g. "Flash CS4 (player 10)"
    int scenes = 0;
    int symbols = 0;
    int layers = 0;
    int keyframes = 0;
    int shapes = 0;
    int instances = 0;
    QStringList warnings;   ///< things that could not be imported exactly
};

FlaFormat detectFla(const QString& path);

/// Imports a .fla file (binary or XFL zip) or an .xfl folder / DOMDocument.xml.
bool importFla(const QString& path, Document& doc, ImportReport* report = nullptr, QString* error = nullptr);

/// XFL .fla (ZIP) from memory.
bool importXflZip(const QByteArray& zip, Document& doc, ImportReport* report = nullptr, QString* error = nullptr);

/// Binary (pre-CS5) .fla from memory.
bool importBinaryFla(const QByteArray& data, Document& doc, ImportReport* report = nullptr, QString* error = nullptr);

} // namespace vx::io
