// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — context sensitive Properties panel: tool options, selected
// object (instance colour effects, blending, looping, transform), frame
// (labels, tweens, easing, shape hints), layer and document settings.
#pragma once

#include "../Editor.h"

#include <QScrollArea>

#include <functional>
#include <string>
#include <tuple>
#include <vector>

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
    /// What the panel shows of the current frame: the keyframe under the
    /// playhead and the selected elements as shown there (tweened ones are
    /// new objects on every frame). Moving the playhead rebuilds the panel
    /// only when these change.
    std::vector<const void*> frameDependencies() const;
    /// Everything the panel shows of the document. Edits that leave it alone
    /// (painting, moving things on other layers) do not rebuild the panel.
    struct Shown {
        std::tuple<double, double, double, uint32_t> document;
        std::vector<std::tuple<std::string, std::string, int, std::vector<double>>> symbols;
        std::vector<std::string> context;
        int layerIndex = -1;
        std::tuple<uint32_t, std::string, int, int, double> layer;
        std::tuple<int, int, std::string, int, int, ClassicTweenSettings, ShapeTweenSettings, std::vector<ShapeHint>> key;
        std::vector<const void*> selection;
        bool operator==(const Shown&) const = default;
    };
    Shown shown() const;
    /// Rebuilds only the Frame section (the playhead moved to another
    /// keyframe; tool options, layer and document stay as they are).
    void rebuildFrame();
    void buildFrameSection();
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
    bool m_pendingFrame = false;
    QWidget* m_frameHost = nullptr; ///< holds the Frame section when nothing is selected
    std::string m_shownBrush; ///< paint brush preset shown in the tool options
    double m_shownSize = 0.0;
    std::vector<const void*> m_frameDeps;
    Shown m_shown;
};

} // namespace vx::app
