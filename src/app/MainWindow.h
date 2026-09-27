// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — main window: menus and Animate-compatible shortcuts, docks
// (tools, timeline, properties, library, color, brushes), file I/O and export.
#pragma once

#include "Editor.h"

#include <QHash>
#include <QMainWindow>

class QLabel;
class QMenu;

namespace vx::app {

class StageView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Editor* editor, QWidget* parent = nullptr);
    QAction* action(const QString& name) const { return m_actions.value(name); }
    bool openFile(const QString& path);
    StageView* stage() const { return m_stage; }

protected:
    void closeEvent(QCloseEvent*) override;

private:
    QAction* add(const QString& name, const QString& text, const QKeySequence& key, std::function<void()> fn,
                 const QString& icon = {});
    QAction* addCheck(const QString& name, const QString& text, const QKeySequence& key, bool on, std::function<void(bool)> fn);
    void createActions();
    void createMenus();
    void createDocks();
    QWidget* createStageArea();
    void updateTitle();
    void updateBreadcrumb();
    void rebuildRecentMenu();
    void addRecent(const QString& path);

    bool maybeSave();
    bool save();
    bool saveAs();
    void newDocument();
    void open();
    void exportPngSequence();
    void exportSvg();
    void exportVideo();
    void documentSettings();
    void convertToSymbol();
    void toggleEditSymbol();

    Editor* m_ed;
    StageView* m_stage = nullptr;
    QHash<QString, QAction*> m_actions;
    QList<QAction*> m_actionOrder;
    QWidget* m_breadcrumb = nullptr;
    QLabel* m_zoomLabel = nullptr;
    QLabel* m_coords = nullptr;
    QMenu* m_recentMenu = nullptr;
    QMenu* m_windowMenu = nullptr;
};

} // namespace vx::app
