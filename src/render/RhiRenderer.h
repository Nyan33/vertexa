// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — GPU rendering through Qt's RHI: Vulkan, Metal, Direct3D 11 / 12
// or OpenGL, whichever the platform offers.
//
// The same Renderer walks the document; an RhiSurface draws on the GPU:
//  * fills and strokes: stencil-then-cover into a multisampled target (the
//    non-zero winding counted in the stencil buffer, then one quad in the
//    fill's colour or gradient), as in the OpenGL renderer;
//  * isolated layers, masks, colour transforms and all blend modes: extra
//    targets composited by shaders that port the CPU formulas;
//  * filters: the filtered instance is drawn and filtered on the CPU and
//    uploaded, so nothing is ever read back from the GPU.
// Draws are recorded per surface and turned into render passes when the
// surface's pixels are needed (composited, shown or read). The frame is
// shown straight from its texture (StageView's canvas, a QRhiWidget), with
// the stage backdrop drawn by a shader.
#pragma once

#include "Renderer.h"

#include <QColor>
#include <QImage>
#include <QRectF>
#include <QString>

#include <functional>
#include <memory>

class QRhi;
class QRhiCommandBuffer;
class QRhiRenderTarget;

namespace vx {

class RhiRenderer {
public:
    /// Draws with `rhi`, owned by the caller (a QRhiWidget's).
    explicit RhiRenderer(QRhi* rhi);
    ~RhiRenderer();

    /// A renderer with its own QRhi and no window ("vulkan", "opengl",
    /// "metal", "d3d11", "d3d12"; empty picks the platform's usual one).
    static std::unique_ptr<RhiRenderer> createOffscreen(const QString& api, QString* why = nullptr);

    bool isValid() const;
    QRhi* rhi() const;
    QString backendName() const; ///< "Vulkan", "Metal", …
    QString deviceName() const;  ///< "Vulkan — NVIDIA GeForce …"
    /// Runs on the CPU (llvmpipe, lavapipe, WARP): slower than the CPU renderer.
    bool isSoftware() const;
    int samples() const;
    /// CPU time of the last frame: walking the document and recording draws.
    double lastSubmitMs() const;

    using DrawFn = std::function<void(Surface& target)>;

    /// The stage around the frame, in device pixels.
    struct Backdrop {
        QRectF stage;
        QColor margin, paper;
        double shadowAlpha = 16 / 255.0;
        double dpr = 1.0;
    };
    /// Inside a frame of the renderer's QRhi (a QRhiWidget's render()):
    /// when `redraw`, draws a new frame of `size` with `draw` on a
    /// transparent surface; then shows the latest frame on `rt`.
    void present(QRhiCommandBuffer* cb, QRhiRenderTarget* rt, QSize size, const DrawFn& draw, bool redraw,
                 const Backdrop& backdrop);
    /// Forgets the last frame and the GPU memory kept between frames.
    void releaseFrame();

    /// Offscreen renderers: draws a frame and reads it back (premultiplied
    /// ARGB32). Null on failure.
    QImage renderImage(QSize size, const DrawFn& draw);

    struct Impl;

private:
    std::unique_ptr<Impl> d;
};

} // namespace vx
