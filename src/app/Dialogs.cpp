// SPDX-License-Identifier: GPL-3.0-or-later
#include "Dialogs.h"
#include "Theme.h"
#include "Widgets.h"

#include "render/QtConvert.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QUrl>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTabletEvent>
#include <QVBoxLayout>

namespace vx::app {

using ui::Theme;

namespace {

QDialogButtonBox* okCancel(QDialog* d)
{
    auto* b = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, d);
    b->button(QDialogButtonBox::Ok)->setProperty("accent", true);
    QObject::connect(b, &QDialogButtonBox::accepted, d, &QDialog::accept);
    QObject::connect(b, &QDialogButtonBox::rejected, d, &QDialog::reject);
    return b;
}

QLabel* caption(const QString& t, QWidget* parent)
{
    auto* l = new QLabel(t, parent);
    l->setStyleSheet(QString("color: %1; font-size: 11px;").arg(Theme::p().text2.name()));
    return l;
}

/// 3x3 registration grid picker.
class RegistrationGrid : public QWidget {
public:
    RegistrationGrid(int* value, QWidget* parent) : QWidget(parent), m_value(value) { setFixedSize(66, 66); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const ui::Palette& pal = Theme::p();
        p.setPen(QPen(pal.line, 1));
        p.setBrush(pal.bg2);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
        for (int i = 0; i < 9; ++i) {
            const QPointF c(11 + (i % 3) * 22, 11 + (i / 3) * 22);
            p.setPen(Qt::NoPen);
            p.setBrush(i == *m_value ? pal.accent : pal.text3);
            p.drawRoundedRect(QRectF(c.x() - 5, c.y() - 5, 10, 10), 2, 2);
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const int col = std::clamp(int(e->position().x() / 22), 0, 2), row = std::clamp(int(e->position().y() / 22), 0, 2);
        *m_value = row * 3 + col;
        update();
    }

private:
    int* m_value;
};

} // namespace

// --- ConvertToSymbolDialog ------------------------------------------------------------------

ConvertToSymbolDialog::ConvertToSymbolDialog(const QString& defaultName, const std::vector<std::string>& folders, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Convert to Symbol"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(12);
    lay->addWidget(new SectionTitle(tr("Convert to Symbol"), this));
    auto* g = new QGridLayout();
    g->setHorizontalSpacing(14);
    g->setVerticalSpacing(10);
    m_name = new QLineEdit(defaultName, this);
    m_name->selectAll();
    g->addWidget(caption(tr("Name"), this), 0, 0);
    g->addWidget(m_name, 0, 1);
    m_type = new Segmented(this);
    m_type->addSegment(tr("Movie Clip"), "movieclip");
    m_type->addSegment(tr("Graphic"), "graphic");
    m_type->addSegment(tr("Button"), "button");
    m_type->setCurrent(QSettings().value("ui/lastSymbolType", 0).toInt());
    g->addWidget(caption(tr("Type"), this), 1, 0);
    g->addWidget(m_type, 1, 1);
    g->addWidget(caption(tr("Registration"), this), 2, 0);
    g->addWidget(new RegistrationGrid(&m_reg, this), 2, 1, Qt::AlignLeft);
    m_folder = new QComboBox(this);
    m_folder->setEditable(true);
    m_folder->addItem(tr("Library root"), QString());
    for (const std::string& f : folders) m_folder->addItem(QString::fromStdString(f), QString::fromStdString(f));
    m_folder->setToolTip(tr("Pick a folder or type a new path such as Characters/Hero"));
    g->addWidget(caption(tr("Folder"), this), 3, 0);
    g->addWidget(m_folder, 3, 1);
    m_scale9 = new QCheckBox(tr("Enable guides for 9-slice scaling"), this);
    g->addWidget(m_scale9, 4, 1);
    lay->addLayout(g);
    lay->addWidget(okCancel(this));
    resize(420, 0);
}

QString ConvertToSymbolDialog::name() const { return m_name->text().trimmed(); }

std::string ConvertToSymbolDialog::folder() const
{
    const QString text = m_folder->currentText().trimmed();
    if (m_folder->currentIndex() == 0 && text == m_folder->itemText(0)) return {};
    QStringList parts = text.split('/', Qt::SkipEmptyParts);
    for (QString& p : parts) p = p.trimmed();
    parts.removeAll(QString());
    return parts.join('/').toStdString();
}

bool ConvertToSymbolDialog::scale9() const { return m_scale9->isChecked(); }

SymbolType ConvertToSymbolDialog::type() const
{
    QSettings().setValue("ui/lastSymbolType", m_type->current());
    return SymbolType(m_type->current());
}

// --- DocumentDialog ------------------------------------------------------------------------------

DocumentDialog::DocumentDialog(const Document& d, QWidget* parent) : QDialog(parent), m_bg(d.background)
{
    setWindowTitle(tr("Document Settings"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(12);
    lay->addWidget(new SectionTitle(tr("Document"), this));
    auto* g = new QGridLayout();
    g->setHorizontalSpacing(14);
    g->setVerticalSpacing(10);
    auto mk = [this](double v, double lo, double hi, int dec, const QString& suffix) {
        auto* n = new HotNumber(this);
        n->setRange(lo, hi);
        n->setDecimals(dec);
        n->setSuffix(suffix);
        n->setValue(v);
        return n;
    };
    m_w = mk(d.width, 1, 16384, 0, " px");
    m_h = mk(d.height, 1, 16384, 0, " px");
    m_fps = mk(d.fps, 1, 120, 2, "");
    g->addWidget(caption(tr("Width"), this), 0, 0);
    g->addWidget(m_w, 0, 1);
    g->addWidget(caption(tr("Height"), this), 1, 0);
    g->addWidget(m_h, 1, 1);
    g->addWidget(caption(tr("Frame rate"), this), 2, 0);
    g->addWidget(m_fps, 2, 1);
    auto* bg = new ColorSwatch(this);
    bg->setFill(FillStyle::solid(m_bg));
    connect(bg, &ColorSwatch::clicked, this, [this, bg]() {
        popupColorPicker(bg, toQColor(m_bg), [this, bg](const QColor& c, bool) {
            m_bg = fromQColor(c);
            bg->setFill(FillStyle::solid(m_bg));
        });
    });
    g->addWidget(caption(tr("Background"), this), 3, 0);
    g->addWidget(bg, 3, 1, Qt::AlignLeft);
    lay->addLayout(g);
    lay->addWidget(okCancel(this));
    resize(360, 0);
}

double DocumentDialog::width() const { return m_w->value(); }
double DocumentDialog::height() const { return m_h->value(); }
double DocumentDialog::fps() const { return m_fps->value(); }

// --- LayerDialog -------------------------------------------------------------------------------------

LayerDialog::LayerDialog(Editor* editor, int layerIndex, QWidget* parent)
    : QDialog(parent), m_ed(editor), m_index(layerIndex), m_layer(editor->timeline().layers.at(layerIndex))
{
    setWindowTitle(tr("Layer Properties"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(12);
    lay->addWidget(new SectionTitle(tr("Layer"), this));
    auto* g = new QGridLayout();
    g->setHorizontalSpacing(14);
    g->setVerticalSpacing(10);
    int row = 0;
    auto add = [&](const QString& label, QWidget* w) {
        if (!label.isEmpty()) g->addWidget(caption(label, this), row, 0);
        g->addWidget(w, row, label.isEmpty() ? 0 : 1, 1, label.isEmpty() ? 2 : 1);
        ++row;
    };
    m_name = new QLineEdit(QString::fromStdString(m_layer.name), this);
    add(tr("Name"), m_name);
    m_type = new QComboBox(this);
    m_type->addItems({tr("Normal"), tr("Guide"), tr("Mask"), tr("Folder")});
    m_type->setCurrentIndex(int(m_layer.type));
    add(tr("Type"), m_type);
    m_opacity = new HotNumber(this);
    m_opacity->setRange(0, 100);
    m_opacity->setDecimals(0);
    m_opacity->setSuffix("%");
    m_opacity->setValue(m_layer.opacity * 100);
    add(tr("Opacity"), m_opacity);
    m_blend = blendModeCombo(this, m_layer.blend);
    add(tr("Blending"), m_blend);
    m_visible = new QCheckBox(tr("Show"), this);
    m_visible->setChecked(m_layer.visible);
    add({}, m_visible);
    m_locked = new QCheckBox(tr("Lock"), this);
    m_locked->setChecked(m_layer.locked);
    add({}, m_locked);
    m_outline = new QCheckBox(tr("View layer as outlines"), this);
    m_outline->setChecked(m_layer.outline);
    add({}, m_outline);
    lay->addLayout(g);
    lay->addWidget(okCancel(this));
    resize(380, 0);
    const bool folder = m_layer.type == LayerType::Folder;
    m_opacity->setEnabled(!folder);
    m_blend->setEnabled(!folder);

    connect(m_opacity, &HotNumber::valueChanged, this, &LayerDialog::preview);
    connect(m_opacity, &HotNumber::valueCommitted, this, &LayerDialog::preview);
    connect(m_blend, qOverload<int>(&QComboBox::activated), this, &LayerDialog::preview);
    for (QCheckBox* c : {m_visible, m_outline}) connect(c, &QCheckBox::toggled, this, &LayerDialog::preview);
}

LayerDialog::~LayerDialog() { m_ed->setPreview(std::nullopt); }

Layer LayerDialog::values() const
{
    Layer l = m_layer;
    l.name = m_name->text().trimmed().isEmpty() ? m_layer.name : m_name->text().trimmed().toStdString();
    l.type = LayerType(m_type->currentIndex());
    l.opacity = std::clamp(m_opacity->value() / 100.0, 0.0, 1.0);
    const QVariant blend = m_blend->currentData();
    if (blend.isValid()) l.blend = BlendMode(blend.toInt());
    l.visible = m_visible->isChecked();
    l.locked = m_locked->isChecked();
    l.outline = m_outline->isChecked();
    return l;
}

void LayerDialog::preview()
{
    Document d = m_ed->doc();
    std::vector<Layer>& layers = m_ed->mutableTimeline(d).layers;
    if (m_index < 0 || m_index >= int(layers.size())) return;
    const Layer v = values();
    Layer& l = layers[m_index];
    l.opacity = v.opacity;
    l.blend = v.blend;
    l.visible = v.visible;
    l.outline = v.outline;
    m_ed->setPreview(std::move(d));
}

void LayerDialog::edit(Editor* editor, int layerIndex, QWidget* parent)
{
    if (layerIndex < 0 || layerIndex >= int(editor->timeline().layers.size())) return;
    Layer v;
    {
        LayerDialog dlg(editor, layerIndex, parent);
        if (dlg.exec() != QDialog::Accepted) return;
        v = dlg.values();
    }
    editor->setLayerProperty(layerIndex, [&v](Layer& l) {
        l.name = v.name;
        l.type = v.type;
        l.opacity = v.opacity;
        l.blend = v.blend;
        l.visible = v.visible;
        l.locked = v.locked;
        l.outline = v.outline;
    }, tr("Layer Properties"));
}

// --- TabletDialog ------------------------------------------------------------------------------------

namespace {

class TabletReadout : public QWidget {
public:
    TabletReadout(double* p, double* tx, double* ty, double* rot, QString* dev, QWidget* parent)
        : QWidget(parent), m_p(p), m_tx(tx), m_ty(ty), m_rot(rot), m_dev(dev)
    {
        setMinimumHeight(120);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const ui::Palette& pal = Theme::p();
        p.setPen(Qt::NoPen);
        p.setBrush(pal.bg2);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 12, 12);
        p.setFont(Theme::display(34));
        p.setPen(pal.text);
        p.drawText(QRectF(16, 8, width() - 32, 50), Qt::AlignLeft | Qt::AlignVCenter, QString::number(int(*m_p * 100)) + "%");
        p.setFont(Theme::ui(11, QFont::Bold));
        p.setPen(pal.accent);
        p.drawText(QRectF(18, 56, width(), 16), tr("PRESSURE"));
        p.setPen(pal.text2);
        p.setFont(Theme::ui(11));
        p.drawText(QRectF(18, 76, width() - 36, 36), Qt::AlignLeft | Qt::TextWordWrap,
                   tr("Tilt %1° / %2°   Rotation %3°\n%4")
                       .arg(int(*m_tx))
                       .arg(int(*m_ty))
                       .arg(int(*m_rot))
                       .arg(m_dev->isEmpty() ? tr("Draw here with your pen to test it") : *m_dev));
        const QRectF bar(width() - 36, 14, 14, height() - 28);
        p.setBrush(pal.bg3);
        p.drawRoundedRect(bar, 7, 7);
        p.setBrush(pal.accent);
        const double h = bar.height() * std::clamp(*m_p, 0.0, 1.0);
        p.drawRoundedRect(QRectF(bar.left(), bar.bottom() - h, bar.width(), h), 7, 7);
    }

private:
    double *m_p, *m_tx, *m_ty, *m_rot;
    QString* m_dev;
};

} // namespace

TabletDialog::TabletDialog(Editor* editor, QWidget* parent) : QDialog(parent), m_ed(editor)
{
    setWindowTitle(tr("Tablet"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(12);
    lay->addWidget(new SectionTitle(tr("Tablet"), this));
    m_readout = new TabletReadout(&m_pressure, &m_tiltX, &m_tiltY, &m_rotation, &m_device, this);
    lay->addWidget(m_readout);
    lay->addWidget(caption(tr("Pressure curve — drag points, double-click to add, right-click to remove"), this));
    m_curve = new CurveEditor(this);
    m_curve->setCurve(m_ed->settings().pressureCurve);
    m_curve->setMinimumHeight(180);
    connect(m_curve, &CurveEditor::curveChanged, this, [this](const ResponseCurve& c) { m_ed->settings().pressureCurve = c; });
    lay->addWidget(m_curve);
    auto* presets = new Segmented(this);
    presets->addSegment(tr("Soft"));
    presets->addSegment(tr("Linear"));
    presets->addSegment(tr("Firm"));
    presets->setCurrent(1);
    connect(presets, &Segmented::changed, this, [this](int i) {
        ResponseCurve c;
        if (i == 0) c.points = {{0, 0}, {0.3, 0.55}, {1, 1}};
        if (i == 2) c.points = {{0, 0}, {0.6, 0.35}, {1, 1}};
        m_curve->setCurve(c);
        m_ed->settings().pressureCurve = c;
    });
    lay->addWidget(presets);
    auto* eraser = new QCheckBox(tr("The eraser end of the pen switches to the Eraser tool"), this);
    eraser->setChecked(m_ed->settings().eraserTipSwitches);
    connect(eraser, &QCheckBox::toggled, this, [this](bool b) { m_ed->settings().eraserTipSwitches = b; });
    lay->addWidget(eraser);
#ifdef Q_OS_WIN
    auto* wintab = new QCheckBox(tr("Use WinTab instead of Windows Ink (restart required)"), this);
    wintab->setChecked(QSettings().value("tablet/wintab", false).toBool());
    connect(wintab, &QCheckBox::toggled, this, [](bool b) { QSettings().setValue("tablet/wintab", b); });
    lay->addWidget(wintab);
#endif
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::accept);
    lay->addWidget(close);
    resize(440, 0);
}

void TabletDialog::tabletEvent(QTabletEvent* e)
{
    e->accept();
    m_pressure = e->pressure();
    m_tiltX = e->xTilt();
    m_tiltY = e->yTilt();
    m_rotation = e->rotation();
    if (const QPointingDevice* d = e->pointingDevice()) m_device = d->name();
    if (e->type() == QEvent::TabletRelease) m_pressure = 0;
    m_curve->setMarker(e->type() == QEvent::TabletRelease ? -1 : m_pressure);
    m_readout->update();
}

// --- ExportDialog -------------------------------------------------------------------------------------

ExportDialog::ExportDialog(const QString& title, bool allowTransparent, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(title);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(12);
    lay->addWidget(new SectionTitle(title, this));
    auto* g = new QGridLayout();
    g->setHorizontalSpacing(14);
    g->setVerticalSpacing(10);
    m_scale = new HotNumber(this);
    m_scale->setRange(10, 800);
    m_scale->setSuffix("%");
    m_scale->setValue(100);
    g->addWidget(caption(tr("Scale"), this), 0, 0);
    g->addWidget(m_scale, 0, 1);
    m_range = new Segmented(this);
    m_range->addSegment(tr("All frames"));
    m_range->addSegment(tr("Current frame"));
    g->addWidget(caption(tr("Frames"), this), 1, 0);
    g->addWidget(m_range, 1, 1);
    m_transparent = new QCheckBox(tr("Transparent background"), this);
    m_transparent->setVisible(allowTransparent);
    g->addWidget(m_transparent, 2, 1);
    lay->addLayout(g);
    lay->addWidget(okCancel(this));
    resize(380, 0);
}

ExportOptions ExportDialog::options() const
{
    ExportOptions o;
    o.scale = m_scale->value() / 100.0;
    o.transparent = m_transparent->isChecked();
    o.allFrames = m_range->current() == 0;
    return o;
}

// --- HotkeysDialog ------------------------------------------------------------------------------------

HotkeysDialog::HotkeysDialog(const QList<QAction*>& actions, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Keyboard Shortcuts"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->addWidget(new SectionTitle(tr("Shortcuts"), this));
    auto* table = new QTableWidget(this);
    table->setColumnCount(2);
    table->horizontalHeader()->setVisible(false);
    table->verticalHeader()->setVisible(false);
    table->setShowGrid(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    int row = 0;
    for (QAction* a : actions) {
        if (a->shortcut().isEmpty()) continue;
        table->insertRow(row);
        auto* name = new QTableWidgetItem(a->text().remove('&'));
        auto* key = new QTableWidgetItem(a->shortcut().toString(QKeySequence::NativeText));
        key->setForeground(Theme::p().accent);
        key->setFont(Theme::ui(12.5, QFont::Bold));
        table->setItem(row, 0, name);
        table->setItem(row, 1, key);
        ++row;
    }
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    lay->addWidget(table, 1);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::accept);
    lay->addWidget(close);
    resize(520, 640);
}

// --- CrashDialog --------------------------------------------------------------------------

QUrl CrashDialog::issueUrl(const QString& report, const QString& reportPath)
{
    QString body = tr("**What I was doing:**\n\n\n**Crash report** (the full file is `%1` in *Help ▸ Logs and Crash Reports*; "
                      "attach it if you can):\n\n```\n")
                       .arg(QFileInfo(reportPath).fileName());
    // A URL has room for the head of the report: the system, the crash and the stack.
    body += report.left(3500);
    body += "\n```\n";
    const QString title = "Crash: " + crash::reportSummary(report);
    QUrl url("https://github.com/Nyan33/vertexa/issues/new");
    url.setQuery("title=" + QString::fromLatin1(QUrl::toPercentEncoding(title)) +
                     "&body=" + QString::fromLatin1(QUrl::toPercentEncoding(body)),
                 QUrl::StrictMode);
    return url;
}

CrashDialog::CrashDialog(const crash::Session& s, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Vertexa closed unexpectedly"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(12);
    lay->addWidget(new SectionTitle(tr("Vertexa closed unexpectedly"), this));
    const QString report = s.report.isEmpty() ? QString() : crash::readReport(s.report);
    auto* intro = new QLabel(this);
    intro->setWordWrap(true);
    intro->setText(report.isEmpty()
                       ? tr("The last session did not end normally: the app was stopped by the system or stopped responding.")
                       : tr("The last session crashed (%1). A report was saved: sending it helps to find and fix the cause. "
                            "It holds the version, the system, the recent actions and the program's call stack.")
                             .arg(crash::reportSummary(report).toHtmlEscaped()));
    lay->addWidget(intro);
    if (!s.recovery.isEmpty()) {
        const QString name = s.originalPath.isEmpty() ? tr("an untitled document") : QFileInfo(s.originalPath).fileName();
        auto* rec = new QLabel(tr("<b>Unsaved work of %1 was kept.</b> Restore it to go on; it opens as an unsaved copy.")
                                   .arg(name.toHtmlEscaped()),
                               this);
        rec->setWordWrap(true);
        lay->addWidget(rec);
    }
    if (!report.isEmpty()) {
        auto* view = new QPlainTextEdit(this);
        view->setReadOnly(true);
        view->setLineWrapMode(QPlainTextEdit::NoWrap);
        QFont mono("monospace");
        mono.setStyleHint(QFont::TypeWriter);
        mono.setPointSizeF(9);
        view->setFont(mono);
        view->setPlainText(report);
        view->setMinimumSize(640, 260);
        lay->addWidget(view, 1);
    }
    auto* row = new QHBoxLayout();
    row->setSpacing(8);
    auto button = [&](const QString& text) {
        auto* b = new QPushButton(text, this);
        b->setAutoDefault(false);
        row->addWidget(b);
        return b;
    };
    if (!report.isEmpty()) {
        QPushButton* copy = button(tr("Copy Report"));
        connect(copy, &QPushButton::clicked, this, [copy, report]() {
            QApplication::clipboard()->setText(report);
            copy->setText(tr("Copied"));
        });
        connect(button(tr("Report on GitHub…")), &QPushButton::clicked, this,
                [report, path = s.report]() { QDesktopServices::openUrl(issueUrl(report, path)); });
    }
    connect(button(tr("Open Folder")), &QPushButton::clicked, this,
            []() { QDesktopServices::openUrl(QUrl::fromLocalFile(crash::reportsDir())); });
    row->addStretch(1);
    if (!s.recovery.isEmpty()) {
        connect(button(tr("Don't Restore")), &QPushButton::clicked, this, [this]() {
            m_choice = Choice::Discard;
            reject();
        });
        QPushButton* restore = button(tr("Restore Unsaved Work"));
        restore->setDefault(true);
        restore->setProperty("accent", true);
        connect(restore, &QPushButton::clicked, this, [this]() {
            m_choice = Choice::Restore;
            accept();
        });
    } else {
        QPushButton* close = button(tr("Close"));
        close->setDefault(true);
        connect(close, &QPushButton::clicked, this, &QDialog::reject);
    }
    lay->addLayout(row);
}

} // namespace vx::app
