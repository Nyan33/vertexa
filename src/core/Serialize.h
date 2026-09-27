// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — the .vtx document format (JSON). Coordinates are written with
// shortest round-trip precision, so saving and loading is lossless.
#pragma once

#include "Document.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace vx {

constexpr int kFormatVersion = 1;

QByteArray serializeDocument(const Document& doc, bool pretty = false);
bool deserializeDocument(const QByteArray& data, Document& doc, QString* error = nullptr);
bool saveDocument(const Document& doc, const QString& path, QString* error = nullptr);
bool loadDocument(const QString& path, Document& doc, QString* error = nullptr);

/// Clipboard payload: elements plus the symbols they reference.
QByteArray serializeClipboard(const Document& doc, const std::vector<ElementPtr>& elements);
/// Returns the pasted elements; referenced symbols missing from `doc` are added.
std::vector<ElementPtr> deserializeClipboard(const QByteArray& data, Document& doc);

QJsonObject vectorBrushToJson(const VectorBrushPreset& p);
VectorBrushPreset vectorBrushFromJson(const QJsonObject& o);

QJsonObject shapeGraphToJson(const ShapeGraph& g);
ShapeGraph shapeGraphFromJson(const QJsonObject& o);

} // namespace vx
