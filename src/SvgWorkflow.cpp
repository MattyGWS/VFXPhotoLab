#include "SvgWorkflow.h"

#include "TextLayer.h"
#include "VectorLayer.h"

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QMap>
#include <QObject>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <QtMath>
#include <QXmlStreamAttributes>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>
#include <functional>

namespace vfx {
namespace {

constexpr double SvgDpi = 96.0;
constexpr double MaximumCoordinate = 1.0e9;
constexpr int MaximumCanvasExtent = 32768;
constexpr int MaximumWarningCount = 64;
constexpr int MaximumCssValueLength = 1 << 20;

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) *errorMessage = message;
}

bool finiteBounded(const double value, const double limit = MaximumCoordinate)
{
    return std::isfinite(value) && std::abs(value) <= limit;
}

QString numberText(const double value)
{
    if (!std::isfinite(value)) return QStringLiteral("0");
    double cleaned = std::abs(value) < 1.0e-12 ? 0.0 : value;
    return QString::number(cleaned, 'g', 14);
}

QString transformText(const QTransform &transform)
{
    if (transform.isIdentity()) return {};
    return QStringLiteral("matrix(%1 %2 %3 %4 %5 %6)")
        .arg(numberText(transform.m11()), numberText(transform.m12()),
             numberText(transform.m21()), numberText(transform.m22()),
             numberText(transform.dx()), numberText(transform.dy()));
}

QString colourText(const QColor &colour)
{
    return colour.toRgb().name(QColor::HexRgb).toUpper();
}

QString compactWarningKey(const QString &warning)
{
    QString key = warning;
    key.replace(QRegularExpression(QStringLiteral("\\d+")), QStringLiteral("#"));
    return key;
}

class WarningSink final {
public:
    void add(const QString &warning)
    {
        if (warning.trimmed().isEmpty()) return;
        const QString key = compactWarningKey(warning);
        if (m_keys.contains(key)) return;
        m_keys.insert(key);
        if (m_warnings.size() < MaximumWarningCount) {
            m_warnings.push_back(warning);
        } else {
            ++m_suppressed;
        }
    }

    QStringList take()
    {
        if (m_suppressed > 0) {
            m_warnings.push_back(QObject::tr("%1 additional SVG warnings were suppressed.")
                                     .arg(m_suppressed));
        }
        return std::move(m_warnings);
    }

private:
    QStringList m_warnings;
    QSet<QString> m_keys;
    int m_suppressed = 0;
};

std::optional<double> parseNumber(const QString &text)
{
    bool ok = false;
    const double value = text.trimmed().toDouble(&ok);
    if (!ok || !finiteBounded(value)) return std::nullopt;
    return value;
}

std::optional<double> parseLength(const QString &raw,
                                  const double percentReference = 0.0,
                                  const bool permitPercent = true)
{
    QString text = raw.trimmed();
    if (text.isEmpty() || text.size() > 128) return std::nullopt;
    static const QRegularExpression pattern(
        QStringLiteral(R"(^([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)\s*([a-zA-Z%]*)$)"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) return std::nullopt;
    bool ok = false;
    double value = match.captured(1).toDouble(&ok);
    if (!ok || !finiteBounded(value)) return std::nullopt;
    const QString unit = match.captured(2).toLower();
    if (unit.isEmpty() || unit == QStringLiteral("px")) {
        return value;
    }
    if (unit == QStringLiteral("pt")) value *= SvgDpi / 72.0;
    else if (unit == QStringLiteral("pc")) value *= SvgDpi / 6.0;
    else if (unit == QStringLiteral("in")) value *= SvgDpi;
    else if (unit == QStringLiteral("cm")) value *= SvgDpi / 2.54;
    else if (unit == QStringLiteral("mm")) value *= SvgDpi / 25.4;
    else if (unit == QStringLiteral("q")) value *= SvgDpi / 101.6;
    else if (unit == QStringLiteral("%") && permitPercent) value *= percentReference / 100.0;
    else return std::nullopt;
    return finiteBounded(value) ? std::optional<double>(value) : std::nullopt;
}

QVector<double> parseNumberList(const QString &text,
                                const int maximumCount = 100000,
                                bool *complete = nullptr)
{
    if (complete) *complete = false;
    QVector<double> values;
    qsizetype offset = 0;
    const qsizetype size = text.size();
    while (offset < size && values.size() < maximumCount) {
        while (offset < size && (text.at(offset).isSpace() || text.at(offset) == QLatin1Char(','))) {
            ++offset;
        }
        if (offset >= size) break;
        qsizetype end = offset;
        if (text.at(end) == QLatin1Char('+') || text.at(end) == QLatin1Char('-')) ++end;
        bool digits = false;
        while (end < size && text.at(end).isDigit()) { digits = true; ++end; }
        if (end < size && text.at(end) == QLatin1Char('.')) {
            ++end;
            while (end < size && text.at(end).isDigit()) { digits = true; ++end; }
        }
        if (!digits) break;
        if (end < size && (text.at(end) == QLatin1Char('e') || text.at(end) == QLatin1Char('E'))) {
            qsizetype exponent = end + 1;
            if (exponent < size && (text.at(exponent) == QLatin1Char('+')
                                    || text.at(exponent) == QLatin1Char('-'))) ++exponent;
            const qsizetype exponentStart = exponent;
            while (exponent < size && text.at(exponent).isDigit()) ++exponent;
            if (exponent > exponentStart) end = exponent;
        }
        bool ok = false;
        const double value = text.mid(offset, end - offset).toDouble(&ok);
        if (!ok || !finiteBounded(value)) break;
        values.push_back(value);
        offset = end;
    }
    while (offset < size && (text.at(offset).isSpace() || text.at(offset) == QLatin1Char(','))) {
        ++offset;
    }
    if (complete) *complete = offset == size;
    return values;
}

QColor parseRgbFunction(const QString &text, bool *ok)
{
    if (ok) *ok = false;
    const int open = text.indexOf(QLatin1Char('('));
    const int close = text.lastIndexOf(QLatin1Char(')'));
    if (open < 0 || close <= open) return {};
    QString body = text.mid(open + 1, close - open - 1);
    body.replace(QLatin1Char('/'), QLatin1Char(','));
    const QStringList parts = body.split(QRegularExpression(QStringLiteral("\\s*,\\s*|\\s+")),
                                         Qt::SkipEmptyParts);
    const bool rgba = text.startsWith(QStringLiteral("rgba"), Qt::CaseInsensitive);
    if (parts.size() != (rgba ? 4 : 3)) return {};
    auto component = [](const QString &part, bool alpha, bool *componentOk) {
        QString token = part.trimmed();
        bool percent = token.endsWith(QLatin1Char('%'));
        if (percent) token.chop(1);
        bool localOk = false;
        double value = token.toDouble(&localOk);
        if (!localOk || !std::isfinite(value)) {
            *componentOk = false;
            return 0.0;
        }
        if (percent) value /= 100.0;
        else if (!alpha) value /= 255.0;
        *componentOk = true;
        return std::clamp(value, 0.0, 1.0);
    };
    bool good = true;
    bool componentOk = false;
    const double red = component(parts.at(0), false, &componentOk); good = good && componentOk;
    const double green = component(parts.at(1), false, &componentOk); good = good && componentOk;
    const double blue = component(parts.at(2), false, &componentOk); good = good && componentOk;
    double alpha = 1.0;
    if (rgba) { alpha = component(parts.at(3), true, &componentOk); good = good && componentOk; }
    if (!good) return {};
    QColor colour;
    colour.setRgbF(red, green, blue, alpha);
    if (ok) *ok = colour.isValid();
    return colour;
}

QColor parseColour(const QString &raw, const QColor &currentColour, bool *ok)
{
    if (ok) *ok = false;
    const QString text = raw.trimmed();
    if (text.compare(QStringLiteral("currentColor"), Qt::CaseInsensitive) == 0) {
        if (ok) *ok = currentColour.isValid();
        return currentColour;
    }
    if (text.startsWith(QStringLiteral("rgb("), Qt::CaseInsensitive)
        || text.startsWith(QStringLiteral("rgba("), Qt::CaseInsensitive)) {
        return parseRgbFunction(text, ok);
    }
    QColor colour(text);
    if (colour.isValid()) {
        if (ok) *ok = true;
        return colour;
    }
    return {};
}

struct SvgStyle {
    bool fillEnabled = true;
    QColor fill = QColor(Qt::black);
    double fillOpacity = 1.0;
    // SVG defaults to nonzero winding. Photo Lab persists both nonzero and
    // even-odd rules, so closed compound subpaths can remain one editable
    // object without changing standard-SVG appearance.
    bool evenOddFill = false;
    bool strokeEnabled = false;
    QColor stroke = QColor(Qt::black);
    double strokeOpacity = 1.0;
    double strokeWidth = 1.0;
    VectorStrokeCap strokeCap = VectorStrokeCap::Butt;
    VectorStrokeJoin strokeJoin = VectorStrokeJoin::Miter;
    double miterLimit = 4.0;
    VectorStrokePattern strokePattern = VectorStrokePattern::Solid;
    double dashLength = 8.0;
    double gapLength = 8.0;
    double dashOffset = 0.0;
    VectorArrowheadType startArrowhead = VectorArrowheadType::None;
    VectorArrowheadType endArrowhead = VectorArrowheadType::None;
    double startArrowScale = 1.0;
    double endArrowScale = 1.0;
    QColor currentColour = QColor(Qt::black);
    QString fontFamily;
    QString fontStyle;
    int fontWeight = 400;
    bool italic = false;
    double fontSize = 16.0;
    double letterSpacing = 0.0;
    double lineHeight = 1.2;
    QString textAnchor = QStringLiteral("start");
    bool visible = true;
};

QHash<QString, QString> presentationMap(const QXmlStreamAttributes &attributes)
{
    QHash<QString, QString> properties;
    for (const QXmlStreamAttribute &attribute : attributes) {
        properties.insert(attribute.name().toString().toLower(), attribute.value().toString());
    }
    const QString inlineStyle = properties.value(QStringLiteral("style"));
    if (!inlineStyle.isEmpty() && inlineStyle.size() <= MaximumCssValueLength) {
        const QStringList declarations = inlineStyle.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString &declaration : declarations) {
            const qsizetype colon = declaration.indexOf(QLatin1Char(':'));
            if (colon <= 0) continue;
            properties.insert(declaration.left(colon).trimmed().toLower(),
                              declaration.mid(colon + 1).trimmed());
        }
    }
    return properties;
}

bool parseOpacity(const QString &raw, double *value)
{
    QString text = raw.trimmed();
    if (text.isEmpty()) return false;
    const bool percent = text.endsWith(QLatin1Char('%'));
    if (percent) text.chop(1);
    const std::optional<double> parsed = parseNumber(text);
    if (!parsed) return false;
    *value = std::clamp(percent ? *parsed / 100.0 : *parsed, 0.0, 1.0);
    return true;
}

