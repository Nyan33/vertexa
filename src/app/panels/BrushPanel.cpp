// SPDX-License-Identifier: GPL-3.0-or-later
#include "BrushPanel.h"
#include "../Theme.h"
#include "../Widgets.h"

#include "core/DocumentOps.h"
#include "core/Serialize.h"
#include "render/QtConvert.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace vx::app {

using ui::Theme;

namespace {

constexpr QSize kTilePreview(200, 46);

HotNumber* num(QWidget* parent, double v, double lo, double hi, int dec, double step, const QString& suffix,
               std::function<void(double)> fn)
{
    auto* n = new HotNumber(parent);
    n->setRange(lo, hi);
    n->setDecimals(dec);
    n->setStep(step);
    n->setSuffix(suffix);
    n->setValue(v);
    QObject::connect(n, &HotNumber::valueChanged, parent, [fn](double x) { fn(x); });
    QObject::connect(n, &HotNumber::valueCommitted, parent, [fn](double x) { fn(x); });
    return n;
}

QCheckBox* chk(QWidget* parent, const QString& text, bool on, std::function<void(bool)> fn)
{
    auto* c = new QCheckBox(text, parent);
    c->setChecked(on);
    QObject::connect(c, &QCheckBox::toggled, parent, [fn](bool b) { fn(b); });
    return c;
}

QString kindLabel(VectorBrushKind k)
{
    switch (k) {
    case VectorBrushKind::Art: return QObject::tr("Art brush");
    case VectorBrushKind::Pattern: return QObject::tr("Pattern brush");
    case VectorBrushKind::Textured: return QObject::tr("Textured brush");
    case VectorBrushKind::Scatter: return QObject::tr("Scatter brush");
    }
    return {};
}

bool isDocumentBrush(const std::string& id) { return id.rfind("doc.", 0) == 0; }
bool isUserBrush(const std::string& id) { return id.rfind("user.", 0) == 0; }

Color previewInk() { return Theme::isDark() ? Color(240, 240, 245) : Color(20, 20, 26); }

} // namespace

QImage vectorBrushPreview(const VectorBrushPreset& p, const FillStyle& paint, QSize size, qreal dpr)
{
    QImage img(size * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(0);
    const double w = size.width(), h = size.height();
    const double sz = std::max(p.size, 0.5);
    // An S curve eight widths long with a pressure swell, fitted to the image.
    const double length = sz * 8.0;
    const double guess = std::min(w * 0.92 / (length + sz), h * 0.88 / (2.2 * sz));
    std::vector<InputSample> samples;
    for (int i = 0; i <= 60; ++i) {
        const double t = i / 60.0;
        InputSample q;
        q.pos = {t * length, std::sin(t * 2 * kPi) * sz * 0.55};
        q.pressure = 0.3 + 0.7 * std::sin(t * kPi);
        samples.push_back(q);
    }
    const std::vector<BrushPiece> pieces = vectorBrushStroke(p, vectorBrushPath(p, samples), paint, 7, 0.3 / guess);
    Rect b;
    for (const BrushPiece& piece : pieces) b.include(piece.region.bounds());
    if (b.isEmpty()) return img;
    const double k = std::min(w * 0.94 / std::max(b.width(), 1e-6), h * 0.9 / std::max(b.height(), 1e-6));
    QPainter qp(&img);
    qp.setRenderHint(QPainter::Antialiasing);
    qp.translate(w * 0.5, h * 0.5);
    qp.scale(k, k);
    qp.translate(-b.center().x, -b.center().y);
    qp.setPen(Qt::NoPen);
    for (const BrushPiece& piece : pieces) {
        QPainterPath path = toQPath(piece.region);
        path.setFillRule(Qt::WindingFill);
        qp.setBrush(toQColor(piece.fill.mainColor()));
        qp.drawPath(path);
    }
    return img;
}

// --- PresetGrid -----------------------------------------------------------------------------

PresetGrid::PresetGrid(Editor* editor, QWidget* parent) : QWidget(parent), m_ed(editor)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    connect(m_ed, &Editor::settingsChanged, this, qOverload<>(&QWidget::update));
}

void PresetGrid::setPresets(std::vector<VectorBrushPreset> presets)
{
    m_presets = std::move(presets);
    invalidatePreviews();
    updateGeometry();
}

void PresetGrid::invalidatePreviews()
{
    m_previews.assign(m_presets.size(), QImage());
    update();
}

int PresetGrid::heightForWidth(int) const { return int((m_presets.size() + 1) / 2) * 70 + 4; }

