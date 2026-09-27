// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — Library panel: symbols in folders with a live preview, drag to
// the stage or into folders.
#pragma once

#include "../Editor.h"

#include <QTreeWidget>
#include <QWidget>

#include <set>

class QLabel;

namespace vx::app {

class SectionTitle;

/// Library tree: folders and symbols. Symbols drag to the stage or into
/// folders.
class LibraryTree : public QTreeWidget {
    Q_OBJECT
public:
    LibraryTree(Editor* editor, QWidget* parent = nullptr);
    enum Kind { SymbolItem = 0, FolderItem = 1 };
    static constexpr int KindRole = Qt::UserRole + 1;

protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    QStringList mimeTypes() const override;
    void startDrag(Qt::DropActions actions) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    Editor* m_ed;
};

class SymbolPreview : public QWidget {
    Q_OBJECT
public:
    SymbolPreview(Editor* editor, QWidget* parent = nullptr);
    void setSymbol(const std::string& id);
    QSize sizeHint() const override { return {240, 150}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    Editor* m_ed;
    std::string m_id;
};

class LibraryPanel : public QWidget {
    Q_OBJECT
public:
    explicit LibraryPanel(Editor* editor, QWidget* parent = nullptr);

private:
    void refresh();
    std::string currentId() const;
    /// Folder of the current item (the folder itself or the symbol's folder).
    std::string currentFolder() const;
    void newFolder(const std::string& parent);
    void contextMenu(const QPoint& pos);
    Editor* m_ed;
    SectionTitle* m_title;
    SymbolPreview* m_preview;
    LibraryTree* m_tree;
    std::set<std::string> m_collapsed; ///< folders the user closed
    bool m_refreshing = false;
};

} // namespace vx::app
