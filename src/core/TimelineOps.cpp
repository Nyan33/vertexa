// SPDX-License-Identifier: GPL-3.0-or-later
#include "TimelineOps.h"
#include "Evaluate.h"

#include <algorithm>

namespace vx {

void extendTo(Layer& l, int frame)
{
    l.normalize();
    if (frame >= l.length()) l.keys.back().duration += frame + 1 - l.length();
}

int splitAt(Layer& l, int frame)
{
    l.normalize();
    extendTo(l, frame);
    const int i = l.keyIndexAt(frame);
    Keyframe& k = l.keys[i];
    if (k.start == frame) return i;
    Keyframe n = k;
    n.start = frame;
    n.duration = k.end() - frame;
    n.label.clear();
    n.hints.clear();
    k.duration = frame - k.start;
    l.keys.insert(l.keys.begin() + i + 1, std::move(n));
    return i + 1;
}

void insertFrames(Layer& l, int frame, int count)
{
    l.normalize();
    if (frame >= l.length()) {
        extendTo(l, frame + count - 1);
        return;
    }
    const int i = l.keyIndexAt(frame);
    l.keys[i].duration += count;
    l.normalize();
}

void removeFrames(Layer& l, int frame, int count)
{
    for (int c = 0; c < count; ++c) {
        l.normalize();
        if (frame >= l.length()) break;
        const int i = l.keyIndexAt(frame);
        if (l.keys[i].duration > 1) --l.keys[i].duration;
        else l.keys.erase(l.keys.begin() + i);
    }
    l.normalize();
}

int insertKeyframe(const Document& doc, Timeline& tl, int layerIndex, int frame, bool blank)
{
    if (layerIndex < 0 || layerIndex >= int(tl.layers.size()) || frame < 0) return -1;
    Layer& l = tl.layers[layerIndex];
    if (l.type == LayerType::Folder) return -1;
    l.normalize();
    if (frame >= l.length()) {
        std::vector<ElementPtr> content;
        if (!blank) content = bakeFrame(doc, tl, layerIndex, l.length() - 1);
        if (frame > 0) extendTo(l, frame - 1);
        Keyframe k;
        k.start = frame;
        k.duration = 1;
        k.elements = std::move(content);
        l.keys.push_back(std::move(k));
        l.normalize();
        return frame;
    }
    const int i = l.keyIndexAt(frame);
    if (l.keys[i].start == frame) {
        if (frame + 1 < l.keys[i].end()) ++frame;
        else return -1;
    }
    std::vector<ElementPtr> content;
    if (!blank) content = bakeFrame(doc, tl, layerIndex, frame);
    const int j = splitAt(l, frame);
    l.keys[j].elements = std::move(content);
    if (blank) l.keys[j].tween = TweenType::None;
    return frame;
}

bool inClassicTween(const Layer& l, int frame)
{
    const int ki = l.keyIndexAt(frame);
    if (ki < 0 || ki + 1 >= int(l.keys.size())) return false;
    const Keyframe& k = l.keys[ki];
    return k.tween == TweenType::Classic && frame > k.start && !l.keys[ki + 1].elements.empty();
}

bool clearKeyframe(Layer& l, int frame)
{
    l.normalize();
    const int i = l.keyIndexAt(frame);
    if (i < 0 || l.keys[i].start != frame) return false;
    if (i == 0) {
        l.keys[0].elements.clear();
        l.keys[0].tween = TweenType::None;
        return true;
    }
    l.keys[i - 1].duration += l.keys[i].duration;
    l.keys.erase(l.keys.begin() + i);
    return true;
}

void convertToKeyframes(const Document& doc, Timeline& tl, int layerIndex, int from, int to, bool blank)
{
    Layer& l = tl.layers[layerIndex];
    for (int f = std::max(0, from); f <= to; ++f) {
        if (f < l.length() && l.isKeyStart(f)) {
            if (blank) {
                Keyframe* k = l.keyAt(f);
                k->elements.clear();
                k->tween = TweenType::None;
            }
            continue;
        }
        insertKeyframe(doc, tl, layerIndex, f, blank);
    }
}

void clearFrames(Layer& l, int from, int to)
{
    l.normalize();
    if (from > to) std::swap(from, to);
    from = std::max(0, from);
    extendTo(l, to);
    splitAt(l, from);
    if (to + 1 < l.length()) splitAt(l, to + 1);
    auto first = std::find_if(l.keys.begin(), l.keys.end(), [&](const Keyframe& k) { return k.start >= from; });
    auto last = std::find_if(first, l.keys.end(), [&](const Keyframe& k) { return k.start > to; });
    const int idx = int(first - l.keys.begin());
    l.keys.erase(first, last);
    Keyframe blank;
    blank.start = from;
    blank.duration = to - from + 1;
    l.keys.insert(l.keys.begin() + idx, blank);
    l.normalize();
}

bool setTween(Layer& l, int frame, TweenType type)
{
    Keyframe* k = l.keyAt(frame);
    if (!k) return false;
    k->tween = type;
    return true;
}

void reverseFrames(Layer& l, int from, int to)
{
    l.normalize();
    if (from > to) std::swap(from, to);
    if (from >= l.length()) return;
    to = std::min(to, l.length() - 1);
    splitAt(l, from);
    if (to + 1 < l.length()) splitAt(l, to + 1);
    auto first = std::find_if(l.keys.begin(), l.keys.end(), [&](const Keyframe& k) { return k.start >= from; });
    auto last = std::find_if(first, l.keys.end(), [&](const Keyframe& k) { return k.start > to; });
    std::reverse(first, last);
    l.normalize();
}

std::vector<Keyframe> copyFrames(const Layer& l, int from, int to)
{
    std::vector<Keyframe> out;
    if (from > to) std::swap(from, to);
    for (const Keyframe& k : l.keys) {
        const int s = std::max(k.start, from), e = std::min(k.end(), to + 1);
        if (s >= e) continue;
        Keyframe c = k;
        c.start = s - from;
        c.duration = e - s;
        if (s != k.start) {
            c.label.clear();
            c.hints.clear();
        }
        out.push_back(std::move(c));
    }
    // Frames past the end of the layer are copied as blank frames.
    int covered = 0;
    for (const Keyframe& k : out) covered = std::max(covered, k.end());
    if (covered < to - from + 1) {
        Keyframe b;
        b.start = covered;
        b.duration = to - from + 1 - covered;
        out.push_back(b);
    }
    return out;
}

void pasteFrames(Layer& l, int at, const std::vector<Keyframe>& frames, bool replace)
{
    if (frames.empty()) return;
    l.normalize();
    int total = 0;
    for (const Keyframe& k : frames) total += k.duration;
    at = std::max(0, at);
    if (replace && at < l.length()) removeFrames(l, at, std::min(total, l.length() - at));
    int idx;
    if (at >= l.length()) {
        if (at > 0) extendTo(l, at - 1);
        // A fresh one-frame layer has a single blank keyframe we can replace.
        if (at == 0) l.keys.clear();
        idx = int(l.keys.size());
    } else {
        idx = splitAt(l, at);
    }
    std::vector<Keyframe> ins = frames;
    l.keys.insert(l.keys.begin() + idx, ins.begin(), ins.end());
    l.normalize();
}

void moveFrames(Layer& l, int from, int to, int delta)
{
    if (delta == 0) return;
    if (from > to) std::swap(from, to);
    const std::vector<Keyframe> frames = copyFrames(l, from, to);
    clearFrames(l, from, to);
    if (from > 0) clearKeyframe(l, from);
    pasteFrames(l, std::max(0, from + delta), frames, true);
}

namespace {

bool isAncestor(const Timeline& tl, uint32_t ancestor, const Layer& l)
{
    uint32_t p = l.parentId;
    for (int guard = 0; p != 0 && guard < 64; ++guard) {
        if (p == ancestor) return true;
        const Layer* pl = tl.layerById(p);
        if (!pl) break;
        p = pl->parentId;
    }
    return false;
}

bool isContainer(LayerType t) { return t == LayerType::Folder || t == LayerType::Mask || t == LayerType::Guide; }

} // namespace

int layerBlockSize(const Timeline& tl, int index)
{
    if (index < 0 || index >= int(tl.layers.size())) return 0;
    const uint32_t id = tl.layers[index].id;
    int n = 0;
    while (index + 1 + n < int(tl.layers.size()) && isAncestor(tl, id, tl.layers[index + 1 + n])) ++n;
    return n;
}

int moveLayer(Timeline& tl, int from, int before)
{
    const int count = int(tl.layers.size());
    if (from < 0 || from >= count) return -1;
    before = std::clamp(before, 0, count);
    const int n = 1 + layerBlockSize(tl, from);
    if (before >= from && before <= from + n) return -1; // onto itself

    const std::vector<Layer> block(tl.layers.begin() + from, tl.layers.begin() + from + n);
    tl.layers.erase(tl.layers.begin() + from, tl.layers.begin() + from + n);
    const int at = before > from ? before - n : before;
    tl.layers.insert(tl.layers.begin() + at, block.begin(), block.end());

    // The parent follows from the nearest row above that the panel shows.
    Layer& moved = tl.layers[at];
    int above = at - 1;
    while (above >= 0 && tl.isCollapsed(above)) --above;
    uint32_t parent = 0;
    if (above >= 0) {
        const Layer& a = tl.layers[above];
        const bool open = isContainer(a.type) && (a.type != LayerType::Folder || a.expanded);
        parent = open ? a.id : a.parentId;
    }
    const bool maskOrGuide = moved.type == LayerType::Mask || moved.type == LayerType::Guide;
    for (int guard = 0; parent != 0 && guard < 64; ++guard) {
        const Layer* p = tl.layerById(parent);
        if (!p) {
            parent = 0;
            break;
        }
        if (!maskOrGuide || p->type == LayerType::Folder) break;
        parent = p->parentId;
    }
    moved.parentId = parent;
    return at;
}

} // namespace vx
