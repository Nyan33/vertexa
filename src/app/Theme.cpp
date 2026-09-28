// SPDX-License-Identifier: GPL-3.0-or-later
#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QSettings>

namespace vx::ui {

namespace {

Palette darkPalette()
{
    Palette p;
    p.dark = true;
    p.bg0 = QColor("#0E0E11");
    p.bg1 = QColor("#17171C");
    p.bg2 = QColor("#202027");
    p.bg3 = QColor("#2A2A33");
    p.line = QColor("#2B2B34");
    p.text = QColor("#F3F3F6");
    p.text2 = QColor("#A3A3B1");
    p.text3 = QColor("#6A6A78");
    p.accent = QColor("#FF5B2E");
    p.accentInk = QColor("#140A06");
    p.violet = QColor("#8B6CFF");
    p.mint = QColor("#2BD9A8");
    p.yellow = QColor("#FFC42E");
    p.danger = QColor("#FF4D6A");
    p.selection = QColor("#3D8BFF");
    return p;
}

Palette lightPalette()
{
    Palette p;
    p.dark = false;
    p.bg0 = QColor("#E6E6EC");
    p.bg1 = QColor("#F6F6F9");
    p.bg2 = QColor("#FFFFFF");
    p.bg3 = QColor("#ECECF2");
    p.line = QColor("#DADAE2");
    p.text = QColor("#131317");
    p.text2 = QColor("#5A5A68");
    p.text3 = QColor("#9A9AA8");
    p.accent = QColor("#FF4A1C");
    p.accentInk = QColor("#FFFFFF");
    p.violet = QColor("#6E4DFF");
    p.mint = QColor("#12B98B");
    p.yellow = QColor("#E6A800");
    p.danger = QColor("#E8364F");
    p.selection = QColor("#1F74FF");
    return p;
}

QString hex(const QColor& c) { return c.name(QColor::HexArgb); }

} // namespace

QColor mix(const QColor& a, const QColor& b, double t)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t),
                            float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

Theme* Theme::instance()
{
    static Theme* t = new Theme();
    return t;
}

void Theme::init(QApplication& app)
{
    Theme* t = instance();
    for (const char* f : {":/fonts/Inter-Regular.otf", ":/fonts/Inter-SemiBold.otf", ":/fonts/InterDisplay-Black.otf"})
        QFontDatabase::addApplicationFont(QString::fromLatin1(f));
    const QStringList families = QFontDatabase::families();
    if (!families.contains(t->m_uiFamily)) t->m_uiFamily = app.font().family();
    if (!families.contains(t->m_displayFamily)) t->m_displayFamily = t->m_uiFamily;
    QSettings s;
    t->m_palette = s.value("ui/dark", true).toBool() ? darkPalette() : lightPalette();
    QFont f = ui(12.5);
    app.setFont(f);
    t->apply();
}

const Palette& Theme::p() { return instance()->m_palette; }
bool Theme::isDark() { return p().dark; }

void Theme::setDark(bool dark)
{
    Theme* t = instance();
    t->m_palette = dark ? darkPalette() : lightPalette();
    QSettings().setValue("ui/dark", dark);
    t->apply();
    emit t->changed();
}

QFont Theme::ui(double px, int weight)
{
    QFont f(instance()->m_uiFamily);
    f.setPixelSize(int(std::lround(px)));
    f.setWeight(QFont::Weight(weight));
    f.setHintingPreference(QFont::PreferNoHinting);
    return f;
}

QFont Theme::display(double px)
{
    QFont f(instance()->m_displayFamily);
    f.setPixelSize(int(std::lround(px)));
    f.setWeight(QFont::Black);
    f.setLetterSpacing(QFont::PercentageSpacing, 96);
    return f;
}

void Theme::apply()
{
    const Palette& c = m_palette;
    QPalette pal;
    pal.setColor(QPalette::Window, c.bg1);
    pal.setColor(QPalette::WindowText, c.text);
    pal.setColor(QPalette::Base, c.bg2);
    pal.setColor(QPalette::AlternateBase, c.bg3);
    pal.setColor(QPalette::Text, c.text);
    pal.setColor(QPalette::Button, c.bg2);
    pal.setColor(QPalette::ButtonText, c.text);
    pal.setColor(QPalette::Highlight, c.accent);
    pal.setColor(QPalette::HighlightedText, c.accentInk);
    pal.setColor(QPalette::ToolTipBase, c.bg3);
    pal.setColor(QPalette::ToolTipText, c.text);
    pal.setColor(QPalette::PlaceholderText, c.text3);
    pal.setColor(QPalette::Disabled, QPalette::Text, c.text3);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, c.text3);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, c.text3);
    QApplication::setPalette(pal);
    qApp->setStyleSheet(styleSheet());
}

