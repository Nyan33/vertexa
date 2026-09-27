// SPDX-License-Identifier: GPL-3.0-or-later
#include "LibraryPanel.h"
#include "../Icons.h"
#include "../Theme.h"
#include "../Widgets.h"

#include "core/Evaluate.h"
#include "render/QtConvert.h"
#include "render/Renderer.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <map>

namespace vx::app {

using ui::Theme;

namespace {

constexpr const char* kSymbolMime = "application/x-vertexa-symbol";

QString leafName(const std::string& path)
{
    const size_t slash = path.rfind('/');
    return QString::fromStdString(slash == std::string::npos ? path : path.substr(slash + 1));
}

} // namespace

LibraryTree::LibraryTree(Editor* editor, QWidget* parent) : QTreeWidget(parent), m_ed(editor)
{
    setHeaderHidden(true);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);
    setIndentation(14);
    setAnimated(true);
}

QStringList LibraryTree::mimeTypes() const { return {kSymbolMime}; }

QMimeData* LibraryTree::mimeData(const QList<QTreeWidgetItem*>& items) const
{
    if (items.isEmpty() || items.front()->data(0, KindRole).toInt() != SymbolItem) return nullptr;
    auto* m = new QMimeData();
    m->setData(kSymbolMime, items.front()->data(0, Qt::UserRole).toByteArray());
    return m;
}

void LibraryTree::startDrag(Qt::DropActions)
{
    // Copy only: a move would make the view delete the dragged row itself.
    QTreeWidget::startDrag(Qt::CopyAction);
}

void LibraryTree::dragMoveEvent(QDragMoveEvent* e)
{
    if (e->source() == this && e->mimeData()->hasFormat(kSymbolMime)) e->acceptProposedAction();
    else e->ignore();
}

void LibraryTree::dropEvent(QDropEvent* e)
{
    // Moving a symbol into a folder (or out of all folders onto empty space).
    if (e->source() != this || !e->mimeData()->hasFormat(kSymbolMime)) {
        e->ignore();
        return;
    }
    const std::string id = e->mimeData()->data(kSymbolMime).toStdString();
    std::string folder;
    if (QTreeWidgetItem* it = itemAt(e->position().toPoint())) {
        if (it->data(0, KindRole).toInt() == FolderItem) folder = it->data(0, Qt::UserRole).toByteArray().toStdString();
        else if (const Symbol* s = m_ed->doc().symbol(it->data(0, Qt::UserRole).toByteArray().toStdString())) folder = s->folder;
    }
    e->setDropAction(Qt::CopyAction); // the tree is rebuilt from the document
    e->accept();
    m_ed->moveSymbolToFolder(id, folder);
}

SymbolPreview::SymbolPreview(Editor* editor, QWidget* parent) : QWidget(parent), m_ed(editor)
{
    setMinimumHeight(130);
    connect(m_ed, &Editor::documentChanged, this, qOverload<>(&QWidget::update));
}

void SymbolPreview::setSymbol(const std::string& id)
{
    m_id = id;
    update();
}

