// SPDX-License-Identifier: GPL-3.0-or-later
#include "RhiRenderer.h"
#include "Blend.h"
#include "GpuGeometry.h"
#include "QtConvert.h"

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <QElapsedTimer>
#include <QFile>
#include <QOffscreenSurface>
// Vulkan needs its headers at build time; without them the backend is left out.
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#define VX_RHI_VULKAN 1
#include <QVulkanInstance>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

namespace vx {

using gpu::deviceRect;
using gpu::maxScale;

namespace {

/// The uniform block of every shader (std140, see shaders/quad.vert).
struct Uniforms {
    float xform[16];
    float viewport[4];
    float color[4];
    float toGradient[16];
    float paint[4];
    float src[4];
    float dst[4];
    float extra[4];
    float mult[4];
    float add[4];
    float paper[4];
};

void setMatrix(float* out, const Affine& m)
{
    // Column-major mat4 of the 2D affine map.
    const float v[16] = {float(m.a), float(m.b), 0, 0, float(m.c), float(m.d), 0, 0, 0, 0, 1, 0, float(m.tx), float(m.ty), 0, 1};
    std::memcpy(out, v, sizeof v);
}

void set4(float* out, double a, double b, double c, double d)
{
    out[0] = float(a);
    out[1] = float(b);
    out[2] = float(c);
    out[3] = float(d);
}

QShader loadShader(const char* name)
{
    QFile f(QStringLiteral(":/vertexa/shaders/%1.qsb").arg(QLatin1String(name)));
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

quint32 aligned(quint32 v, quint32 a) { return (v + a - 1) / a * a; }

/// Per-frame space in dynamic buffers (uniforms, transient vertices). Every
/// draw gets its own slice, so passes recorded earlier in the frame keep
/// their data; the slices are uploaded before the pass that first needs them.
class Arena {
public:
    Arena(QRhiBuffer::UsageFlag usage, quint32 blockSize) : m_usage(usage), m_blockSize(blockSize) {}

    void setRhi(QRhi* rhi) { m_rhi = rhi; }

    /// Copies `data` into the frame's space: (buffer, offset).
    std::pair<QRhiBuffer*, quint32> add(const void* data, quint32 size, quint32 align)
    {
        for (;;) {
            if (m_current < m_blocks.size()) {
                Block& b = m_blocks[m_current];
                const quint32 at = aligned(b.used, align);
                if (at + size <= b.cpu.size()) {
                    std::memcpy(b.cpu.data() + at, data, size);
                    b.used = at + size;
                    return {b.buf.get(), at};
                }
                ++m_current;
                continue;
            }
            Block b;
            const quint32 bytes = std::max(m_blockSize, aligned(size, 256));
            b.buf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, m_usage, bytes));
            if (!b.buf->create()) return {nullptr, 0};
            b.cpu.resize(bytes);
            m_blocks.push_back(std::move(b));
        }
    }

    void upload(QRhiResourceUpdateBatch* u)
    {
        for (Block& b : m_blocks) {
            if (b.used > b.uploaded) u->updateDynamicBuffer(b.buf.get(), b.uploaded, b.used - b.uploaded, b.cpu.data() + b.uploaded);
            b.uploaded = b.used;
        }
    }

    void reset()
    {
        for (Block& b : m_blocks) b.used = b.uploaded = 0;
        m_current = 0;
        // Keep a couple of blocks around; big frames get new ones again.
        while (m_blocks.size() > 4) m_blocks.pop_back();
    }

    void clear() { m_blocks.clear(); }

private:
    struct Block {
        std::unique_ptr<QRhiBuffer> buf;
        std::vector<char> cpu;
        quint32 used = 0, uploaded = 0;
    };
    QRhi* m_rhi = nullptr;
    QRhiBuffer::UsageFlag m_usage;
    quint32 m_blockSize;
    std::vector<Block> m_blocks;
    size_t m_current = 0;
};

/// A multisampled render target resolving into a texture.
struct Target {
    QSize size;
    std::unique_ptr<QRhiTexture> tex; ///< resolved pixels (the colour attachment without MSAA)
    std::unique_ptr<QRhiRenderBuffer> ms;
    std::unique_ptr<QRhiTextureRenderTarget> rt;
    std::unique_ptr<QRhiRenderPassDescriptor> rp;
};
using TargetPtr = std::shared_ptr<Target>;
using TexturePtr = std::shared_ptr<QRhiTexture>;

/// One recorded draw.
struct Draw {
    QRhiGraphicsPipeline* pipeline = nullptr;
    QRhiBuffer* ubo = nullptr;
    quint32 uboOffset = 0;
    QRhiBuffer* vbuf = nullptr;
    quint32 vOffset = 0;
    quint32 vertices = 0;
    QRhiTexture* tex1 = nullptr;
    QRhiTexture* tex2 = nullptr;
    QRect scissor; ///< device pixels
};

} // namespace

class RhiSurface;

// --- engine ---------------------------------------------------------------------------------

struct RhiRenderer::Impl {
    QRhi* rhi = nullptr;
    std::unique_ptr<QRhi> ownedRhi;
#ifdef VX_RHI_VULKAN
    std::unique_ptr<QVulkanInstance> vulkan;
#endif
    std::unique_ptr<QOffscreenSurface> fallbackSurface;
    bool valid = false;
    bool software = false;
    int samples = 1;
    QString backend, device;
    double lastSubmitMs = 0;

