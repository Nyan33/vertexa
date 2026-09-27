// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — Brushes panel: Krita-like preset grid and brush engine editor
// (tip, spacing, sensor curves, jitter, texture), .gbr/.png tip import and
// user presets.
#pragma once

#include "../Editor.h"

#include <QScrollArea>

class QGridLayout;
class QLabel;
class QVBoxLayout;

namespace vx::app {

class PresetGrid : public QWidget {
    Q_OBJECT
public:
    explicit PresetGrid(Editor* editor, QWidget* parent = nullptr);
    void setPresets(std::vector<BrushPreset> presets);
    const std::vector<BrushPreset>& presets() const { return m_presets; }
    int heightForWidth(int w) const override;
    bool hasHeightForWidth() const override { return true; }
    QSize sizeHint() const override { return {240, heightForWidth(240)}; }

signals:
    void picked(const BrushPreset& p);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    QRectF tile(int i) const;
    Editor* m_ed;
    std::vector<BrushPreset> m_presets;
    std::vector<QImage> m_previews;
    int m_hover = -1;
};

class BrushPanel : public QScrollArea {
    Q_OBJECT
public:
    explicit BrushPanel(Editor* editor, QWidget* parent = nullptr);

private:
    void rebuildEditor();
    void refreshPreview();
    void loadUserPresets();
    void saveUserPresets();
    QGridLayout* section(const QString& title);
    void row(QGridLayout* g, const QString& label, QWidget* w);

    Editor* m_ed;
    QWidget* m_content = nullptr;
    QVBoxLayout* m_layout = nullptr;
    PresetGrid* m_grid = nullptr;
    QLabel* m_preview = nullptr;
    QWidget* m_editor = nullptr;
    QVBoxLayout* m_editorLayout = nullptr;
    std::vector<BrushPreset> m_user;
};

} // namespace vx::app
