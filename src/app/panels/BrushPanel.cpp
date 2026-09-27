// SPDX-License-Identifier: GPL-3.0-or-later
#include "BrushPanel.h"
#include "../Icons.h"
#include "../Theme.h"
#include "../Widgets.h"

#include "core/Serialize.h"
#include "render/BrushResources.h"
#include "render/DabEngine.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace vx::app {

using ui::Theme;

namespace {

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

QString prettyResource(const std::string& id)
{
    QString s = QString::fromStdString(id);
    if (s.startsWith("builtin:")) {
        s = s.mid(8);
        if (!s.isEmpty()) s[0] = s[0].toUpper();
    }
    return s;
}

} // namespace

// --- PresetGrid -----------------------------------------------------------------------------

PresetGrid::PresetGrid(Editor* editor, QWidget* parent) : QWidget(parent), m_ed(editor)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    connect(m_ed, &Editor::settingsChanged, this, qOverload<>(&QWidget::update));
}

void PresetGrid::setPresets(std::vector<BrushPreset> presets)
{
    m_presets = std::move(presets);
    m_previews.clear();
    for (const BrushPreset& p : m_presets) m_previews.push_back(brushPreview(p, Theme::isDark() ? Color(240, 240, 245) : Color(20, 20, 26), 220, 54, &m_ed->doc()));
    updateGeometry();
    update();
}

int PresetGrid::heightForWidth(int) const { return int((m_presets.size() + 1) / 2) * 76 + 4; }

QRectF PresetGrid::tile(int i) const
{
    const double w = (width() - 10) / 2.0;
    return QRectF(1 + (i % 2) * (w + 8), (i / 2) * 76, w, 70);
}

void PresetGrid::resizeEvent(QResizeEvent*) { update(); }

void PresetGrid::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const ui::Palette& pal = Theme::p();
    for (int i = 0; i < int(m_presets.size()); ++i) {
        const QRectF r = tile(i);
        const bool cur = m_presets[i].id == m_ed->settings().paint.id;
        p.setPen(cur ? QPen(pal.accent, 2) : Qt::NoPen);
        p.setBrush(i == m_hover ? pal.bg3 : pal.bg2);
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 10, 10);
        const QRectF img = r.adjusted(6, 4, -6, -20);
        p.drawImage(img, m_previews[i]);
        p.setFont(Theme::ui(11, cur ? QFont::Bold : QFont::DemiBold));
        p.setPen(cur ? pal.accent : pal.text2);
        p.drawText(r.adjusted(10, 0, -8, -5), Qt::AlignBottom | Qt::AlignLeft,
                   QFontMetrics(p.font()).elidedText(QString::fromStdString(m_presets[i].name), Qt::ElideRight, int(r.width() - 18)));
    }
}