    QShader vs, fsStencil, fsFill, fsComposite, fsMask, fsCt, fsCopy, fsDisplay;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiTexture> dummy;
    std::unique_ptr<QRhiBuffer> layoutUbo;
    std::unique_ptr<QRhiShaderResourceBindings> layout;
    TargetPtr templateTarget;
    std::unique_ptr<QRhiGraphicsPipeline> pStencil, pCover, pNormal, pAlpha, pErase, pReplace, pMask, pCt, pCopy, pDisplay;
    QVector<quint32> displayFormat;
    int displaySamples = 0;

    // Frame state.
    QRhiCommandBuffer* cb = nullptr;
    QRhiResourceUpdateBatch* pending = nullptr;
    Arena ubo{QRhiBuffer::UniformBuffer, 65536};
    Arena vbo{QRhiBuffer::VertexBuffer, 1 << 20};
    quint32 uboStride = 256;
    std::vector<std::unique_ptr<QRhiTexture>> lutAtlases;
    int lutRow = 0;
    static constexpr int kLutRows = 64;
    struct SrbKey {
        QRhiBuffer* ubo;
        QRhiTexture* t1;
        QRhiTexture* t2;
        bool operator==(const SrbKey&) const = default;
    };
    struct SrbHash {
        size_t operator()(const SrbKey& k) const
        {
            return std::hash<const void*>{}(k.ubo) ^ (std::hash<const void*>{}(k.t1) << 1) ^ (std::hash<const void*>{}(k.t2) << 2);
        }
    };
    std::unordered_map<SrbKey, QRhiShaderResourceBindings*, SrbHash> srbs;
    TargetPtr frame; ///< the frame shown by present()

    // Render targets, reused as soon as nothing refers to them any more.
    std::shared_ptr<std::vector<std::unique_ptr<Target>>> freeTargets = std::make_shared<std::vector<std::unique_ptr<Target>>>();
    std::map<std::pair<int, int>, std::unique_ptr<QRhiRenderBuffer>> depthStencil;

    // Shape geometry kept in GPU buffers while its render data lives.
    struct Part {
        std::shared_ptr<QRhiBuffer> vbuf; ///< shared with the draws recorded from it
        quint32 vertices = 0;
        Rect bounds;
        bool cosmetic = false;
    };
    struct Geometry {
        std::weak_ptr<const ShapeRenderData> owner;
        std::vector<Part> fills, strokes;
        size_t bytes = 0;
        uint64_t used = 0;
    };
    /// Keyed by shape and scale bucket: instances of a symbol at different
    /// sizes keep their own flattening.
    struct GeometryKey {
        const ShapeRenderData* rd;
        int bucket;
        bool operator==(const GeometryKey&) const = default;
    };
    struct GeometryHash {
        size_t operator()(const GeometryKey& k) const { return std::hash<const void*>{}(k.rd) ^ (size_t(k.bucket) * 0x9e3779b97f4a7c15ull); }
    };
    std::unordered_map<GeometryKey, Geometry, GeometryHash> geometry;
    size_t geometryBytes = 0;
    uint64_t tick = 0;
    static constexpr size_t kGeometryBudget = size_t(256) << 20;

    ~Impl()
    {
        if (!rhi) return;
        if (pending) pending->release();
        frame.reset();
        templateTarget.reset();
        freeTargets->clear();
        for (auto& [key, srb] : srbs) delete srb;
        srbs.clear();
        geometry.clear();
        ubo.clear();
        vbo.clear();
        lutAtlases.clear();
        depthStencil.clear();
        for (auto* p : {&pStencil, &pCover, &pNormal, &pAlpha, &pErase, &pReplace, &pMask, &pCt, &pCopy, &pDisplay}) p->reset();
        layout.reset();
        layoutUbo.reset();
        dummy.reset();
        sampler.reset();
        ownedRhi.reset();
    }

