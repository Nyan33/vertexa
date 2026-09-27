// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — visual language: flat, minimal surfaces with expressive
// typography (Inter Display Black for big numbers and titles) and a single
// energetic accent colour. Everything alive is animated a little.
#pragma once

#include <QColor>
#include <QFont>
#include <QObject>

class QApplication;

namespace vx::ui {

struct Palette {
    bool dark = true;
    QColor bg0;       ///< app background / pasteboard
    QColor bg1;       ///< panels
    QColor bg2;       ///< inputs, raised surfaces
    QColor bg3;       ///< hover
    QColor line;      ///< hairlines
    QColor text;      ///< primary text
    QColor text2;     ///< secondary text
    QColor text3;     ///< disabled / hints
    QColor accent;    ///< the Vertexa orange
    QColor accentInk; ///< text on accent
    QColor violet;    ///< classic tweens
    QColor mint;      ///< shape tweens
    QColor yellow;    ///< labels, warnings
    QColor danger;
    QColor selection; ///< on-stage selection
};

class Theme : public QObject {
    Q_OBJECT
public:
    static Theme* instance();
    static void init(QApplication& app);
    static const Palette& p();
    static bool isDark();
    static void setDark(bool dark);

    /// UI text (Inter).
    static QFont ui(double px, int weight = QFont::Normal);
    /// Expressive display face (Inter Display Black).
    static QFont display(double px);
    static QString styleSheet();

signals:
    void changed();

private:
    Theme() = default;
    void apply();
    Palette m_palette;
    QString m_uiFamily = QStringLiteral("Inter");
    QString m_displayFamily = QStringLiteral("Inter Display");
};

/// Mixes two colours.
QColor mix(const QColor& a, const QColor& b, double t);
QColor withAlpha(QColor c, int alpha);

} // namespace vx::ui
