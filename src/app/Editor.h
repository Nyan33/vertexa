// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — editor state: the document, the editing context (scene or
// symbol, possibly edited in place), playhead, current layer, selections,
// tool settings, preview documents for live feedback and undo/redo.
#pragma once

#include "core/Document.h"
#include "core/ShapeOps.h"
#include "geom/Outline.h"

#include <QObject>
#include <QString>

#include <functional>
#include <optional>

class QTimer;
class QUndoStack;

namespace vx::app {

enum class ToolId {
    Selection,
    Subselection,
    FreeTransform,
    Lasso,
    Pen,
    Line,
    Rectangle,
    Oval,
    PolyStar,
    Pencil,
    Brush,
    PaintBrush,
    Eraser,
    PaintBucket,
    InkBottle,
    Eyedropper,
    Hand,
    Zoom,
    Count
};

enum class PencilMode { Straighten, Smooth, Ink };
enum class GapSize { None, Small, Medium, Large };

struct ToolSettings {
    FillStyle fill = FillStyle::solid(Color(0x2F, 0x6B, 0xFF));
    StrokeStyle stroke;
    bool fillEnabled = true;
    bool strokeEnabled = true;
    bool objectDrawing = false;

    // Brush (Animate classic brush: paints fills)
    double brushSize = 14.0;
    TipShape brushShape = TipShape::Round;
    double brushAngle = 0.0; ///< degrees
    bool brushPressure = true;
    bool brushTilt = false;
    double brushMinSize = 0.08;
    double brushSmoothing = 50.0;
    PaintMode brushMode = PaintMode::Normal;

    // Eraser
    double eraserSize = 22.0;
    TipShape eraserShape = TipShape::Round;
    EraseMode eraseMode = EraseMode::Normal;
    bool eraserPressure = false;
    bool faucet = false;

    // Paint bucket
    GapSize gap = GapSize::Small;

    // Shapes
    double rectRadius = 0.0;
    int polySides = 5;
    bool polyStar = false;
    double starDepth = 0.5;

    // Pencil
    PencilMode pencilMode = PencilMode::Smooth;
    double pencilSmoothing = 50.0;

    // Paint brush (vector art, pattern, textured and scatter brushes)
    VectorBrushPreset paint;
    PaintMode paintMode = PaintMode::Normal;
    bool paintErase = false;

    // Tablet
    ResponseCurve pressureCurve;
    bool eraserTipSwitches = true;

    bool snap = true;

    StrokeStyle strokeStyle() const { return stroke; }
    double gapPixels() const;
};

struct FrameSelection {
    int layerFrom = -1, layerTo = -1;
    int frameFrom = 0, frameTo = 0;
    bool valid() const { return layerFrom >= 0 && layerTo >= layerFrom && frameTo >= frameFrom; }
    bool contains(int layer, int frame) const
    {
        return valid() && layer >= layerFrom && layer <= layerTo && frame >= frameFrom && frame <= frameTo;
    }
};

struct ContextEntry {
    std::string symbolId;
    bool inPlace = false;
    uint32_t parentLayerId = 0; ///< layer holding the instance (in place)
    int elementIndex = -1;      ///< index of the instance in its keyframe
    int parentFrame = 0;        ///< playhead of the parent when entering
    int parentLayer = 0;
    Affine matrix;              ///< symbol space -> parent timeline space
};

struct ElementRef {
    uint32_t layerId = 0;
    int index = -1;
    bool operator==(const ElementRef&) const = default;
};

/// Selection inside a merge shape (faces/edges or a cut region).
struct ShapePick {
    uint32_t layerId = 0;
    ShapeGraphPtr graph;          ///< the graph the topology indices refer to
    ShapeSelection sel;
    std::optional<Region> region; ///< marquee / lasso cut
    bool valid() const { return layerId != 0 && graph && (!sel.isEmpty() || region.has_value()); }
};

class Editor : public QObject {
    Q_OBJECT
public:
    explicit Editor(QObject* parent = nullptr);
    ~Editor() override;

    // --- document -----------------------------------------------------
    const Document& doc() const { return m_doc; }
    /// Document shown on stage (a live preview while a tool is dragging).
    const Document& displayDoc() const { return m_preview ? *m_preview : m_doc; }
    void setPreview(std::optional<Document> d);
    bool hasPreview() const { return m_preview.has_value(); }
    void setDocument(Document d, const QString& path);
    QString filePath() const { return m_path; }
    void setFilePath(const QString& p);
    bool isDirty() const;
    void markClean();
    /// Unsaved changes without an undo step (recovered work).
    void markDirty();
    QUndoStack* undoStack() const { return m_undo; }