SvgStyle inheritedStyle(const SvgStyle &parent,
                        const QXmlStreamAttributes &attributes,
                        WarningSink *warnings)
{
    SvgStyle style = parent;
    const QHash<QString, QString> properties = presentationMap(attributes);

    const QString visibility = properties.value(QStringLiteral("visibility")).trimmed().toLower();
    if (visibility == QStringLiteral("hidden") || visibility == QStringLiteral("collapse")) {
        style.visible = false;
    } else if (visibility == QStringLiteral("visible")) {
        style.visible = true;
    }

    if (properties.contains(QStringLiteral("color"))) {
        bool ok = false;
        const QColor colour = parseColour(properties.value(QStringLiteral("color")),
                                          style.currentColour, &ok);
        if (ok) style.currentColour = colour;
    }

    if (properties.contains(QStringLiteral("fill"))) {
        const QString fill = properties.value(QStringLiteral("fill")).trimmed();
        if (fill.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) {
            style.fillEnabled = false;
        } else if (fill.startsWith(QStringLiteral("url("), Qt::CaseInsensitive)) {
            style.fillEnabled = false;
            if (warnings) warnings->add(QObject::tr("Gradient and paint-server fills are not editable yet and were omitted."));
        } else {
            bool ok = false;
            const QColor colour = parseColour(fill, style.currentColour, &ok);
            if (ok) { style.fillEnabled = true; style.fill = colour; }
            else if (warnings) warnings->add(QObject::tr("An unsupported SVG fill colour was replaced with the inherited fill."));
        }
    }
    parseOpacity(properties.value(QStringLiteral("fill-opacity")), &style.fillOpacity);
    if (properties.contains(QStringLiteral("fill-rule"))) {
        const QString fillRule = properties.value(
            QStringLiteral("fill-rule")).trimmed().toLower();
        if (fillRule == QStringLiteral("evenodd")) {
            style.evenOddFill = true;
        } else if (fillRule == QStringLiteral("nonzero")) {
            style.evenOddFill = false;
        } else if (!fillRule.isEmpty() && warnings) {
            warnings->add(QObject::tr(
                "An unsupported SVG fill rule was replaced with the inherited fill rule."));
        }
    }

    if (properties.contains(QStringLiteral("stroke"))) {
        const QString stroke = properties.value(QStringLiteral("stroke")).trimmed();
        if (stroke.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) {
            style.strokeEnabled = false;
        } else if (stroke.startsWith(QStringLiteral("url("), Qt::CaseInsensitive)) {
            style.strokeEnabled = false;
            if (warnings) warnings->add(QObject::tr("Gradient and paint-server strokes are not editable yet and were omitted."));
        } else {
            bool ok = false;
            const QColor colour = parseColour(stroke, style.currentColour, &ok);
            if (ok) { style.strokeEnabled = true; style.stroke = colour; }
            else if (warnings) warnings->add(QObject::tr("An unsupported SVG stroke colour was replaced with the inherited stroke."));
        }
    }
    parseOpacity(properties.value(QStringLiteral("stroke-opacity")), &style.strokeOpacity);
    if (const auto width = parseLength(properties.value(QStringLiteral("stroke-width")), 0.0, false)) {
        style.strokeWidth = std::clamp(*width, 0.0, 100000.0);
        style.strokeEnabled = style.strokeEnabled && style.strokeWidth > 0.0;
    }
    const QString cap = properties.value(QStringLiteral("stroke-linecap")).trimmed().toLower();
    if (cap == QStringLiteral("round")) style.strokeCap = VectorStrokeCap::Round;
    else if (cap == QStringLiteral("square")) style.strokeCap = VectorStrokeCap::Square;
    else if (cap == QStringLiteral("butt")) style.strokeCap = VectorStrokeCap::Butt;
    const QString join = properties.value(QStringLiteral("stroke-linejoin")).trimmed().toLower();
    if (join == QStringLiteral("round")) style.strokeJoin = VectorStrokeJoin::Round;
    else if (join == QStringLiteral("bevel")) style.strokeJoin = VectorStrokeJoin::Bevel;
    else if (join == QStringLiteral("miter") || join == QStringLiteral("mitre")) style.strokeJoin = VectorStrokeJoin::Miter;
    if (const auto miter = parseNumber(properties.value(QStringLiteral("stroke-miterlimit")))) {
        style.miterLimit = std::clamp(*miter, 1.0, 1000.0);
    }
    if (properties.contains(QStringLiteral("stroke-dasharray"))) {
        const QString dashArray = properties.value(
            QStringLiteral("stroke-dasharray")).trimmed();
        if (dashArray.isEmpty()
            || dashArray.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) {
            style.strokePattern = VectorStrokePattern::Solid;
        } else {
            QString tokenText = dashArray;
            tokenText.replace(QLatin1Char(','), QLatin1Char(' '));
            const QStringList tokens = tokenText.split(
                QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            QVector<double> lengths;
            lengths.reserve(std::min(tokens.size(), qsizetype(64)));
            bool validDashArray = !tokens.isEmpty() && tokens.size() <= 64;
            for (const QString &token : tokens) {
                const std::optional<double> length = parseLength(token, 0.0, false);
                if (!length || *length < 0.0) {
                    validDashArray = false;
                    break;
                }
                lengths.push_back(*length);
            }
            if (validDashArray && !lengths.isEmpty()) {
                const double dash = lengths.at(0);
                const double gap = lengths.size() == 1 ? dash : lengths.at(1);
                if (dash > 0.0 && gap > 0.0) {
                    style.strokePattern = VectorStrokePattern::Dashed;
                    style.dashLength = std::clamp(dash, 0.01, 1.0e6);
                    style.gapLength = std::clamp(gap, 0.01, 1.0e6);
                    if (lengths.size() > 2 && warnings) {
                        warnings->add(QObject::tr(
                            "Complex SVG dash sequences are approximated using their first dash and gap lengths."));
                    }
                } else {
                    style.strokePattern = VectorStrokePattern::Solid;
                    if (warnings) warnings->add(QObject::tr(
                        "An SVG dash sequence containing a zero-length dash or gap was imported as a solid stroke."));
                }
            } else if (warnings) {
                warnings->add(QObject::tr(
                    "An unsupported SVG dash sequence was imported as a solid stroke."));
            }
        }
    }
    if (properties.contains(QStringLiteral("stroke-dashoffset"))) {
        const std::optional<double> offset = parseLength(
            properties.value(QStringLiteral("stroke-dashoffset")), 0.0, false);
        if (offset) style.dashOffset = std::clamp(*offset, -1.0e9, 1.0e9);
        else if (warnings) warnings->add(QObject::tr(
            "An unsupported SVG dash offset was reset to zero."));
    }

    const auto parseArrowhead = [&properties, warnings](const QString &name,
                                                        VectorArrowheadType inherited) {
        if (!properties.contains(name)) return inherited;
        bool arrowOk = false;
        const VectorArrowheadType parsed = vectorArrowheadTypeFromString(
            properties.value(name), &arrowOk);
        if (!arrowOk && warnings) {
            warnings->add(QObject::tr(
                "An unsupported VFX SVG arrowhead was replaced with None."));
        }
        return arrowOk ? parsed : VectorArrowheadType::None;
    };
    style.startArrowhead = parseArrowhead(
        QStringLiteral("data-vfx-start-arrowhead"), style.startArrowhead);
    style.endArrowhead = parseArrowhead(
        QStringLiteral("data-vfx-end-arrowhead"), style.endArrowhead);
    if (const auto scale = parseNumber(properties.value(
            QStringLiteral("data-vfx-start-arrow-scale")))) {
        style.startArrowScale = std::clamp(*scale, 0.1, 10.0);
    }
    if (const auto scale = parseNumber(properties.value(
            QStringLiteral("data-vfx-end-arrow-scale")))) {
        style.endArrowScale = std::clamp(*scale, 0.1, 10.0);
    }

    if (properties.contains(QStringLiteral("font-family"))) {
        QString family = properties.value(QStringLiteral("font-family")).trimmed();
        if (family.startsWith(QLatin1Char('\'')) || family.startsWith(QLatin1Char('"'))) family.remove(0, 1);
        if (family.endsWith(QLatin1Char('\'')) || family.endsWith(QLatin1Char('"'))) family.chop(1);
        const qsizetype comma = family.indexOf(QLatin1Char(','));
        if (comma > 0) family = family.left(comma).trimmed();
        style.fontFamily = family;
    }
    const QString fontStyle = properties.value(QStringLiteral("font-style")).trimmed().toLower();
    if (!fontStyle.isEmpty()) {
        style.fontStyle = fontStyle.left(128);
        style.italic = fontStyle == QStringLiteral("italic") || fontStyle == QStringLiteral("oblique");
    }
    if (properties.contains(QStringLiteral("font-weight"))) {
        const QString token = properties.value(QStringLiteral("font-weight")).trimmed().toLower();
        bool ok = false;
        int weight = token.toInt(&ok);
        if (token == QStringLiteral("normal")) { weight = 400; ok = true; }
        else if (token == QStringLiteral("bold")) { weight = 700; ok = true; }
        else if (token == QStringLiteral("bolder")) { weight = 700; ok = true; }
        else if (token == QStringLiteral("lighter")) { weight = 300; ok = true; }
        if (ok) style.fontWeight = std::clamp(weight, 1, 1000);
    }
    if (const auto size = parseLength(properties.value(QStringLiteral("font-size")), style.fontSize)) {
        style.fontSize = std::clamp(*size, 1.0, 4096.0);
    }
    const QString spacing = properties.value(QStringLiteral("letter-spacing")).trimmed().toLower();
    if (spacing == QStringLiteral("normal")) {
        style.letterSpacing = 0.0;
    } else if (const auto parsedSpacing = parseLength(spacing, style.fontSize, false)) {
        style.letterSpacing = std::clamp(*parsedSpacing, -1000.0, 10000.0);
    }
    const QString lineHeight = properties.value(QStringLiteral("line-height")).trimmed().toLower();
    if (lineHeight == QStringLiteral("normal")) {
        style.lineHeight = 1.2;
    } else if (const auto ratio = parseNumber(lineHeight)) {
        style.lineHeight = std::clamp(*ratio, 0.1, 20.0);
    } else if (const auto absolute = parseLength(lineHeight, style.fontSize, true)) {
        style.lineHeight = std::clamp(*absolute / std::max(1.0, style.fontSize), 0.1, 20.0);
    }
    const QString anchor = properties.value(QStringLiteral("text-anchor")).trimmed().toLower();
    if (anchor == QStringLiteral("start") || anchor == QStringLiteral("middle")
        || anchor == QStringLiteral("end")) style.textAnchor = anchor;
    return style;
}

bool localDisplayVisible(const QXmlStreamAttributes &attributes)
{
    const QString display = presentationMap(attributes)
        .value(QStringLiteral("display")).trimmed().toLower();
    return display != QStringLiteral("none");
}

double localOpacity(const QXmlStreamAttributes &attributes)
{
    const QHash<QString, QString> properties = presentationMap(attributes);
    double opacity = 1.0;
    parseOpacity(properties.value(QStringLiteral("opacity")), &opacity);
    return opacity;
}

BlendMode blendModeFromCss(const QString &raw)
{
    const QString token = raw.trimmed().toLower();
    if (token == QStringLiteral("multiply")) return BlendMode::Multiply;
    if (token == QStringLiteral("screen")) return BlendMode::Screen;
    if (token == QStringLiteral("overlay")) return BlendMode::Overlay;
    if (token == QStringLiteral("darken")) return BlendMode::Darken;
    if (token == QStringLiteral("lighten")) return BlendMode::Lighten;
    if (token == QStringLiteral("color-dodge")) return BlendMode::ColourDodge;
    if (token == QStringLiteral("color-burn")) return BlendMode::ColourBurn;
    if (token == QStringLiteral("difference")) return BlendMode::Difference;
    if (token == QStringLiteral("exclusion")) return BlendMode::Exclusion;
    if (token == QStringLiteral("plus-lighter")) return BlendMode::Add;
    return BlendMode::Copy;
}

QString blendModeCss(const BlendMode mode)
{
    switch (mode) {
    case BlendMode::Multiply: return QStringLiteral("multiply");
    case BlendMode::Screen: return QStringLiteral("screen");
    case BlendMode::Overlay: return QStringLiteral("overlay");
    case BlendMode::Darken: return QStringLiteral("darken");
    case BlendMode::Lighten: return QStringLiteral("lighten");
    case BlendMode::ColourDodge: return QStringLiteral("color-dodge");
    case BlendMode::ColourBurn: return QStringLiteral("color-burn");
    case BlendMode::Difference: return QStringLiteral("difference");
    case BlendMode::Exclusion: return QStringLiteral("exclusion");
    case BlendMode::Add: return QStringLiteral("plus-lighter");
    case BlendMode::Subtract:
    case BlendMode::Copy: return {};
    }
    return {};
}

QString attributeValue(const QXmlStreamAttributes &attributes, const QString &name)
{
    for (const QXmlStreamAttribute &attribute : attributes) {
        if (attribute.name().toString().compare(name, Qt::CaseInsensitive) == 0) {
            return attribute.value().toString();
        }
    }
    return {};
}

QTransform parseTransform(const QString &raw, bool *ok)
{
    if (ok) *ok = true;
    QTransform result;
    const QString text = raw.trimmed();
    qsizetype offset = 0;
    int operations = 0;
    while (offset < text.size()) {
        while (offset < text.size() && (text.at(offset).isSpace() || text.at(offset) == QLatin1Char(','))) ++offset;
        if (offset >= text.size()) break;
        qsizetype nameEnd = offset;
        while (nameEnd < text.size() && text.at(nameEnd).isLetter()) ++nameEnd;
        const QString name = text.mid(offset, nameEnd - offset).toLower();
        while (nameEnd < text.size() && text.at(nameEnd).isSpace()) ++nameEnd;
        if (name.isEmpty() || nameEnd >= text.size() || text.at(nameEnd) != QLatin1Char('(')) {
            if (ok) *ok = false;
            return {};
        }
        const qsizetype close = text.indexOf(QLatin1Char(')'), nameEnd + 1);
        if (close < 0) {
            if (ok) *ok = false;
            return {};
        }
        bool argumentsComplete = false;
        const QVector<double> values = parseNumberList(
            text.mid(nameEnd + 1, close - nameEnd - 1), 16, &argumentsComplete);
        QTransform operation;
        bool valid = argumentsComplete;
        if (name == QStringLiteral("matrix") && values.size() == 6) {
            operation = QTransform(values.at(0), values.at(1), values.at(2),
                                   values.at(3), values.at(4), values.at(5));
        } else if (name == QStringLiteral("translate") && (values.size() == 1 || values.size() == 2)) {
            operation.translate(values.at(0), values.size() == 2 ? values.at(1) : 0.0);
        } else if (name == QStringLiteral("scale") && (values.size() == 1 || values.size() == 2)) {
            operation.scale(values.at(0), values.size() == 2 ? values.at(1) : values.at(0));
        } else if (name == QStringLiteral("rotate") && (values.size() == 1 || values.size() == 3)) {
            if (values.size() == 3) {
                operation.translate(values.at(1), values.at(2));
                operation.rotate(values.at(0));
                operation.translate(-values.at(1), -values.at(2));
            } else operation.rotate(values.at(0));
        } else if (name == QStringLiteral("skewx") && values.size() == 1) {
            operation.shear(std::tan(values.at(0) * std::numbers::pi / 180.0), 0.0);
        } else if (name == QStringLiteral("skewy") && values.size() == 1) {
            operation.shear(0.0, std::tan(values.at(0) * std::numbers::pi / 180.0));
        } else {
            valid = false;
        }
        if (!valid
            || !finiteBounded(operation.m11()) || !finiteBounded(operation.m12())
            || !finiteBounded(operation.m21()) || !finiteBounded(operation.m22())
            || !finiteBounded(operation.dx()) || !finiteBounded(operation.dy())) {
            if (ok) *ok = false;
            return {};
        }
        // SVG lists use column-vector composition. QTransform uses row-vector
        // ordering, so each following SVG operation is prepended here.
        result = operation * result;
        if (!finiteBounded(result.m11()) || !finiteBounded(result.m12())
            || !finiteBounded(result.m13()) || !finiteBounded(result.m21())
            || !finiteBounded(result.m22()) || !finiteBounded(result.m23())
            || !finiteBounded(result.m31()) || !finiteBounded(result.m32())
            || !finiteBounded(result.m33())) {
            if (ok) *ok = false;
            return {};
        }
        offset = close + 1;
        if (++operations > 1024) {
            if (ok) *ok = false;
            return {};
        }
    }
    return result;
}

VectorFill fillFromStyle(const SvgStyle &style)
{
    VectorFill fill;
    fill.enabled = style.fillEnabled;
    fill.colour = style.fill;
    fill.opacity = style.fillOpacity;
    fill.normalise();
    return fill;
}

VectorStroke strokeFromStyle(const SvgStyle &style, const bool openPath)
{
    VectorStroke stroke;
    stroke.enabled = style.strokeEnabled;
    stroke.colour = style.stroke;
    stroke.opacity = style.strokeOpacity;
    stroke.width = style.strokeWidth;
    stroke.alignment = VectorStrokeAlignment::Centre;
    stroke.cap = style.strokeCap;
    stroke.join = style.strokeJoin;
    stroke.miterLimit = style.miterLimit;
    stroke.pattern = style.strokePattern;
    stroke.dashLength = style.dashLength;
    stroke.gapLength = style.gapLength;
    stroke.dashOffset = style.dashOffset;
    stroke.startArrowhead = style.startArrowhead;
    stroke.endArrowhead = style.endArrowhead;
    stroke.startArrowScale = style.startArrowScale;
    stroke.endArrowScale = style.endArrowScale;
    stroke.normalise(openPath);
    return stroke;
}

void regenerateVectorIds(VectorLayerData *data)
{
    if (!data) return;
    for (VectorShape &shape : data->objects) {
        shape.id = QUuid::createUuid();
        for (VectorPathNode &node : shape.bezierPath.nodes) node.id = QUuid::createUuid();
        for (VectorBezierPath &path : shape.additionalBezierPaths) {
            for (VectorPathNode &node : path.nodes) node.id = QUuid::createUuid();
        }
        shape.revision = std::max<quint64>(1, shape.revision + 1);
    }
    data->normalise();
}

void regenerateLayerIds(LayerNode *layer)
{
    if (!layer) return;
    layer->id = QUuid::createUuid();
    layer->revision = std::max<quint64>(1, layer->revision + 1);
    if (layer->type == LayerType::Vector) regenerateVectorIds(&layer->vectorData);
    if (layer->type == LayerType::Text) {
        layer->textData.revision = std::max<quint64>(1, layer->textData.revision + 1);
    }
    for (LayerNode &child : layer->children) regenerateLayerIds(&child);
}

QString base64Json(const QJsonObject &object)
{
    return QString::fromLatin1(QJsonDocument(object).toJson(QJsonDocument::Compact)
                                   .toBase64(QByteArray::Base64UrlEncoding
                                             | QByteArray::OmitTrailingEquals));
}

QJsonObject jsonFromBase64(const QString &text, bool *ok)
{
    if (ok) *ok = false;
    if (text.size() > SvgWorkflow::MaximumFileBytes * 2) return {};
    const QByteArray decoded = QByteArray::fromBase64(text.toLatin1(), QByteArray::Base64UrlEncoding);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(decoded, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return {};
    if (ok) *ok = true;
    return document.object();
}

class SvgPathParser final {
public:
    explicit SvgPathParser(QString data) : m_data(std::move(data)) {}

    bool parse(QVector<VectorBezierPath> *paths, QString *errorMessage)
    {
        if (!paths) return false;
        paths->clear();
        QChar command;
        while (true) {
            skipSeparators();
            if (atEnd()) break;
            if (m_data.at(m_offset).isLetter()) {
                command = m_data.at(m_offset++);
            } else if (command.isNull()) {
                return fail(errorMessage, QObject::tr("SVG path data begins without a command."));
            }
            const bool relative = command.isLower();
            const QChar upper = command.toUpper();
            if (upper == QLatin1Char('Z')) {
                if (!closeSubpath(errorMessage)) return false;
                command = {};
                continue;
            }
            if (upper == QLatin1Char('M')) {
                QPointF point;
                if (!readPoint(relative, &point)) return fail(errorMessage, QObject::tr("Invalid SVG move command."));
                if (!startSubpath(point, errorMessage)) return false;
                m_previousCommand = upper;
                // Additional coordinate pairs after moveto are lineto.
                while (hasNumberAhead()) {
                    if (!readPoint(relative, &point) || !lineTo(point, errorMessage)) {
                        return fail(errorMessage, QObject::tr("Invalid SVG moveto continuation."));
                    }
                    m_previousCommand = QLatin1Char('L');
                }
                continue;
            }
            if (m_current.nodes.isEmpty()) return fail(errorMessage, QObject::tr("SVG path drawing command appears before moveto."));

            bool consumed = false;
            do {
                consumed = false;
                if (upper == QLatin1Char('L')) {
                    QPointF point;
                    if (!readPoint(relative, &point)) break;
                    if (!lineTo(point, errorMessage)) return false;
                    consumed = true;
                } else if (upper == QLatin1Char('H')) {
                    double value = 0.0;
                    if (!readNumber(&value)) break;
                    QPointF point = currentPoint();
                    point.setX(relative ? point.x() + value : value);
                    if (!lineTo(point, errorMessage)) return false;
                    consumed = true;
                } else if (upper == QLatin1Char('V')) {
                    double value = 0.0;
                    if (!readNumber(&value)) break;
                    QPointF point = currentPoint();
                    point.setY(relative ? point.y() + value : value);
                    if (!lineTo(point, errorMessage)) return false;
                    consumed = true;
                } else if (upper == QLatin1Char('C')) {
                    QPointF c1, c2, point;
                    if (!readPoint(relative, &c1) || !readPoint(relative, &c2)
                        || !readPoint(relative, &point)) break;
                    if (!cubicTo(c1, c2, point, errorMessage)) return false;
                    m_lastCubicControl = c2;
                    consumed = true;
                } else if (upper == QLatin1Char('S')) {
                    QPointF c2, point;
                    if (!readPoint(relative, &c2) || !readPoint(relative, &point)) break;
                    const QPointF c1 = (m_previousCommand == QLatin1Char('C')
                                         || m_previousCommand == QLatin1Char('S'))
                        ? currentPoint() * 2.0 - m_lastCubicControl
                        : currentPoint();
                    if (!cubicTo(c1, c2, point, errorMessage)) return false;
                    m_lastCubicControl = c2;
                    consumed = true;
                } else if (upper == QLatin1Char('Q')) {
                    QPointF q, point;
                    if (!readPoint(relative, &q) || !readPoint(relative, &point)) break;
                    const QPointF start = currentPoint();
                    const QPointF c1 = start + (q - start) * (2.0 / 3.0);
                    const QPointF c2 = point + (q - point) * (2.0 / 3.0);
                    if (!cubicTo(c1, c2, point, errorMessage)) return false;
                    m_lastQuadraticControl = q;
                    consumed = true;
                } else if (upper == QLatin1Char('T')) {
                    QPointF point;
                    if (!readPoint(relative, &point)) break;
                    const QPointF q = (m_previousCommand == QLatin1Char('Q')
                                        || m_previousCommand == QLatin1Char('T'))
                        ? currentPoint() * 2.0 - m_lastQuadraticControl
                        : currentPoint();
                    const QPointF start = currentPoint();
                    const QPointF c1 = start + (q - start) * (2.0 / 3.0);
                    const QPointF c2 = point + (q - point) * (2.0 / 3.0);
                    if (!cubicTo(c1, c2, point, errorMessage)) return false;
                    m_lastQuadraticControl = q;
                    consumed = true;
                } else if (upper == QLatin1Char('A')) {
                    double rx = 0.0, ry = 0.0, rotation = 0.0, largeFlag = 0.0, sweepFlag = 0.0;
                    QPointF point;
                    if (!readNumber(&rx) || !readNumber(&ry) || !readNumber(&rotation)
                        || !readNumber(&largeFlag) || !readNumber(&sweepFlag)
                        || !readPoint(relative, &point)) break;
                    const bool validLargeFlag = largeFlag == 0.0 || largeFlag == 1.0;
                    const bool validSweepFlag = sweepFlag == 0.0 || sweepFlag == 1.0;
                    if (!validLargeFlag || !validSweepFlag) {
                        return fail(errorMessage, QObject::tr("SVG arc flags must be exactly 0 or 1."));
                    }
                    if (!arcTo(std::abs(rx), std::abs(ry), rotation,
                               largeFlag == 1.0, sweepFlag == 1.0,
                               point, errorMessage)) return false;
                    consumed = true;
                } else {
                    return fail(errorMessage, QObject::tr("Unsupported SVG path command %1.").arg(upper));
                }
                if (consumed) m_previousCommand = upper;
            } while (hasNumberAhead());
            if (!consumed) return fail(errorMessage, QObject::tr("Invalid or incomplete SVG path command %1.").arg(upper));
        }
        if (!finishSubpath(errorMessage)) return false;
        *paths = std::move(m_paths);
        return !paths->isEmpty();
    }

private:
    bool fail(QString *errorMessage, const QString &message)
    {
        setError(errorMessage, message);
        return false;
    }

    void skipSeparators()
    {
        while (m_offset < m_data.size()
               && (m_data.at(m_offset).isSpace() || m_data.at(m_offset) == QLatin1Char(','))) ++m_offset;
    }

    bool atEnd() const { return m_offset >= m_data.size(); }

    bool hasNumberAhead()
    {
        skipSeparators();
        if (atEnd()) return false;
        const QChar character = m_data.at(m_offset);
        return character.isDigit() || character == QLatin1Char('+')
            || character == QLatin1Char('-') || character == QLatin1Char('.');
    }

    bool readNumber(double *value)
    {
        skipSeparators();
        if (atEnd()) return false;
        const qsizetype start = m_offset;
        if (m_data.at(m_offset) == QLatin1Char('+') || m_data.at(m_offset) == QLatin1Char('-')) ++m_offset;
        bool digits = false;
        while (m_offset < m_data.size() && m_data.at(m_offset).isDigit()) { digits = true; ++m_offset; }
        if (m_offset < m_data.size() && m_data.at(m_offset) == QLatin1Char('.')) {
            ++m_offset;
            while (m_offset < m_data.size() && m_data.at(m_offset).isDigit()) { digits = true; ++m_offset; }
        }
        if (!digits) { m_offset = start; return false; }
        if (m_offset < m_data.size() && (m_data.at(m_offset) == QLatin1Char('e')
                                         || m_data.at(m_offset) == QLatin1Char('E'))) {
            const qsizetype exponentMark = m_offset++;
            if (m_offset < m_data.size() && (m_data.at(m_offset) == QLatin1Char('+')
                                             || m_data.at(m_offset) == QLatin1Char('-'))) ++m_offset;
            const qsizetype exponentStart = m_offset;
            while (m_offset < m_data.size() && m_data.at(m_offset).isDigit()) ++m_offset;
            if (m_offset == exponentStart) m_offset = exponentMark;
        }
        bool ok = false;
        const double parsed = m_data.mid(start, m_offset - start).toDouble(&ok);
        if (!ok || !finiteBounded(parsed)) { m_offset = start; return false; }
        *value = parsed;
        return true;
    }

    bool readPoint(const bool relative, QPointF *point)
    {
        double x = 0.0, y = 0.0;
        if (!readNumber(&x) || !readNumber(&y)) return false;
        QPointF parsed(x, y);
        if (relative) parsed += currentPoint();
        if (!finiteBounded(parsed.x()) || !finiteBounded(parsed.y())) return false;
        *point = parsed;
        return true;
    }

    QPointF currentPoint() const
    {
        return m_cursor;
    }

    bool startSubpath(const QPointF &point, QString *errorMessage)
    {
        if (!finishSubpath(errorMessage)) return false;
        if (m_totalNodes >= VectorBezierPath::MaximumNodeCount) {
            return fail(errorMessage, QObject::tr("SVG path exceeds the editable node limit."));
        }
        VectorPathNode node;
        node.anchor = point;
        node.clearHandles();
        m_current.nodes = {node};
        m_current.closed = false;
        m_subpathStart = point;
        m_cursor = point;
        m_lastCubicControl = point;
        m_lastQuadraticControl = point;
        ++m_totalNodes;
        return true;
    }

    bool appendNode(VectorPathNode node, QString *errorMessage)
    {
        if (m_totalNodes >= VectorBezierPath::MaximumNodeCount) {
            return fail(errorMessage, QObject::tr("SVG path exceeds the editable node limit."));
        }
        node.normalise();
        m_cursor = node.anchor;
        m_current.nodes.push_back(std::move(node));
        ++m_totalNodes;
        return true;
    }

    bool lineTo(const QPointF &point, QString *errorMessage)
    {
        VectorPathNode node;
        node.anchor = point;
        node.clearHandles();
        m_lastCubicControl = point;
        m_lastQuadraticControl = point;
        return appendNode(std::move(node), errorMessage);
    }

    bool cubicTo(const QPointF &c1, const QPointF &c2, const QPointF &point,
                 QString *errorMessage)
    {
        if (!finiteBounded(c1.x()) || !finiteBounded(c1.y())
            || !finiteBounded(c2.x()) || !finiteBounded(c2.y())
            || !finiteBounded(point.x()) || !finiteBounded(point.y())) return false;
        VectorPathNode &previous = m_current.nodes.last();
        previous.outHandle = c1;
        previous.outHandleActive = QLineF(previous.anchor, c1).length() > 1.0e-7;
        previous.mode = VectorNodeMode::Corner;
        VectorPathNode node;
        node.anchor = point;
        node.inHandle = c2;
        node.inHandleActive = QLineF(point, c2).length() > 1.0e-7;
        node.outHandle = point;
        node.outHandleActive = false;
        node.mode = VectorNodeMode::Corner;
        return appendNode(std::move(node), errorMessage);
    }

    bool arcTo(double rx, double ry, const double rotationDegrees,
               const bool largeArc, const bool sweep, const QPointF &end,
               QString *errorMessage)
    {
        const QPointF start = currentPoint();
        if (QLineF(start, end).length() <= 1.0e-12) return true;
        if (rx <= 1.0e-12 || ry <= 1.0e-12) return lineTo(end, errorMessage);
        const double phi = std::fmod(rotationDegrees, 360.0) * std::numbers::pi / 180.0;
        const double cosPhi = std::cos(phi);
        const double sinPhi = std::sin(phi);
        const double dx = (start.x() - end.x()) * 0.5;
        const double dy = (start.y() - end.y()) * 0.5;
        const double xPrime = cosPhi * dx + sinPhi * dy;
        const double yPrime = -sinPhi * dx + cosPhi * dy;
        double lambda = (xPrime * xPrime) / (rx * rx) + (yPrime * yPrime) / (ry * ry);
        if (lambda > 1.0) {
            const double scale = std::sqrt(lambda);
            rx *= scale;
            ry *= scale;
        }
        const double rx2 = rx * rx;
        const double ry2 = ry * ry;
        const double xp2 = xPrime * xPrime;
        const double yp2 = yPrime * yPrime;
        const double numerator = std::max(0.0, rx2 * ry2 - rx2 * yp2 - ry2 * xp2);
        const double denominator = std::max(1.0e-30, rx2 * yp2 + ry2 * xp2);
        const double sign = largeArc == sweep ? -1.0 : 1.0;
        const double factor = sign * std::sqrt(numerator / denominator);
        const double cxPrime = factor * (rx * yPrime / ry);
        const double cyPrime = factor * (-ry * xPrime / rx);
        const QPointF centre(cosPhi * cxPrime - sinPhi * cyPrime + (start.x() + end.x()) * 0.5,
                             sinPhi * cxPrime + cosPhi * cyPrime + (start.y() + end.y()) * 0.5);

        auto vectorAngle = [](const double ux, const double uy, const double vx, const double vy) {
            const double dot = ux * vx + uy * vy;
            const double determinant = ux * vy - uy * vx;
            return std::atan2(determinant, dot);
        };
        const double ux = (xPrime - cxPrime) / rx;
        const double uy = (yPrime - cyPrime) / ry;
        const double vx = (-xPrime - cxPrime) / rx;
        const double vy = (-yPrime - cyPrime) / ry;
        double theta1 = vectorAngle(1.0, 0.0, ux, uy);
        double delta = vectorAngle(ux, uy, vx, vy);
        if (!sweep && delta > 0.0) delta -= 2.0 * std::numbers::pi;
        else if (sweep && delta < 0.0) delta += 2.0 * std::numbers::pi;
        const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(delta) / (std::numbers::pi * 0.5))));
        const double segmentDelta = delta / segments;
        auto pointOnEllipse = [&](const double theta) {
            const double x = rx * std::cos(theta);
            const double y = ry * std::sin(theta);
            return centre + QPointF(cosPhi * x - sinPhi * y,
                                    sinPhi * x + cosPhi * y);
        };
        auto derivative = [&](const double theta) {
            const double dxTheta = -rx * std::sin(theta);
            const double dyTheta = ry * std::cos(theta);
            return QPointF(cosPhi * dxTheta - sinPhi * dyTheta,
                           sinPhi * dxTheta + cosPhi * dyTheta);
        };
        for (int segment = 0; segment < segments; ++segment) {
            const double next = theta1 + segmentDelta;
            const double alpha = (4.0 / 3.0) * std::tan(segmentDelta * 0.25);
            const QPointF p0 = currentPoint();
            QPointF p3 = pointOnEllipse(next);
            if (segment == segments - 1) p3 = end;
            const QPointF c1 = p0 + derivative(theta1) * alpha;
            const QPointF c2 = p3 - derivative(next) * alpha;
            if (!cubicTo(c1, c2, p3, errorMessage)) return false;
            theta1 = next;
        }
        m_lastCubicControl = currentPoint();
        m_lastQuadraticControl = currentPoint();
        return true;
    }

    bool closeSubpath(QString *errorMessage)
    {
        if (m_current.nodes.isEmpty()) return true;
        if (m_current.nodes.size() >= 2
            && QLineF(m_current.nodes.constLast().anchor, m_subpathStart).length() <= 1.0e-7) {
            VectorPathNode duplicate = m_current.nodes.takeLast();
            VectorPathNode &first = m_current.nodes.first();
            if (duplicate.inHandleActive) {
                first.inHandle = duplicate.inHandle;
                first.inHandleActive = true;
            }
        }
        m_current.closed = m_current.nodes.size() >= 2;
        m_cursor = m_subpathStart;
        m_previousCommand = QLatin1Char('Z');
        return finishSubpath(errorMessage);
    }

    bool finishSubpath(QString *errorMessage)
    {
        if (m_current.nodes.isEmpty()) return true;
        m_current.normalise();
        if (m_current.nodes.size() == 1) {
            m_current = {};
            return true;
        }
        if (!m_current.isSafe()) return fail(errorMessage, QObject::tr("SVG path produced unsafe or unbounded geometry."));
        m_paths.push_back(std::move(m_current));
        m_current = {};
        return true;
    }

    QString m_data;
    qsizetype m_offset = 0;
    int m_totalNodes = 0;
    QVector<VectorBezierPath> m_paths;
    VectorBezierPath m_current;
    QPointF m_subpathStart;
    QPointF m_cursor;
    QPointF m_lastCubicControl;
    QPointF m_lastQuadraticControl;
    QChar m_previousCommand;
};

