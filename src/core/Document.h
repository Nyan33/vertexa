// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — document: stage settings, scenes and the symbol library.
// Documents have value semantics; copies share immutable elements, which
// makes whole-document undo snapshots cheap.
#pragma once

#include "Timeline.h"
#include "VectorBrush.h"

#include <map>
#include <string>
#include <vector>

namespace vx {

struct Symbol {
    std::string id;       ///< stable unique id (never shown)
    std::string name;     ///< library name
    std::string folder;   ///< library folder path ("" = root)
    SymbolType type = SymbolType::MovieClip;
    Timeline timeline;
};

struct Document {
    double width = 1280.0;
    double height = 720.0;
    double fps = 24.0;
    Color background = Color(255, 255, 255);

    std::vector<Timeline> scenes;
    std::vector<Symbol> symbols;
    /// Brushes made from artwork in this document (Paint Brush tool).
    std::vector<VectorBrushPreset> brushes;

    uint32_t nextLayerId = 1;
    uint64_t nextSymbolSerial = 1;

    /// A new document with one scene and one empty layer.
    static Document createDefault();

    const Symbol* symbol(const std::string& id) const;
    Symbol* symbol(const std::string& id);
    int symbolIndex(const std::string& id) const;
    const Symbol* symbolByName(const std::string& name) const;

    std::string newSymbolId();
    std::string uniqueSymbolName(const std::string& base) const;
    uint32_t newLayerId() { return nextLayerId++; }
    Layer makeLayer(const std::string& name);
    std::string uniqueLayerName(const Timeline& tl) const;

    /// Number of instances of a symbol anywhere in the document.
    int useCount(const std::string& symbolId) const;
    /// True if `symbolId` (transitively) contains an instance of `target`
    /// (used to prevent recursive symbols).
    bool symbolContains(const std::string& symbolId, const std::string& target) const;
};

/// Layer colours cycle like in Animate.
Color layerColorForIndex(int i);

} // namespace vx