    /// Apply `fn` to a copy of the document and record an undo step when
    /// it returns true.
    bool edit(const QString& label, const std::function<bool(Document&)>& fn);

    // --- context --------------------------------------------------------
    int scene() const { return m_scene; }
    bool inSymbol() const { return !m_stack.empty(); }
    const std::vector<ContextEntry>& contextStack() const { return m_stack; }
    const Timeline& timeline() const;
    const Timeline& timelineOf(const Document& d) const;
    Timeline& mutableTimeline(Document& d) const;
    /// Current timeline space -> stage (scene) space.
    Affine contextMatrix() const;
    /// Path of (layer id, element index) pairs locating the edited instance.
    std::vector<std::pair<uint32_t, int>> focusPath() const;
    int rootFrame() const;
    void enterSymbol(const std::string& symbolId);
    bool enterInstance(int layerIndex, int elementIndex);
    void exitContext(int levels = 1);
    void exitToScene();
    QString contextName() const;

    // --- time & layers ------------------------------------------------------
    int frame() const { return m_frame; }
    void setFrame(int f);
    int layerIndex() const { return m_layer; }
    void setLayerIndex(int i);
    const Layer* currentLayer() const;
    const FrameSelection& frameSelection() const { return m_frameSel; }
    void setFrameSelection(const FrameSelection& s);

    bool isPlaying() const { return m_playing; }
    void setPlaying(bool play);
    /// Loop playback (Control > Loop Playback): plays the loop range over
    /// and over; without it playback stops on the last frame.
    bool loopPlayback() const { return m_loop; }
    /// Turning the loop on with several frames selected loops those frames.
    void setLoopPlayback(bool on);
    /// Loop range, inclusive and clamped to the current timeline.
    int loopStart() const;
    int loopEnd() const;
    void setLoopRange(int from, int to);

    // Onion skin
    bool onionSkin = false;
    bool onionOutline = false;
    int onionBefore = 3;
    int onionAfter = 3;
    void setOnion(bool on, bool outline);
    /// Control > Enable Simple Buttons: buttons on the stage react to the
    /// pointer (Over / Down) and can't be selected.
    bool simpleButtons() const { return m_simpleButtons; }
    void setSimpleButtons(bool on);

    // --- selection -----------------------------------------------------------
    const std::vector<ElementRef>& selection() const { return m_selection; }
    void setSelection(std::vector<ElementRef> s);
    const ShapePick& shapePick() const { return m_shapePick; }
    void setShapePick(ShapePick p);
    void clearSelection();
    bool hasSelection() const { return !m_selection.empty() || m_shapePick.valid(); }
    /// Selected elements as shown at the current frame (tweened on the
    /// in-between frames of a classic tween).
    std::vector<ElementPtr> selectedElements() const;
    ElementPtr shownElement(const ElementRef& r) const;
    /// Bounds of the selection in timeline space.
    Rect selectionBounds() const;
    /// Transformation point of a selection other than one symbol, group or
    /// drawing object (those keep their own): moved with the Free Transform
    /// tool, follows transforms, forgotten when the selection changes.
    std::optional<Vec2> selectionPivot() const { return m_selectionPivot; }
    void setSelectionPivot(std::optional<Vec2> p);
    /// The merge shape of a layer at the current frame (may be null).
    ShapeGraphPtr mergeShape(int layerIndex) const;

    // --- editing helpers --------------------------------------------------------
    /// Keyframe to draw into on `layerIndex` at `frame` (the current frame
    /// when negative; creates a keyframe past the end of the layer). Returns
    /// nullptr with a reason if the layer can't be edited.
    Keyframe* editableKey(Document& d, int layerIndex, QString* why = nullptr, int frame = -1) const;
    /// Keyframe holding the selected elements of a layer at the current frame.
    /// On an in-between frame of a classic tween a keyframe with the tweened
    /// state is inserted first, so changes apply to what is shown.
    Keyframe* selectionKey(Document& d, int layerIndex) const;
    bool canEdit(int layerIndex, QString* why = nullptr) const;
    void notify(const QString& message);

    // --- tools ---------------------------------------------------------------------
    ToolId tool() const { return m_tool; }
    void setTool(ToolId t);
    ToolSettings& settings() { return m_settings; }
    const ToolSettings& settings() const { return m_settings; }
    void emitSettingsChanged() { emit settingsChanged(); }
    static QString toolName(ToolId t);