VectorBezierPath pathFromPoints(const QVector<QPointF> &points, const bool closed)
{
    VectorBezierPath path;
    path.closed = closed;
    path.nodes.reserve(points.size());
    for (const QPointF &point : points) {
        VectorPathNode node;
        node.anchor = point;
        node.clearHandles();
        path.nodes.push_back(node);
    }
    path.normalise();
    return path;
}

VectorBezierPath ellipticalRoundedRectPath(const QRectF &rect, double rx, double ry)
{
    rx = std::clamp(rx, 0.0, rect.width() * 0.5);
    ry = std::clamp(ry, 0.0, rect.height() * 0.5);
    constexpr double kappa = 0.5522847498307936;
    VectorBezierPath path;
    path.closed = true;
    path.nodes.resize(8);
    const std::array<QPointF, 8> anchors = {
        QPointF(rect.left() + rx, rect.top()), QPointF(rect.right() - rx, rect.top()),
        QPointF(rect.right(), rect.top() + ry), QPointF(rect.right(), rect.bottom() - ry),
        QPointF(rect.right() - rx, rect.bottom()), QPointF(rect.left() + rx, rect.bottom()),
        QPointF(rect.left(), rect.bottom() - ry), QPointF(rect.left(), rect.top() + ry)};
    for (int index = 0; index < 8; ++index) {
        path.nodes[index].anchor = anchors[index];
        path.nodes[index].clearHandles();
    }
    path.nodes[1].outHandle = path.nodes[1].anchor + QPointF(kappa * rx, 0.0); path.nodes[1].outHandleActive = true;
    path.nodes[2].inHandle = path.nodes[2].anchor + QPointF(0.0, -kappa * ry); path.nodes[2].inHandleActive = true;
    path.nodes[3].outHandle = path.nodes[3].anchor + QPointF(0.0, kappa * ry); path.nodes[3].outHandleActive = true;
    path.nodes[4].inHandle = path.nodes[4].anchor + QPointF(kappa * rx, 0.0); path.nodes[4].inHandleActive = true;
    path.nodes[5].outHandle = path.nodes[5].anchor + QPointF(-kappa * rx, 0.0); path.nodes[5].outHandleActive = true;
    path.nodes[6].inHandle = path.nodes[6].anchor + QPointF(0.0, kappa * ry); path.nodes[6].inHandleActive = true;
    path.nodes[7].outHandle = path.nodes[7].anchor + QPointF(0.0, -kappa * ry); path.nodes[7].outHandleActive = true;
    path.nodes[0].inHandle = path.nodes[0].anchor + QPointF(-kappa * rx, 0.0); path.nodes[0].inHandleActive = true;
    path.normalise();
    return path;
}

