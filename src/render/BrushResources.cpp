// SPDX-License-Identifier: GPL-3.0-or-later
#include "BrushResources.h"

#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace vx {

namespace {

uint32_t hash2(int x, int y, uint32_t seed)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

double lattice(int x, int y, int period, uint32_t seed)
{
    x = ((x % period) + period) % period;
    y = ((y % period) + period) % period;
    return (hash2(x, y, seed) & 0xffffff) / double(0xffffff);
}

double smooth(double t) { return t * t * (3 - 2 * t); }

// Tileable value noise: `period` lattice cells across the tile.
double valueNoise(double u, double v, int period, uint32_t seed)
{
    const double x = u * period, y = v * period;
    const int xi = int(std::floor(x)), yi = int(std::floor(y));
    const double fx = smooth(x - xi), fy = smooth(y - yi);
    const double a = lattice(xi, yi, period, seed), b = lattice(xi + 1, yi, period, seed);
    const double c = lattice(xi, yi + 1, period, seed), d = lattice(xi + 1, yi + 1, period, seed);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

double fbm(double u, double v, int basePeriod, int octaves, uint32_t seed, double gain = 0.5)
{
    double sum = 0, amp = 1, norm = 0;
    int period = basePeriod;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * valueNoise(u, v, period, seed + uint32_t(o) * 101u);
        norm += amp;
        amp *= gain;
        period *= 2;
    }
    return sum / norm;
}

using Gen = double (*)(double u, double v); // u,v in [0,1)

GrayImagePtr generate(int size, Gen fn)
{
    auto img = std::make_shared<GrayImage>();
    img->width = img->height = size;
    img->pixels.resize(size_t(size) * size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const double v = std::clamp(fn((x + 0.5) / size, (y + 0.5) / size), 0.0, 1.0);
            img->pixels[size_t(y) * size + x] = uint8_t(std::lround(v * 255.0));
        }
    return img;
}

double contrast(double v, double c, double mid = 0.5) { return std::clamp((v - mid) * c + mid, 0.0, 1.0); }

// --- textures (tileable) ---
double texPaper(double u, double v) { return contrast(0.6 * fbm(u, v, 32, 4, 7) + 0.4 * fbm(u, v, 128, 2, 8), 2.4, 0.5); }
double texGrain(double u, double v) { return contrast(0.5 * fbm(u, v, 64, 3, 11, 0.6) + 0.5 * fbm(u, v, 128, 2, 12), 3.0, 0.5); }
double texCanvas(double u, double v)
{
    // Soft woven threads: two perpendicular sine families modulated by noise.
    const double w = 20.0;
    const double warp = 0.5 + 0.5 * std::sin(2 * kPi * (u * w + 0.15 * fbm(u, v, 8, 2, 3)));
    const double weft = 0.5 + 0.5 * std::sin(2 * kPi * (v * w + 0.15 * fbm(v, u, 8, 2, 4)));
    const double threads = 0.5 * (warp * warp + weft * weft);
    return contrast(0.45 * threads + 0.55 * fbm(u, v, 32, 4, 5), 1.9, 0.5);
}
double texPlain(double, double) { return 1.0; }

// --- tips (centered) ---
double tipChalk(double u, double v)
{
    const double x = u * 2 - 1, y = v * 2 - 1, r = std::hypot(x, y);
    const double edge = 0.82 + 0.18 * (fbm(u, v, 6, 3, 21) - 0.5) * 2;
    if (r > edge) return 0.0;
    const double body = std::clamp((edge - r) / 0.12, 0.0, 1.0);
    const double grain = fbm(u, v, 24, 3, 22);
    return body * std::clamp((grain - 0.32) * 3.0, 0.0, 1.0);
}
double tipCharcoal(double u, double v)
{
    const double x = (u * 2 - 1), y = (v * 2 - 1) / 0.45, r = std::hypot(x, y);
    const double edge = 0.85 + 0.25 * (fbm(u, v, 8, 3, 31) - 0.5);
    if (r > edge) return 0.0;
    const double body = std::clamp((edge - r) / 0.2, 0.0, 1.0);
    return body * std::clamp((fbm(u, v, 32, 2, 32) - 0.25) * 2.2, 0.0, 1.0);
}
double tipBlot(double u, double v)
{
    const double x = u * 2 - 1, y = v * 2 - 1;
    const double ang = std::atan2(y, x), r = std::hypot(x, y);
    const double wobble = 0.72 + 0.18 * std::sin(ang * 3 + 1.3) + 0.08 * std::sin(ang * 7 + 0.4);
    const double m = std::clamp((wobble - r) / 0.35, 0.0, 1.0);
    return m * (0.75 + 0.25 * fbm(u, v, 8, 3, 41));
}
double tipSpray(double u, double v)
{
    // Speckles: a dense set of tiny dots within the unit disc, denser at the centre.
    const double x = u * 2 - 1, y = v * 2 - 1, r = std::hypot(x, y);
    if (r > 1.0) return 0.0;
    const int cells = 48;
    const int cx = int(u * cells), cy = int(v * cells);
    double best = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const uint32_t h = hash2(cx + dx, cy + dy, 77);
            const double px = (cx + dx + (h & 0xff) / 255.0) / cells, py = (cy + dy + ((h >> 8) & 0xff) / 255.0) / cells;
            const double keep = ((h >> 16) & 0xff) / 255.0;
            const double pr = std::hypot(px * 2 - 1, py * 2 - 1);
            if (keep > 0.25 + 0.7 * pr) continue;
            const double d = std::hypot(u - px, v - py) * cells;
            best = std::max(best, std::clamp(1.0 - d / 0.45, 0.0, 1.0));
        }
    return best;
}
double tipRough(double u, double v)
{
    const double x = u * 2 - 1, y = v * 2 - 1, r = std::hypot(x, y);
    const double edge = 0.9 + 0.1 * (fbm(u, v, 10, 2, 51) - 0.5) * 2;
    return std::clamp((edge - r) / 0.08, 0.0, 1.0) * (0.6 + 0.4 * fbm(u, v, 40, 2, 52));
}