    // --- commands (EditorCommands.cpp) --------------------------------------
    void deleteSelection();
    void selectAll();
    void copySelection();
    void cutSelection();
    void paste(bool inPlace);
    void duplicateSelection();
    void nudge(double dx, double dy);
    void transformSelection(const Affine& m, const QString& label);
    void convertSelectionToSymbol(const QString& name, SymbolType type, int registration, const std::string& folder = {},
                                  bool scale9 = false);
    void breakApart();
    void groupSelection();
    void ungroupSelection();
    enum class Arrange { Front, Forward, Backward, Back };
    void arrange(Arrange a);
    enum class Combine { Union, Intersect, Punch, Crop };
    void combineObjects(Combine c);
    void applyFillToSelection(const FillStyle& f);
    void applyStrokeToSelection(const StrokeStyle& s);
    void setInstanceProperty(const std::function<void(InstanceElement&)>& fn, const QString& label);
    void setElementMatrix(int selIndex, const Affine& m);

    // frames
    void insertFrames();
    void removeFrames();
    void insertKeyframe(bool blank);
    void clearKeyframe();
    void convertToKeyframes(bool blank);
    void clearFrames();
    void createTween(TweenType t);
    void removeTween();
    void reverseFrames();
    void copyFrames(bool cut);
    void pasteFrames();
    void moveFrames(int layerIndex, int from, int to, int delta);
    void setKeyframeProperty(const std::function<void(Keyframe&)>& fn, const QString& label);
    void addShapeHint();
    void removeShapeHints();

    // layers
    void addLayer(LayerType type = LayerType::Normal);
    void addFolder();
    void deleteLayer();
    void renameLayer(int index, const QString& name);
    void setLayerProperty(int index, const std::function<void(Layer&)>& fn, const QString& label);
    /// Drag a layer (with its children) before the layer at `before`.
    void moveLayer(int from, int before);
    void toggleOthersLocked(int index);
    void toggleOthersHidden(int index);

    // symbols
    void newSymbol(const QString& name, SymbolType type, const std::string& folder = {}, bool scale9 = false);
    void duplicateSymbol(const std::string& id);
    void deleteSymbol(const std::string& id);
    void renameSymbol(const std::string& id, const QString& name);
    void setSymbolType(const std::string& id, SymbolType type);
    /// 9-slice scaling guides of a symbol (nothing disables 9-slice scaling).
    void setSymbolScale9(const std::string& id, std::optional<Rect> grid);
    /// Guides one third in from each side of the symbol's content.
    std::optional<Rect> defaultScale9(const std::string& id) const;
    // Library folders ("a/b" paths, "" is the root).
    void createLibraryFolder(const std::string& path);
    void moveSymbolToFolder(const std::string& id, const std::string& folder);
    void renameLibraryFolder(const std::string& path, const std::string& newName);
    /// Removes a folder; its symbols and subfolders move to its parent.
    void deleteLibraryFolder(const std::string& path);
    void placeSymbol(const std::string& id, Vec2 timelinePos);
    void setStageSettings(double w, double h, double fps, Color bg);

signals:
    void documentChanged();
    void previewChanged();
    void frameChanged(int frame);
    void layerChanged(int layer);
    void frameSelectionChanged();
    void selectionChanged();
    void toolChanged(vx::app::ToolId tool);
    void contextChanged();
    void settingsChanged();
    void playingChanged(bool playing);
    void onionChanged();
    void simpleButtonsChanged(bool on);
    void loopChanged();
    void message(const QString& text);
    void pathChanged();

private:
    struct State {
        int scene = 0;
        std::vector<ContextEntry> stack;
        int frame = 0;
        int layer = 0;
    };
    friend class SnapshotCommand;
    State state() const;
    void restore(const Document& d, const State& s);
    void validateState();
    void tick();
    Rect elementsBounds(const std::vector<ElementPtr>& els) const;

    Document m_doc;
    std::optional<Document> m_preview;
    QString m_path;
    QUndoStack* m_undo = nullptr;
    bool m_simpleButtons = false;
    int m_scene = 0;
    std::vector<ContextEntry> m_stack;
    int m_frame = 0;
    int m_layer = 0;
    FrameSelection m_frameSel;
    std::vector<ElementRef> m_selection;
    ShapePick m_shapePick;
    std::optional<Vec2> m_selectionPivot;
    ToolId m_tool = ToolId::Brush;
    ToolSettings m_settings;
    bool m_playing = false;
    bool m_loop = false;
    int m_loopStart = 0, m_loopEnd = -1; ///< -1: to the last frame
    QTimer* m_timer = nullptr;
    std::vector<Keyframe> m_frameClipboard;
    int m_pasteCount = 0;
};

} // namespace vx::app