    bool init()
    {
        if (!rhi) return false;
        const std::pair<QShader*, const char*> shaders[] = {{&vs, "quad.vert"},         {&fsStencil, "stencil.frag"},
                                                            {&fsFill, "fill.frag"},     {&fsComposite, "composite.frag"},
                                                            {&fsMask, "mask.frag"},     {&fsCt, "colortransform.frag"},
                                                            {&fsCopy, "copy.frag"},     {&fsDisplay, "display.frag"}};
        for (const auto& [shader, name] : shaders) {
            *shader = loadShader(name);
            if (!shader->isValid()) return false;
        }
        backend = QString::fromLatin1(rhi->backendName());
        const QRhiDriverInfo info = rhi->driverInfo();
        device = QStringLiteral("%1 — %2").arg(backend, QString::fromUtf8(info.deviceName));
        software = info.deviceType == QRhiDriverInfo::CpuDevice || gpu::isSoftwareDevice(QString::fromUtf8(info.deviceName));
        samples = 1;
        if (rhi->isFeatureSupported(QRhi::MultisampleRenderBuffer))
            for (int s : rhi->supportedSampleCounts())
                if (s <= 8) samples = std::max(samples, s);
        uboStride = aligned(sizeof(Uniforms), quint32(rhi->ubufAlignment()));
        ubo.setRhi(rhi);
        vbo.setRhi(rhi);

        sampler.reset(rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                      QRhiSampler::ClampToEdge));
        dummy.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
        layoutUbo.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, uboStride));
        if (!sampler->create() || !dummy->create() || !layoutUbo->create()) return false;
        QImage clear(1, 1, QImage::Format_RGBA8888_Premultiplied);
        clear.fill(0);
        updates()->uploadTexture(dummy.get(), clear);
        layout.reset(makeSrb(layoutUbo.get(), dummy.get(), dummy.get()));
        if (!layout) return false;

        templateTarget = acquire(QSize(16, 16));
        if (!templateTarget) return false;
        QRhiRenderPassDescriptor* rp = templateTarget->rp.get();
        using P = QRhiGraphicsPipeline;
        P::StencilOpState count;
        count.passOp = P::IncrementAndWrap;
        P::StencilOpState uncount = count;
        uncount.passOp = P::DecrementAndWrap;
        P::StencilOpState cover;
        cover.failOp = cover.depthFailOp = cover.passOp = P::StencilZero;
        cover.compareOp = P::NotEqual;
        pStencil.reset(makePipeline(fsStencil, Blend::NoColor, rp, samples, &count, &uncount));
        pCover.reset(makePipeline(fsFill, Blend::Over, rp, samples, &cover, &cover));
        pNormal.reset(makePipeline(fsComposite, Blend::Over, rp, samples));
        pAlpha.reset(makePipeline(fsComposite, Blend::DestIn, rp, samples));
        pErase.reset(makePipeline(fsComposite, Blend::DestOut, rp, samples));
        pReplace.reset(makePipeline(fsComposite, Blend::None, rp, samples));
        pMask.reset(makePipeline(fsMask, Blend::DestIn, rp, samples));
        pCt.reset(makePipeline(fsCt, Blend::None, rp, samples));
        pCopy.reset(makePipeline(fsCopy, Blend::None, rp, samples));
        for (auto* p : {&pStencil, &pCover, &pNormal, &pAlpha, &pErase, &pReplace, &pMask, &pCt, &pCopy})
            if (!*p) return false;
        valid = true;
        return true;
    }

    enum class Blend { None, Over, DestIn, DestOut, NoColor };

    QRhiGraphicsPipeline* makePipeline(const QShader& fs, Blend blend, QRhiRenderPassDescriptor* rp, int sampleCount,
                                       const QRhiGraphicsPipeline::StencilOpState* front = nullptr,
                                       const QRhiGraphicsPipeline::StencilOpState* back = nullptr, bool scissor = true)
    {
        using P = QRhiGraphicsPipeline;
        std::unique_ptr<P> ps(rhi->newGraphicsPipeline());
        ps->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
        QRhiVertexInputLayout input;
        input.setBindings({{2 * sizeof(float)}});
        input.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        ps->setVertexInputLayout(input);
        ps->setShaderResourceBindings(layout.get());
        ps->setRenderPassDescriptor(rp);
        ps->setSampleCount(sampleCount);
        ps->setCullMode(P::None);
        P::Flags flags;
        if (scissor) flags |= P::UsesScissor;
        if (front) {
            flags |= P::UsesStencilRef;
            ps->setStencilTest(true);
            ps->setStencilFront(*front);
            ps->setStencilBack(back ? *back : *front);
            ps->setStencilReadMask(0xff);
            ps->setStencilWriteMask(0xff);
        }
        ps->setFlags(flags);
        P::TargetBlend tb;
        switch (blend) {
        case Blend::None: break;
        case Blend::NoColor: tb.colorWrite = {}; break;
        case Blend::Over:
            tb.enable = true;
            tb.srcColor = tb.srcAlpha = P::One;
            tb.dstColor = tb.dstAlpha = P::OneMinusSrcAlpha;
            break;
        case Blend::DestIn:
            tb.enable = true;
            tb.srcColor = tb.srcAlpha = P::Zero;
            tb.dstColor = tb.dstAlpha = P::SrcAlpha;
            break;
        case Blend::DestOut:
            tb.enable = true;
            tb.srcColor = tb.srcAlpha = P::Zero;
            tb.dstColor = tb.dstAlpha = P::OneMinusSrcAlpha;
            break;
        }
        ps->setTargetBlends({tb});
        if (!ps->create()) return nullptr;
        return ps.release();
    }

    QRhiShaderResourceBindings* makeSrb(QRhiBuffer* buf, QRhiTexture* t1, QRhiTexture* t2)
    {
        std::unique_ptr<QRhiShaderResourceBindings> srb(rhi->newShaderResourceBindings());
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        srb->setBindings({QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, buf, sizeof(Uniforms)),
                          QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, t1, sampler.get()),
                          QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, t2, sampler.get())});
        if (!srb->create()) return nullptr;
        return srb.release();
    }

    QRhiShaderResourceBindings* srbFor(QRhiBuffer* buf, QRhiTexture* t1, QRhiTexture* t2)
    {
        const SrbKey key{buf, t1 ? t1 : dummy.get(), t2 ? t2 : dummy.get()};
        auto it = srbs.find(key);
        if (it != srbs.end()) return it->second;
        QRhiShaderResourceBindings* srb = makeSrb(key.ubo, key.t1, key.t2);
        srbs[key] = srb;
        return srb;
    }

    QRhiRenderBuffer* depthStencilFor(QSize size)
    {
        auto& ds = depthStencil[{size.width(), size.height()}];
        if (!ds) {
            ds.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, samples));
            if (!ds->create()) {
                ds.reset();
                return nullptr;
            }
        }
        return ds.get();
    }

    std::unique_ptr<Target> createTarget(QSize size)
    {
        auto t = std::make_unique<Target>();
        t->size = size;
        t->tex.reset(rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!t->tex->create()) return nullptr;
        QRhiColorAttachment color;
        if (samples > 1) {
            t->ms.reset(rhi->newRenderBuffer(QRhiRenderBuffer::Color, size, samples, {}, QRhiTexture::RGBA8));
            if (!t->ms->create()) return nullptr;
            color.setRenderBuffer(t->ms.get());
            color.setResolveTexture(t->tex.get());
        } else {
            color.setTexture(t->tex.get());
        }
        QRhiTextureRenderTargetDescription desc(color);
        QRhiRenderBuffer* ds = depthStencilFor(size);
        if (!ds) return nullptr;
        desc.setDepthStencilBuffer(ds);
        t->rt.reset(rhi->newTextureRenderTarget(desc));
        t->rp.reset(t->rt->newCompatibleRenderPassDescriptor());
        t->rt->setRenderPassDescriptor(t->rp.get());
        if (!t->rt->create()) return nullptr;
        return t;
    }

    /// A render target of `size`, back in the pool once nothing refers to it.
    /// Passes are recorded in order, so a target reused later in the frame
    /// is only overwritten after every pass that read it.
    TargetPtr acquire(QSize size)
    {
        size = size.expandedTo({1, 1});
        std::unique_ptr<Target> t;
        auto& pool = *freeTargets;
        for (auto it = pool.begin(); it != pool.end(); ++it)
            if ((*it)->size == size) {
                t = std::move(*it);
                pool.erase(it);
                break;
            }
        if (!t) t = createTarget(size);
        if (!t) return nullptr;
        std::weak_ptr<std::vector<std::unique_ptr<Target>>> home = freeTargets;
        return TargetPtr(t.release(), [home](Target* p) {
            if (auto pool = home.lock()) pool->emplace_back(p);
            else delete p;
        });
    }

    /// A texture that lives until the end of the frame's GPU work.
    TexturePtr texture(QSize size, QRhiTexture::Flags flags = {})
    {
        QRhiTexture* t = rhi->newTexture(QRhiTexture::RGBA8, size.expandedTo({1, 1}), 1, flags);
        if (!t->create()) {
            delete t;
            return nullptr;
        }
        return TexturePtr(t, [](QRhiTexture* p) { p->deleteLater(); });
    }

    TexturePtr upload(const QImage& image)
    {
        TexturePtr t = texture(image.size());
        if (t) updates()->uploadTexture(t.get(), image.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
        return t;
    }

    QRhiResourceUpdateBatch* updates()
    {
        if (!pending) pending = rhi->nextResourceUpdateBatch();
        return pending;
    }

    /// Everything to upload before the next pass.
    QRhiResourceUpdateBatch* takeUpdates()
    {
        QRhiResourceUpdateBatch* u = updates();
        pending = nullptr;
        ubo.upload(u);
        vbo.upload(u);
        return u;
    }

    std::pair<QRhiBuffer*, quint32> uniforms(const Uniforms& u) { return ubo.add(&u, sizeof u, uboStride); }
    std::pair<QRhiBuffer*, quint32> vertices(const float* xy, size_t floats)
    {
        return vbo.add(xy, quint32(floats * sizeof(float)), 8);
    }

    std::pair<QRhiBuffer*, quint32> quad(const QRect& r)
    {
        const float x0 = float(r.left()), y0 = float(r.top()), x1 = float(r.right() + 1), y1 = float(r.bottom() + 1);
        const float xy[12] = {x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1};
        return vertices(xy, 12);
    }

    /// Uniforms of a surface of `size` with an identity transform.
    Uniforms base(QSize size) const
    {
        Uniforms u{};
        setMatrix(u.xform, Affine{});
        // Device row r lands in framebuffer row r on every backend.
        const bool flipNdc = rhi->isYUpInNDC() && !rhi->isYUpInFramebuffer();
        set4(u.viewport, size.width(), size.height(), flipNdc ? -1.0 : 1.0, 0.0);
        return u;
    }

    QRhiScissor scissor(const QRect& r, QSize size) const
    {
        const int y = rhi->isYUpInFramebuffer() ? r.y() : size.height() - r.y() - r.height();
        return QRhiScissor(r.x(), y, r.width(), r.height());
    }

    /// A row of the gradient atlas holding `lut`: (atlas, row).
    std::pair<QRhiTexture*, int> lutRowFor(const std::array<uint8_t, 256 * 4>& lut)
    {
        const size_t atlas = size_t(lutRow / kLutRows);
        while (lutAtlases.size() <= atlas) {
            std::unique_ptr<QRhiTexture> t(rhi->newTexture(QRhiTexture::RGBA8, QSize(256, kLutRows)));
            if (!t->create()) return {nullptr, 0};
            lutAtlases.push_back(std::move(t));
        }
        const int row = lutRow % kLutRows;
        ++lutRow;
        QImage img(256, 1, QImage::Format_RGBA8888_Premultiplied);
        std::memcpy(img.bits(), lut.data(), lut.size());
        QRhiTextureSubresourceUploadDescription sub(img);
        sub.setDestinationTopLeft(QPoint(0, row));
        updates()->uploadTexture(lutAtlases[atlas].get(), QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, sub)));
        return {lutAtlases[atlas].get(), row};
    }

    Geometry& geometryFor(const std::shared_ptr<const ShapeRenderData>& rd, double scale)
    {
        const int bucket = gpu::scaleBucket(scale);
        Geometry& g = geometry[{rd.get(), bucket}];
        if (g.owner.lock() == rd) {
            g.used = ++tick;
            return g;
        }
        // New, or left by a shape that is gone and whose address was reused.
        geometryBytes -= std::min(geometryBytes, g.bytes);
        g = Geometry{};
        g.owner = rd;
        g.used = ++tick;
        const gpu::FlatShape flat = gpu::flattenShape(*rd, bucket);
        auto upload = [&](const gpu::FlatShape::Part& in) {
            Part p;
            p.bounds = in.bounds;
            p.cosmetic = in.cosmetic;
            p.vertices = quint32(in.tri.size() / 2);
            if (p.vertices == 0) return p;
            const quint32 bytes = quint32(in.tri.size() * sizeof(float));
            QRhiBuffer* buf = rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, bytes);
            if (!buf->create()) {
                delete buf;
                p.vertices = 0;
                return p;
            }
            p.vbuf = std::shared_ptr<QRhiBuffer>(buf, [](QRhiBuffer* b) { b->deleteLater(); });
            updates()->uploadStaticBuffer(buf, 0, bytes, in.tri.data());
            g.bytes += bytes;
            return p;
        };
        for (const auto& part : flat.fills) g.fills.push_back(upload(part));
        for (const auto& part : flat.strokes) g.strokes.push_back(upload(part));
        geometryBytes += g.bytes;
        return g;
    }

    void sweepGeometry()
    {
        for (auto it = geometry.begin(); it != geometry.end();) {
            if (it->second.owner.expired()) {
                geometryBytes -= std::min(geometryBytes, it->second.bytes);
                it = geometry.erase(it);
            } else {
                ++it;
            }
        }
        while (geometryBytes > kGeometryBudget && !geometry.empty()) {
            auto oldest = geometry.begin();
            for (auto it = geometry.begin(); it != geometry.end(); ++it)
                if (it->second.used < oldest->second.used) oldest = it;
            geometryBytes -= std::min(geometryBytes, oldest->second.bytes);
            geometry.erase(oldest);
        }
    }

    void execute(const Draw& d, QSize size)
    {
        if (!d.pipeline || !d.ubo || !d.vbuf || d.vertices == 0) return;
        cb->setGraphicsPipeline(d.pipeline);
        cb->setViewport(QRhiViewport(0, 0, float(size.width()), float(size.height())));
        const QRhiCommandBuffer::DynamicOffset offset(0, d.uboOffset);
        cb->setShaderResources(srbFor(d.ubo, d.tex1, d.tex2), 1, &offset);
        const QRhiCommandBuffer::VertexInput input(d.vbuf, d.vOffset);
        cb->setVertexInput(0, 1, &input);
        if (d.pipeline->flags().testFlag(QRhiGraphicsPipeline::UsesScissor)) cb->setScissor(scissor(d.scissor, size));
        if (d.pipeline->flags().testFlag(QRhiGraphicsPipeline::UsesStencilRef)) cb->setStencilRef(0);
        cb->draw(d.vertices);
    }

    void beginFrame(QRhiCommandBuffer* c)
    {
        cb = c;
        ubo.reset();
        vbo.reset();
        lutRow = 0;
    }

    void endFrame()
    {
        if (pending) {
            cb->resourceUpdate(pending);
            pending = nullptr;
        }
        for (auto& [key, srb] : srbs) srb->deleteLater();
        srbs.clear();
        sweepGeometry();
        // Keep a few spare targets (a full-size multisampled one is tens of MB).
        auto& pool = *freeTargets;
        while (pool.size() > 4) pool.erase(pool.begin());
        cb = nullptr;
    }

    bool ensureDisplayPipeline(QRhiRenderTarget* rt)
    {
        QRhiRenderPassDescriptor* rp = rt->renderPassDescriptor();
        const QVector<quint32> format = rp->serializedFormat();
        if (pDisplay && displayFormat == format && displaySamples == rt->sampleCount()) return true;
        pDisplay.reset(makePipeline(fsDisplay, Blend::None, rp, rt->sampleCount(), nullptr, nullptr, false));
        displayFormat = format;
        displaySamples = rt->sampleCount();
        return pDisplay != nullptr;
    }
};