QRectF PresetGrid::tile(int i) const
{
    const double w = (width() - 10) / 2.0;
    return QRectF(1 + (i % 2) * (w + 8), (i / 2) * 70, w, 64);
}

void PresetGrid::paintEvent(QPaintEvent* e)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const ui::Palette& pal = Theme::p();
    const qreal dpr = devicePixelRatioF();
    for (int i = 0; i < int(m_presets.size()); ++i) {
        const QRectF r = tile(i);
        if (!r.intersects(e->rect())) continue;
        const VectorBrushPreset& b = m_presets[size_t(i)];
        const bool cur = b.id == m_ed->settings().paint.id;
        p.setPen(cur ? QPen(pal.accent, 2) : Qt::NoPen);
        p.setBrush(i == m_hover ? pal.bg3 : pal.bg2);
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 10, 10);
        if (m_previews[size_t(i)].isNull())
            m_previews[size_t(i)] = vectorBrushPreview(b, FillStyle::solid(previewInk()), kTilePreview, dpr);
        const QRectF area = r.adjusted(8, 4, -8, -20);
        const QSizeF fit = QSizeF(kTilePreview).scaled(area.size(), Qt::KeepAspectRatio);
        p.drawImage(QRectF(area.center() - QPointF(fit.width() * 0.5, fit.height() * 0.5), fit), m_previews[size_t(i)]);
        p.setFont(Theme::ui(11, cur ? QFont::Bold : QFont::DemiBold));
        p.setPen(cur ? pal.accent : pal.text2);
        QString label = QString::fromStdString(b.name);
        if (isDocumentBrush(b.id)) label += QStringLiteral(" ·");
        p.drawText(r.adjusted(10, 0, -8, -5), Qt::AlignBottom | Qt::AlignLeft,
                   QFontMetrics(p.font()).elidedText(label, Qt::ElideRight, int(r.width() - 18)));
    }
}

void PresetGrid::mousePressEvent(QMouseEvent* e)
{
    for (int i = 0; i < int(m_presets.size()); ++i)
        if (tile(i).contains(e->position())) {
            emit picked(m_presets[size_t(i)]);
            return;
        }
}

void PresetGrid::mouseMoveEvent(QMouseEvent* e)
{
    int h = -1;
    for (int i = 0; i < int(m_presets.size()); ++i)
        if (tile(i).contains(e->position())) h = i;
    if (h != m_hover) {
        m_hover = h;
        if (h >= 0) {
            const VectorBrushPreset& b = m_presets[size_t(h)];
            setToolTip(QString("%1 — %2").arg(QString::fromStdString(b.name), kindLabel(b.kind)));
        }
        update();
    }
}

void PresetGrid::leaveEvent(QEvent*)
{
    m_hover = -1;
    update();
}

// --- BrushPanel -------------------------------------------------------------------------------

BrushPanel::BrushPanel(Editor* editor, QWidget* parent) : QScrollArea(parent), m_ed(editor)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content = new QWidget();
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(10, 8, 16, 12);
    m_layout->setSpacing(8);
    auto* title = new SectionTitle(tr("Brushes"), m_content);
    title->setSubtitle(tr("vector"));
    m_layout->addWidget(title);
    m_grid = new PresetGrid(m_ed, m_content);
    m_layout->addWidget(m_grid);
    m_preview = new QLabel(m_content);
    m_preview->setFixedHeight(70);
    // The pixmap follows the panel width; it must not hold the panel open.
    m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_preview->setAlignment(Qt::AlignCenter);
    m_layout->addWidget(m_preview);
    m_editor = new QWidget(m_content);
    m_editorLayout = new QVBoxLayout(m_editor);
    m_editorLayout->setContentsMargins(0, 0, 0, 0);
    m_layout->addWidget(m_editor);
    m_layout->addStretch(1);
    setWidget(m_content);

    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(40);
    connect(m_previewTimer, &QTimer::timeout, this, &BrushPanel::refreshPreview);

    loadUserPresets();
    reloadPresets();
    connect(m_grid, &PresetGrid::picked, this, &BrushPanel::pick);
    connect(Theme::instance(), &ui::Theme::changed, this, [this]() {
        m_grid->invalidatePreviews();
        m_previewTimer->start();
    });
    connect(m_ed, &Editor::documentChanged, this, [this]() {
        std::vector<std::string> ids;
        for (const VectorBrushPreset& b : m_ed->doc().brushes) ids.push_back(b.id);
        if (ids != m_docBrushes) reloadPresets();
    });
    connect(m_ed, &Editor::settingsChanged, this, [this]() {
        const VectorBrushPreset& b = m_ed->settings().paint;
        if (b.id != m_shownId || b.size != m_shownSize) scheduleRebuild();
        else m_previewTimer->start(); // the paint colour may have changed
    });
    rebuildEditor();
}