LayerNode makeVectorLayer(VectorShape shape, const QString &name,
                          const QTransform &transform, const SvgStyle &style,
                          const double opacity, const BlendMode blendMode)
{
    shape.fill = fillFromStyle(style);
    shape.stroke = strokeFromStyle(style, shape.isOpenPath());
    if (shape.isOpenPath()) shape.fill.enabled = false;
    shape.normalise();
    LayerNode layer;
    layer.type = LayerType::Vector;
    layer.name = name.isEmpty() ? vectorShapeTypeDisplayName(shape.type) : name;
    layer.visible = style.visible;
    layer.opacity = std::clamp(opacity, 0.0, 1.0);
    layer.blendMode = blendMode;
    layer.transform = transform;
    layer.vectorData.objects = {shape};
    layer.vectorData.normalise();
    return layer;
}

QString elementName(const QXmlStreamAttributes &attributes, const QString &fallback)
{
    QString name = attributeValue(attributes, QStringLiteral("data-vfx-name")).trimmed();
    if (name.isEmpty()) name = attributeValue(attributes, QStringLiteral("label")).trimmed();
    if (name.isEmpty()) name = attributeValue(attributes, QStringLiteral("aria-label")).trimmed();
    if (name.isEmpty()) name = attributeValue(attributes, QStringLiteral("id")).trimmed();
    return name.isEmpty() ? fallback : name.left(256);
}

void consumeCurrentElement(QXmlStreamReader *reader, int *elementCount = nullptr)
{
    if (!reader || !reader->isStartElement()) return;
    int depth = 1;
    while (!reader->atEnd() && depth > 0) {
        reader->readNext();
        if (reader->isDTD() || reader->tokenType() == QXmlStreamReader::EntityReference) {
            reader->raiseError(QObject::tr("DTD and entity declarations are not accepted in SVG imports."));
            return;
        }
        if (reader->isStartElement()) {
            ++depth;
            if (depth > SvgWorkflow::MaximumNestingDepth) {
                reader->raiseError(QObject::tr("SVG nesting exceeds the safety limit."));
                return;
            }
            if (elementCount && ++*elementCount > SvgWorkflow::MaximumElementCount) {
                reader->raiseError(QObject::tr("SVG contains too many elements."));
                return;
            }
        } else if (reader->isEndElement()) {
            --depth;
        }
    }
}

void collectTextContentInto(QXmlStreamReader *reader,
                            const bool insertLineBreak,
                            const int depth,
                            WarningSink *warnings,
                            int *elementCount,
                            QString *output)
{
    if (!reader || !output) return;
    if (depth > SvgWorkflow::MaximumNestingDepth) {
        reader->raiseError(QObject::tr("SVG text nesting exceeds the safety limit."));
        return;
    }

    auto appendBounded = [&](const QString &fragment) {
        const qsizetype remaining = TextLayerData::MaximumTextLength - output->size();
        if (remaining <= 0) {
            if (warnings) warnings->add(QObject::tr("SVG text exceeded the editable text limit and was truncated."));
            return;
        }
        output->append(fragment.left(remaining));
        if (fragment.size() > remaining && warnings) {
            warnings->add(QObject::tr("SVG text exceeded the editable text limit and was truncated."));
        }
    };

    if (insertLineBreak) appendBounded(QStringLiteral("\n"));
    while (!reader->atEnd()) {
        reader->readNext();
        if (reader->isDTD() || reader->tokenType() == QXmlStreamReader::EntityReference) {
            reader->raiseError(QObject::tr("DTD and entity declarations are not accepted in SVG imports."));
            return;
        }
        if (reader->isCharacters() || reader->isCDATA()) {
            appendBounded(reader->text().toString());
        } else if (reader->isStartElement()) {
            if (elementCount && ++*elementCount > SvgWorkflow::MaximumElementCount) {
                reader->raiseError(QObject::tr("SVG contains too many elements."));
                return;
            }
            const QString local = reader->name().toString().toLower();
            const QXmlStreamAttributes attributes = reader->attributes();
            if (local == QStringLiteral("textpath") && warnings) {
                warnings->add(QObject::tr(
                    "SVG text-on-path layout is not editable yet; its text was imported as ordinary line text."));
            }
            if (local == QStringLiteral("tspan") && warnings) {
                const QHash<QString, QString> spanProperties = presentationMap(attributes);
                static const std::array<QString, 11> styledProperties = {
                    QStringLiteral("fill"), QStringLiteral("fill-opacity"),
                    QStringLiteral("stroke"), QStringLiteral("stroke-opacity"),
                    QStringLiteral("font-family"), QStringLiteral("font-size"),
                    QStringLiteral("font-style"), QStringLiteral("font-weight"),
                    QStringLiteral("letter-spacing"), QStringLiteral("text-anchor"),
                    QStringLiteral("baseline-shift")};
                for (const QString &property : styledProperties) {
                    if (spanProperties.contains(property)) {
                        warnings->add(QObject::tr(
                            "Per-span SVG text styling is flattened to the parent editable text style."));
                        break;
                    }
                }
            }
            const bool line = local == QStringLiteral("tspan") && !output->isEmpty()
                && (!attributeValue(attributes, QStringLiteral("x")).isEmpty()
                    || !attributeValue(attributes, QStringLiteral("dy")).isEmpty());
            collectTextContentInto(reader, line, depth + 1, warnings, elementCount, output);
            if (reader->hasError()) return;
        } else if (reader->isEndElement()) {
            return;
        }
    }
}

QString collectTextContent(QXmlStreamReader *reader,
                           const bool insertLineBreak,
                           WarningSink *warnings,
                           int *elementCount)
{
    QString output;
    collectTextContentInto(reader, insertLineBreak, 0, warnings, elementCount, &output);
    return output;
}

struct ImportContext {
    WarningSink warnings;
    int elementCount = 0;
    int skippedElementCount = 0;
    qint64 layerTreeNodeCount = 0;
    qint64 editableLayerCount = 0;
    qint64 vectorObjectCount = 0;
    qint64 pathNodeCount = 0;
    qsizetype textCharacterCount = 0;
    QSizeF viewport;
    QString sourceName;
};

struct EditableCost {
    qint64 layers = 0;
    qint64 vectorObjects = 0;
    qint64 pathNodes = 0;
    qsizetype textCharacters = 0;
};

EditableCost editableCost(const LayerNode &layer)
{
    EditableCost cost;
    if (layer.type == LayerType::Vector) {
        cost.layers = 1;
        cost.vectorObjects = layer.vectorData.objects.size();
        for (const VectorShape &shape : layer.vectorData.objects) {
            if (shape.type == VectorShapeType::Path) {
                cost.pathNodes += shape.bezierPath.nodes.size();
                for (const VectorBezierPath &path : shape.additionalBezierPaths) {
                    cost.pathNodes += path.nodes.size();
                }
            }
        }
    } else if (layer.type == LayerType::Text) {
        cost.layers = 1;
        cost.textCharacters = layer.textData.text.size();
    }
    return cost;
}

bool admitEditableLayer(const LayerNode &layer, ImportContext *context)
{
    if (!context) return false;
    const bool supported = layer.type == LayerType::Vector
        || layer.type == LayerType::Text || layer.type == LayerType::Group;
    if (!supported) return false;
    const EditableCost cost = editableCost(layer);
    const bool permitted = context->layerTreeNodeCount < SvgWorkflow::MaximumLayerTreeNodeCount
        && context->editableLayerCount <= SvgWorkflow::MaximumEditableLayerCount - cost.layers
        && context->vectorObjectCount <= SvgWorkflow::MaximumVectorObjectCount - cost.vectorObjects
        && context->pathNodeCount <= SvgWorkflow::MaximumPathNodeCount - cost.pathNodes
        && context->textCharacterCount <= SvgWorkflow::MaximumTextCharacterCount - cost.textCharacters;
    if (!permitted) {
        ++context->skippedElementCount;
        context->warnings.add(QObject::tr(
            "Additional editable SVG content was skipped after the document complexity safety limit was reached."));
        return false;
    }
    ++context->layerTreeNodeCount;
    context->editableLayerCount += cost.layers;
    context->vectorObjectCount += cost.vectorObjects;
    context->pathNodeCount += cost.pathNodes;
    context->textCharacterCount += cost.textCharacters;
    return true;
}

QVector<LayerNode> parseChildren(QXmlStreamReader *reader,
                                 const SvgStyle &parentStyle,
                                 ImportContext *context,
                                 int depth);

LayerNode exactVectorLayer(const QXmlStreamAttributes &attributes,
                           const SvgStyle &style,
                           const QTransform &transform,
                           const double opacity,
                           bool *ok)
{
    if (ok) *ok = false;
    bool jsonOk = false;
    const QJsonObject object = jsonFromBase64(
        attributeValue(attributes, QStringLiteral("data-vfx-vector-data")), &jsonOk);
    if (!jsonOk) return {};
    bool vectorOk = false;
    VectorLayerData data = VectorLayerData::fromJson(object, &vectorOk);
    if (!vectorOk || !data.isSafe()) return {};
    regenerateVectorIds(&data);
    LayerNode layer;
    layer.type = LayerType::Vector;
    layer.name = elementName(attributes, QObject::tr("Vector Layer"));
    layer.visible = style.visible;
    layer.opacity = opacity;
    layer.transform = transform;
    layer.vectorData = std::move(data);
    const QString blend = attributeValue(attributes, QStringLiteral("data-vfx-blend-mode"));
    if (!blend.isEmpty()) layer.blendMode = blendModeFromString(blend);
    else layer.blendMode = blendModeFromCss(presentationMap(attributes).value(QStringLiteral("mix-blend-mode")));
    if (ok) *ok = layer.vectorData.isSafe();
    return layer;
}

LayerNode exactTextLayer(const QXmlStreamAttributes &attributes,
                         const SvgStyle &style,
                         const QTransform &transform,
                         const double opacity,
                         bool *ok)
{
    if (ok) *ok = false;
    bool jsonOk = false;
    const QJsonObject object = jsonFromBase64(
        attributeValue(attributes, QStringLiteral("data-vfx-text-data")), &jsonOk);
    if (!jsonOk) return {};
    bool textOk = false;
    TextLayerData data = TextLayerData::fromJson(object, &textOk);
    if (!textOk || !data.isSafe()) return {};
    data.revision = std::max<quint64>(1, data.revision + 1);
    LayerNode layer;
    layer.type = LayerType::Text;
    layer.name = elementName(attributes, QObject::tr("Text Layer"));
    layer.visible = style.visible;
    layer.opacity = opacity;
    layer.transform = transform;
    layer.textData = std::move(data);
    const QString blend = attributeValue(attributes, QStringLiteral("data-vfx-blend-mode"));
    if (!blend.isEmpty()) layer.blendMode = blendModeFromString(blend);
    else layer.blendMode = blendModeFromCss(presentationMap(attributes).value(QStringLiteral("mix-blend-mode")));
    if (ok) *ok = layer.textData.isSafe();
    return layer;
}

QTransform nestedSvgViewportTransform(const QXmlStreamAttributes &attributes,
                                      const QSizeF &parentViewport,
                                      QSizeF *childViewport,
                                      WarningSink *warnings)
{
    const double referenceWidth = std::max(1.0, parentViewport.width());
    const double referenceHeight = std::max(1.0, parentViewport.height());
    const QString xText = attributeValue(attributes, QStringLiteral("x"));
    const QString yText = attributeValue(attributes, QStringLiteral("y"));
    const QString widthText = attributeValue(attributes, QStringLiteral("width"));
    const QString heightText = attributeValue(attributes, QStringLiteral("height"));
    const auto parsedX = parseLength(xText, referenceWidth);
    const auto parsedY = parseLength(yText, referenceHeight);
    const auto parsedWidth = parseLength(widthText, referenceWidth);
    const auto parsedHeight = parseLength(heightText, referenceHeight);
    const double x = parsedX.value_or(0.0);
    const double y = parsedY.value_or(0.0);
    double width = parsedWidth.value_or(referenceWidth);
    double height = parsedHeight.value_or(referenceHeight);
    if ((!xText.trimmed().isEmpty() && !parsedX)
        || (!yText.trimmed().isEmpty() && !parsedY)) {
        if (warnings) warnings->add(QObject::tr("Invalid nested SVG viewport coordinates were replaced with zero."));
    }
    if ((!widthText.trimmed().isEmpty() && (!parsedWidth || *parsedWidth <= 0.0))
        || (!heightText.trimmed().isEmpty() && (!parsedHeight || *parsedHeight <= 0.0))) {
        if (warnings) warnings->add(QObject::tr("Invalid nested SVG viewport dimensions were replaced with the parent viewport size."));
        if (!parsedWidth || *parsedWidth <= 0.0) width = referenceWidth;
        if (!parsedHeight || *parsedHeight <= 0.0) height = referenceHeight;
    }
    if (width > MaximumCanvasExtent || height > MaximumCanvasExtent) {
        if (warnings) warnings->add(QObject::tr("Nested SVG viewport dimensions were clamped to the supported range."));
    }
    width = std::clamp(width, 1.0, static_cast<double>(MaximumCanvasExtent));
    height = std::clamp(height, 1.0, static_cast<double>(MaximumCanvasExtent));
    if (childViewport) *childViewport = QSizeF(width, height);
    const QTransform viewportTransform = QTransform::fromTranslate(x, y);
    bool viewBoxComplete = false;
    const QString viewBoxText = attributeValue(attributes, QStringLiteral("viewBox"));
    const QVector<double> viewBox = parseNumberList(viewBoxText, 4, &viewBoxComplete);
    if (viewBox.size() != 4 || !viewBoxComplete || viewBox.at(2) <= 0.0 || viewBox.at(3) <= 0.0) {
        if (!viewBoxText.trimmed().isEmpty() && warnings) {
            warnings->add(QObject::tr("An invalid nested SVG viewBox was ignored."));
        }
        return viewportTransform;
    }
    double sx = width / viewBox.at(2);
    double sy = height / viewBox.at(3);
    double tx = -viewBox.at(0) * sx;
    double ty = -viewBox.at(1) * sy;
    const QString preserve = attributeValue(attributes, QStringLiteral("preserveAspectRatio")).trimmed();
    if (!preserve.startsWith(QStringLiteral("none"), Qt::CaseInsensitive)) {
        const bool slice = preserve.contains(QStringLiteral("slice"), Qt::CaseInsensitive);
        const double uniform = slice ? std::max(sx, sy) : std::min(sx, sy);
        const double renderedWidth = viewBox.at(2) * uniform;
        const double renderedHeight = viewBox.at(3) * uniform;
        double alignX = (width - renderedWidth) * 0.5;
        double alignY = (height - renderedHeight) * 0.5;
        if (preserve.contains(QStringLiteral("xMin"), Qt::CaseInsensitive)) alignX = 0.0;
        else if (preserve.contains(QStringLiteral("xMax"), Qt::CaseInsensitive)) alignX = width - renderedWidth;
        if (preserve.contains(QStringLiteral("YMin"), Qt::CaseInsensitive)) alignY = 0.0;
        else if (preserve.contains(QStringLiteral("YMax"), Qt::CaseInsensitive)) alignY = height - renderedHeight;
        sx = sy = uniform;
        tx = alignX - viewBox.at(0) * uniform;
        ty = alignY - viewBox.at(1) * uniform;
    }
    const QTransform view(sx, 0.0, 0.0, sy, tx, ty);
    if (!view.isInvertible()) {
        if (warnings) warnings->add(QObject::tr("An invalid nested SVG viewport transform was ignored."));
        return viewportTransform;
    }
    return view * viewportTransform;
}