// --- surfaces -------------------------------------------------------------------------------

/// Draws are recorded and become a render pass when the surface's pixels are
/// needed. A surface that must read its own pixels (a blend mode that needs
/// the destination, a colour transform) ends its pass, copies its texture
/// and starts a new pass from the copy.
class RhiSurface final : public Surface {
public:
    RhiSurface(RhiRenderer::Impl& e, QSize size) : m_e(e), m_size(size.expandedTo({1, 1})), m_target(e.acquire(m_size)) {}

    bool valid() const { return m_target != nullptr; }
    QSize size() const override { return m_size; }
    QRhiTexture* texture() const { return m_target ? m_target->tex.get() : nullptr; }
    const TargetPtr& target() const { return m_target; }

    void drawShape(const std::shared_ptr<const ShapeRenderData>& rd, const Affine& m, const ColorTransform& ct,
                   const QRect& clip) override
    {
        if (!rd || rd->isEmpty() || !valid()) return;
        const QRect box = clip.intersected(rect());
        if (box.isEmpty()) return;
        if (!rd->bounds.isEmpty() && !deviceRect(m.mapRect(rd->bounds)).intersects(box)) return;
        RhiRenderer::Impl::Geometry& g = m_e.geometryFor(rd, maxScale(m));
        for (size_t i = 0; i < rd->fills.size() && i < g.fills.size(); ++i) {
            const auto& fp = rd->fills[i];
            const auto& part = g.fills[i];
            if (!part.vbuf) continue;
            m_keep.push_back(part.vbuf);
            fill(part.vbuf.get(), 0, part.vertices, m, deviceRect(m.mapRect(part.bounds)).intersected(box),
                 gpu::Paint(ct.isIdentity() ? fp.style : fp.style.withColorTransform(ct), m), box);
        }
        for (size_t i = 0; i < rd->strokes.size() && i < g.strokes.size(); ++i) {
            const auto& sp = rd->strokes[i];
            const auto& part = g.strokes[i];
            const gpu::Paint paint(ct.isIdentity() ? sp.style.paint : sp.style.paint.withColorTransform(ct), m);
            if (!part.cosmetic) {
                if (part.vbuf) m_keep.push_back(part.vbuf);
                if (part.vbuf) fill(part.vbuf.get(), 0, part.vertices, m, deviceRect(m.mapRect(part.bounds)).intersected(box), paint, box);
                continue;
            }
            // Cosmetic strokes keep their width on screen: stroked in device space.
            std::vector<float> tri;
            Rect bounds;
            gpu::strokeInDeviceSpace(sp, m, tri, bounds);
            fillTransient(tri, bounds, paint, box);
        }
    }