void BrushPanel::resizeEvent(QResizeEvent* e)
{
    QScrollArea::resizeEvent(e);
    m_previewTimer->start();
}

void BrushPanel::pick(const VectorBrushPreset& p)
{
    m_ed->settings().paint = p;
    m_ed->setTool(ToolId::PaintBrush);
    m_ed->emitSettingsChanged();
}

void BrushPanel::loadUserPresets()
{
    m_user.clear();
    const QJsonArray arr = QJsonDocument::fromJson(QSettings().value("vectorBrushes/user").toByteArray()).array();
    for (const QJsonValue& v : arr) {
        VectorBrushPreset p = vectorBrushFromJson(v.toObject());
        if (!isUserBrush(p.id)) p.id = "user." + p.id;
        m_user.push_back(std::move(p));
    }
}

void BrushPanel::saveUserPresets()
{
    QJsonArray arr;
    for (const VectorBrushPreset& p : m_user) arr.append(vectorBrushToJson(p));
    QSettings().setValue("vectorBrushes/user", QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void BrushPanel::reloadPresets()
{
    std::vector<VectorBrushPreset> all = builtinVectorBrushes();
    m_docBrushes.clear();
    for (const VectorBrushPreset& b : m_ed->doc().brushes) {
        all.push_back(b);
        m_docBrushes.push_back(b.id);
    }
    all.insert(all.end(), m_user.begin(), m_user.end());
    m_grid->setPresets(std::move(all));
}

void BrushPanel::scheduleRebuild()
{
    if (m_rebuildPending) return;
    m_rebuildPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_rebuildPending = false;
        rebuildEditor();
    });
}

