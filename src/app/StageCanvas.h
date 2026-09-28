// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — the stage drawn and shown on the GPU through Qt RHI.
//
// StageCanvas (a QRhiWidget) renders the frame with RhiRenderer straight
// into the window: nothing is read back to the CPU. Tool feedback
// (selection, handles, brush previews) is painted with QPainter by
// StageOverlay, a transparent widget on top. Both let mouse and tablet
// input through to the StageView underneath.
#pragma once

#ifdef VERTEXA_HAVE_RHI

#include "render/RhiRenderer.h"

#include <QRhiWidget>

#include <memory>

namespace vx::app {

class StageView;

/// The RHI backend the stage uses in this run, found by trying the
/// preferred ones (setting render/api, or VERTEXA_RHI) on a real device.
struct RhiChoice {
    bool usable = false;
    QRhiWidget::Api api = QRhiWidget::Api::Null;
    QString backend; ///< "Vulkan", "Metal", …
    QString device;  ///< "Vulkan — NVIDIA GeForce …"
    QString why;     ///< why no backend is usable
};
const RhiChoice& rhiChoice();

/// Backends the Graphics API menu offers on this platform: (id, label).
QList<QPair<QString, QString>> rhiApis();

class StageCanvas final : public QRhiWidget {
public:
    explicit StageCanvas(StageView* view);
    ~StageCanvas() override;

    /// Draws a new frame on the next render (the stage changed).
    void redraw();
    bool failed() const { return m_failed; }
    QString deviceName() const;
    /// CPU time of the last frame: walking the document and recording draws.
    double lastSubmitMs() const;

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    void fail();

    StageView* m_view;
    std::unique_ptr<RhiRenderer> m_renderer;
    bool m_redraw = true;
    bool m_failed = false;
};

class StageOverlay final : public QWidget {
public:
    explicit StageOverlay(StageView* view);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    StageView* m_view;
};

} // namespace vx::app

#endif // VERTEXA_HAVE_RHI