QVector<LayerNode> parseElement(QXmlStreamReader *reader,
                                const SvgStyle &parentStyle,
                                ImportContext *context,
                                const int depth)
{
    QVector<LayerNode> output;
    if (!reader || !context || !reader->isStartElement()) return output;
    if (depth > SvgWorkflow::MaximumNestingDepth) {
        context->warnings.add(QObject::tr("SVG nesting deeper than %1 levels was skipped.")
                                  .arg(SvgWorkflow::MaximumNestingDepth));
        ++context->skippedElementCount;
        consumeCurrentElement(reader, &context->elementCount);
        return output;
    }
    if (++context->elementCount > SvgWorkflow::MaximumElementCount) {
        reader->raiseError(QObject::tr("SVG contains too many elements."));
        return output;
    }
    const QString tag = reader->name().toString().toLower();
    const QXmlStreamAttributes attributes = reader->attributes();
    const SvgStyle style = inheritedStyle(parentStyle, attributes, &context->warnings);
    SvgStyle renderStyle = style;
    renderStyle.visible = style.visible && localDisplayVisible(attributes);
    const double opacity = localOpacity(attributes);
    bool transformOk = true;
    const QTransform transform = parseTransform(attributeValue(attributes, QStringLiteral("transform")), &transformOk);
    if (!transformOk) {
        context->warnings.add(QObject::tr("An invalid transform on <%1> was ignored.").arg(tag));
    }
    const QHash<QString, QString> presentation = presentationMap(attributes);
    const QString customBlend = attributeValue(attributes, QStringLiteral("data-vfx-blend-mode"));
    const QString cssBlend = presentation.value(QStringLiteral("mix-blend-mode")).trimmed().toLower();
    const BlendMode elementBlend = customBlend.isEmpty()
        ? blendModeFromCss(cssBlend)
        : blendModeFromString(customBlend);
    if (customBlend.isEmpty() && !cssBlend.isEmpty() && cssBlend != QStringLiteral("normal")
        && elementBlend == BlendMode::Copy) {
        context->warnings.add(QObject::tr("An unsupported SVG blend mode was replaced with Normal."));
    }
    const auto hasReferencedEffect = [&](const QString &property) {
        const QString value = presentation.value(property).trimmed();
        return !value.isEmpty() && value.compare(QStringLiteral("none"), Qt::CaseInsensitive) != 0;
    };
    if (hasReferencedEffect(QStringLiteral("clip-path"))) {
        context->warnings.add(QObject::tr("SVG clipping paths are not editable yet; clipped artwork was imported without clipping."));
    }
    if (hasReferencedEffect(QStringLiteral("mask"))) {
        context->warnings.add(QObject::tr("SVG masks are not editable yet; masked artwork was imported without its SVG mask."));
    }
    if (hasReferencedEffect(QStringLiteral("filter"))) {
        context->warnings.add(QObject::tr("SVG filters are not editable yet; filtered artwork was imported without filter effects."));
    }
    const QString vectorEffect = presentation.value(QStringLiteral("vector-effect")).trimmed().toLower();
    if (vectorEffect == QStringLiteral("non-scaling-stroke")) {
        context->warnings.add(QObject::tr(
            "SVG non-scaling strokes are imported as ordinary editable strokes and will scale with the layer."));
    } else if (!vectorEffect.isEmpty() && vectorEffect != QStringLiteral("none")) {
        context->warnings.add(QObject::tr("Unsupported SVG vector effects were omitted."));
    }
    const bool hasMarkerStart = hasReferencedEffect(QStringLiteral("marker-start"));
    const bool hasMarkerMid = hasReferencedEffect(QStringLiteral("marker-mid"));
    const bool hasMarkerEnd = hasReferencedEffect(QStringLiteral("marker-end"));
    const bool hasEditableVfxMarkers = style.startArrowhead != VectorArrowheadType::None
        || style.endArrowhead != VectorArrowheadType::None;
    if (hasMarkerMid || ((hasMarkerStart || hasMarkerEnd) && !hasEditableVfxMarkers)) {
        context->warnings.add(QObject::tr(
            "Unsupported SVG path markers were omitted. VFX Photo Lab arrowhead metadata is imported where present."));
    }

    if (!attributeValue(attributes, QStringLiteral("data-vfx-vector-data")).isEmpty()) {
        bool exactOk = false;
        LayerNode layer = exactVectorLayer(attributes, renderStyle,
                                           transformOk ? transform : QTransform(), opacity, &exactOk);
        if (exactOk) {
            consumeCurrentElement(reader, &context->elementCount);
            output.push_back(std::move(layer));
            return output;
        }
        context->warnings.add(QObject::tr("Damaged VFX Photo Lab vector metadata was ignored; standard SVG child geometry was used instead."));
    }
    if (!attributeValue(attributes, QStringLiteral("data-vfx-text-data")).isEmpty()) {
        bool exactOk = false;
        LayerNode layer = exactTextLayer(attributes, renderStyle,
                                         transformOk ? transform : QTransform(), opacity, &exactOk);
        if (exactOk) {
            consumeCurrentElement(reader, &context->elementCount);
            output.push_back(std::move(layer));
            return output;
        }
        context->warnings.add(QObject::tr("Damaged VFX Photo Lab text metadata was ignored; standard SVG child text was used instead."));
    }

    const QTransform localTransform = transformOk ? transform : QTransform();
    const QString name = elementName(attributes, {});
    if (tag == QStringLiteral("symbol")) {
        ++context->skippedElementCount;
        context->warnings.add(QObject::tr(
            "SVG <symbol> definitions require <use>, which is not editable yet, and were skipped."));
        consumeCurrentElement(reader, &context->elementCount);
        return output;
    }
    if (tag == QStringLiteral("g") || tag == QStringLiteral("a")
        || tag == QStringLiteral("switch")) {
        if (tag == QStringLiteral("switch")) {
            context->warnings.add(QObject::tr(
                "SVG <switch> conditional selection is not evaluated; all supported alternatives were imported."));
        }
        QVector<LayerNode> children = parseChildren(reader, style, context, depth + 1);
        if (!children.isEmpty()) {
            LayerNode group;
            group.type = LayerType::Group;
            group.name = name.isEmpty() ? QObject::tr("SVG Group") : name;
            group.visible = localDisplayVisible(attributes);
            group.opacity = opacity;
            group.transform = localTransform;
            group.children = std::move(children);
            group.blendMode = elementBlend;
            const QString composite = attributeValue(attributes, QStringLiteral("data-vfx-group-mode"));
            if (composite.compare(QStringLiteral("pass-through"), Qt::CaseInsensitive) == 0) {
                group.groupCompositeMode = GroupCompositeMode::PassThrough;
            }
            output.push_back(std::move(group));
        }
        return output;
    }
    if (tag == QStringLiteral("svg")) {
        QSizeF nestedViewport;
        QTransform viewportTransform = nestedSvgViewportTransform(attributes, context->viewport,
                                                                  &nestedViewport, &context->warnings);
        QSizeF previousViewport = context->viewport;
        context->viewport = nestedViewport;
        QVector<LayerNode> children = parseChildren(reader, style, context, depth + 1);
        context->viewport = previousViewport;
        if (!children.isEmpty()) {
            LayerNode group;
            group.type = LayerType::Group;
            group.name = name.isEmpty() ? QObject::tr("Nested SVG") : name;
            group.visible = localDisplayVisible(attributes);
            group.opacity = opacity;
            group.transform = viewportTransform * localTransform;
            group.blendMode = elementBlend;
            group.children = std::move(children);
            output.push_back(std::move(group));
        }
        return output;
    }
    if (tag == QStringLiteral("defs") || tag == QStringLiteral("metadata")
        || tag == QStringLiteral("title") || tag == QStringLiteral("desc")
        || tag == QStringLiteral("clippath") || tag == QStringLiteral("mask")
        || tag == QStringLiteral("lineargradient") || tag == QStringLiteral("radialgradient")
        || tag == QStringLiteral("pattern") || tag == QStringLiteral("filter")
        || tag == QStringLiteral("style") || tag == QStringLiteral("script")) {
        if (tag == QStringLiteral("style")) {
            context->warnings.add(QObject::tr("Embedded CSS stylesheets are not interpreted; inline SVG styles are supported."));
        }
        consumeCurrentElement(reader, &context->elementCount);
        return output;
    }

    auto xLength = [&](const QString &key, const double fallback = 0.0) {
        return parseLength(attributeValue(attributes, key), context->viewport.width()).value_or(fallback);
    };
    auto yLength = [&](const QString &key, const double fallback = 0.0) {
        return parseLength(attributeValue(attributes, key), context->viewport.height()).value_or(fallback);
    };

    if (tag == QStringLiteral("rect")) {
        const double x = xLength(QStringLiteral("x"));
        const double y = yLength(QStringLiteral("y"));
        const double width = xLength(QStringLiteral("width"), -1.0);
        const double height = yLength(QStringLiteral("height"), -1.0);
        double rx = xLength(QStringLiteral("rx"), 0.0);
        double ry = yLength(QStringLiteral("ry"), 0.0);
        if (width > 0.0 && height > 0.0) {
            if (rx <= 0.0 && ry > 0.0) rx = ry;
            if (ry <= 0.0 && rx > 0.0) ry = rx;
            VectorShape shape;
            if (rx > 0.0 || ry > 0.0) {
                if (std::abs(rx - ry) <= 1.0e-6) {
                    shape.type = VectorShapeType::RoundedRectangle;
                    shape.bounds = QRectF(x, y, width, height);
                    shape.cornerRadii.setAll(std::min(rx, std::min(width, height) * 0.5));
                } else {
                    shape.type = VectorShapeType::Path;
                    shape.pathFillRule = style.evenOddFill
                        ? VectorPathFillRule::EvenOdd
                        : VectorPathFillRule::NonZero;
                    shape.bezierPath = ellipticalRoundedRectPath(QRectF(x, y, width, height), rx, ry);
                }
            } else {
                shape.type = VectorShapeType::Rectangle;
                shape.bounds = QRectF(x, y, width, height);
            }
            output.push_back(makeVectorLayer(shape, name, localTransform, renderStyle, opacity, elementBlend));
        } else {
            ++context->skippedElementCount;
            context->warnings.add(QObject::tr("A rectangle with missing or non-positive dimensions was skipped."));
        }
        consumeCurrentElement(reader, &context->elementCount);
        return output;
    }
    if (tag == QStringLiteral("circle") || tag == QStringLiteral("ellipse")) {
        const double cx = xLength(QStringLiteral("cx"));
        const double cy = yLength(QStringLiteral("cy"));
        const double rx = tag == QStringLiteral("circle")
            ? parseLength(attributeValue(attributes, QStringLiteral("r")),
                          std::min(context->viewport.width(), context->viewport.height())).value_or(-1.0)
            : xLength(QStringLiteral("rx"), -1.0);
        const double ry = tag == QStringLiteral("circle") ? rx : yLength(QStringLiteral("ry"), -1.0);
        if (rx > 0.0 && ry > 0.0) {
            VectorShape shape;
            shape.type = VectorShapeType::Ellipse;
            shape.bounds = QRectF(cx - rx, cy - ry, rx * 2.0, ry * 2.0);
            output.push_back(makeVectorLayer(shape, name, localTransform, renderStyle, opacity, elementBlend));
        } else {
            ++context->skippedElementCount;
            context->warnings.add(QObject::tr("An ellipse with missing or non-positive radii was skipped."));
        }
        consumeCurrentElement(reader, &context->elementCount);
        return output;
    }
    if (tag == QStringLiteral("line")) {
        const QPointF start(xLength(QStringLiteral("x1")), yLength(QStringLiteral("y1")));
        const QPointF end(xLength(QStringLiteral("x2")), yLength(QStringLiteral("y2")));
        if (QLineF(start, end).length() > 1.0e-9 && style.strokeEnabled) {
            VectorShape shape;
            shape.type = VectorShapeType::Line;
            shape.lineStart = start;
            shape.lineEnd = end;
            shape.bounds = QRectF(start, end).normalized();
            output.push_back(makeVectorLayer(shape, name, localTransform, renderStyle, opacity, elementBlend));
        } else {
            ++context->skippedElementCount;
            context->warnings.add(QObject::tr("An invisible or zero-length SVG line was skipped."));
        }
        consumeCurrentElement(reader, &context->elementCount);
        return output;
    }
    if (tag == QStringLiteral("polygon") || tag == QStringLiteral("polyline")) {
        bool pointsComplete = false;
        const QVector<double> values = parseNumberList(
            attributeValue(attributes, QStringLiteral("points")),
            VectorBezierPath::MaximumNodeCount * 2, &pointsComplete);
        QVector<QPointF> points;
        points.reserve(values.size() / 2);
        for (int index = 0; index + 1 < values.size(); index += 2) {
            points.push_back(QPointF(values.at(index), values.at(index + 1)));
        }
        const bool closed = tag == QStringLiteral("polygon");
        if (!pointsComplete || values.size() % 2 != 0) {
            points.clear();
        }
        if (!closed && style.fillEnabled) {
            context->warnings.add(QObject::tr("Open SVG polyline fills are not represented; the editable polyline keeps its stroke only."));
        }
        if (points.size() >= (closed ? 3 : 2)) {
            VectorShape shape;
            shape.type = VectorShapeType::Path;
            shape.pathFillRule = style.evenOddFill
                ? VectorPathFillRule::EvenOdd
                : VectorPathFillRule::NonZero;
            shape.bezierPath = pathFromPoints(points, closed);
            output.push_back(makeVectorLayer(shape, name, localTransform, renderStyle, opacity, elementBlend));
        } else {
            ++context->skippedElementCount;
            context->warnings.add(QObject::tr("An SVG polygon/polyline with too few points was skipped."));
        }
        consumeCurrentElement(reader, &context->elementCount);
        return output;
    }
    if (tag == QStringLiteral("path")) {
        QVector<VectorBezierPath> paths;
        QString pathError;
        SvgPathParser parser(attributeValue(attributes, QStringLiteral("d")));
        if (!parser.parse(&paths, &pathError)) {
            ++context->skippedElementCount;
            context->warnings.add(QObject::tr("An SVG path was skipped: %1").arg(pathError));
        } else {
            const bool allClosed = !paths.isEmpty()
                && std::all_of(paths.cbegin(), paths.cend(),
                               [](const VectorBezierPath &path) {
                                   return path.closed;
                               });
            const bool compoundCanRemainExact = allClosed;
            if (compoundCanRemainExact) {
                VectorShape shape;
                shape.type = VectorShapeType::Path;
                shape.pathFillRule = style.evenOddFill
                    ? VectorPathFillRule::EvenOdd
                    : VectorPathFillRule::NonZero;
                shape.bezierPath = paths.constFirst();
                for (int index = 1; index < paths.size(); ++index) {
                    shape.additionalBezierPaths.push_back(paths.at(index));
                }
                shape.normalise();
                output.push_back(makeVectorLayer(
                    shape,
                    name.isEmpty()
                        ? vectorShapeTypeDisplayName(VectorShapeType::Path)
                        : name,
                    localTransform, renderStyle, opacity, elementBlend));
            } else {
                int pathIndex = 0;
                for (const VectorBezierPath &path : std::as_const(paths)) {
                    if (!path.closed && style.fillEnabled) {
                        context->warnings.add(QObject::tr("Open SVG path fills are not represented; the editable path keeps its stroke only."));
                    }
                    if (!path.closed && !style.strokeEnabled) {
                        ++context->skippedElementCount;
                        context->warnings.add(QObject::tr("An open SVG path without a stroke was skipped."));
                        continue;
                    }
                    VectorShape shape;
                    shape.type = VectorShapeType::Path;
                    shape.pathFillRule = style.evenOddFill
                        ? VectorPathFillRule::EvenOdd
                        : VectorPathFillRule::NonZero;
                    shape.bezierPath = path;
                    QString layerName = name.isEmpty()
                        ? vectorShapeTypeDisplayName(VectorShapeType::Path)
                        : name;
                    if (paths.size() > 1) {
                        layerName += QStringLiteral(" %1").arg(++pathIndex);
                    }
                    output.push_back(makeVectorLayer(shape, layerName,
                                                     localTransform, renderStyle,
                                                     opacity, elementBlend));
                }
                if (paths.size() > 1) {
                    context->warnings.add(QObject::tr(
                        "An SVG path containing both open and closed subpaths was split into editable layers."));
                }
            }
        }
        consumeCurrentElement(reader, &context->elementCount);
        return output;
    }
    if (tag == QStringLiteral("text")) {
        const QString dx = attributeValue(attributes, QStringLiteral("dx")).trimmed();
        const QString dy = attributeValue(attributes, QStringLiteral("dy")).trimmed();
        const QString rotate = attributeValue(attributes, QStringLiteral("rotate")).trimmed();
        if (!dx.isEmpty() || !dy.isEmpty() || !rotate.isEmpty()) {
            context->warnings.add(QObject::tr(
                "Per-glyph SVG text offsets or rotations are not editable yet and were omitted."));
        }
        const double x = xLength(QStringLiteral("x"));
        const double baselineY = yLength(QStringLiteral("y"));
        QString text = collectTextContent(reader, false, &context->warnings, &context->elementCount);
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        if (text.trimmed().isEmpty()) {
            ++context->skippedElementCount;
            return output;
        }
        if (!style.fillEnabled && !style.strokeEnabled) {
            ++context->skippedElementCount;
            context->warnings.add(QObject::tr("Invisible SVG text with neither fill nor stroke was skipped."));
            return output;
        }
        TextLayerData data;
        data.mode = TextLayoutMode::Point;
        data.text = text;
        data.requestedFamily = style.fontFamily;
        data.requestedStyle = style.fontStyle;
        data.weight = style.fontWeight;
        data.italic = style.italic;
        data.fontSize = style.fontSize;
        data.tracking = style.letterSpacing;
        data.leading = style.lineHeight;
        if (style.fillEnabled) {
            data.colour = style.fill;
            data.opacity = style.fillOpacity;
            if (style.strokeEnabled) {
                context->warnings.add(QObject::tr(
                    "SVG text strokes are not editable yet and were omitted while preserving the text fill."));
            }
        } else {
            data.colour = style.stroke;
            data.opacity = style.strokeOpacity;
            context->warnings.add(QObject::tr(
                "Stroke-only SVG text was converted to filled editable text using the stroke colour."));
        }
        QFont font(data.resolvedFamily());
        font.setPixelSize(std::max(1, qRound(data.fontSize)));
        font.setWeight(static_cast<QFont::Weight>(std::clamp(data.weight, 1, 1000)));
        font.setItalic(data.italic);
        const QFontMetricsF metrics(font);
        data.origin = QPointF(x, baselineY - metrics.ascent());
        data.normalise();
        const double width = data.semanticBox().width();
        if (style.textAnchor == QStringLiteral("middle")) data.origin.rx() -= width * 0.5;
        else if (style.textAnchor == QStringLiteral("end")) data.origin.rx() -= width;
        data.normalise();
        LayerNode layer;
        layer.type = LayerType::Text;
        layer.name = name.isEmpty() ? QObject::tr("Text") : name;
        layer.visible = renderStyle.visible;
        layer.opacity = opacity;
        layer.blendMode = elementBlend;
        layer.transform = localTransform;
        layer.textData = std::move(data);
        output.push_back(std::move(layer));
        return output;
    }

    ++context->skippedElementCount;
    if (tag == QStringLiteral("image") || tag == QStringLiteral("use")
        || tag == QStringLiteral("foreignobject")) {
        context->warnings.add(QObject::tr("SVG <%1> content is not editable yet and was skipped.").arg(tag));
    } else {
        context->warnings.add(QObject::tr("Unsupported SVG element <%1> was skipped.").arg(tag));
    }
    consumeCurrentElement(reader, &context->elementCount);
    return output;
}