void BrushPanel::refreshPreview()
{
    const qreal dpr = devicePixelRatioF();
    const int w = std::max(120, m_preview->width() - 4);
    const FillStyle paint = m_ed->settings().stroke.paint;
    const Color ink = paint.mainColor();
    QPixmap pm(QSize(w, 70) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(ink.r + ink.g + ink.b > 382 ? QColor(40, 40, 46) : QColor(250, 250, 252));
    p.drawRoundedRect(QRectF(0, 0, w, 70), 10, 10);
    p.drawImage(QPointF(0, 0), vectorBrushPreview(m_ed->settings().paint, FillStyle::solid(ink), QSize(w, 70), dpr));
    p.end();
    m_preview->setPixmap(pm);
}

QGridLayout* BrushPanel::section(const QString& title)
{
    auto* l = new QLabel(title.toUpper(), m_editor);
    l->setProperty("role", "heading");
    m_editorLayout->addWidget(l);
    auto* box = new QWidget(m_editor);
    auto* g = new QGridLayout(box);
    g->setContentsMargins(4, 2, 2, 6);
    g->setHorizontalSpacing(10);
    g->setVerticalSpacing(6);
    g->setColumnStretch(1, 1);
    m_editorLayout->addWidget(box);
    return g;
}

void BrushPanel::row(QGridLayout* g, const QString& label, QWidget* w)
{
    const int r = g->rowCount();
    if (label.isEmpty()) {
        g->addWidget(w, r, 0, 1, 2);
        return;
    }
    auto* l = new QLabel(label, m_editor);
    l->setProperty("role", "caption");
    g->addWidget(l, r, 0);
    g->addWidget(w, r, 1);
}

void BrushPanel::rebuildEditor()
{
    while (QLayoutItem* it = m_editorLayout->takeAt(0)) {
        if (QWidget* w = it->widget()) w->deleteLater();
        delete it;
    }
    const VectorBrushPreset& p = m_ed->settings().paint;
    m_shownId = p.id;
    m_shownSize = p.size;
    m_grid->update();
    auto changed = [this]() { m_previewTimer->start(); };
    auto brush = [this]() -> VectorBrushPreset& { return m_ed->settings().paint; };

    auto* name = new QLabel(QString::fromStdString(p.name), m_editor);
    name->setFont(Theme::display(18));
    m_editorLayout->addWidget(name);
    auto* kind = new QLabel(kindLabel(p.kind) + (isDocumentBrush(p.id) ? tr(" · saved in this document") : QString()), m_editor);
    kind->setProperty("role", "hint");
    m_editorLayout->addWidget(kind);

    QGridLayout* stroke = section(tr("Stroke"));
    auto* type = new QComboBox(m_editor);
    type->addItems({tr("Art"), tr("Pattern"), tr("Textured"), tr("Scatter")});
    if (!p.art || p.art->isEmpty())
        if (auto* model = qobject_cast<QStandardItemModel*>(type->model()))
            for (int i : {0, 1}) model->item(i)->setEnabled(false);
    type->setCurrentIndex(int(p.kind));
    connect(type, qOverload<int>(&QComboBox::activated), this, [this, brush](int i) {
        brush().kind = VectorBrushKind(i);
        m_shownId.clear(); // rebuild with the new kind's options
        scheduleRebuild();
        m_previewTimer->start();
    });
    row(stroke, tr("Type"), type);
    row(stroke, tr("Size"), num(m_editor, p.size, 0.5, 1000, 1, 0.5, " px", [this, brush, changed](double v) {
            brush().size = v;
            m_shownSize = v;
            m_ed->emitSettingsChanged();
            changed();
        }));
    row(stroke, tr("Smoothing"), num(m_editor, p.smoothing, 0, 100, 0, 1, "", [brush](double v) { brush().smoothing = v; }));
    row(stroke, {}, chk(m_editor, tr("Pressure → size"), p.pressureSize, [brush, changed](bool b) {
            brush().pressureSize = b;
            changed();
        }));
    auto* curve = new CurveEditor(m_editor);
    curve->setCurve(p.sizeCurve);
    curve->setFixedHeight(96);
    connect(curve, &CurveEditor::curveChanged, this, [brush, changed](const ResponseCurve& c) {
        brush().sizeCurve = c;
        changed();
    });
    row(stroke, {}, curve);
    row(stroke, tr("Min size"), num(m_editor, p.minSize * 100, 0, 100, 0, 1, "%", [brush, changed](double v) {
            brush().minSize = v / 100;
            changed();
        }));

    switch (p.kind) {
    case VectorBrushKind::Art:
    case VectorBrushKind::Pattern: {
        QGridLayout* art = section(p.kind == VectorBrushKind::Art ? tr("Artwork") : tr("Pattern"));
        row(art, {}, chk(m_editor, tr("Paint with the stroke colour"), p.colorize, [brush, changed](bool b) {
                brush().colorize = b;
                changed();
            }));
        if (p.kind == VectorBrushKind::Pattern) {
            row(art, tr("Gap"), num(m_editor, p.patternGap * 100, 0, 500, 0, 1, "%", [brush, changed](double v) {
                    brush().patternGap = v / 100;
                    changed();
                }));
            row(art, {}, chk(m_editor, tr("Stretch tiles to fit"), p.stretchToFit, [brush, changed](bool b) {
                    brush().stretchToFit = b;
                    changed();
                }));
        }
        auto* hint = new QLabel(tr("The artwork's width runs along the stroke and its height across it."), m_editor);
        hint->setWordWrap(true);
        hint->setProperty("role", "hint");
        row(art, {}, hint);
        break;
    }
    case VectorBrushKind::Textured: {
        QGridLayout* tex = section(tr("Texture"));
        row(tex, tr("Rough edge"), num(m_editor, p.roughness * 100, 0, 100, 0, 1, "%", [brush, changed](double v) {
                brush().roughness = v / 100;
                changed();
            }));
        row(tex, tr("Edge scale"), num(m_editor, p.roughScale, 0.5, 200, 1, 0.1, " px", [brush, changed](double v) {
                brush().roughScale = v;
                changed();
            }));
        row(tex, tr("Grain"), num(m_editor, p.grain * 100, 0, 100, 0, 1, "%", [brush, changed](double v) {
                brush().grain = v / 100;
                changed();
            }));
        row(tex, tr("Grain size"), num(m_editor, p.grainSize, 0.2, 50, 1, 0.1, " px", [brush, changed](double v) {
                brush().grainSize = v;
                changed();
            }));
        break;
    }
    case VectorBrushKind::Scatter: {
        QGridLayout* sc = section(tr("Scatter"));
        row(sc, tr("Dab size"), num(m_editor, p.dabSize * 100, 1, 200, 0, 1, "%", [brush, changed](double v) {
                brush().dabSize = v / 100;
                changed();
            }));
        row(sc, tr("Density"), num(m_editor, p.density, 0.1, 30, 1, 0.1, "", [brush, changed](double v) {
                brush().density = v;
                changed();
            }));
        row(sc, tr("Spread"), num(m_editor, p.scatter * 100, 0, 300, 0, 1, "%", [brush, changed](double v) {
                brush().scatter = v / 100;
                changed();
            }));
        break;
    }
    }

    QGridLayout* actions = section(tr("Brushes"));
    auto* save = new QPushButton(tr("Save as Preset…"), m_editor);
    save->setProperty("accent", true);
    connect(save, &QPushButton::clicked, this, &BrushPanel::saveAsPreset);
    auto* art = new QPushButton(tr("Art Brush from Selection"), m_editor);
    art->setToolTip(tr("Stretches the selected artwork along each stroke"));
    connect(art, &QPushButton::clicked, this, [this]() { createFromSelection(false); });
    auto* pattern = new QPushButton(tr("Pattern Brush from Selection"), m_editor);
    pattern->setToolTip(tr("Repeats the selected artwork along each stroke"));
    connect(pattern, &QPushButton::clicked, this, [this]() { createFromSelection(true); });
    row(actions, {}, save);
    row(actions, {}, art);
    row(actions, {}, pattern);
    if (isUserBrush(p.id) || isDocumentBrush(p.id)) {
        auto* del = new QPushButton(tr("Delete Brush"), m_editor);
        connect(del, &QPushButton::clicked, this, &BrushPanel::deleteCurrent);
        row(actions, {}, del);
    }
    m_previewTimer->start(0);
}

void BrushPanel::saveAsPreset()
{
    bool ok = false;
    const QString n = QInputDialog::getText(this, tr("Save Brush Preset"), tr("Name"), QLineEdit::Normal,
                                            QString::fromStdString(m_ed->settings().paint.name) + tr(" copy"), &ok);
    if (!ok || n.trimmed().isEmpty()) return;
    VectorBrushPreset b = m_ed->settings().paint;
    b.name = n.trimmed().toStdString();
    b.id = "user." + std::to_string(QDateTime::currentMSecsSinceEpoch());
    b.builtin = false;
    m_user.push_back(b);
    saveUserPresets();
    reloadPresets();
    pick(b);
}

void BrushPanel::createFromSelection(bool pattern)
{
    std::vector<ElementPtr> els = m_ed->selectedElements();
    const ShapePick& sel = m_ed->shapePick();
    if (sel.valid()) {
        ShapeGraph rest, lifted;
        if (sel.region) cutByRegion(*sel.graph, *sel.region, rest, lifted);
        else liftSelection(*sel.graph, sel.sel, rest, lifted);
        if (!lifted.isEmpty()) els.insert(els.begin(), makeShapeElement(lifted, false));
    }
    const ShapeGraph art = linesToFills(flattenToShape(m_ed->doc(), els, 0));
    if (art.fills.empty() || art.bounds(false).isEmpty()) {
        m_ed->notify(tr("Select some artwork (fills or lines) to make a brush from"));
        return;
    }
    int n = 1;
    const std::string stem = pattern ? "Pattern Brush " : "Art Brush ";
    auto taken = [&](const std::string& name) {
        for (const VectorBrushPreset& b : m_grid->presets())
            if (b.name == name) return true;
        return false;
    };
    while (taken(stem + std::to_string(n))) ++n;
    bool ok = false;
    const QString name = QInputDialog::getText(this, pattern ? tr("New Pattern Brush") : tr("New Art Brush"), tr("Name"),
                                               QLineEdit::Normal, QString::fromStdString(stem + std::to_string(n)), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    VectorBrushPreset b = makeArtBrush(art, pattern, name.trimmed().toStdString());
    b.id = "doc." + std::to_string(QDateTime::currentMSecsSinceEpoch());
    const bool done = m_ed->edit(pattern ? tr("New Pattern Brush") : tr("New Art Brush"), [&](Document& d) {
        d.brushes.push_back(b);
        return true;
    });
    if (!done) return;
    reloadPresets();
    pick(b);
}

void BrushPanel::deleteCurrent()
{
    const std::string id = m_ed->settings().paint.id;
    if (isUserBrush(id)) {
        m_user.erase(std::remove_if(m_user.begin(), m_user.end(), [&](const VectorBrushPreset& x) { return x.id == id; }), m_user.end());
        saveUserPresets();
    } else if (isDocumentBrush(id)) {
        m_ed->edit(tr("Delete Brush"), [&](Document& d) {
            const auto it = std::remove_if(d.brushes.begin(), d.brushes.end(), [&](const VectorBrushPreset& x) { return x.id == id; });
            if (it == d.brushes.end()) return false;
            d.brushes.erase(it, d.brushes.end());
            return true;
        });
    } else {
        return;
    }
    reloadPresets();
    if (const VectorBrushPreset* chalk = builtinVectorBrush("chalk")) pick(*chalk);
}

} // namespace vx::app