    void drawOutline(const ShapeRenderData& rd, const Affine& m, const QColor& color, const QRect& clip) override
    {
        if (rd.isEmpty() || !valid()) return;
        const QRect box = clip.intersected(rect());
        if (box.isEmpty()) return;
        std::vector<float> tri;
        Rect bounds;
        gpu::outlineInDeviceSpace(rd, m, tri, bounds);
        fillTransient(tri, bounds, gpu::Paint(FillStyle::solid(fromQColor(color)), m), box);
    }

    std::unique_ptr<Surface> makeLayer(QSize size) override { return std::make_unique<RhiSurface>(m_e, size); }
    std::unique_ptr<Surface> makeFilterLayer(QSize size) override { return std::make_unique<CpuSurface>(size); }

    void composite(Surface& layerSurface, QPoint at, BlendMode mode, double opacity) override
    {
        const QRect area = QRect(at, layerSurface.size()).intersected(rect());
        if (area.isEmpty() || opacity <= 0.0 || !valid()) return;
        QRhiTexture* src = nullptr;
        if (auto* cpu = dynamic_cast<CpuSurface*>(&layerSurface)) {
            TexturePtr t = m_e.upload(cpu->image());
            if (!t) return;
            src = t.get();
            m_keep.push_back(t);
        } else {
            auto& layer = static_cast<RhiSurface&>(layerSurface);
            if (!layer.valid()) return;
            layer.flush();
            src = layer.texture();
            m_keep.push_back(layer.target());
        }
        const bool simple = mode == BlendMode::Normal || mode == BlendMode::Layer || mode == BlendMode::Alpha || mode == BlendMode::Erase;
        QRhiTexture* dst = nullptr;
        if (!simple) {
            // The shader blends with a copy of what is here.
            TexturePtr copy = snapshot(true);
            if (!copy) return;
            dst = copy.get();
        }
        Uniforms u = m_e.base(m_size);
        set4(u.src, at.x(), at.y(), layerSurface.size().width(), layerSurface.size().height());
        set4(u.dst, m_size.width(), m_size.height(), int(mode), std::clamp(opacity, 0.0, 1.0));
        set4(u.extra, simple ? 0.0 : 1.0, 0, 0, 0);
        QRhiGraphicsPipeline* p = mode == BlendMode::Alpha ? m_e.pAlpha.get()
                                  : mode == BlendMode::Erase ? m_e.pErase.get()
                                  : simple                   ? m_e.pNormal.get()
                                                             : m_e.pReplace.get();
        add(p, u, m_e.quad(area), 6, area, src, dst);
    }