QVector<LayerNode> parseChildren(QXmlStreamReader *reader,
                                 const SvgStyle &parentStyle,
                                 ImportContext *context,
                                 const int depth)
{
    QVector<LayerNode> svgOrder;
    while (!reader->atEnd()) {
        reader->readNext();
        if (reader->isDTD() || reader->tokenType() == QXmlStreamReader::EntityReference) {
            reader->raiseError(QObject::tr("DTD and entity declarations are not accepted in SVG imports."));
            break;
        }
        if (reader->isStartElement()) {
            QVector<LayerNode> layers = parseElement(reader, parentStyle, context, depth);
            for (LayerNode &layer : layers) {
                if (admitEditableLayer(layer, context)) {
                    svgOrder.push_back(std::move(layer));
                }
            }
        } else if (reader->isEndElement()) {
            break;
        }
    }
    // SVG paints first child at the bottom; Photo Lab stores the top layer at index 0.
    std::reverse(svgOrder.begin(), svgOrder.end());
    return svgOrder;
}

bool importedTransformSafe(const QTransform &transform)
{
    return transform.isInvertible()
        && finiteBounded(transform.m11()) && finiteBounded(transform.m12())
        && finiteBounded(transform.m13()) && finiteBounded(transform.m21())
        && finiteBounded(transform.m22()) && finiteBounded(transform.m23())
        && finiteBounded(transform.m31()) && finiteBounded(transform.m32())
        && finiteBounded(transform.m33());
}

void filterUnsafeImportedLayers(QVector<LayerNode> *layers,
                                ImportContext *context)
{
    if (!layers || !context) return;
    for (int index = layers->size() - 1; index >= 0; --index) {
        LayerNode &layer = (*layers)[index];
        filterUnsafeImportedLayers(&layer.children, context);
        const bool typeSafe = (layer.type == LayerType::Vector && layer.vectorData.isSafe())
            || (layer.type == LayerType::Text && layer.textData.isSafe())
            || (layer.type == LayerType::Group && !layer.children.isEmpty());
        const bool metadataSafe = typeSafe && importedTransformSafe(layer.transform)
            && std::isfinite(layer.opacity) && layer.opacity >= 0.0 && layer.opacity <= 1.0;
        if (!metadataSafe) {
            layers->removeAt(index);
            ++context->skippedElementCount;
            context->warnings.add(QObject::tr("Unsafe, empty or unbounded imported SVG geometry was skipped."));
        }
    }
}

bool importedTreeWithinLimits(const QVector<LayerNode> &layers,
                              const int depth,
                              qint64 *treeNodes)
{
    if (!treeNodes) return false;
    if (layers.isEmpty()) return true;
    if (depth > SvgWorkflow::MaximumNestingDepth) return false;
    for (const LayerNode &layer : layers) {
        if (++*treeNodes > SvgWorkflow::MaximumLayerTreeNodeCount) return false;
        if (!importedTreeWithinLimits(layer.children, depth + 1, treeNodes)) return false;
    }
    return true;
}

QTransform rootViewBoxTransform(const QXmlStreamAttributes &attributes,
                                QSizeF viewport,
                                WarningSink *warnings)
{
    bool viewBoxComplete = false;
    const QString viewBoxText = attributeValue(attributes, QStringLiteral("viewBox"));
    const QVector<double> viewBox = parseNumberList(viewBoxText, 4, &viewBoxComplete);
    if (viewBox.size() != 4 || !viewBoxComplete || viewBox.at(2) <= 0.0 || viewBox.at(3) <= 0.0) {
        if (!viewBoxText.trimmed().isEmpty() && warnings) {
            warnings->add(QObject::tr("The SVG viewBox was invalid and was ignored."));
        }
        return {};
    }
    double sx = viewport.width() / viewBox.at(2);
    double sy = viewport.height() / viewBox.at(3);
    double tx = -viewBox.at(0) * sx;
    double ty = -viewBox.at(1) * sy;
    const QString preserve = attributeValue(attributes, QStringLiteral("preserveAspectRatio")).trimmed();
    if (!preserve.startsWith(QStringLiteral("none"), Qt::CaseInsensitive)) {
        const bool slice = preserve.contains(QStringLiteral("slice"), Qt::CaseInsensitive);
        const double uniform = slice ? std::max(sx, sy) : std::min(sx, sy);
        const double renderedWidth = viewBox.at(2) * uniform;
        const double renderedHeight = viewBox.at(3) * uniform;
        double alignX = (viewport.width() - renderedWidth) * 0.5;
        double alignY = (viewport.height() - renderedHeight) * 0.5;
        if (preserve.contains(QStringLiteral("xMin"), Qt::CaseInsensitive)) alignX = 0.0;
        else if (preserve.contains(QStringLiteral("xMax"), Qt::CaseInsensitive)) alignX = viewport.width() - renderedWidth;
        if (preserve.contains(QStringLiteral("YMin"), Qt::CaseInsensitive)) alignY = 0.0;
        else if (preserve.contains(QStringLiteral("YMax"), Qt::CaseInsensitive)) alignY = viewport.height() - renderedHeight;
        sx = sy = uniform;
        tx = alignX - viewBox.at(0) * uniform;
        ty = alignY - viewBox.at(1) * uniform;
    }
    QTransform result(sx, 0.0, 0.0, sy, tx, ty);
    if (!result.isInvertible()) {
        if (warnings) warnings->add(QObject::tr("The SVG viewBox transform was invalid and was ignored."));
        return {};
    }
    return result;
}

QSizeF rootViewport(const QXmlStreamAttributes &attributes, WarningSink *warnings)
{
    bool viewBoxComplete = false;
    const QVector<double> viewBox = parseNumberList(
        attributeValue(attributes, QStringLiteral("viewBox")), 4, &viewBoxComplete);
    const bool validViewBox = viewBoxComplete && viewBox.size() == 4
        && viewBox.at(2) > 0.0 && viewBox.at(3) > 0.0;
    const double fallbackWidth = validViewBox ? viewBox.at(2) : 300.0;
    const double fallbackHeight = validViewBox ? viewBox.at(3) : 150.0;
    const QString widthText = attributeValue(attributes, QStringLiteral("width"));
    const QString heightText = attributeValue(attributes, QStringLiteral("height"));
    const auto parsedWidth = parseLength(widthText, fallbackWidth, false);
    const auto parsedHeight = parseLength(heightText, fallbackHeight, false);
    if (!widthText.trimmed().isEmpty() && !parsedWidth && warnings) {
        warnings->add(QObject::tr("The SVG width was invalid; the viewBox or standalone default width was used."));
    }
    if (!heightText.trimmed().isEmpty() && !parsedHeight && warnings) {
        warnings->add(QObject::tr("The SVG height was invalid; the viewBox or standalone default height was used."));
    }
    double width = parsedWidth.value_or(fallbackWidth);
    double height = parsedHeight.value_or(fallbackHeight);
    if (width <= 0.0 || height <= 0.0 || width > MaximumCanvasExtent || height > MaximumCanvasExtent) {
        if (warnings) warnings->add(QObject::tr("SVG canvas dimensions were clamped to the supported 1–32768 pixel range."));
    }
    width = std::clamp(width, 1.0, static_cast<double>(MaximumCanvasExtent));
    height = std::clamp(height, 1.0, static_cast<double>(MaximumCanvasExtent));
    return QSizeF(width, height);
}

QString painterPathData(const QPainterPath &path, const bool close)
{
    QString output;
    bool subpathActive = false;
    for (int index = 0; index < path.elementCount(); ++index) {
        const QPainterPath::Element element = path.elementAt(index);
        if (element.isMoveTo()) {
            if (close && subpathActive) output += QStringLiteral("Z ");
            output += QStringLiteral("M %1 %2 ").arg(numberText(element.x), numberText(element.y));
            subpathActive = true;
        } else if (element.isLineTo()) {
            output += QStringLiteral("L %1 %2 ").arg(numberText(element.x), numberText(element.y));
        } else if (element.isCurveTo() && index + 2 < path.elementCount()) {
            const QPainterPath::Element c2 = path.elementAt(index + 1);
            const QPainterPath::Element end = path.elementAt(index + 2);
            output += QStringLiteral("C %1 %2 %3 %4 %5 %6 ")
                .arg(numberText(element.x), numberText(element.y),
                     numberText(c2.x), numberText(c2.y),
                     numberText(end.x), numberText(end.y));
            index += 2;
        }
    }
    if (close && subpathActive) output += QLatin1Char('Z');
    return output.trimmed();
}

QVector<QPointF> regularShapePoints(const VectorShape &shape)
{
    QVector<QPointF> points;
    const int count = shape.type == VectorShapeType::Star
        ? shape.polygonSides * 2 : shape.polygonSides;
    points.reserve(count);
    const QPointF centre = shape.bounds.center();
    const double rx = shape.bounds.width() * 0.5;
    const double ry = shape.bounds.height() * 0.5;
    const double rotation = shape.vertexRotationDegrees * std::numbers::pi / 180.0;
    for (int index = 0; index < count; ++index) {
        const double angle = rotation + (2.0 * std::numbers::pi * index / count);
        const double ratio = shape.type == VectorShapeType::Star && (index % 2 == 1)
            ? shape.starInnerRatio : 1.0;
        points.push_back(QPointF(centre.x() + std::cos(angle) * rx * ratio,
                                 centre.y() + std::sin(angle) * ry * ratio));
    }
    return points;
}

QString pointsText(const QVector<QPointF> &points)
{
    QStringList parts;
    parts.reserve(points.size());
    for (const QPointF &point : points) {
        parts.push_back(QStringLiteral("%1,%2").arg(numberText(point.x()), numberText(point.y())));
    }
    return parts.join(QLatin1Char(' '));
}

struct ArrowMarkerSpec {
    VectorArrowheadType type = VectorArrowheadType::None;
    double scale = 1.0;
    bool start = false;
};

QString arrowMarkerId(const VectorArrowheadType type,
                      const double scale,
                      const bool start)
{
    const int scaleToken = std::clamp(qRound(scale * 1000.0), 100, 10000);
    return QStringLiteral("vfx-arrow-%1-%2-%3")
        .arg(start ? QStringLiteral("start") : QStringLiteral("end"),
             vectorArrowheadTypeToString(type), QString::number(scaleToken));
}

void collectArrowMarkers(const LayerNode &layer,
                         QMap<QString, ArrowMarkerSpec> *markers)
{
    if (!markers) return;
    if (layer.type == LayerType::Vector) {
        for (const VectorShape &shape : layer.vectorData.objects) {
            if (!shape.isOpenPath() || !shape.stroke.enabled) continue;
            if (shape.stroke.startArrowhead != VectorArrowheadType::None) {
                const QString id = arrowMarkerId(shape.stroke.startArrowhead,
                                                 shape.stroke.startArrowScale, true);
                markers->insert(id, ArrowMarkerSpec {shape.stroke.startArrowhead,
                                                      shape.stroke.startArrowScale, true});
            }
            if (shape.stroke.endArrowhead != VectorArrowheadType::None) {
                const QString id = arrowMarkerId(shape.stroke.endArrowhead,
                                                 shape.stroke.endArrowScale, false);
                markers->insert(id, ArrowMarkerSpec {shape.stroke.endArrowhead,
                                                      shape.stroke.endArrowScale, false});
            }
        }
    }
    for (const LayerNode &child : layer.children) collectArrowMarkers(child, markers);
}

