#pragma once

#include <QColor>
#include <QString>
#include <QPalette>

namespace vfx {

enum class AppTheme {
    Midnight,
    Graphite,
    Daylight
};

AppTheme defaultApplicationTheme();
AppTheme applicationThemeFromId(const QString &themeId);
QString applicationThemeId(AppTheme theme);
QString applicationThemeName(AppTheme theme);
AppTheme activeApplicationTheme();
void setActiveApplicationTheme(AppTheme theme);
QColor themeColour(const QString &role);
bool activeThemeIsLight();
QPalette applicationPalette();
QString applicationStyleSheet();

} // namespace vfx