    void applyMask(Surface& maskSurface) override
    {
        auto& mask = static_cast<RhiSurface&>(maskSurface);
        if (!valid() || !mask.valid()) return;
        mask.flush();
        m_keep.push_back(mask.target());
        add(m_e.pMask.get(), m_e.base(m_size), m_e.quad(rect()), 6, rect(), mask.texture());
    }

    void applyFilters(const FilterList&, double) override
    {
        // Never reached: filtered instances are drawn on CPU surfaces
        // (makeFilterLayer), which filter themselves.
    }

    void applyColorTransform(const ColorTransform& ct) override
    {
        if (ct.isIdentity() || !valid()) return;
        TexturePtr copy = snapshot(false);
        if (!copy) return;
        Uniforms u = m_e.base(m_size);
        set4(u.mult, ct.rm, ct.gm, ct.bm, ct.am);
        set4(u.add, ct.ro / 255.0, ct.go / 255.0, ct.bo / 255.0, ct.ao / 255.0);
        add(m_e.pCt.get(), u, m_e.quad(rect()), 6, rect(), copy.get());
    }

    /// Records the pending draws as a render pass (the first pass clears).
    void flush()
    {
        if (!valid() || (m_passes > 0 && m_draws.empty())) return;
        QRhiCommandBuffer* cb = m_e.cb;
        cb->beginPass(m_target->rt.get(), QColor(0, 0, 0, 0), {1.0f, 0}, m_e.takeUpdates());
        for (const Draw& d : m_draws) m_e.execute(d, m_size);
        cb->endPass();
        ++m_passes;
        m_draws.clear();
        // Targets and textures read by the pass may be reused from now on.
        m_keep.clear();
    }

private:
    void add(QRhiGraphicsPipeline* p, const Uniforms& u, std::pair<QRhiBuffer*, quint32> verts, quint32 count, const QRect& scissor,
             QRhiTexture* t1 = nullptr, QRhiTexture* t2 = nullptr)
    {
        const auto [ubuf, uoff] = m_e.uniforms(u);
        if (!ubuf || !verts.first) return;
        m_draws.push_back({p, ubuf, uoff, verts.first, verts.second, count, t1, t2, scissor});
    }