void writeArrowMarkerDefinition(QXmlStreamWriter *writer,
                                const QString &id,
                                const ArrowMarkerSpec &spec)
{
    if (!writer || spec.type == VectorArrowheadType::None) return;
    // SVG orients an end marker along the path and a start marker in the
    // opposite visual direction. `baseSign` therefore points from the marker
    // centre back into the shaft for either endpoint. Marker geometry is
    // centred on refX=0, matching the native renderer: the sharp point sits
    // beyond the path endpoint while the shaft terminates inside the marker.
    const double baseSign = spec.start ? 1.0 : -1.0;
    writer->writeStartElement(QStringLiteral("marker"));
    writer->writeAttribute(QStringLiteral("id"), id);
    writer->writeAttribute(QStringLiteral("markerUnits"), QStringLiteral("strokeWidth"));
    writer->writeAttribute(QStringLiteral("orient"), QStringLiteral("auto"));
    writer->writeAttribute(QStringLiteral("refX"), QStringLiteral("0"));
    writer->writeAttribute(QStringLiteral("refY"), QStringLiteral("0"));
    writer->writeAttribute(QStringLiteral("markerWidth"), numberText(10.0 * spec.scale));
    writer->writeAttribute(QStringLiteral("markerHeight"), numberText(6.0 * spec.scale));
    writer->writeAttribute(QStringLiteral("viewBox"), QStringLiteral("-5 -3 10 6"));
    writer->writeAttribute(QStringLiteral("overflow"), QStringLiteral("visible"));

    const auto point = [baseSign](const double x, const double y) {
        return QStringLiteral("%1,%2").arg(numberText(baseSign * x), numberText(y));
    };
    if (spec.type == VectorArrowheadType::Open) {
        writer->writeStartElement(QStringLiteral("path"));
        writer->writeAttribute(QStringLiteral("d"),
            QStringLiteral("M %1 L %2 L %3")
                .arg(point(2.0, -1.8), point(-2.0, 0.0), point(2.0, 1.8)));
        writer->writeAttribute(QStringLiteral("fill"), QStringLiteral("none"));
        writer->writeAttribute(QStringLiteral("stroke"), QStringLiteral("context-stroke"));
        writer->writeAttribute(QStringLiteral("stroke-width"), QStringLiteral("0.7"));
        writer->writeAttribute(QStringLiteral("stroke-linecap"), QStringLiteral("round"));
        writer->writeAttribute(QStringLiteral("stroke-linejoin"), QStringLiteral("round"));
        writer->writeEndElement();
    } else if (spec.type == VectorArrowheadType::Circle) {
        writer->writeStartElement(QStringLiteral("circle"));
        writer->writeAttribute(QStringLiteral("cx"), QStringLiteral("0"));
        writer->writeAttribute(QStringLiteral("cy"), QStringLiteral("0"));
        writer->writeAttribute(QStringLiteral("r"), QStringLiteral("1.5"));
        writer->writeAttribute(QStringLiteral("fill"), QStringLiteral("context-stroke"));
        writer->writeEndElement();
    } else {
        QString points;
        if (spec.type == VectorArrowheadType::Triangle) {
            points = QStringLiteral("%1 %2 %3")
                .arg(point(-2.0, 0.0), point(2.0, -2.0), point(2.0, 2.0));
        } else if (spec.type == VectorArrowheadType::Stealth) {
            points = QStringLiteral("%1 %2 %3 %4")
                .arg(point(-2.3, 0.0))
                .arg(point(2.3, -2.0))
                .arg(point(0.368, 0.0))
                .arg(point(2.3, 2.0));
        } else {
            points = QStringLiteral("%1 %2 %3 %4")
                .arg(point(-1.9, 0.0))
                .arg(point(0.0, -1.55))
                .arg(point(1.9, 0.0))
                .arg(point(0.0, 1.55));
        }
        writer->writeStartElement(QStringLiteral("polygon"));
        writer->writeAttribute(QStringLiteral("points"), points);
        writer->writeAttribute(QStringLiteral("fill"), QStringLiteral("context-stroke"));
        writer->writeEndElement();
    }
    writer->writeEndElement();
}

void writeStyle(QXmlStreamWriter *writer, const VectorShape &shape,
                WarningSink *warnings)
{
    if (shape.fill.enabled && !shape.isOpenPath()) {
        writer->writeAttribute(QStringLiteral("fill"), colourText(shape.fill.colour));
        const double fillOpacity = std::clamp(shape.fill.opacity * shape.fill.colour.alphaF(), 0.0, 1.0);
        if (fillOpacity < 1.0) writer->writeAttribute(QStringLiteral("fill-opacity"), numberText(fillOpacity));
    } else writer->writeAttribute(QStringLiteral("fill"), QStringLiteral("none"));
    if (shape.stroke.enabled) {
        writer->writeAttribute(QStringLiteral("stroke"), colourText(shape.stroke.colour));
        writer->writeAttribute(QStringLiteral("stroke-width"), numberText(shape.stroke.width));
        writer->writeAttribute(QStringLiteral("vector-effect"), QStringLiteral("non-scaling-stroke"));
        const double strokeOpacity = std::clamp(shape.stroke.opacity * shape.stroke.colour.alphaF(), 0.0, 1.0);
        if (strokeOpacity < 1.0) writer->writeAttribute(QStringLiteral("stroke-opacity"), numberText(strokeOpacity));
        switch (shape.stroke.cap) {
        case VectorStrokeCap::Butt: writer->writeAttribute(QStringLiteral("stroke-linecap"), QStringLiteral("butt")); break;
        case VectorStrokeCap::Round: writer->writeAttribute(QStringLiteral("stroke-linecap"), QStringLiteral("round")); break;
        case VectorStrokeCap::Square: writer->writeAttribute(QStringLiteral("stroke-linecap"), QStringLiteral("square")); break;
        }
        switch (shape.stroke.join) {
        case VectorStrokeJoin::Miter: writer->writeAttribute(QStringLiteral("stroke-linejoin"), QStringLiteral("miter")); break;
        case VectorStrokeJoin::Round: writer->writeAttribute(QStringLiteral("stroke-linejoin"), QStringLiteral("round")); break;
        case VectorStrokeJoin::Bevel: writer->writeAttribute(QStringLiteral("stroke-linejoin"), QStringLiteral("bevel")); break;
        }
        writer->writeAttribute(QStringLiteral("stroke-miterlimit"), numberText(shape.stroke.miterLimit));
        if (shape.stroke.pattern == VectorStrokePattern::Dashed) {
            writer->writeAttribute(
                QStringLiteral("stroke-dasharray"),
                QStringLiteral("%1 %2").arg(numberText(shape.stroke.dashLength),
                                               numberText(shape.stroke.gapLength)));
            if (std::abs(shape.stroke.dashOffset) > 1.0e-12) {
                writer->writeAttribute(QStringLiteral("stroke-dashoffset"),
                                       numberText(shape.stroke.dashOffset));
            }
        }
        if (shape.isOpenPath()) {
            if (shape.stroke.startArrowhead != VectorArrowheadType::None) {
                writer->writeAttribute(
                    QStringLiteral("marker-start"),
                    QStringLiteral("url(#%1)").arg(arrowMarkerId(
                        shape.stroke.startArrowhead, shape.stroke.startArrowScale, true)));
                writer->writeAttribute(QStringLiteral("data-vfx-start-arrowhead"),
                                       vectorArrowheadTypeToString(shape.stroke.startArrowhead));
                writer->writeAttribute(QStringLiteral("data-vfx-start-arrow-scale"),
                                       numberText(shape.stroke.startArrowScale));
            }
            if (shape.stroke.endArrowhead != VectorArrowheadType::None) {
                writer->writeAttribute(
                    QStringLiteral("marker-end"),
                    QStringLiteral("url(#%1)").arg(arrowMarkerId(
                        shape.stroke.endArrowhead, shape.stroke.endArrowScale, false)));
                writer->writeAttribute(QStringLiteral("data-vfx-end-arrowhead"),
                                       vectorArrowheadTypeToString(shape.stroke.endArrowhead));
                writer->writeAttribute(QStringLiteral("data-vfx-end-arrow-scale"),
                                       numberText(shape.stroke.endArrowScale));
            }
        }
        if (shape.stroke.alignment != VectorStrokeAlignment::Centre && warnings) {
            warnings->add(QObject::tr("Inside/outside stroke alignment is retained for Photo Lab round-tripping but standard SVG viewers render it centred."));
        }
    } else writer->writeAttribute(QStringLiteral("stroke"), QStringLiteral("none"));
}

void writeShape(QXmlStreamWriter *writer, const VectorShape &shape, WarningSink *warnings)
{
    QString tag;
    switch (shape.type) {
    case VectorShapeType::Rectangle: tag = QStringLiteral("rect"); break;
    case VectorShapeType::RoundedRectangle:
        tag = shape.cornerRadii.allEqual() ? QStringLiteral("rect") : QStringLiteral("path");
        break;
    case VectorShapeType::Ellipse: tag = QStringLiteral("ellipse"); break;
    case VectorShapeType::Line: tag = QStringLiteral("line"); break;
    case VectorShapeType::Polygon:
    case VectorShapeType::Star: tag = QStringLiteral("polygon"); break;
    case VectorShapeType::Arrow:
    case VectorShapeType::Path: tag = QStringLiteral("path"); break;
    }
    writer->writeStartElement(tag);
    writer->writeAttribute(QStringLiteral("data-vfx-shape-type"), vectorShapeTypeToString(shape.type));
    const QString transform = transformText(shape.transform);
    if (!transform.isEmpty()) writer->writeAttribute(QStringLiteral("transform"), transform);
    switch (shape.type) {
    case VectorShapeType::Rectangle:
        writer->writeAttribute(QStringLiteral("x"), numberText(shape.bounds.x()));
        writer->writeAttribute(QStringLiteral("y"), numberText(shape.bounds.y()));
        writer->writeAttribute(QStringLiteral("width"), numberText(shape.bounds.width()));
        writer->writeAttribute(QStringLiteral("height"), numberText(shape.bounds.height()));
        break;
    case VectorShapeType::RoundedRectangle:
        if (shape.cornerRadii.allEqual()) {
            writer->writeAttribute(QStringLiteral("x"), numberText(shape.bounds.x()));
            writer->writeAttribute(QStringLiteral("y"), numberText(shape.bounds.y()));
            writer->writeAttribute(QStringLiteral("width"), numberText(shape.bounds.width()));
            writer->writeAttribute(QStringLiteral("height"), numberText(shape.bounds.height()));
            writer->writeAttribute(QStringLiteral("rx"), numberText(shape.cornerRadii.topLeft));
            writer->writeAttribute(QStringLiteral("ry"), numberText(shape.cornerRadii.topLeft));
        } else {
            writer->writeAttribute(QStringLiteral("d"), painterPathData(shape.geometryPath(), true));
        }
        break;
    case VectorShapeType::Ellipse:
        writer->writeAttribute(QStringLiteral("cx"), numberText(shape.bounds.center().x()));
        writer->writeAttribute(QStringLiteral("cy"), numberText(shape.bounds.center().y()));
        writer->writeAttribute(QStringLiteral("rx"), numberText(shape.bounds.width() * 0.5));
        writer->writeAttribute(QStringLiteral("ry"), numberText(shape.bounds.height() * 0.5));
        break;
    case VectorShapeType::Line:
        writer->writeAttribute(QStringLiteral("x1"), numberText(shape.lineStart.x()));
        writer->writeAttribute(QStringLiteral("y1"), numberText(shape.lineStart.y()));
        writer->writeAttribute(QStringLiteral("x2"), numberText(shape.lineEnd.x()));
        writer->writeAttribute(QStringLiteral("y2"), numberText(shape.lineEnd.y()));
        break;
    case VectorShapeType::Polygon:
    case VectorShapeType::Star:
        writer->writeAttribute(QStringLiteral("points"), pointsText(regularShapePoints(shape)));
        break;
    case VectorShapeType::Arrow:
        writer->writeAttribute(QStringLiteral("d"), painterPathData(shape.geometryPath(), true));
        writer->writeAttribute(QStringLiteral("data-vfx-arrow-head-length"),
                               numberText(shape.arrowHeadLengthRatio));
        writer->writeAttribute(QStringLiteral("data-vfx-arrow-shaft-width"),
                               numberText(shape.arrowShaftWidthRatio));
        break;
    case VectorShapeType::Path:
        writer->writeAttribute(QStringLiteral("d"),
            painterPathData(shape.geometryPath(), shape.bezierPath.closed));
        break;
    }
    if (shape.type == VectorShapeType::Path && shape.bezierPath.closed
        && shape.fill.enabled) {
        writer->writeAttribute(
            QStringLiteral("fill-rule"),
            shape.pathFillRule == VectorPathFillRule::NonZero
                ? QStringLiteral("nonzero") : QStringLiteral("evenodd"));
    }
    writeStyle(writer, shape, warnings);
    writer->writeEndElement();
}

void writeCommonLayerAttributes(QXmlStreamWriter *writer, const LayerNode &layer)
{
    writer->writeAttribute(QStringLiteral("data-vfx-name"), layer.name.left(1024));
    writer->writeAttribute(QStringLiteral("data-vfx-blend-mode"), blendModeToString(layer.blendMode));
    if (!layer.visible) writer->writeAttribute(QStringLiteral("display"), QStringLiteral("none"));
    if (layer.opacity < 1.0) writer->writeAttribute(QStringLiteral("opacity"), numberText(layer.opacity));
    const QString transform = transformText(layer.transform);
    if (!transform.isEmpty()) writer->writeAttribute(QStringLiteral("transform"), transform);
    const QString cssBlend = blendModeCss(layer.blendMode);
    if (!cssBlend.isEmpty()) writer->writeAttribute(QStringLiteral("style"), QStringLiteral("mix-blend-mode:%1").arg(cssBlend));
}

bool subtreeHasSupportedLayer(const LayerNode &layer)
{
    if (layer.type == LayerType::Vector || layer.type == LayerType::Text) return true;
    if (layer.type != LayerType::Group) return false;
    for (const LayerNode &child : layer.children) {
        if (subtreeHasSupportedLayer(child)) return true;
    }
    return false;
}

struct ExportComplexity {
    qint64 treeNodes = 0;
    qint64 editableLayers = 0;
    qint64 vectorObjects = 0;
    qint64 pathNodes = 0;
    qsizetype textCharacters = 0;
};

bool inspectExportLayer(const LayerNode &layer,
                        const int depth,
                        ExportComplexity *complexity,
                        QString *errorMessage)
{
    if (!complexity) return false;
    if (depth > SvgWorkflow::MaximumNestingDepth) {
        setError(errorMessage, QObject::tr("The selected layer hierarchy exceeds the SVG nesting safety limit."));
        return false;
    }
    if (layer.type != LayerType::Vector && layer.type != LayerType::Text
        && layer.type != LayerType::Group) {
        return true;
    }
    if (++complexity->treeNodes > SvgWorkflow::MaximumLayerTreeNodeCount) {
        setError(errorMessage, QObject::tr("The editable layer hierarchy exceeds the SVG export safety limit."));
        return false;
    }
    if (!importedTransformSafe(layer.transform) || !std::isfinite(layer.opacity)
        || layer.opacity < 0.0 || layer.opacity > 1.0) {
        setError(errorMessage, QObject::tr("A selected editable layer contains an unsafe transform or opacity."));
        return false;
    }
    if (layer.type == LayerType::Vector) {
        if (!layer.vectorData.isSafe()) {
            setError(errorMessage, QObject::tr("A selected vector layer contains unsafe or malformed semantic geometry."));
            return false;
        }
        ++complexity->editableLayers;
        complexity->vectorObjects += layer.vectorData.objects.size();
        for (const VectorShape &shape : layer.vectorData.objects) {
            if (shape.type == VectorShapeType::Path) {
                complexity->pathNodes += shape.bezierPath.nodes.size();
                for (const VectorBezierPath &path : shape.additionalBezierPaths) {
                    complexity->pathNodes += path.nodes.size();
                }
            }
        }
    } else if (layer.type == LayerType::Text) {
        if (!layer.textData.isSafe()) {
            setError(errorMessage, QObject::tr("A selected text layer contains unsafe or malformed semantic text."));
            return false;
        }
        ++complexity->editableLayers;
        complexity->textCharacters += layer.textData.text.size();
    } else {
        for (const LayerNode &child : layer.children) {
            if (!inspectExportLayer(child, depth + 1, complexity, errorMessage)) return false;
        }
    }
    if (complexity->editableLayers > SvgWorkflow::MaximumEditableLayerCount
        || complexity->vectorObjects > SvgWorkflow::MaximumVectorObjectCount
        || complexity->pathNodes > SvgWorkflow::MaximumPathNodeCount
        || complexity->textCharacters > SvgWorkflow::MaximumTextCharacterCount) {
        setError(errorMessage, QObject::tr("The editable SVG content exceeds the bounded export complexity limit."));
        return false;
    }
    return true;
}

