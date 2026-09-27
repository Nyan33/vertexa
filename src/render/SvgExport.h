// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — SVG export of a single frame (vector shapes stay vector, texture
// paint is embedded as PNG).
#pragma once

#include "core/Document.h"

#include <QString>

namespace vx {

QString frameToSvg(const Document& doc, const Timeline& tl, int frame);
bool saveFrameSvg(const Document& doc, const Timeline& tl, int frame, const QString& path, QString* error = nullptr);

} // namespace vx