    /// Stencil-then-cover: count the winding of the fans in the stencil
    /// buffer, then paint the covered samples once and reset the stencil.
    void fill(QRhiBuffer* vbuf, quint32 offset, quint32 count, const Affine& xform, const QRect& cover, const gpu::Paint& paint,
              const QRect& clip)
    {
        if (cover.isEmpty() || count == 0) return;
        Uniforms s = m_e.base(m_size);
        setMatrix(s.xform, xform);
        add(m_e.pStencil.get(), s, {vbuf, offset}, count, clip);
        Uniforms c = m_e.base(m_size);
        std::memcpy(c.color, paint.color, sizeof paint.color);
        QRhiTexture* lut = nullptr;
        if (paint.kind != 0) {
            const auto [atlas, row] = m_e.lutRowFor(paint.lut);
            lut = atlas;
            setMatrix(c.toGradient, paint.toGradient);
            set4(c.paint, paint.kind, paint.focal, paint.spread, row);
        }
        add(m_e.pCover.get(), c, m_e.quad(cover), 6, clip, lut);
    }

    void fillTransient(const std::vector<float>& tri, const Rect& deviceBounds, const gpu::Paint& paint, const QRect& clip)
    {
        if (tri.empty() || deviceBounds.isEmpty()) return;
        const auto [vbuf, offset] = m_e.vertices(tri.data(), tri.size());
        if (!vbuf) return;
        fill(vbuf, offset, quint32(tri.size() / 2), Affine{}, deviceRect(deviceBounds).intersected(clip), paint, clip);
    }

    /// Ends the pass and copies the pixels. With `restore`, the next pass
    /// starts from the copy (otherwise its first draw covers everything).
    TexturePtr snapshot(bool restore)
    {
        flush();
        TexturePtr copy = m_e.texture(m_size);
        if (!copy) return nullptr;
        m_e.updates()->copyTexture(copy.get(), m_target->tex.get());
        m_keep.push_back(copy);
        if (restore) add(m_e.pCopy.get(), m_e.base(m_size), m_e.quad(rect()), 6, rect(), copy.get());
        return copy;
    }

    RhiRenderer::Impl& m_e;
    QSize m_size;
    TargetPtr m_target;
    std::vector<Draw> m_draws;
    std::vector<std::shared_ptr<void>> m_keep; ///< what the pending draws read
    int m_passes = 0;
};

// --- renderer -------------------------------------------------------------------------------

RhiRenderer::RhiRenderer(QRhi* rhi) : d(std::make_unique<Impl>())
{
    d->rhi = rhi;
    d->init();
}

RhiRenderer::~RhiRenderer() = default;