void SymbolPreview::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ui::Palette& pal = Theme::p();
    const QRectF r = QRectF(rect()).adjusted(2, 2, -2, -2);
    p.setPen(Qt::NoPen);
    p.setBrush(pal.bg2);
    p.drawRoundedRect(r, 12, 12);
    const Document& d = m_ed->doc();
    const Symbol* s = d.symbol(m_id);
    if (!s) {
        p.setPen(pal.text3);
        p.setFont(Theme::ui(12));
        p.drawText(r, Qt::AlignCenter, tr("Select a symbol"));
        return;
    }
    const Rect b = timelineBounds(d, s->timeline, 0);
    if (b.isEmpty()) {
        p.setPen(pal.text3);
        p.drawText(r, Qt::AlignCenter, tr("Empty symbol"));
        return;
    }
    const qreal dpr = devicePixelRatioF();
    const QRectF area = r.adjusted(14, 14, -14, -14);
    const double k = std::min(area.width() / std::max(1e-6, b.width()), area.height() / std::max(1e-6, b.height()));
    QImage img((r.size() * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    const Affine m = Affine::scale(dpr) * Affine::translate(area.center().x() - r.left(), area.center().y() - r.top()) *
                     Affine::scale(k) * Affine::translate(-b.center().x, -b.center().y);
    Renderer(d).render(img, s->timeline, 0, m);
    img.setDevicePixelRatio(dpr);
    p.drawImage(r.topLeft(), img);
    p.setFont(Theme::ui(10, QFont::Bold));
    p.setPen(pal.text3);
    const QString type = s->type == SymbolType::MovieClip ? tr("MOVIE CLIP") : s->type == SymbolType::Graphic ? tr("GRAPHIC") : tr("BUTTON");
    p.drawText(r.adjusted(12, 8, -12, -8), Qt::AlignTop | Qt::AlignLeft, type);
    p.drawText(r.adjusted(12, 8, -12, -8), Qt::AlignTop | Qt::AlignRight, tr("%n frame(s)", "", s->timeline.frameCount()));
}

LibraryPanel::LibraryPanel(Editor* editor, QWidget* parent) : QWidget(parent), m_ed(editor)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 10);
    lay->setSpacing(8);
    m_title = new SectionTitle(tr("Library"), this);
    lay->addWidget(m_title);
    m_preview = new SymbolPreview(m_ed, this);
    lay->addWidget(m_preview);
    m_tree = new LibraryTree(m_ed, this);
    m_tree->setIconSize(QSize(20, 20));
    m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    lay->addWidget(m_tree, 1);

    auto* buttons = new QHBoxLayout();
    auto mk = [this, buttons](const QString& icon, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(ui::icon(icon));
        b->setIconSize(QSize(18, 18));
        b->setToolTip(tip);
        buttons->addWidget(b);
        return b;
    };
    QToolButton* add = mk("plus", tr("New Symbol (Ctrl+F8)"));
    QToolButton* folder = mk("folder", tr("New Folder"));
    QToolButton* dup = mk("symbol", tr("Duplicate Symbol"));
    QToolButton* edit = mk("sliders", tr("Edit Symbol"));
    buttons->addStretch(1);
    QToolButton* del = mk("trash", tr("Delete"));
    lay->addLayout(buttons);

    connect(add, &QToolButton::clicked, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New Symbol"), tr("Name"), QLineEdit::Normal,
                                                   QString::fromStdString(m_ed->doc().uniqueSymbolName("Symbol 1")), &ok);
        if (ok) m_ed->newSymbol(name, SymbolType::MovieClip, currentFolder());
    });
    connect(folder, &QToolButton::clicked, this, [this]() { newFolder(currentFolder()); });
    connect(dup, &QToolButton::clicked, this, [this]() {
        if (!currentId().empty()) m_ed->duplicateSymbol(currentId());
    });
    connect(edit, &QToolButton::clicked, this, [this]() {
        if (!currentId().empty()) m_ed->enterSymbol(currentId());
    });
    connect(del, &QToolButton::clicked, this, [this]() {
        QTreeWidgetItem* it = m_tree->currentItem();
        if (!it) return;
        if (it->data(0, LibraryTree::KindRole).toInt() == LibraryTree::FolderItem) {
            m_ed->deleteLibraryFolder(it->data(0, Qt::UserRole).toByteArray().toStdString());
            return;
        }
        const std::string id = currentId();
        const int uses = m_ed->doc().useCount(id);
        if (uses > 0 &&
            QMessageBox::question(this, tr("Delete Symbol"), tr("The symbol is used %n time(s). Delete it and its instances?", "", uses)) !=
                QMessageBox::Yes)
            return;
        m_ed->deleteSymbol(id);
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this]() { m_preview->setSymbol(currentId()); });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it) {
        if (it->data(0, LibraryTree::KindRole).toInt() == LibraryTree::SymbolItem)
            m_ed->enterSymbol(it->data(0, Qt::UserRole).toByteArray().toStdString());
    });
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it) {
        if (m_refreshing) return;
        const std::string key = it->data(0, Qt::UserRole).toByteArray().toStdString();
        if (it->data(0, LibraryTree::KindRole).toInt() == LibraryTree::FolderItem)
            m_ed->renameLibraryFolder(key, it->text(0).trimmed().toStdString());
        else m_ed->renameSymbol(key, it->text(0));
    });
    auto remember = [this](QTreeWidgetItem* it, bool collapsed) {
        if (m_refreshing || it->data(0, LibraryTree::KindRole).toInt() != LibraryTree::FolderItem) return;
        const std::string path = it->data(0, Qt::UserRole).toByteArray().toStdString();
        if (collapsed) m_collapsed.insert(path);
        else m_collapsed.erase(path);
    };
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [remember](QTreeWidgetItem* it) { remember(it, true); });
    connect(m_tree, &QTreeWidget::itemExpanded, this, [remember](QTreeWidgetItem* it) { remember(it, false); });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &LibraryPanel::contextMenu);
    connect(m_ed, &Editor::documentChanged, this, &LibraryPanel::refresh);
    refresh();
}

