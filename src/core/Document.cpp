// SPDX-License-Identifier: GPL-3.0-or-later
#include "Document.h"

#include <functional>
#include <set>

namespace vx {

Color layerColorForIndex(int i)
{
    static const Color palette[] = {
        Color(0x4F, 0x8C, 0xFF), Color(0xFF, 0x5A, 0x36), Color(0x2B, 0xD9, 0xA8), Color(0xB0, 0x6C, 0xFF),
        Color(0xFF, 0xC4, 0x2E), Color(0xFF, 0x4F, 0xA3), Color(0x36, 0xC8, 0xFF), Color(0x9B, 0xE0, 0x3F),
    };
    return palette[((i % 8) + 8) % 8];
}

Document Document::createDefault()
{
    Document d;
    Timeline scene;
    scene.name = "Scene 1";
    scene.layers.push_back(d.makeLayer("Layer 1"));
    d.scenes.push_back(std::move(scene));
    return d;
}

const Symbol* Document::symbol(const std::string& id) const
{
    for (const Symbol& s : symbols)
        if (s.id == id) return &s;
    return nullptr;
}

Symbol* Document::symbol(const std::string& id)
{
    for (Symbol& s : symbols)
        if (s.id == id) return &s;
    return nullptr;
}

int Document::symbolIndex(const std::string& id) const
{
    for (int i = 0; i < int(symbols.size()); ++i)
        if (symbols[i].id == id) return i;
    return -1;
}

const Symbol* Document::symbolByName(const std::string& name) const
{
    for (const Symbol& s : symbols)
        if (s.name == name) return &s;
    return nullptr;
}

std::string Document::newSymbolId()
{
    std::string id;
    do {
        id = "sym" + std::to_string(nextSymbolSerial++);
    } while (symbol(id));
    return id;
}

std::vector<std::string> Document::allLibraryFolders() const
{
    std::set<std::string> out;
    auto add = [&](std::string path) {
        while (!path.empty()) {
            out.insert(path);
            const size_t slash = path.rfind('/');
            path = slash == std::string::npos ? std::string() : path.substr(0, slash);
        }
    };
    for (const std::string& f : libraryFolders) add(f);
    for (const Symbol& s : symbols) add(s.folder);
    return {out.begin(), out.end()};
}

std::string Document::uniqueSymbolName(const std::string& base) const
{
    if (!symbolByName(base)) return base;
    for (int i = 2;; ++i) {
        const std::string n = base + " " + std::to_string(i);
        if (!symbolByName(n)) return n;
    }
}

Layer Document::makeLayer(const std::string& name)
{
    Layer l;
    l.id = newLayerId();
    l.name = name;
    l.color = layerColorForIndex(int(l.id) - 1);
    l.keys.push_back(Keyframe{});
    return l;
}

std::string Document::uniqueLayerName(const Timeline& tl) const
{
    std::set<std::string> names;
    for (const Layer& l : tl.layers) names.insert(l.name);
    for (int i = 1;; ++i) {
        const std::string n = "Layer " + std::to_string(i);
        if (!names.count(n)) return n;
    }
}

namespace {

void forEachElement(const std::vector<ElementPtr>& els, const std::function<void(const Element&)>& fn)
{
    for (const ElementPtr& e : els) {
        fn(*e);
        if (const GroupElement* g = asGroup(e)) forEachElement(g->children, fn);
    }
}

void forEachTimelineElement(const Timeline& tl, const std::function<void(const Element&)>& fn)
{
    for (const Layer& l : tl.layers)
        for (const Keyframe& k : l.keys) forEachElement(k.elements, fn);
}

} // namespace

int Document::useCount(const std::string& symbolId) const
{
    int n = 0;
    auto count = [&](const Element& e) {
        if (e.type() == ElementType::Instance && static_cast<const InstanceElement&>(e).symbolId == symbolId) ++n;
    };
    for (const Timeline& t : scenes) forEachTimelineElement(t, count);
    for (const Symbol& s : symbols) forEachTimelineElement(s.timeline, count);
    return n;
}

bool Document::symbolContains(const std::string& symbolId, const std::string& target) const
{
    std::set<std::string> visited;
    std::function<bool(const std::string&)> visit = [&](const std::string& id) -> bool {
        if (id == target) return true;
        if (!visited.insert(id).second) return false;
        const Symbol* s = symbol(id);
        if (!s) return false;
        bool found = false;
        forEachTimelineElement(s->timeline, [&](const Element& e) {
            if (!found && e.type() == ElementType::Instance)
                found = visit(static_cast<const InstanceElement&>(e).symbolId);
        });
        return found;
    };
    return visit(symbolId);
}

} // namespace vx