QString Theme::styleSheet()
{
    const Palette& c = p();
    QString s = QStringLiteral(R"(
QMainWindow, QDialog { background: %bg0%; }
QWidget { color: %text%; }
QToolTip { background: %bg3%; color: %text%; border: 1px solid %line%; padding: 6px 8px; border-radius: 6px; }
QMainWindow::separator { background: %bg0%; width: 6px; height: 6px; }
QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; }
QMenuBar { background: %bg0%; color: %text2%; padding: 2px 6px; }
QMenuBar::item { padding: 6px 10px; border-radius: 6px; background: transparent; }
QMenuBar::item:selected { background: %bg3%; color: %text%; }
QMenu { background: %bg2%; border: 1px solid %line%; border-radius: 10px; padding: 6px; }
QMenu::item { padding: 7px 28px 7px 14px; border-radius: 6px; }
QMenu::item:selected { background: %accent%; color: %accentInk%; }
QMenu::item:disabled { color: %text3%; }
QMenu::separator { height: 1px; background: %line%; margin: 6px 8px; }
QMenu::indicator { width: 14px; height: 14px; left: 6px; }
QPushButton { background: %bg3%; border: none; border-radius: 8px; padding: 7px 14px; font-weight: 600; }
QPushButton:hover { background: %hover%; }
QPushButton:pressed { background: %accent%; color: %accentInk%; }
QPushButton:default, QPushButton[accent="true"] { background: %accent%; color: %accentInk%; }
QPushButton:disabled { color: %text3%; }
QToolButton { background: transparent; border: none; border-radius: 7px; padding: 4px; }
QToolButton:hover { background: %bg3%; }
QToolButton:checked { background: %accentSoft%; color: %accent%; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit {
    background: %bg2%; border: 1px solid %line%; border-radius: 7px; padding: 5px 8px; selection-background-color: %accent%; selection-color: %accentInk%; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border: 1px solid %accent%; }
QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 0; border: none; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox::down-arrow { image: none; width: 0; height: 0; border-left: 4px solid transparent; border-right: 4px solid transparent; border-top: 5px solid %text2%; margin-right: 8px; }
QComboBox QAbstractItemView { background: %bg2%; border: 1px solid %line%; border-radius: 8px; padding: 4px; selection-background-color: %accent%; selection-color: %accentInk%; outline: none; }
QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 16px; height: 16px; border-radius: 5px; border: 1.5px solid %text3%; background: transparent; }
QCheckBox::indicator:checked { background: %accent%; border: 1.5px solid %accent%; }
QRadioButton::indicator { width: 14px; height: 14px; border-radius: 7px; border: 1.5px solid %text3%; }
QRadioButton::indicator:checked { background: %accent%; border: 1.5px solid %accent%; }
QSlider::groove:horizontal { height: 4px; background: %bg3%; border-radius: 2px; }
QSlider::sub-page:horizontal { background: %accent%; border-radius: 2px; }
QSlider::handle:horizontal { width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; background: %text%; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal { background: %bg3%; border-radius: 3px; min-height: 24px; min-width: 24px; }
QScrollBar::handle:hover { background: %text3%; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; border: none; width: 0; height: 0; }
QListWidget, QTreeWidget, QListView, QTreeView { background: transparent; border: none; outline: none; }
QListWidget::item, QTreeWidget::item { border-radius: 7px; padding: 4px; }
QListWidget::item:selected, QTreeWidget::item:selected { background: %accentSoft%; color: %text%; }
QListWidget::item:hover, QTreeWidget::item:hover { background: %bg3%; }
QHeaderView::section { background: transparent; color: %text3%; border: none; padding: 4px; }
QTabBar::tab { background: transparent; color: %text2%; padding: 6px 12px; border-radius: 7px; margin-right: 2px; }
QTabBar::tab:selected { background: %bg3%; color: %text%; }
QTabWidget::pane { border: none; }
QSplitter::handle { background: %bg0%; }
QGroupBox { border: 1px solid %line%; border-radius: 10px; margin-top: 14px; padding-top: 10px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: %text2%; }
QStatusBar { background: %bg0%; color: %text3%; }
QLabel[role="caption"] { color: %text2%; font-size: 11px; }
QLabel[role="hint"] { color: %text3%; font-size: 11px; }
QLabel[role="heading"] { color: %accent%; font-size: 11px; font-weight: 700; letter-spacing: 1px; padding-top: 8px; }
QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
)");
    s.replace("%bg0%", hex(c.bg0));
    s.replace("%bg1%", hex(c.bg1));
    s.replace("%bg2%", hex(c.bg2));
    s.replace("%bg3%", hex(c.bg3));
    s.replace("%hover%", hex(mix(c.bg3, c.text, 0.08)));
    s.replace("%line%", hex(c.line));
    s.replace("%text%", hex(c.text));
    s.replace("%text2%", hex(c.text2));
    s.replace("%text3%", hex(c.text3));
    s.replace("%accent%", hex(c.accent));
    s.replace("%accentInk%", hex(c.accentInk));
    s.replace("%accentSoft%", hex(withAlpha(c.accent, 48)));
    return s;
}

} // namespace vx::ui
