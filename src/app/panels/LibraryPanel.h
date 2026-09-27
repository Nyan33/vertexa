// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — Library panel: symbols with a live preview, drag to stage.
#pragma once

#include "../Editor.h"

#include <QListWidget>
#include <QWidget>

class QLabel;

namespace vx::app {

class SectionTitle;

class LibraryList : public QListWidget {
    Q_OBJECT
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    QStringList mimeTypes() const override;
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
    Editor* m_ed;
    SectionTitle* m_title;
    SymbolPreview* m_preview;
    LibraryList* m_list;
    bool m_refreshing = false;
};

} // namespace vx::app