void LibraryPanel::newFolder(const std::string& parent)
{
    const auto all = m_ed->doc().allLibraryFolders();
    auto taken = [&](const std::string& p) { return std::find(all.begin(), all.end(), p) != all.end(); };
    int n = 1;
    std::string path;
    do path = (parent.empty() ? std::string() : parent + "/") + "Folder " + std::to_string(n++);
    while (taken(path));
    m_ed->createLibraryFolder(path);
    // Start renaming the new folder right away.
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
        if ((*it)->data(0, LibraryTree::KindRole).toInt() == LibraryTree::FolderItem &&
            (*it)->data(0, Qt::UserRole).toByteArray().toStdString() == path) {
            m_tree->setCurrentItem(*it);
            m_tree->editItem(*it);
            break;
        }
}

void LibraryPanel::contextMenu(const QPoint& pos)
{
    QTreeWidgetItem* it = m_tree->itemAt(pos);
    QMenu menu(this);
    if (!it) {
        menu.addAction(tr("New Folder"), this, [this]() { newFolder({}); });
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
        return;
    }
    const std::string key = it->data(0, Qt::UserRole).toByteArray().toStdString();
    if (it->data(0, LibraryTree::KindRole).toInt() == LibraryTree::FolderItem) {
        menu.addAction(tr("New Folder Inside"), this, [this, key]() { newFolder(key); });
        menu.addAction(tr("Rename"), this, [this, it]() { m_tree->editItem(it); });
        menu.addSeparator();
        menu.addAction(tr("Delete Folder (keep contents)"), this, [this, key]() { m_ed->deleteLibraryFolder(key); });
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
        return;
    }
    const std::string id = key;
    const Symbol* sym = m_ed->doc().symbol(id);
    menu.addAction(tr("Edit"), this, [this, id]() { m_ed->enterSymbol(id); });
    menu.addAction(tr("Rename"), this, [this, it]() { m_tree->editItem(it); });
    menu.addAction(tr("Duplicate"), this, [this, id]() { m_ed->duplicateSymbol(id); });
    QMenu* type = menu.addMenu(tr("Behavior"));
    type->addAction(tr("Movie Clip"), this, [this, id]() { m_ed->setSymbolType(id, SymbolType::MovieClip); });
    type->addAction(tr("Graphic"), this, [this, id]() { m_ed->setSymbolType(id, SymbolType::Graphic); });
    type->addAction(tr("Button"), this, [this, id]() { m_ed->setSymbolType(id, SymbolType::Button); });
    QAction* slice = menu.addAction(tr("9-Slice Scaling"));
    slice->setCheckable(true);
    slice->setChecked(sym && sym->scale9);
    connect(slice, &QAction::toggled, this, [this, id](bool on) { m_ed->setSymbolScale9(id, on ? m_ed->defaultScale9(id) : std::nullopt); });
    QMenu* move = menu.addMenu(tr("Move to Folder"));
    move->addAction(tr("Library root"), this, [this, id]() { m_ed->moveSymbolToFolder(id, {}); });
    for (const std::string& f : m_ed->doc().allLibraryFolders())
        move->addAction(QString::fromStdString(f), this, [this, id, f]() { m_ed->moveSymbolToFolder(id, f); });
    move->addSeparator();
    move->addAction(tr("New Folder…"), this, [this, id]() {
        bool ok = false;
        const QString path = QInputDialog::getText(this, tr("Move to New Folder"), tr("Folder path"), QLineEdit::Normal, {}, &ok);
        QStringList parts = path.split('/', Qt::SkipEmptyParts);
        for (QString& p : parts) p = p.trimmed();
        parts.removeAll(QString());
        if (ok && !parts.isEmpty()) m_ed->moveSymbolToFolder(id, parts.join('/').toStdString());
    });
    menu.addSeparator();
    menu.addAction(tr("Delete"), this, [this, id]() { m_ed->deleteSymbol(id); });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

std::string LibraryPanel::currentId() const
{
    QTreeWidgetItem* it = m_tree->currentItem();
    if (!it || it->data(0, LibraryTree::KindRole).toInt() != LibraryTree::SymbolItem) return {};
    return it->data(0, Qt::UserRole).toByteArray().toStdString();
}

std::string LibraryPanel::currentFolder() const
{
    QTreeWidgetItem* it = m_tree->currentItem();
    if (!it) return {};
    const std::string key = it->data(0, Qt::UserRole).toByteArray().toStdString();
    if (it->data(0, LibraryTree::KindRole).toInt() == LibraryTree::FolderItem) return key;
    const Symbol* s = m_ed->doc().symbol(key);
    return s ? s->folder : std::string();
}

void LibraryPanel::refresh()
{
    m_refreshing = true;
    QTreeWidgetItem* cur = m_tree->currentItem();
    const int curKind = cur ? cur->data(0, LibraryTree::KindRole).toInt() : -1;
    const QByteArray curKey = cur ? cur->data(0, Qt::UserRole).toByteArray() : QByteArray();
    m_tree->clear();
    const Document& d = m_ed->doc();
    // Folders first (sorted paths create parents before children).
    std::map<std::string, QTreeWidgetItem*> folders;
    for (const std::string& path : d.allLibraryFolders()) {
        const size_t slash = path.rfind('/');
        QTreeWidgetItem* parent = slash == std::string::npos ? nullptr : folders[path.substr(0, slash)];
        auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        it->setText(0, leafName(path));
        it->setIcon(0, ui::icon("folder"));
        it->setData(0, Qt::UserRole, QByteArray::fromStdString(path));
        it->setData(0, LibraryTree::KindRole, int(LibraryTree::FolderItem));
        it->setFlags((it->flags() | Qt::ItemIsEditable | Qt::ItemIsDropEnabled) & ~Qt::ItemIsDragEnabled);
        folders[path] = it;
    }
    std::vector<const Symbol*> syms;
    for (const Symbol& s : d.symbols) syms.push_back(&s);
    std::sort(syms.begin(), syms.end(), [](const Symbol* a, const Symbol* b) { return a->name < b->name; });
    QTreeWidgetItem* select = nullptr;
    for (const Symbol* s : syms) {
        const QString icon = s->type == SymbolType::MovieClip ? "movieclip" : s->type == SymbolType::Graphic ? "graphic" : "button";
        QTreeWidgetItem* parent = s->folder.empty() ? nullptr : folders[s->folder];
        auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        it->setText(0, QString::fromStdString(s->name));
        it->setIcon(0, ui::icon(icon));
        it->setData(0, Qt::UserRole, QByteArray::fromStdString(s->id));
        it->setData(0, LibraryTree::KindRole, int(LibraryTree::SymbolItem));
        it->setFlags((it->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
        QString tip = tr("Used %n time(s)", "", d.useCount(s->id));
        if (s->scale9) tip += tr(" · 9-slice scaling");
        it->setToolTip(0, tip);
        if (curKind == LibraryTree::SymbolItem && curKey == it->data(0, Qt::UserRole).toByteArray()) select = it;
    }
    for (auto& [path, it] : folders) {
        it->setExpanded(!m_collapsed.count(path));
        if (curKind == LibraryTree::FolderItem && curKey == QByteArray::fromStdString(path)) select = it;
    }
    if (select) m_tree->setCurrentItem(select);
    m_title->setSubtitle(tr("%n item(s)", "", int(d.symbols.size())));
    m_refreshing = false;
    m_preview->setSymbol(currentId());
}

} // namespace vx::app
