// SPDX-License-Identifier: GPL-3.0-or-later
#include "Timeline.h"

#include <algorithm>

namespace vx {

int Layer::keyIndexAt(int frame) const
{
    if (frame < 0 || keys.empty() || frame >= keys.back().end()) return -1;
    auto it = std::upper_bound(keys.begin(), keys.end(), frame, [](int f, const Keyframe& k) { return f < k.start; });
    if (it == keys.begin()) return -1;
    return int(std::distance(keys.begin(), it)) - 1;
}

const Keyframe* Layer::keyAt(int frame) const
{
    const int i = keyIndexAt(frame);
    return i < 0 ? nullptr : &keys[i];
}

Keyframe* Layer::keyAt(int frame)
{
    const int i = keyIndexAt(frame);
    return i < 0 ? nullptr : &keys[i];
}

bool Layer::isKeyStart(int frame) const
{
    const Keyframe* k = keyAt(frame);
    return k && k->start == frame;
}

void Layer::normalize()
{
    keys.erase(std::remove_if(keys.begin(), keys.end(), [](const Keyframe& k) { return k.duration <= 0; }), keys.end());
    if (keys.empty()) {
        keys.push_back(Keyframe{});
        return;
    }
    std::stable_sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.start < b.start; });
    int pos = 0;
    for (Keyframe& k : keys) {
        k.start = pos;
        pos += k.duration;
    }
}

int Timeline::frameCount() const
{
    int n = 0;
    for (const Layer& l : layers)
        if (l.type != LayerType::Folder) n = std::max(n, l.length());
    return std::max(n, 1);
}

int Timeline::layerIndex(uint32_t id) const
{
    for (int i = 0; i < int(layers.size()); ++i)
        if (layers[i].id == id) return i;
    return -1;
}

const Layer* Timeline::layerById(uint32_t id) const
{
    const int i = layerIndex(id);
    return i < 0 ? nullptr : &layers[i];
}

int Timeline::depth(int index) const
{
    int d = 0;
    uint32_t p = layers[index].parentId;
    while (p != 0 && d < 32) {
        const Layer* l = layerById(p);
        if (!l) break;
        ++d;
        p = l->parentId;
    }
    return d;
}

bool Timeline::isCollapsed(int index) const
{
    uint32_t p = layers[index].parentId;
    int guard = 0;
    while (p != 0 && guard++ < 32) {
        const Layer* l = layerById(p);
        if (!l) break;
        if (!l->expanded) return true;
        p = l->parentId;
    }
    return false;
}

const Layer* Timeline::guideOf(int index) const
{
    const Layer* p = layerById(layers[index].parentId);
    return (p && p->type == LayerType::Guide) ? p : nullptr;
}

const Layer* Timeline::maskOf(int index) const
{
    const Layer* p = layerById(layers[index].parentId);
    return (p && p->type == LayerType::Mask) ? p : nullptr;
}

} // namespace vx
