// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — brush tips and textures: procedurally generated built-ins and
// importers for Krita/GIMP compatible tip formats (.gbr, .png).
#pragma once

#include "core/Document.h"

#include <QImage>
#include <QString>

#include <string>
#include <vector>

namespace vx {

class BrushResources {
public:
    /// Tip or texture by id ("builtin:..." or a document image id).
    static GrayImagePtr image(const std::string& id, const Document* doc = nullptr);
    /// Names of the built-in tips and textures.
    static std::vector<std::string> builtinTips();
    static std::vector<std::string> builtinTextures();

    /// GIMP / Krita .gbr brush (versions 1 and 2, grey or RGBA).
    static GrayImagePtr loadGbr(const QByteArray& data, QString* name = nullptr, double* spacing = nullptr);
    /// Image tip: dark or opaque pixels paint (like Krita's predefined tips).
    static GrayImagePtr fromImage(const QImage& img, bool asTexture = false);
    /// Load a .gbr / .png / .jpg tip or texture from disk.
    static GrayImagePtr loadFile(const QString& path, bool asTexture, QString* error = nullptr);

    static QImage toQImage(const GrayImage& g);
};

} // namespace vx
