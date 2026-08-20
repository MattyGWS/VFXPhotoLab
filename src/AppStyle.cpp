#include "AppStyle.h"

#include <QHash>
#include <QStringList>

namespace vfx {
namespace {

struct ThemePalette {
    AppTheme theme;
    const char *id;
    const char *name;
    QHash<QString, QString> colours;
};

const ThemePalette &paletteFor(const AppTheme theme)
{
    static const ThemePalette midnight {
        AppTheme::Midnight,
        "midnight",
        "Midnight",
        {
            {QStringLiteral("window"), QStringLiteral("#17191d")},
            {QStringLiteral("panel"), QStringLiteral("#20242a")},
            {QStringLiteral("panel_alt"), QStringLiteral("#191c20")},
            {QStringLiteral("panel_selected"), QStringLiteral("#292e36")},
            {QStringLiteral("input"), QStringLiteral("#24282f")},
            {QStringLiteral("button"), QStringLiteral("#292e36")},
            {QStringLiteral("button_hover"), QStringLiteral("#343a45")},
            {QStringLiteral("button_pressed"), QStringLiteral("#20242a")},
            {QStringLiteral("border"), QStringLiteral("#343a43")},
            {QStringLiteral("border_strong"), QStringLiteral("#505969")},
            {QStringLiteral("text"), QStringLiteral("#e8eaf0")},
            {QStringLiteral("text_muted"), QStringLiteral("#9aa2af")},
            {QStringLiteral("text_inverse"), QStringLiteral("#ffffff")},
            {QStringLiteral("accent"), QStringLiteral("#7b88ff")},
            {QStringLiteral("accent_hover"), QStringLiteral("#94a0ff")},
            {QStringLiteral("selection"), QStringLiteral("#394052")},
            {QStringLiteral("progress"), QStringLiteral("#ff9d36")},
            {QStringLiteral("error"), QStringLiteral("#ef6678")},
            {QStringLiteral("success"), QStringLiteral("#83e39b")},
            {QStringLiteral("preview_background"), QStringLiteral("#111317")},
            {QStringLiteral("checker_dark"), QStringLiteral("#292d34")},
            {QStringLiteral("checker_light"), QStringLiteral("#3a3e46")},
            {QStringLiteral("scrollbar_track"), QStringLiteral("#181b20")},
            {QStringLiteral("scrollbar_handle"), QStringLiteral("#4b5360")},
            {QStringLiteral("scrollbar_hover"), QStringLiteral("#667181")},
        }
    };
    static const ThemePalette graphite {
        AppTheme::Graphite,
        "graphite",
        "Graphite",
        {
            {QStringLiteral("window"), QStringLiteral("#202020")},
            {QStringLiteral("panel"), QStringLiteral("#292929")},
            {QStringLiteral("panel_alt"), QStringLiteral("#242424")},
            {QStringLiteral("panel_selected"), QStringLiteral("#343434")},
            {QStringLiteral("input"), QStringLiteral("#303030")},
            {QStringLiteral("button"), QStringLiteral("#333333")},
            {QStringLiteral("button_hover"), QStringLiteral("#404040")},
            {QStringLiteral("button_pressed"), QStringLiteral("#282828")},
            {QStringLiteral("border"), QStringLiteral("#454545")},
            {QStringLiteral("border_strong"), QStringLiteral("#606060")},
            {QStringLiteral("text"), QStringLiteral("#eeeeee")},
            {QStringLiteral("text_muted"), QStringLiteral("#aaaaaa")},
            {QStringLiteral("text_inverse"), QStringLiteral("#111111")},
            {QStringLiteral("accent"), QStringLiteral("#49b6a8")},
            {QStringLiteral("accent_hover"), QStringLiteral("#67cbbf")},
            {QStringLiteral("selection"), QStringLiteral("#33524f")},
            {QStringLiteral("progress"), QStringLiteral("#f2a33c")},
            {QStringLiteral("error"), QStringLiteral("#e66b76")},
            {QStringLiteral("success"), QStringLiteral("#79cf91")},
            {QStringLiteral("preview_background"), QStringLiteral("#151515")},
            {QStringLiteral("checker_dark"), QStringLiteral("#2e2e2e")},
            {QStringLiteral("checker_light"), QStringLiteral("#414141")},
            {QStringLiteral("scrollbar_track"), QStringLiteral("#202020")},
            {QStringLiteral("scrollbar_handle"), QStringLiteral("#5b5b5b")},
            {QStringLiteral("scrollbar_hover"), QStringLiteral("#777777")},
        }
    };
    static const ThemePalette daylight {
        AppTheme::Daylight,
        "daylight",
        "Daylight",
        {
            {QStringLiteral("window"), QStringLiteral("#e7e9ed")},
            {QStringLiteral("panel"), QStringLiteral("#f2f3f5")},
            {QStringLiteral("panel_alt"), QStringLiteral("#e9ebee")},
            {QStringLiteral("panel_selected"), QStringLiteral("#ffffff")},
            {QStringLiteral("input"), QStringLiteral("#ffffff")},
            {QStringLiteral("button"), QStringLiteral("#f7f8fa")},
            {QStringLiteral("button_hover"), QStringLiteral("#e5e9ef")},
            {QStringLiteral("button_pressed"), QStringLiteral("#d9dee6")},
            {QStringLiteral("border"), QStringLiteral("#b8bec8")},
            {QStringLiteral("border_strong"), QStringLiteral("#8f98a6")},
            {QStringLiteral("text"), QStringLiteral("#20242a")},
            {QStringLiteral("text_muted"), QStringLiteral("#626b78")},
            {QStringLiteral("text_inverse"), QStringLiteral("#ffffff")},
            {QStringLiteral("accent"), QStringLiteral("#2f6fca")},
            {QStringLiteral("accent_hover"), QStringLiteral("#4384df")},
            {QStringLiteral("selection"), QStringLiteral("#c8dcf6")},
            {QStringLiteral("progress"), QStringLiteral("#d97818")},
            {QStringLiteral("error"), QStringLiteral("#c73e50")},
            {QStringLiteral("success"), QStringLiteral("#2f8a4d")},
            {QStringLiteral("preview_background"), QStringLiteral("#cdd2d9")},
            {QStringLiteral("checker_dark"), QStringLiteral("#c2c7ce")},
            {QStringLiteral("checker_light"), QStringLiteral("#e2e5e9")},
            {QStringLiteral("scrollbar_track"), QStringLiteral("#d3d7dd")},
            {QStringLiteral("scrollbar_handle"), QStringLiteral("#8f98a5")},
            {QStringLiteral("scrollbar_hover"), QStringLiteral("#6f7987")},
        }
    };

    switch (theme) {
    case AppTheme::Midnight: return midnight;
    case AppTheme::Daylight: return daylight;
    case AppTheme::Graphite: return graphite;
    }
    return graphite;
}

AppTheme g_activeTheme = AppTheme::Graphite;

QString colourValue(const ThemePalette &palette, const QString &role)
{
    const auto found = palette.colours.constFind(role);
    return found == palette.colours.constEnd() ? QStringLiteral("#ff00ff") : found.value();
}

} // namespace

AppTheme defaultApplicationTheme()
{
    return AppTheme::Graphite;
}

AppTheme applicationThemeFromId(const QString &themeId)
{
    const QString normalised = themeId.trimmed().toLower();
    if (normalised == QStringLiteral("midnight")) {
        return AppTheme::Midnight;
    }
    if (normalised == QStringLiteral("daylight")) {
        return AppTheme::Daylight;
    }
    return AppTheme::Graphite;
}

QString applicationThemeId(const AppTheme theme)
{
    return QString::fromLatin1(paletteFor(theme).id);
}

QString applicationThemeName(const AppTheme theme)
{
    return QString::fromLatin1(paletteFor(theme).name);
}

AppTheme activeApplicationTheme()
{
    return g_activeTheme;
}

void setActiveApplicationTheme(const AppTheme theme)
{
    g_activeTheme = theme;
}

QColor themeColour(const QString &role)
{
    return QColor(colourValue(paletteFor(g_activeTheme), role));
}

bool activeThemeIsLight()
{
    return g_activeTheme == AppTheme::Daylight;
}

QPalette applicationPalette()
{
    QPalette result;
    result.setColor(QPalette::Window, themeColour(QStringLiteral("window")));
    result.setColor(QPalette::WindowText, themeColour(QStringLiteral("text")));
    result.setColor(QPalette::Base, themeColour(QStringLiteral("input")));
    result.setColor(QPalette::AlternateBase, themeColour(QStringLiteral("panel_alt")));
    result.setColor(QPalette::ToolTipBase, themeColour(QStringLiteral("panel_selected")));
    result.setColor(QPalette::ToolTipText, themeColour(QStringLiteral("text")));
    result.setColor(QPalette::Text, themeColour(QStringLiteral("text")));
    result.setColor(QPalette::Button, themeColour(QStringLiteral("button")));
    result.setColor(QPalette::ButtonText, themeColour(QStringLiteral("text")));
    result.setColor(QPalette::BrightText, themeColour(QStringLiteral("error")));
    result.setColor(QPalette::Highlight, themeColour(QStringLiteral("selection")));
    result.setColor(QPalette::HighlightedText, themeColour(QStringLiteral("text")));
    result.setColor(QPalette::Link, themeColour(QStringLiteral("accent")));
    result.setColor(QPalette::LinkVisited, themeColour(QStringLiteral("accent_hover")));
    result.setColor(QPalette::PlaceholderText, themeColour(QStringLiteral("text_muted")));
    result.setColor(QPalette::Disabled, QPalette::Text,
                    themeColour(QStringLiteral("text_muted")));
    result.setColor(QPalette::Disabled, QPalette::ButtonText,
                    themeColour(QStringLiteral("text_muted")));
    return result;
}

QString applicationStyleSheet()
{
    const ThemePalette &palette = paletteFor(g_activeTheme);
    QString style = QStringLiteral(R"QSS(
        QWidget {
            background: ${window};
            color: ${text};
            font-size: 10pt;
        }
        QMainWindow::separator {
            background: ${border};
            width: 1px;
            height: 1px;
        }
        QMenuBar, QMenu, QStatusBar {
            background: ${panel_alt};
            color: ${text};
        }
        QMenuBar::item:selected, QMenu::item:selected {
            background: ${selection};
            color: ${text};
        }
        QMenu::separator {
            background: ${border};
            height: 1px;
            margin: 4px 7px;
        }
        QToolBar {
            background: ${panel_alt};
            border: none;
            spacing: 4px;
            padding: 4px;
        }
        QToolBar#ToolOptionsToolbar {
            border-bottom: 1px solid ${border};
            min-height: 44px;
            max-height: 44px;
            padding: 4px 7px;
            spacing: 4px;
        }
        QToolBar#ToolOptionsToolbar QLabel,
        QToolBar#ToolOptionsToolbar QCheckBox {
            min-height: 26px;
            max-height: 26px;
        }
        QToolBar#ToolOptionsToolbar QPushButton,
        QToolBar#ToolOptionsToolbar QToolButton,
        QToolBar#ToolOptionsToolbar QComboBox,
        QToolBar#ToolOptionsToolbar QSpinBox,
        QToolBar#ToolOptionsToolbar QDoubleSpinBox {
            min-height: 28px;
            max-height: 28px;
        }
        QToolBar#ToolOptionsToolbar QWidget#CompactScrubField {
            min-height: 28px;
            max-height: 28px;
        }
        /* Qt style-sheet height properties describe the content box. The
           compact spin box has a 1 px frame above and below, so its styled
           content must be 26 px inside the 28 px QWidget geometry. Asking for
           28 px of content made the complete box 30 px tall and clipped the
           lower frame in the fixed-height toolbar. */
        QToolBar#ToolOptionsToolbar QDoubleSpinBox#ScrubbableNumericField {
            min-height: 26px;
            max-height: 26px;
        }
        QToolBar#ToolsToolbar {
            border-right: 1px solid ${border};
            min-width: 42px;
            spacing: 3px;
            padding: 5px 4px;
        }
        QToolBar#ToolsToolbar QToolButton {
            min-width: 32px;
            max-width: 32px;
            min-height: 32px;
            max-height: 32px;
            padding: 3px;
            border-radius: 4px;
        }
        QToolBar#ToolsToolbar QToolButton:checked {
            background: ${selection};
            border-color: ${accent};
        }
        QWidget#DocumentStrip {
            background: ${panel_alt};
            border-top: 1px solid ${border};
        }
        QWidget#ZoomStatusControls {
            background: transparent;
        }
        QLabel#ZoomStatusLabel {
            background: transparent;
            min-width: 48px;
            padding: 0 3px;
        }
        QToolButton#ZoomStatusStepButton {
            min-width: 22px;
            max-width: 22px;
            min-height: 20px;
            max-height: 20px;
            padding: 0;
            border-radius: 3px;
            font-weight: 700;
        }
        QToolButton#ZoomStatusPresetButton,
        QToolButton#SnappingStatusButton {
            min-height: 20px;
            max-height: 20px;
            padding: 0 7px;
            border-radius: 3px;
        }
        QFrame#StatusControlSeparator {
            color: ${border};
            margin: 2px 3px;
        }
        QWidget#RulerCorner {
            background: ${panel_alt};
            border-right: 1px solid ${border};
            border-bottom: 1px solid ${border};
        }
        QListView#DocumentStripView {
            background: ${panel_alt};
            border: none;
            outline: none;
            padding: 0;
        }
        QListView#DocumentStripView::item {
            background: transparent;
            border: none;
            padding: 0;
        }
        QLabel#ToolOptionsTitle {
            font-weight: 700;
            color: ${text};
            padding-right: 4px;
        }
        QDockWidget::title {
            background: ${panel_alt};
            color: ${text};
            border-top: 1px solid ${border};
            border-bottom: 1px solid ${border};
            padding: 7px;
            text-align: left;
            font-weight: 600;
        }
        QTabWidget::pane {
            border: 1px solid ${border};
            background: ${window};
        }
        QTabBar {
            background: ${panel_alt};
        }
        QTabBar::tab {
            background: ${panel_alt};
            color: ${text_muted};
            border: 1px solid ${border};
            border-bottom-color: ${border};
            padding: 7px 12px;
            min-width: 72px;
            font-weight: 600;
        }
        QTabBar::tab:selected {
            background: ${window};
            color: ${text};
            border-bottom-color: ${window};
        }
        QTabBar::tab:hover:!selected {
            background: ${button_hover};
            color: ${text};
        }
        QTabBar#ColourTabs::tab {
            min-width: 52px;
            padding: 5px 9px;
        }
        QStackedWidget#ColourStack {
            border: 1px solid ${border};
            border-radius: 4px;
        }
        QPushButton#ColourSwatchButton {
            background: transparent;
            border: none;
            border-radius: 0;
            padding: 0;
        }
        QPushButton#ColourSwatchButton:hover {
            background: transparent;
            border: none;
        }
        QToolButton#ColourPairCornerButton {
            background: transparent;
            border: none;
            border-radius: 0;
            min-width: 18px;
            max-width: 18px;
            min-height: 18px;
            max-height: 18px;
            margin: 0;
            padding: 0;
        }
        QToolButton#ColourPairCornerButton:hover,
        QToolButton#ColourPairCornerButton:pressed,
        QToolButton#ColourPairCornerButton:focus {
            background: transparent;
            border: none;
        }
        QLineEdit#ColourHexEdit {
            min-height: 20px;
        }
        QPushButton, QToolButton {
            background: ${button};
            color: ${text};
            border: 1px solid ${border};
            border-radius: 5px;
            padding: 6px 10px;
        }
        QPushButton:hover, QToolButton:hover {
            background: ${button_hover};
            border-color: ${border_strong};
        }
        QPushButton:pressed, QToolButton:pressed {
            background: ${button_pressed};
        }
        QPushButton:checked, QToolButton:checked {
            background: ${selection};
            border-color: ${accent};
        }
        QPushButton:disabled, QToolButton:disabled {
            color: ${text_muted};
            background: ${panel_alt};
        }
        QPushButton#CompactButton {
            min-width: 26px;
            max-width: 26px;
            min-height: 24px;
            max-height: 24px;
            padding: 0;
        }
        QPushButton#CompactButton:checked {
            background: ${selection};
            border-color: ${accent};
        }
        QToolButton#LayerControlButton {
            padding: 0;
            min-width: 28px;
            max-width: 28px;
            min-height: 26px;
            max-height: 26px;
        }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QListWidget, QTreeWidget, QTextEdit, QPlainTextEdit {
            background: ${input};
            color: ${text};
            border: 1px solid ${border};
            border-radius: 4px;
            padding: 4px;
            selection-background-color: ${selection};
            selection-color: ${text};
        }
        QDoubleSpinBox#ScrubbableNumericField {
            background: ${input};
            color: ${text};
            border: 1px solid ${border};
            border-radius: 4px;
            padding: 0px 4px;
            selection-background-color: ${selection};
            selection-color: ${text};
        }
        QDoubleSpinBox#ScrubbableNumericField:focus {
            border-color: ${accent};
        }
        QComboBox::drop-down {
            border: none;
            width: 20px;
        }
        QListWidget::item {
            min-height: 28px;
            padding: 3px 6px;
        }
        QListWidget::item:selected, QTreeWidget::item:selected {
            background: ${selection};
            color: ${text};
        }
        QListWidget::item:hover:!selected, QTreeWidget::item:hover:!selected {
            background: ${button_hover};
        }
        QTreeWidget::item {
            min-height: 24px;
            padding: 1px 3px;
        }
        QHeaderView::section {
            background: ${panel_alt};
            color: ${text};
            border: 0;
            border-right: 1px solid ${border};
            border-bottom: 1px solid ${border};
            padding: 4px;
        }
        QSlider::groove:horizontal {
            height: 4px;
            background: ${border};
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 13px;
            margin: -5px 0;
            background: ${accent};
            border-radius: 6px;
        }
        QSlider::handle:horizontal:hover {
            background: ${accent_hover};
        }
        QSlider::groove:horizontal:disabled {
            background: ${button};
        }
        QSlider::handle:horizontal:disabled {
            background: ${text_muted};
        }
        QProgressBar {
            background: ${input};
            color: ${text};
            border: 1px solid ${border};
            border-radius: 4px;
            text-align: center;
        }
        QProgressBar::chunk {
            background: ${progress};
            border-radius: 3px;
        }
        QScrollBar:vertical, QScrollBar:horizontal {
            background: ${scrollbar_track};
            border: none;
        }
        QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
            background: ${scrollbar_handle};
            border-radius: 4px;
            min-width: 24px;
            min-height: 24px;
        }
        QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover {
            background: ${scrollbar_hover};
        }
        QScrollBar::add-line, QScrollBar::sub-line {
            width: 0;
            height: 0;
        }
        QToolTip {
            background: ${panel_selected};
            color: ${text};
            border: 1px solid ${border_strong};
            padding: 4px;
        }
        QLabel#WelcomeTitle {
            font-size: 28pt;
            font-weight: 700;
        }
        QLabel#WelcomeSubtitle {
            color: ${text_muted};
            font-size: 11pt;
        }
        QLabel#SectionTitle, QLabel#SectionTitleCompact {
            font-weight: 700;
            color: ${text};
        }
        QLabel#SectionTitle {
            font-size: 11pt;
        }
        QLabel#MutedLabel {
            color: ${text_muted};
        }
        QLabel#WarningLabel {
            color: ${error};
        }
        QLabel#SuccessLabel {
            color: ${success};
        }
    )QSS");

    for (auto it = palette.colours.constBegin(); it != palette.colours.constEnd(); ++it) {
        style.replace(QStringLiteral("${") + it.key() + QLatin1Char('}'), it.value());
    }
    return style;
}

} // namespace vfx
