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
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace vx::app {

using ui::Theme;

QStringList LibraryList::mimeTypes() const { return {"application/x-vertexa-symbol"}; }

QMimeData* LibraryList::mimeData(const QList<QListWidgetItem*>& items) const
{
    if (items.isEmpty()) return nullptr;
    auto* m = new QMimeData();
    m->setData("application/x-vertexa-symbol", items.front()->data(Qt::UserRole).toByteArray());
    return m;
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
    m_list = new LibraryList(this);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setIconSize(QSize(20, 20));
    m_list->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    lay->addWidget(m_list, 1);

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
    QToolButton* dup = mk("symbol", tr("Duplicate Symbol"));
    QToolButton* edit = mk("sliders", tr("Edit Symbol"));
    buttons->addStretch(1);
    QToolButton* del = mk("trash", tr("Delete Symbol"));
    lay->addLayout(buttons);

    connect(add, &QToolButton::clicked, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New Symbol"), tr("Name"), QLineEdit::Normal,
                                                   QString::fromStdString(m_ed->doc().uniqueSymbolName("Symbol 1")), &ok);
        if (ok) m_ed->newSymbol(name, SymbolType::MovieClip);
    });
    connect(dup, &QToolButton::clicked, this, [this]() {
        if (!currentId().empty()) m_ed->duplicateSymbol(currentId());
    });
    connect(edit, &QToolButton::clicked, this, [this]() {
        if (!currentId().empty()) m_ed->enterSymbol(currentId());
    });
    connect(del, &QToolButton::clicked, this, [this]() {
        const std::string id = currentId();
        if (id.empty()) return;
        const int uses = m_ed->doc().useCount(id);
        if (uses > 0 &&
            QMessageBox::question(this, tr("Delete Symbol"), tr("The symbol is used %n time(s). Delete it and its instances?", "", uses)) !=
                QMessageBox::Yes)
            return;
        m_ed->deleteSymbol(id);
    });
    connect(m_list, &QListWidget::currentItemChanged, this, [this]() { m_preview->setSymbol(currentId()); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        m_ed->enterSymbol(it->data(Qt::UserRole).toByteArray().toStdString());
    });
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
        if (m_refreshing) return;
        m_ed->renameSymbol(it->data(Qt::UserRole).toByteArray().toStdString(), it->text());
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = m_list->itemAt(pos);
        if (!it) return;
        const std::string id = it->data(Qt::UserRole).toByteArray().toStdString();
        QMenu menu(this);
        menu.addAction(tr("Edit"), this, [this, id]() { m_ed->enterSymbol(id); });
        menu.addAction(tr("Rename"), this, [this, it]() { m_list->editItem(it); });
        menu.addAction(tr("Duplicate"), this, [this, id]() { m_ed->duplicateSymbol(id); });
        QMenu* type = menu.addMenu(tr("Behavior"));
        type->addAction(tr("Movie Clip"), this, [this, id]() { m_ed->setSymbolType(id, SymbolType::MovieClip); });
        type->addAction(tr("Graphic"), this, [this, id]() { m_ed->setSymbolType(id, SymbolType::Graphic); });
        type->addAction(tr("Button"), this, [this, id]() { m_ed->setSymbolType(id, SymbolType::Button); });
        menu.addSeparator();
        menu.addAction(tr("Delete"), this, [this, id]() { m_ed->deleteSymbol(id); });
        menu.exec(m_list->mapToGlobal(pos));
    });
    connect(m_ed, &Editor::documentChanged, this, &LibraryPanel::refresh);
    refresh();
}

std::string LibraryPanel::currentId() const
{
    QListWidgetItem* it = m_list->currentItem();
    return it ? it->data(Qt::UserRole).toByteArray().toStdString() : std::string();
}

void LibraryPanel::refresh()
{
    m_refreshing = true;
    const std::string cur = currentId();
    m_list->clear();
    const Document& d = m_ed->doc();
    std::vector<const Symbol*> syms;
    for (const Symbol& s : d.symbols) syms.push_back(&s);
    std::sort(syms.begin(), syms.end(), [](const Symbol* a, const Symbol* b) { return a->name < b->name; });
    for (const Symbol* s : syms) {
        const QString icon = s->type == SymbolType::MovieClip ? "movieclip" : s->type == SymbolType::Graphic ? "graphic" : "button";
        auto* it = new QListWidgetItem(ui::icon(icon), QString::fromStdString(s->name));
        it->setData(Qt::UserRole, QByteArray::fromStdString(s->id));
        it->setFlags(it->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
        it->setToolTip(tr("Used %n time(s)", "", d.useCount(s->id)));
        m_list->addItem(it);
        if (s->id == cur) m_list->setCurrentItem(it);
    }
    m_title->setSubtitle(tr("%n item(s)", "", int(d.symbols.size())));
    m_refreshing = false;
    m_preview->setSymbol(currentId());
}

} // namespace vx::app
