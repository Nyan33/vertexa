// SPDX-License-Identifier: GPL-3.0-or-later
#include "FlaImport.h"
#include "Cfb.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace vx::io {

bool importXfl(const QString& path, Document& doc, ImportReport& report, QString* error);

FlaFormat detectFla(const QString& path)
{
    const QFileInfo fi(path);
    if (fi.isDir()) return QFileInfo::exists(QDir(path).filePath(QStringLiteral("DOMDocument.xml"))) ? FlaFormat::XflFolder
                                                                                                    : FlaFormat::Unknown;
    if (fi.fileName().compare(QLatin1String("DOMDocument.xml"), Qt::CaseInsensitive) == 0) return FlaFormat::XflFolder;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return FlaFormat::Unknown;
    const QByteArray head = f.read(8);
    if (head.startsWith("PK")) return FlaFormat::XflZip;
    if (CompoundFile::isCompoundFile(head + QByteArray(512, '\0'))) return FlaFormat::Binary;
    if (fi.suffix().compare(QLatin1String("xfl"), Qt::CaseInsensitive) == 0) return FlaFormat::XflFolder;
    return FlaFormat::Unknown;
}

bool importFla(const QString& path, Document& doc, ImportReport* reportOut, QString* error)
{
    ImportReport local;
    ImportReport& report = reportOut ? *reportOut : local;
    report = ImportReport{};
    const FlaFormat format = detectFla(path);
    report.format = format;
    switch (format) {
    case FlaFormat::Binary: {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error) *error = f.errorString();
            return false;
        }
        return importBinaryFla(f.readAll(), doc, &report, error);
    }
    case FlaFormat::XflZip:
    case FlaFormat::XflFolder: return importXfl(path, doc, report, error);
    case FlaFormat::Unknown: break;
    }
    if (error) *error = QStringLiteral("Not a Flash or Animate document");
    return false;
}

} // namespace vx::io
