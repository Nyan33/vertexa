// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — Brushes panel: vector brush presets (built-in, document and user),
// a per-kind brush editor and Animate-style art / pattern brushes made from
// the selection.
#pragma once

#include "../Editor.h"

#include <QScrollArea>

class QGridLayout;
class QLabel;
class QTimer;
class QVBoxLayout;

namespace vx::app {

/// Renders a sample stroke of a brush (an S curve with a pressure swell),
/// fitted into `size` logical pixels.
QImage vectorBrushPreview(const VectorBrushPreset& p, const FillStyle& paint, QSize size, qreal dpr);

class PresetGrid : public QWidget {
    Q_OBJECT
public:
    explicit PresetGrid(Editor* editor, QWidget* parent = nullptr);
    void setPresets(std::vector<VectorBrushPreset> presets);
    const std::vector<VectorBrushPreset>& presets() const { return m_presets; }
    void invalidatePreviews();
    int heightForWidth(int w) const override;
    bool hasHeightForWidth() const override { return true; }
    QSize sizeHint() const override { return {240, heightForWidth(240)}; }

signals:
    void picked(const VectorBrushPreset& p);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QRectF tile(int i) const;
    Editor* m_ed;
    std::vector<VectorBrushPreset> m_presets;
    std::vector<QImage> m_previews; ///< rendered lazily
    int m_hover = -1;
};

class BrushPanel : public QScrollArea {
    Q_OBJECT
public:
    explicit BrushPanel(Editor* editor, QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void rebuildEditor();
    void scheduleRebuild();
    void refreshPreview();
    void reloadPresets();
    void loadUserPresets();
    void saveUserPresets();
    void saveAsPreset();
    void createFromSelection(bool pattern);
    void deleteCurrent();
    void pick(const VectorBrushPreset& p);
    QGridLayout* section(const QString& title);
    void row(QGridLayout* g, const QString& label, QWidget* w);

    Editor* m_ed;
    QWidget* m_content = nullptr;
    QVBoxLayout* m_layout = nullptr;
    PresetGrid* m_grid = nullptr;
    QLabel* m_preview = nullptr;
    QWidget* m_editor = nullptr;
    QVBoxLayout* m_editorLayout = nullptr;
    QTimer* m_previewTimer = nullptr;
    std::vector<VectorBrushPreset> m_user;
    std::vector<std::string> m_docBrushes; ///< ids of the document's brushes shown
    std::string m_shownId;
    double m_shownSize = 0.0;
    bool m_rebuildPending = false;
};

} // namespace vx::app