struct Builtin {
    const char* id;
    int size;
    Gen fn;
    bool texture;
};

constexpr Builtin kBuiltins[] = {
    {"builtin:paper", 512, texPaper, true},     {"builtin:grain", 512, texGrain, true},
    {"builtin:canvas", 512, texCanvas, true},   {"builtin:plain", 8, texPlain, true},
    {"builtin:chalk", 128, tipChalk, false},    {"builtin:charcoal", 128, tipCharcoal, false},
    {"builtin:blot", 128, tipBlot, false},      {"builtin:spray", 128, tipSpray, false},
    {"builtin:rough", 128, tipRough, false},
};

} // namespace

GrayImagePtr BrushResources::image(const std::string& id, const Document* doc)
{
    static std::mutex mutex;
    static std::map<std::string, GrayImagePtr> cache;
    if (id.rfind("builtin:", 0) == 0) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(id);
        if (it != cache.end()) return it->second;
        for (const Builtin& b : kBuiltins)
            if (id == b.id) {
                GrayImagePtr img = generate(b.size, b.fn);
                cache[id] = img;
                return img;
            }
        return nullptr;
    }
    if (doc) {
        auto it = doc->images.find(id);
        if (it != doc->images.end()) return it->second;
    }
    return nullptr;
}

std::vector<std::string> BrushResources::builtinTips()
{
    std::vector<std::string> out;
    for (const Builtin& b : kBuiltins)
        if (!b.texture) out.push_back(b.id);
    return out;
}

std::vector<std::string> BrushResources::builtinTextures()
{
    std::vector<std::string> out;
    for (const Builtin& b : kBuiltins)
        if (b.texture) out.push_back(b.id);
    return out;
}

GrayImagePtr BrushResources::loadGbr(const QByteArray& data, QString* name, double* spacing)
{
    if (data.size() < 20) return nullptr;
    auto u32 = [&](int off) { return qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data.constData()) + off); };
    const quint32 headerSize = u32(0), version = u32(4), width = u32(8), height = u32(12), bytes = u32(16);
    if (version != 1 && version != 2) return nullptr;
    if (width == 0 || height == 0 || width > 8192 || height > 8192) return nullptr;
    if (bytes != 1 && bytes != 4) return nullptr;
    int nameOffset = 20;
    if (version == 2) {
        if (data.size() < 28 || data.mid(20, 4) != "GIMP") return nullptr;
        if (spacing) *spacing = u32(24) / 100.0;
        nameOffset = 28;
    }
    if (headerSize < quint32(nameOffset) || qsizetype(headerSize) > data.size()) return nullptr;
    if (name) *name = QString::fromUtf8(data.mid(nameOffset, int(headerSize) - nameOffset)).remove(QChar(0));
    const qsizetype need = qsizetype(width) * height * bytes;
    if (data.size() < qsizetype(headerSize) + need) return nullptr;
    auto img = std::make_shared<GrayImage>();
    img->width = int(width);
    img->height = int(height);
    img->pixels.resize(size_t(width) * height);
    const auto* p = reinterpret_cast<const uchar*>(data.constData()) + headerSize;
    for (size_t i = 0; i < img->pixels.size(); ++i)
        img->pixels[i] = bytes == 1 ? p[i] : p[i * 4 + 3]; // RGBA brushes: alpha is the mask
    return img;
}

GrayImagePtr BrushResources::fromImage(const QImage& src, bool asTexture)
{
    if (src.isNull()) return nullptr;
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
    bool hasAlpha = false;
    for (int y = 0; y < img.height() && !hasAlpha; ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(line[x]) < 255) {
                hasAlpha = true;
                break;
            }
    }
    auto g = std::make_shared<GrayImage>();
    g->width = img.width();
    g->height = img.height();
    g->pixels.resize(size_t(g->width) * g->height);
    for (int y = 0; y < img.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = line[x];
            const int l = qGray(c);
            int v;
            if (asTexture) v = hasAlpha ? l * qAlpha(c) / 255 + (255 - qAlpha(c)) : l;
            else v = hasAlpha ? qAlpha(c) * (255 - l) / 255 : 255 - l;
            if (!asTexture && hasAlpha && l > 250) v = qAlpha(c); // white shapes with alpha
            g->pixels[size_t(y) * g->width + x] = uint8_t(std::clamp(v, 0, 255));
        }
    }
    return g;
}

GrayImagePtr BrushResources::loadFile(const QString& path, bool asTexture, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return nullptr;
    }
    const QByteArray data = f.readAll();
    if (QFileInfo(path).suffix().compare("gbr", Qt::CaseInsensitive) == 0) {
        GrayImagePtr g = loadGbr(data);
        if (!g && error) *error = QStringLiteral("Unsupported .gbr brush");
        return g;
    }
    QImage img;
    if (!img.loadFromData(data)) {
        if (error) *error = QStringLiteral("Unsupported image");
        return nullptr;
    }
    return fromImage(img, asTexture);
}

QImage BrushResources::toQImage(const GrayImage& g)
{
    QImage img(g.width, g.height, QImage::Format_Grayscale8);
    for (int y = 0; y < g.height; ++y) std::copy_n(g.pixels.data() + size_t(y) * g.width, g.width, img.scanLine(y));
    return img;
}

} // namespace vx