void writeLayer(QXmlStreamWriter *writer, const LayerNode &layer,
                WarningSink *warnings, int *exported, int *skipped)
{
    if (layer.type == LayerType::Vector) {
        if (!layer.vectorData.isSafe()) { if (skipped) ++*skipped; return; }
        writer->writeStartElement(QStringLiteral("g"));
        writer->writeAttribute(QStringLiteral("data-vfx-layer-type"), QStringLiteral("vector"));
        writer->writeAttribute(QStringLiteral("data-vfx-vector-data"), base64Json(layer.vectorData.toJson()));
        if (layer.vectorData.featherRadius > 0.0) {
            writer->writeAttribute(QStringLiteral("data-vfx-feather-radius"),
                                   numberText(layer.vectorData.featherRadius));
        }
        writeCommonLayerAttributes(writer, layer);
        if (layer.vectorData.featherRadius > 0.0 && warnings) {
            warnings->add(QObject::tr(
                "Vector Feather is preserved exactly in VFX Photo Lab round-trip metadata, but standard SVG has no exact representation for Photo Lab's combined-silhouette Feather kernel. External SVG viewers therefore receive the editable unfeathered vector appearance."));
        }
        if (layer.hasMask() && warnings) warnings->add(QObject::tr("Raster layer masks are not represented in SVG export."));
        for (const VectorShape &shape : layer.vectorData.objects) writeShape(writer, shape, warnings);
        writer->writeEndElement();
        if (exported) ++*exported;
        return;
    }
    if (layer.type == LayerType::Text) {
        if (!layer.textData.isSafe()) { if (skipped) ++*skipped; return; }
        writer->writeStartElement(QStringLiteral("g"));
        writer->writeAttribute(QStringLiteral("data-vfx-layer-type"), QStringLiteral("text"));
        writer->writeAttribute(QStringLiteral("data-vfx-text-data"), base64Json(layer.textData.toJson()));
        writeCommonLayerAttributes(writer, layer);
        if (layer.hasMask() && warnings) warnings->add(QObject::tr("Raster layer masks are not represented in SVG export."));
        const TextLayerData &data = layer.textData;
        QFont font(data.resolvedFamily());
        font.setPixelSize(std::max(1, qRound(data.fontSize)));
        font.setWeight(static_cast<QFont::Weight>(std::clamp(data.weight, 1, 1000)));
        font.setItalic(data.italic);
        const QFontMetricsF metrics(font);
        const QRectF box = data.mode == TextLayoutMode::Area ? data.area : data.semanticBox();
        double x = box.left();
        QString anchor = QStringLiteral("start");
        if (data.horizontalAlignment.testFlag(Qt::AlignHCenter)) { x = box.center().x(); anchor = QStringLiteral("middle"); }
        else if (data.horizontalAlignment.testFlag(Qt::AlignRight)) { x = box.right(); anchor = QStringLiteral("end"); }
        const double baseline = box.top() + metrics.ascent();
        writer->writeStartElement(QStringLiteral("text"));
        writer->writeAttribute(QStringLiteral("x"), numberText(x));
        writer->writeAttribute(QStringLiteral("y"), numberText(baseline));
        writer->writeAttribute(QStringLiteral("font-family"), data.requestedFamily.isEmpty() ? data.resolvedFamily() : data.requestedFamily);
        writer->writeAttribute(QStringLiteral("font-size"), numberText(data.fontSize));
        writer->writeAttribute(QStringLiteral("font-weight"), QString::number(data.weight));
        if (data.italic) writer->writeAttribute(QStringLiteral("font-style"), QStringLiteral("italic"));
        if (std::abs(data.tracking) > 1.0e-9) {
            writer->writeAttribute(QStringLiteral("letter-spacing"), numberText(data.tracking));
        }
        writer->writeAttribute(QStringLiteral("text-anchor"), anchor);
        writer->writeAttribute(QStringLiteral("fill"), colourText(data.colour));
        const double textOpacity = std::clamp(data.opacity * data.colour.alphaF(), 0.0, 1.0);
        if (textOpacity < 1.0) writer->writeAttribute(QStringLiteral("fill-opacity"), numberText(textOpacity));
        const QStringList lines = data.text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        const double lineAdvance = std::max(metrics.height(), data.fontSize * data.leading);
        for (int index = 0; index < lines.size(); ++index) {
            if (index == 0 && lines.size() == 1) writer->writeCharacters(lines.at(index));
            else {
                writer->writeStartElement(QStringLiteral("tspan"));
                writer->writeAttribute(QStringLiteral("x"), numberText(x));
                writer->writeAttribute(QStringLiteral("dy"), index == 0 ? QStringLiteral("0") : numberText(lineAdvance));
                writer->writeCharacters(lines.at(index));
                writer->writeEndElement();
            }
        }
        writer->writeEndElement();
        writer->writeEndElement();
        if (data.mode == TextLayoutMode::Area && warnings) {
            warnings->add(QObject::tr("Area-text wrapping is retained for Photo Lab round-tripping; external SVG viewers receive editable line text."));
        }
        if (exported) ++*exported;
        return;
    }
    if (layer.type == LayerType::Group) {
        if (!subtreeHasSupportedLayer(layer)) {
            if (skipped) ++*skipped;
            return;
        }
        writer->writeStartElement(QStringLiteral("g"));
        writer->writeAttribute(QStringLiteral("data-vfx-layer-type"), QStringLiteral("group"));
        writer->writeAttribute(QStringLiteral("data-vfx-group-mode"),
                               layer.groupCompositeMode == GroupCompositeMode::PassThrough
                                   ? QStringLiteral("pass-through") : QStringLiteral("isolated"));
        writeCommonLayerAttributes(writer, layer);
        if (layer.hasMask() && warnings) warnings->add(QObject::tr("Raster group masks are not represented in SVG export."));
        for (auto iterator = layer.children.crbegin(); iterator != layer.children.crend(); ++iterator) {
            writeLayer(writer, *iterator, warnings, exported, skipped);
        }
        writer->writeEndElement();
        return;
    }
    if (skipped) ++*skipped;
    if (warnings) {
        if (layer.type == LayerType::Raster || layer.type == LayerType::BaseImage) {
            warnings->add(QObject::tr("Raster layers are omitted from editable SVG export."));
        } else if (layer.type == LayerType::Adjustment) {
            warnings->add(QObject::tr("Adjustment layers are omitted from editable SVG export."));
        }
    }
}

void collectSelected(const QVector<LayerNode> &layers, const QSet<QUuid> &selected,
                     const QTransform &parentWorld, QVector<LayerNode> *output)
{
    for (const LayerNode &layer : layers) {
        const QTransform world = layer.transform * parentWorld;
        if (selected.contains(layer.id)) {
            LayerNode detached = layer;
            detached.transform = world;
            output->push_back(std::move(detached));
        } else {
            collectSelected(layer.children, selected, world, output);
        }
    }
}

} // namespace

bool SvgWorkflow::importFile(const QString &filePath,
                             SvgImportResult *result,
                             QString *errorMessage)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage, QObject::tr("Could not open the SVG file: %1").arg(file.errorString()));
        return false;
    }
    if (file.size() < 1 || file.size() > MaximumFileBytes) {
        setError(errorMessage, QObject::tr("SVG files must be between 1 byte and %1 MiB.")
                                 .arg(MaximumFileBytes / (1024 * 1024)));
        return false;
    }
    return importData(file.readAll(), QFileInfo(filePath).completeBaseName(), result, errorMessage);
}

bool SvgWorkflow::importData(const QByteArray &data,
                             const QString &sourceName,
                             SvgImportResult *result,
                             QString *errorMessage)
{
    if (!result) {
        setError(errorMessage, QObject::tr("No SVG import destination was supplied."));
        return false;
    }
    *result = {};
    if (data.isEmpty() || data.size() > MaximumFileBytes) {
        setError(errorMessage, QObject::tr("SVG data is empty or exceeds the %1 MiB safety limit.")
                                 .arg(MaximumFileBytes / (1024 * 1024)));
        return false;
    }
    QXmlStreamReader reader(data);
    while (!reader.atEnd() && !reader.isStartElement()) {
        reader.readNext();
        if (reader.isDTD() || reader.tokenType() == QXmlStreamReader::EntityReference) {
            setError(errorMessage, QObject::tr("DTD and entity declarations are not accepted in SVG imports."));
            return false;
        }
    }
    if (reader.atEnd() || reader.name().toString().compare(QStringLiteral("svg"), Qt::CaseInsensitive) != 0) {
        setError(errorMessage, QObject::tr("The file does not contain an SVG root element."));
        return false;
    }

    ImportContext context;
    // Reserve one tree node for the synthetic document root group created below.
    context.layerTreeNodeCount = 1;
    context.sourceName = sourceName.trimmed().isEmpty() ? QObject::tr("Imported SVG") : sourceName.trimmed();
    const QXmlStreamAttributes rootAttributes = reader.attributes();
    const QSizeF viewport = rootViewport(rootAttributes, &context.warnings);
    context.viewport = viewport;
    const SvgStyle rootStyle = inheritedStyle(SvgStyle(), rootAttributes, &context.warnings);
    bool rootTransformOk = true;
    const QTransform rootElementTransform = parseTransform(
        attributeValue(rootAttributes, QStringLiteral("transform")), &rootTransformOk);
    if (!rootTransformOk) context.warnings.add(QObject::tr("The SVG root transform was invalid and was ignored."));
    const QTransform viewBox = rootViewBoxTransform(rootAttributes, viewport, &context.warnings);
    QVector<LayerNode> children = parseChildren(&reader, rootStyle, &context, 1);
    filterUnsafeImportedLayers(&children, &context);
    qint64 importedTreeNodes = 1;
    if (!importedTreeWithinLimits(children, 1, &importedTreeNodes)) {
        setError(errorMessage, QObject::tr(
            "The editable SVG layer hierarchy exceeds the bounded complexity limit."));
        return false;
    }
    if (reader.hasError()) {
        setError(errorMessage, QObject::tr("SVG parse error at line %1, column %2: %3")
                                 .arg(reader.lineNumber()).arg(reader.columnNumber())
                                 .arg(reader.errorString()));
        return false;
    }
    if (children.isEmpty()) {
        setError(errorMessage, QObject::tr("The SVG contains no supported editable shapes, paths, groups, or text."));
        return false;
    }

    LayerNode root;
    root.type = LayerType::Group;
    root.name = context.sourceName.left(256);
    root.visible = localDisplayVisible(rootAttributes);
    root.opacity = localOpacity(rootAttributes);
    const QHash<QString, QString> rootPresentation = presentationMap(rootAttributes);
    const QString rootCustomBlend = attributeValue(rootAttributes, QStringLiteral("data-vfx-blend-mode"));
    const QString rootCssBlend = rootPresentation.value(QStringLiteral("mix-blend-mode")).trimmed().toLower();
    root.blendMode = rootCustomBlend.isEmpty()
        ? blendModeFromCss(rootCssBlend)
        : blendModeFromString(rootCustomBlend);
    if (rootCustomBlend.isEmpty() && !rootCssBlend.isEmpty()
        && rootCssBlend != QStringLiteral("normal") && root.blendMode == BlendMode::Copy) {
        context.warnings.add(QObject::tr("An unsupported SVG root blend mode was replaced with Normal."));
    }
    root.transform = viewBox * (rootTransformOk ? rootElementTransform : QTransform());
    if (!importedTransformSafe(root.transform)) {
        setError(errorMessage, QObject::tr("The SVG root transform is non-invertible or exceeds the safety bounds."));
        return false;
    }
    root.children = std::move(children);
    regenerateLayerIds(&root);

    result->canvasSize = QSize(std::clamp(qCeil(viewport.width()), 1, MaximumCanvasExtent),
                               std::clamp(qCeil(viewport.height()), 1, MaximumCanvasExtent));
    result->layers = {std::move(root)};
    result->skippedElementCount = context.skippedElementCount;
    result->warnings = context.warnings.take();
    std::function<int(const QVector<LayerNode> &)> countLayers = [&](const QVector<LayerNode> &layers) {
        int count = 0;
        for (const LayerNode &layer : layers) count += 1 + countLayers(layer.children);
        return count;
    };
    result->importedLayerCount = countLayers(result->layers);
    return true;
}

bool SvgWorkflow::exportFile(const QString &filePath,
                             const QSize &canvasSize,
                             const QVector<LayerNode> &layers,
                             const QSet<QUuid> &selectedRootIds,
                             SvgExportResult *result,
                             QString *errorMessage)
{
    SvgExportResult localResult;
    QByteArray data = exportData(canvasSize, layers, selectedRootIds, &localResult, errorMessage);
    if (data.isEmpty()) return false;
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(errorMessage, QObject::tr("Could not create the SVG file: %1").arg(file.errorString()));
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        setError(errorMessage, QObject::tr("Could not finish writing the SVG file: %1").arg(file.errorString()));
        return false;
    }
    if (result) *result = std::move(localResult);
    return true;
}

QByteArray SvgWorkflow::exportData(const QSize &canvasSize,
                                   const QVector<LayerNode> &layers,
                                   const QSet<QUuid> &selectedRootIds,
                                   SvgExportResult *result,
                                   QString *errorMessage)
{
    if (result) *result = {};
    if (canvasSize.width() < 1 || canvasSize.height() < 1
        || canvasSize.width() > MaximumCanvasExtent || canvasSize.height() > MaximumCanvasExtent) {
        setError(errorMessage, QObject::tr("SVG export requires a canvas between 1 and 32768 pixels."));
        return {};
    }
    QVector<LayerNode> roots;
    if (selectedRootIds.isEmpty()) roots = layers;
    else collectSelected(layers, selectedRootIds, QTransform(), &roots);
    ExportComplexity complexity;
    for (const LayerNode &root : std::as_const(roots)) {
        if (!inspectExportLayer(root, 1, &complexity, errorMessage)) return {};
    }
    if (complexity.editableLayers < 1) {
        setError(errorMessage, QObject::tr("There are no vector, path, text, or supported group layers to export."));
        return {};
    }

    QByteArray output;
    QBuffer buffer(&output);
    buffer.open(QIODevice::WriteOnly);
    QXmlStreamWriter writer(&buffer);
    writer.setAutoFormatting(true);
    writer.writeStartDocument(QStringLiteral("1.0"));
    writer.writeStartElement(QStringLiteral("svg"));
    writer.writeDefaultNamespace(QStringLiteral("http://www.w3.org/2000/svg"));
    writer.writeAttribute(QStringLiteral("version"), QStringLiteral("1.1"));
    writer.writeAttribute(QStringLiteral("width"), QString::number(canvasSize.width()));
    writer.writeAttribute(QStringLiteral("height"), QString::number(canvasSize.height()));
    writer.writeAttribute(QStringLiteral("viewBox"),
                          QStringLiteral("0 0 %1 %2").arg(canvasSize.width()).arg(canvasSize.height()));
    QMap<QString, ArrowMarkerSpec> arrowMarkers;
    for (const LayerNode &root : std::as_const(roots)) {
        collectArrowMarkers(root, &arrowMarkers);
    }
    if (!arrowMarkers.isEmpty()) {
        writer.writeStartElement(QStringLiteral("defs"));
        for (auto iterator = arrowMarkers.cbegin(); iterator != arrowMarkers.cend(); ++iterator) {
            writeArrowMarkerDefinition(&writer, iterator.key(), iterator.value());
        }
        writer.writeEndElement();
    }
    writer.writeStartElement(QStringLiteral("metadata"));
    writer.writeCharacters(QObject::tr("Editable SVG exported by VFX Photo Lab. data-vfx attributes preserve exact semantic round-tripping."));
    writer.writeEndElement();

    WarningSink warnings;
    int exported = 0;
    int skipped = 0;
    for (auto iterator = roots.crbegin(); iterator != roots.crend(); ++iterator) {
        writeLayer(&writer, *iterator, &warnings, &exported, &skipped);
    }
    writer.writeEndElement();
    writer.writeEndDocument();
    buffer.close();
    if (output.size() > MaximumFileBytes) {
        setError(errorMessage, QObject::tr("The editable SVG exceeds the %1 MiB round-trip safety limit.")
                                 .arg(MaximumFileBytes / (1024 * 1024)));
        return {};
    }
    if (exported < 1) {
        setError(errorMessage, QObject::tr("No supported editable layers could be written to SVG."));
        return {};
    }
    if (result) {
        result->exportedLayerCount = exported;
        result->skippedLayerCount = skipped;
        result->warnings = warnings.take();
    }
    return output;
}

} // namespace vfx
