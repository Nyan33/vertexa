// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — dialogs: Convert to Symbol, Document, Tablet, Export, Hotkeys.
#pragma once

#include "CrashHandler.h"
#include "Editor.h"

#include <QDialog>

class QAction;
class QUrl;
class QCheckBox;
class QComboBox;
class QLineEdit;

namespace vx::app {

class CurveEditor;
class HotNumber;
class Segmented;

class ConvertToSymbolDialog : public QDialog {
    Q_OBJECT
public:
    ConvertToSymbolDialog(const QString& defaultName, const std::vector<std::string>& folders = {},
                          QWidget* parent = nullptr);
    QString name() const;
    SymbolType type() const;
    int registration() const { return m_reg; }
    /// Library folder ("" for the root; a new path creates the folder).
    std::string folder() const;
    bool scale9() const;

private:
    QLineEdit* m_name;
    Segmented* m_type;
    QComboBox* m_folder;
    QCheckBox* m_scale9;
    int m_reg = 4;
};

class DocumentDialog : public QDialog {
    Q_OBJECT
public:
    DocumentDialog(const Document& d, QWidget* parent = nullptr);
    double width() const;
    double height() const;
    double fps() const;
    Color background() const { return m_bg; }

private:
    HotNumber *m_w, *m_h, *m_fps;
    Color m_bg;
};

/// Layer Properties (double-click a layer's icon): name, type, visibility,
/// lock, outline, opacity and blending. Opacity and blending show on the
/// stage while the dialog is open.
class LayerDialog : public QDialog {
    Q_OBJECT
public:
    LayerDialog(Editor* editor, int layerIndex, QWidget* parent = nullptr);
    ~LayerDialog() override;
    /// Opens the dialog and applies the result as one undo step.
    static void edit(Editor* editor, int layerIndex, QWidget* parent);

private:
    Layer values() const;
    void preview();

    Editor* m_ed;
    int m_index;
    Layer m_layer;
    QLineEdit* m_name;
    QComboBox* m_type;
    QCheckBox *m_visible, *m_locked, *m_outline;
    HotNumber* m_opacity;
    QComboBox* m_blend;
};

class TabletDialog : public QDialog {
    Q_OBJECT
public:
    TabletDialog(Editor* editor, QWidget* parent = nullptr);

protected:
    void tabletEvent(QTabletEvent* e) override;

private:
    Editor* m_ed;
    CurveEditor* m_curve;
    QWidget* m_readout;
    double m_pressure = 0, m_tiltX = 0, m_tiltY = 0, m_rotation = 0;
    QString m_device;
};

struct ExportOptions {
    double scale = 1.0;
    bool transparent = false;
    bool allFrames = true;
};

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    ExportDialog(const QString& title, bool allowTransparent, QWidget* parent = nullptr);
    ExportOptions options() const;

private:
    HotNumber* m_scale;
    QCheckBox* m_transparent;
    Segmented* m_range;
};

class HotkeysDialog : public QDialog {
    Q_OBJECT
public:
    HotkeysDialog(const QList<QAction*>& actions, QWidget* parent = nullptr);
};

/// Shown at start when the last run crashed or was killed: its report
/// (copy it, open the folder, file an issue) and restoring unsaved work.
class CrashDialog : public QDialog {
    Q_OBJECT
public:
    enum class Choice { Close, Restore, Discard };
    explicit CrashDialog(const crash::Session& session, QWidget* parent = nullptr);
    Choice choice() const { return m_choice; }
    /// New GitHub issue with the report filled in.
    static QUrl issueUrl(const QString& report, const QString& reportPath);

private:
    Choice m_choice = Choice::Close;
};

} // namespace vx::app
