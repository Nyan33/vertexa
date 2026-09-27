// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — context sensitive Properties panel: tool options, selected
// object (instance colour effects, blending, looping, transform), frame
// (labels, tweens, easing, shape hints), layer and document settings.
#pragma once

#include "../Editor.h"

#include <QScrollArea>

#include <functional>

class QGridLayout;
class QVBoxLayout;

namespace vx::app {

class PropertiesPanel : public QScrollArea {
    Q_OBJECT
public:
    explicit PropertiesPanel(Editor* editor, QWidget* parent = nullptr);

private:
    void scheduleRebuild();
    void rebuild();
    QGridLayout* section(const QString& title, const QString& subtitle = {});
    void row(QGridLayout* g, const QString& label, QWidget* w);
    void buildToolOptions();
    void buildFillStroke(bool applyToSelection);
    void buildSelection();
    void buildFilters(const InstanceElement& in,
                      const std::function<void(std::function<void(InstanceElement&)>)>& previewFx);
    void buildFrame();
    void buildLayer();
    void buildDocument();
    void buildSymbol();

    Editor* m_ed;
    QWidget* m_content = nullptr;
    QVBoxLayout* m_layout = nullptr;
    bool m_pending = false;
    std::string m_shownBrush; ///< paint brush preset shown in the tool options
    double m_shownSize = 0.0;
};

} // namespace vx::app