std::unique_ptr<RhiRenderer> RhiRenderer::createOffscreen(const QString& apiName, QString* why)
{
    auto fail = [why](const QString& message) {
        if (why) *why = message;
        return nullptr;
    };
    std::unique_ptr<RhiRenderer> r(new RhiRenderer(nullptr));
    Impl& d = *r->d;
    QString api = apiName.toLower();
    if (api.isEmpty()) {
#if defined(Q_OS_WIN)
        api = "d3d11";
#elif defined(Q_OS_APPLE)
        api = "metal";
#else
        api = "vulkan";
#endif
    }
    QRhi* rhi = nullptr;
    if (api == "vulkan") {
#ifdef VX_RHI_VULKAN
        d.vulkan = std::make_unique<QVulkanInstance>();
        d.vulkan->setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
        if (!d.vulkan->create()) return fail("no Vulkan instance");
        QRhiVulkanInitParams params;
        params.inst = d.vulkan.get();
        rhi = QRhi::create(QRhi::Vulkan, &params);
#endif
    } else if (api == "opengl") {
#if QT_CONFIG(opengl)
        d.fallbackSurface.reset(QRhiGles2InitParams::newFallbackSurface());
        QRhiGles2InitParams params;
        params.fallbackSurface = d.fallbackSurface.get();
        rhi = QRhi::create(QRhi::OpenGLES2, &params);
#endif
    }
#if defined(Q_OS_WIN)
    else if (api == "d3d11") {
        QRhiD3D11InitParams params;
        rhi = QRhi::create(QRhi::D3D11, &params);
    } else if (api == "d3d12") {
        QRhiD3D12InitParams params;
        rhi = QRhi::create(QRhi::D3D12, &params);
    }
#endif
#if defined(Q_OS_APPLE)
    else if (api == "metal") {
        QRhiMetalInitParams params;
        rhi = QRhi::create(QRhi::Metal, &params);
    }
#endif
    if (!rhi) return fail(QStringLiteral("no %1 device").arg(api));
    d.ownedRhi.reset(rhi);
    d.rhi = rhi;
    if (!d.init()) return fail("shaders or render targets unavailable");
    return r;
}

bool RhiRenderer::isValid() const { return d->valid; }
QRhi* RhiRenderer::rhi() const { return d->rhi; }
QString RhiRenderer::backendName() const { return d->backend; }
QString RhiRenderer::deviceName() const { return d->device; }
bool RhiRenderer::isSoftware() const { return d->software; }
int RhiRenderer::samples() const { return d->samples; }
double RhiRenderer::lastSubmitMs() const { return d->lastSubmitMs; }

void RhiRenderer::present(QRhiCommandBuffer* cb, QRhiRenderTarget* rt, QSize size, const DrawFn& draw, bool redraw,
                          const Backdrop& backdrop)
{
    if (!d->valid || !cb || !rt) return;
    d->beginFrame(cb);
    size = size.expandedTo({1, 1});
    if (redraw || !d->frame || d->frame->size != size) {
        QElapsedTimer timer;
        timer.start();
        RhiSurface root(*d, size);
        if (root.valid()) {
            if (draw) draw(root);
            root.flush();
            d->frame = root.target();
        }
        d->lastSubmitMs = timer.nsecsElapsed() / 1e6;
    }
    const QSize px = rt->pixelSize();
    if (d->frame && d->ensureDisplayPipeline(rt)) {
        Uniforms u = d->base(px);
        u.viewport[3] = d->rhi->isYUpInFramebuffer() ? 1.0f : 0.0f; // window rows run the other way
        set4(u.extra, backdrop.dpr, backdrop.shadowAlpha, 0, 0);
        set4(u.mult, backdrop.stage.left(), backdrop.stage.top(), backdrop.stage.right(), backdrop.stage.bottom());
        set4(u.add, backdrop.margin.redF(), backdrop.margin.greenF(), backdrop.margin.blueF(), 1.0);
        set4(u.paper, backdrop.paper.redF(), backdrop.paper.greenF(), backdrop.paper.blueF(), 1.0);
        const auto [ubuf, uoff] = d->uniforms(u);
        const auto [vbuf, voff] = d->quad(QRect(QPoint(0, 0), px));
        cb->beginPass(rt, backdrop.margin, {1.0f, 0}, d->takeUpdates());
        if (ubuf && vbuf) {
            Draw show{d->pDisplay.get(), ubuf, uoff, vbuf, voff, 6, d->frame->tex.get(), nullptr, QRect(QPoint(0, 0), px)};
            d->execute(show, px);
        }
        cb->endPass();
    }
    d->endFrame();
}

void RhiRenderer::releaseFrame()
{
    d->frame.reset();
    d->freeTargets->clear();
    // Depth-stencil buffers of sizes no target uses any more (the template
    // target keeps its own).
    for (auto it = d->depthStencil.begin(); it != d->depthStencil.end();) {
        if (d->templateTarget && it->first == std::make_pair(d->templateTarget->size.width(), d->templateTarget->size.height())) ++it;
        else it = d->depthStencil.erase(it);
    }
}

QImage RhiRenderer::renderImage(QSize size, const DrawFn& draw)
{
    if (!d->valid || !d->ownedRhi) return {};
    QRhiCommandBuffer* cb = nullptr;
    if (d->rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) return {};
    d->beginFrame(cb);
    QRhiReadbackResult result;
    {
        RhiSurface root(*d, size);
        if (root.valid()) {
            QElapsedTimer timer;
            timer.start();
            if (draw) draw(root);
            root.flush();
            d->lastSubmitMs = timer.nsecsElapsed() / 1e6;
            QRhiResourceUpdateBatch* u = d->takeUpdates();
            u->readBackTexture(QRhiReadbackDescription(root.texture()), &result);
            cb->resourceUpdate(u);
        }
        d->endFrame();
        d->rhi->endOffscreenFrame();
    }
    if (result.data.isEmpty()) return {};
    const QImage img(reinterpret_cast<const uchar*>(result.data.constData()), result.pixelSize.width(), result.pixelSize.height(),
                     QImage::Format_RGBA8888_Premultiplied);
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

} // namespace vx