void PresetGrid::mousePressEvent(QMouseEvent* e)
{
    for (int i = 0; i < int(m_presets.size()); ++i)
        if (tile(i).contains(e->position())) {
            emit picked(m_presets[i]);
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
    m_layout->addWidget(new SectionTitle(tr("Brushes"), m_content));
    m_grid = new PresetGrid(m_ed, m_content);
    m_layout->addWidget(m_grid);
    m_preview = new QLabel(m_content);
    m_preview->setMinimumHeight(70);
    m_preview->setAlignment(Qt::AlignCenter);
    m_layout->addWidget(m_preview);
    m_editor = new QWidget(m_content);
    m_editorLayout = new QVBoxLayout(m_editor);
    m_editorLayout->setContentsMargins(0, 0, 0, 0);
    m_layout->addWidget(m_editor);
    m_layout->addStretch(1);
    setWidget(m_content);

    loadUserPresets();
    connect(m_grid, &PresetGrid::picked, this, [this](const BrushPreset& p) {
        m_ed->settings().paint = p;
        m_ed->settings().paintSizeScale = 1.0;
        m_ed->setTool(ToolId::PaintBrush);
        m_ed->emitSettingsChanged();
        rebuildEditor();
    });
    connect(Theme::instance(), &ui::Theme::changed, this, [this]() {
        m_grid->setPresets(m_grid->presets());
        refreshPreview();
    });
    rebuildEditor();
}

void BrushPanel::loadUserPresets()
{
    m_user.clear();
    const QJsonArray arr = QJsonDocument::fromJson(QSettings().value("brushes/user").toByteArray()).array();
    for (const QJsonValue& v : arr) m_user.push_back(brushPresetFromJson(v.toObject()));
    std::vector<BrushPreset> all = builtinBrushPresets();
    all.insert(all.end(), m_user.begin(), m_user.end());
    m_grid->setPresets(all);
}

void BrushPanel::saveUserPresets()
{
    QJsonArray arr;
    for (const BrushPreset& p : m_user) arr.append(brushPresetToJson(p));
    QSettings().setValue("brushes/user", QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void BrushPanel::refreshPreview()
{
    const qreal dpr = devicePixelRatioF();
    const int w = std::max(120, m_preview->width() - 4);
    const Color ink = m_ed->settings().stroke.paint.mainColor();
    QImage img = brushPreview(m_ed->settings().paint, ink, int(w * dpr), int(70 * dpr), &m_ed->doc());
    img.setDevicePixelRatio(dpr);
    QPixmap pm(QSize(w, 70) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(ink.r + ink.g + ink.b > 382 ? QColor(40, 40, 46) : QColor(250, 250, 252));
    p.drawRoundedRect(QRectF(0, 0, w, 70), 10, 10);
    p.drawImage(QPointF(0, 0), img);
    p.end();
    m_preview->setPixmap(pm);
}

QGridLayout* BrushPanel::section(const QString& title)
{
    auto* l = new QLabel(title.toUpper(), m_editor);
    l->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: 700; letter-spacing: 1px; padding-top: 8px;").arg(Theme::p().accent.name()));
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
    l->setStyleSheet(QString("color: %1; font-size: 11px;").arg(Theme::p().text2.name()));
    g->addWidget(l, r, 0);
    g->addWidget(w, r, 1);
}

void BrushPanel::rebuildEditor()
{
    // Clear the old editor.
    while (QLayoutItem* it = m_editorLayout->takeAt(0)) {
        if (QWidget* w = it->widget()) w->deleteLater();
        delete it;
    }
    BrushPreset& p = m_ed->settings().paint;
    auto changed = [this]() { QTimer::singleShot(0, this, &BrushPanel::refreshPreview); };
    auto* name = new QLabel(QString::fromStdString(p.name), m_editor);
    name->setFont(Theme::display(18));
    m_editorLayout->addWidget(name);

    QGridLayout* tip = section(tr("Tip"));
    QStringList tips{tr("Auto: circle"), tr("Auto: square")};
    std::vector<std::string> tipIds;
    for (const std::string& t : BrushResources::builtinTips()) {
        tips << prettyResource(t);
        tipIds.push_back(t);
    }
    for (const auto& [id, img] : m_ed->doc().images)
        if (id.rfind("tip:", 0) == 0) {
            tips << QString::fromStdString(id.substr(4));
            tipIds.push_back(id);
        }
    int curTip = p.tipType == TipType::Auto ? int(p.autoShape) : 0;
    if (p.tipType == TipType::Image)
        for (size_t i = 0; i < tipIds.size(); ++i)
            if (tipIds[i] == p.tipImage) curTip = int(i) + 2;
    auto* tipCombo = new QComboBox(m_editor);
    tipCombo->addItems(tips);
    tipCombo->setCurrentIndex(curTip);
    connect(tipCombo, qOverload<int>(&QComboBox::activated), this, [this, tipIds, changed](int i) {
        BrushPreset& b = m_ed->settings().paint;
        if (i < 2) {
            b.tipType = TipType::Auto;
            b.autoShape = AutoTipShape(i);
        } else {
            b.tipType = TipType::Image;
            b.tipImage = tipIds[size_t(i - 2)];
        }
        changed();
    });
    row(tip, tr("Tip"), tipCombo);
    row(tip, tr("Size"), num(m_editor, p.size, 0.5, 1000, 1, 0.5, " px", [this, changed](double v) { m_ed->settings().paint.size = v; changed(); }));
    row(tip, tr("Hardness"), num(m_editor, p.hardness * 100, 0, 100, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.hardness = v / 100; changed(); }));
    row(tip, tr("Roundness"), num(m_editor, p.roundness * 100, 5, 100, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.roundness = v / 100; changed(); }));
    row(tip, tr("Angle"), num(m_editor, p.angle, -180, 180, 0, 1, "°", [this, changed](double v) { m_ed->settings().paint.angle = v; changed(); }));
    row(tip, tr("Density"), num(m_editor, p.tipDensity * 100, 1, 100, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.tipDensity = v / 100; changed(); }));

    QGridLayout* basics = section(tr("Paint"));
    row(basics, tr("Spacing"), num(m_editor, p.spacing * 100, 1, 300, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.spacing = v / 100; changed(); }));
    row(basics, tr("Opacity"), num(m_editor, p.opacity * 100, 1, 100, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.opacity = v / 100; changed(); }));
    row(basics, tr("Flow"), num(m_editor, p.flow * 100, 1, 100, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.flow = v / 100; changed(); }));
    row(basics, {}, chk(m_editor, tr("Build-up (instead of wash)"), p.buildUp, [this, changed](bool b) { m_ed->settings().paint.buildUp = b; changed(); }));
    QStringList blends;
    for (const auto& b : kBlendModes) blends << QString::fromUtf8(b.label.data(), int(b.label.size()));
    auto* blend = new QComboBox(m_editor);
    blend->addItems(blends);
    blend->setCurrentIndex(int(p.blend));
    connect(blend, qOverload<int>(&QComboBox::activated), this, [this, changed](int i) { m_ed->settings().paint.blend = BlendMode(i); changed(); });
    row(basics, tr("Blending"), blend);
    row(basics, tr("Smoothing"), num(m_editor, p.smoothing, 0, 100, 0, 1, "", [this](double v) { m_ed->settings().paint.smoothing = v; }));

    QGridLayout* dyn = section(tr("Dynamics"));
    auto curveRow = [&](const QString& label, bool on, const ResponseCurve& c, std::function<void(bool)> toggle,
                        std::function<void(const ResponseCurve&)> set) {
        row(dyn, {}, chk(m_editor, label, on, [toggle, changed](bool b) { toggle(b); changed(); }));
        auto* ce = new CurveEditor(m_editor);
        ce->setCurve(c);
        ce->setFixedHeight(96);
        connect(ce, &CurveEditor::curveChanged, this, [set, changed](const ResponseCurve& rc) { set(rc); changed(); });
        row(dyn, {}, ce);
    };
    curveRow(tr("Pressure → size"), p.pressureSize, p.sizeCurve, [this](bool b) { m_ed->settings().paint.pressureSize = b; },
             [this](const ResponseCurve& c) { m_ed->settings().paint.sizeCurve = c; });
    row(dyn, tr("Min size"), num(m_editor, p.minSize * 100, 0, 100, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.minSize = v / 100; changed(); }));
    curveRow(tr("Pressure → opacity"), p.pressureOpacity, p.opacityCurve, [this](bool b) { m_ed->settings().paint.pressureOpacity = b; },
             [this](const ResponseCurve& c) { m_ed->settings().paint.opacityCurve = c; });
    curveRow(tr("Pressure → flow"), p.pressureFlow, p.flowCurve, [this](bool b) { m_ed->settings().paint.pressureFlow = b; },
             [this](const ResponseCurve& c) { m_ed->settings().paint.flowCurve = c; });
    row(dyn, {}, chk(m_editor, tr("Tilt → rotation"), p.tiltAngle, [this, changed](bool b) { m_ed->settings().paint.tiltAngle = b; changed(); }));
    row(dyn, {}, chk(m_editor, tr("Follow stroke direction"), p.followDirection, [this, changed](bool b) { m_ed->settings().paint.followDirection = b; changed(); }));
    row(dyn, tr("Rotation jitter"), num(m_editor, p.rotationJitter * 100, 0, 100, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.rotationJitter = v / 100; changed(); }));
    row(dyn, tr("Size jitter"), num(m_editor, p.sizeJitter * 100, 0, 100, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.sizeJitter = v / 100; changed(); }));
    row(dyn, tr("Opacity jitter"), num(m_editor, p.opacityJitter * 100, 0, 100, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.opacityJitter = v / 100; changed(); }));
    row(dyn, tr("Scatter"), num(m_editor, p.scatter * 100, 0, 500, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.scatter = v / 100; changed(); }));
    row(dyn, tr("Count"), num(m_editor, p.count, 1, 16, 0, 0.1, "", [this, changed](double v) { m_ed->settings().paint.count = int(v); changed(); }));

    QGridLayout* tex = section(tr("Texture"));
    row(tex, {}, chk(m_editor, tr("Enabled"), p.textureEnabled, [this, changed](bool b) { m_ed->settings().paint.textureEnabled = b; changed(); }));
    std::vector<std::string> texIds = BrushResources::builtinTextures();
    for (const auto& [id, img] : m_ed->doc().images)
        if (id.rfind("texture:", 0) == 0) texIds.push_back(id);
    QStringList texNames;
    int curTex = 0;
    for (size_t i = 0; i < texIds.size(); ++i) {
        texNames << prettyResource(texIds[i].rfind("texture:", 0) == 0 ? texIds[i].substr(8) : texIds[i]);
        if (texIds[i] == p.texture) curTex = int(i);
    }
    auto* texCombo = new QComboBox(m_editor);
    texCombo->addItems(texNames);
    texCombo->setCurrentIndex(curTex);
    connect(texCombo, qOverload<int>(&QComboBox::activated), this, [this, texIds, changed](int i) {
        m_ed->settings().paint.texture = texIds[size_t(i)];
        changed();
    });
    row(tex, tr("Pattern"), texCombo);
    auto* mode = new QComboBox(m_editor);
    mode->addItems({tr("Multiply"), tr("Subtract"), tr("Height")});
    mode->setCurrentIndex(int(p.textureMode));
    connect(mode, qOverload<int>(&QComboBox::activated), this, [this, changed](int i) { m_ed->settings().paint.textureMode = TextureMode(i); changed(); });
    row(tex, tr("Mode"), mode);
    row(tex, tr("Scale"), num(m_editor, p.textureScale * 100, 5, 1000, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.textureScale = v / 100; changed(); }));
    row(tex, tr("Strength"), num(m_editor, p.textureStrength * 100, 0, 100, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.textureStrength = v / 100; changed(); }));
    row(tex, tr("Brightness"), num(m_editor, p.textureBrightness * 100, -100, 100, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.textureBrightness = v / 100; changed(); }));
    row(tex, tr("Contrast"), num(m_editor, p.textureContrast * 100, 0, 300, 0, 1, "%", [this, changed](double v) { m_ed->settings().paint.textureContrast = v / 100; changed(); }));
    row(tex, {}, chk(m_editor, tr("Invert"), p.textureInvert, [this, changed](bool b) { m_ed->settings().paint.textureInvert = b; changed(); }));

    QGridLayout* actions = section(tr("Presets & resources"));
    auto* save = new QPushButton(tr("Save as Preset…"), m_editor);
    save->setProperty("accent", true);
    connect(save, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString n = QInputDialog::getText(this, tr("Save Brush Preset"), tr("Name"), QLineEdit::Normal,
                                                QString::fromStdString(m_ed->settings().paint.name) + tr(" copy"), &ok);
        if (!ok || n.trimmed().isEmpty()) return;
        BrushPreset b = m_ed->settings().paint;
        b.name = n.trimmed().toStdString();
        b.id = "user." + std::to_string(QDateTime::currentMSecsSinceEpoch());
        b.category = "User";
        m_user.push_back(b);
        saveUserPresets();
        m_ed->settings().paint = b;
        loadUserPresets();
        rebuildEditor();
    });
    auto* importTip = new QPushButton(tr("Import Tip (.gbr, .png)…"), m_editor);
    connect(importTip, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Brush Tip"), {}, tr("Brush tips (*.gbr *.png *.jpg *.jpeg)"));
        if (path.isEmpty()) return;
        QString err;
        GrayImagePtr img = BrushResources::loadFile(path, false, &err);
        if (!img) {
            m_ed->notify(err);
            return;
        }
        const std::string id = "tip:" + QFileInfo(path).completeBaseName().toStdString();
        m_ed->edit(tr("Import Brush Tip"), [&](Document& d) {
            d.images[id] = img;
            return true;
        });
        BrushPreset& b = m_ed->settings().paint;
        b.tipType = TipType::Image;
        b.tipImage = id;
        rebuildEditor();
    });
    auto* importTex = new QPushButton(tr("Import Texture…"), m_editor);
    connect(importTex, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Texture"), {}, tr("Images (*.png *.jpg *.jpeg)"));
        if (path.isEmpty()) return;
        QString err;
        GrayImagePtr img = BrushResources::loadFile(path, true, &err);
        if (!img) {
            m_ed->notify(err);
            return;
        }
        const std::string id = "texture:" + QFileInfo(path).completeBaseName().toStdString();
        m_ed->edit(tr("Import Texture"), [&](Document& d) {
            d.images[id] = img;
            return true;
        });
        BrushPreset& b = m_ed->settings().paint;
        b.textureEnabled = true;
        b.texture = id;
        rebuildEditor();
    });
    row(actions, {}, save);
    row(actions, {}, importTip);
    row(actions, {}, importTex);
    if (p.id.rfind("user.", 0) == 0) {
        auto* del = new QPushButton(tr("Delete Preset"), m_editor);
        connect(del, &QPushButton::clicked, this, [this]() {
            const std::string id = m_ed->settings().paint.id;
            m_user.erase(std::remove_if(m_user.begin(), m_user.end(), [&](const BrushPreset& x) { return x.id == id; }), m_user.end());
            saveUserPresets();
            m_ed->settings().paint = builtinBrushPresets()[1];
            loadUserPresets();
            rebuildEditor();
        });
        row(actions, {}, del);
    }
    QTimer::singleShot(0, this, &BrushPanel::refreshPreview);
}

} // namespace vx::app
