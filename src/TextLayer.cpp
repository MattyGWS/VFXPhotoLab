#include "TextLayer.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QJsonValue>
#include <QTextLayout>
#include <QTextOption>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace vfx {
namespace {
constexpr double MaximumCoordinate = 1.0e9;
constexpr double MaximumDimension = 1.0e7;
constexpr double MaximumFontSize = 100000.0;

bool finiteBounded(double v, double bound) { return std::isfinite(v) && std::abs(v) <= bound; }

QFont makeFont(const TextLayerData &data, bool *missing = nullptr,
               const double fontSizeOverride = -1.0)
{
    bool isMissing = false;
    const QString family = data.resolvedFamily(&isMissing);
    const double size = fontSizeOverride > 0.0 ? fontSizeOverride : data.fontSize;
    QFont font(family);
    if (!data.requestedStyle.trimmed().isEmpty()) {
        const QFont styled = QFontDatabase::font(family, data.requestedStyle,
                                                 std::max(1, qRound(size)));
        if (!styled.family().isEmpty()) font = styled;
    }
    font.setPixelSize(std::max(1, qRound(size)));
    font.setWeight(static_cast<QFont::Weight>(std::clamp(data.weight, 1, 1000)));
    font.setItalic(data.italic);
    font.setLetterSpacing(QFont::AbsoluteSpacing, data.tracking);
    if (missing) *missing = isMissing;
    return font;
}

struct LayoutMeasurement {
    double width = 1.0;
    double height = 1.0;
};

LayoutMeasurement measureText(const TextLayerData &data,
                              const double requestedWidth,
                              const double fontSizeOverride)
{
    const QFont font = makeFont(data, nullptr, fontSizeOverride);
    const QFontMetricsF metrics(font);
    const double fontSize = fontSizeOverride > 0.0
        ? fontSizeOverride : data.fontSize;
    const double lineAdvance = std::max(metrics.height(), fontSize * data.leading);
    const QStringList paragraphs = data.text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    LayoutMeasurement result;
    result.width = 1.0;
    result.height = 0.0;

    if (data.mode == TextLayoutMode::Point) {
        for (const QString &paragraph : paragraphs) {
            const QString measured = paragraph.isEmpty() ? QStringLiteral(" ") : paragraph;
            result.width = std::max(result.width, metrics.horizontalAdvance(measured));
            result.height += lineAdvance;
        }
        result.height = std::max(1.0, result.height);
        return result;
    }

    const double width = std::max(1.0, requestedWidth > 0.0
        ? requestedWidth : data.area.width());
    result.width = width;
    for (const QString &paragraph : paragraphs) {
        if (paragraph.isEmpty()) {
            result.height += lineAdvance;
            continue;
        }
        QTextLayout layout(paragraph, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setAlignment(data.horizontalAlignment);
        layout.setTextOption(option);
        layout.beginLayout();
        int guard = 0;
        bool madeLine = false;
        while (++guard < 100000) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(width);
            result.height += std::max(line.height(), lineAdvance);
            madeLine = true;
        }
        layout.endLayout();
        if (!madeLine) result.height += lineAdvance;
    }
    result.height = std::max(1.0, result.height);
    return result;
}
}

QString textLayoutModeToString(TextLayoutMode mode) { return mode == TextLayoutMode::Area ? QStringLiteral("area") : QStringLiteral("point"); }
TextLayoutMode textLayoutModeFromString(const QString &value, bool *ok)
{
    const QString v = value.trimmed().toLower();
    if (ok) *ok = true;
    if (v == QStringLiteral("point")) return TextLayoutMode::Point;
    if (v == QStringLiteral("area")) return TextLayoutMode::Area;
    if (ok) *ok = false;
    return TextLayoutMode::Point;
}
QString textOverflowModeToString(TextOverflowMode mode) { return mode == TextOverflowMode::Clip ? QStringLiteral("clip") : QStringLiteral("auto-height"); }
TextOverflowMode textOverflowModeFromString(const QString &value, bool *ok)
{
    const QString v = value.trimmed().toLower();
    if (ok) *ok = true;
    if (v == QStringLiteral("auto-height")) return TextOverflowMode::AutoHeight;
    if (v == QStringLiteral("clip")) return TextOverflowMode::Clip;
    if (ok) *ok = false;
    return TextOverflowMode::AutoHeight;
}

void TextLayerData::normalise()
{
    schema = CurrentSchema;
    if (text.size() > MaximumTextLength) text.truncate(MaximumTextLength);
    requestedFamily = requestedFamily.trimmed();
    requestedStyle = requestedStyle.trimmed();
    weight = std::clamp(weight, 1, 1000);
    fontSize = std::clamp(std::isfinite(fontSize) ? fontSize : 32.0, 1.0, MaximumFontSize);
    opacity = std::clamp(std::isfinite(opacity) ? opacity : 1.0, 0.0, 1.0);
    tracking = std::clamp(std::isfinite(tracking) ? tracking : 0.0, -1000.0, 10000.0);
    leading = std::clamp(std::isfinite(leading) ? leading : 1.2, 0.1, 20.0);
    if (!colour.isValid()) colour = QColor(Qt::black);
    if (!finiteBounded(origin.x(), MaximumCoordinate) || !finiteBounded(origin.y(), MaximumCoordinate)) origin = {};
    area = area.normalized();
    if (!finiteBounded(area.x(), MaximumCoordinate) || !finiteBounded(area.y(), MaximumCoordinate)
        || !finiteBounded(area.width(), MaximumDimension) || !finiteBounded(area.height(), MaximumDimension)
        || area.width() < 1.0 || area.height() < 1.0) area = QRectF(origin, QSizeF(320.0, 120.0));
    horizontalAlignment &= (Qt::AlignLeft | Qt::AlignRight | Qt::AlignHCenter | Qt::AlignJustify);
    if (!horizontalAlignment) horizontalAlignment = Qt::AlignLeft;
    revision = std::max<quint64>(1, revision);
}

bool TextLayerData::isSafe() const
{
    if (schema != CurrentSchema || text.size() > MaximumTextLength || !colour.isValid()) return false;
    if (!finiteBounded(origin.x(), MaximumCoordinate) || !finiteBounded(origin.y(), MaximumCoordinate)) return false;
    if (!finiteBounded(area.x(), MaximumCoordinate) || !finiteBounded(area.y(), MaximumCoordinate)
        || !finiteBounded(area.width(), MaximumDimension) || !finiteBounded(area.height(), MaximumDimension)
        || area.width() < 1.0 || area.height() < 1.0) return false;
    return weight >= 1 && weight <= 1000 && fontSize >= 1.0 && fontSize <= MaximumFontSize
        && opacity >= 0.0 && opacity <= 1.0 && tracking >= -1000.0 && tracking <= 10000.0
        && leading >= 0.1 && leading <= 20.0 && revision > 0;
}

QString TextLayerData::resolvedFamily(bool *missing) const
{
    const QString wanted = requestedFamily.trimmed();
    const QStringList families = QFontDatabase::families();
    const bool found = !wanted.isEmpty() && families.contains(wanted, Qt::CaseInsensitive);
    if (missing) *missing = !wanted.isEmpty() && !found;
    if (found) {
        for (const QString &family : families) if (family.compare(wanted, Qt::CaseInsensitive) == 0) return family;
    }
    const QFont fallback = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    return fallback.family().isEmpty() ? QStringLiteral("Sans Serif") : fallback.family();
}

QRectF TextLayerData::semanticBox() const
{
    const LayoutMeasurement measurement = measureText(*this, area.width(), fontSize);
    if (mode == TextLayoutMode::Point) {
        return QRectF(origin, QSizeF(measurement.width, measurement.height));
    }
    const double height = overflow == TextOverflowMode::AutoHeight
        ? std::max(area.height(), measurement.height)
        : area.height();
    return QRectF(area.topLeft(), QSizeF(std::max(1.0, area.width()),
                                         std::max(1.0, height)));
}

double TextLayerData::requiredHeight(const double width,
                                     const double fontSizeOverride) const
{
    return measureText(*this, width > 0.0 ? width : area.width(),
                       fontSizeOverride > 0.0 ? fontSizeOverride : fontSize).height;
}

double TextLayerData::fittedFontSize(const double width,
                                     const double height,
                                     const double maximumFontSizeValue) const
{
    const double targetHeight = std::max(1.0, height);
    double low = 1.0;
    double high = std::clamp(maximumFontSizeValue, 1.0, MaximumFontSize);
    if (requiredHeight(width, high) <= targetHeight) return high;
    for (int iteration = 0; iteration < 24; ++iteration) {
        const double middle = (low + high) * 0.5;
        if (requiredHeight(width, middle) <= targetHeight) low = middle;
        else high = middle;
    }
    return std::clamp(low, 1.0, maximumFontSizeValue);
}

void TextLayerData::growBoxToFit()
{
    if (mode == TextLayoutMode::Area && overflow == TextOverflowMode::AutoHeight) {
        area.setHeight(std::max(area.height(), requiredHeight(area.width())));
    }
}

QRectF TextLayerData::localBounds() const { return semanticBox().adjusted(-2.0, -2.0, 2.0, 2.0); }
QRectF TextLayerData::contentBounds(const QTransform &worldTransform) const { return worldTransform.mapRect(localBounds()); }

quint64 TextLayerData::fingerprint() const
{
    quint64 h = qHash(text) ^ (qHash(requestedFamily) << 1) ^ (qHash(requestedStyle) << 2);
    auto mix = [&h](quint64 v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
    mix(qHash(origin.x())); mix(qHash(origin.y())); mix(qHash(area.x())); mix(qHash(area.y()));
    mix(qHash(area.width())); mix(qHash(area.height())); mix(qHash(fontSize)); mix(qHash(tracking)); mix(qHash(leading));
    mix(static_cast<quint64>(mode)); mix(static_cast<quint64>(overflow)); mix(static_cast<quint64>(weight));
    mix(static_cast<quint64>(italic)); mix(static_cast<quint64>(colour.rgba64().toArgb32())); mix(qHash(opacity)); mix(revision);
    return h;
}
qint64 TextLayerData::estimatedBytes() const { return sizeof(TextLayerData) + text.size() * 2LL + requestedFamily.size() * 2LL + requestedStyle.size() * 2LL; }

QJsonObject TextLayerData::toJson(bool *ok) const
{
    if (ok) *ok = isSafe();
    if (!isSafe()) return {};
    QJsonObject o;
    o[QStringLiteral("schema")] = static_cast<int>(schema);
    o[QStringLiteral("text")] = text;
    o[QStringLiteral("mode")] = textLayoutModeToString(mode);
    o[QStringLiteral("originX")] = origin.x(); o[QStringLiteral("originY")] = origin.y();
    o[QStringLiteral("areaX")] = area.x(); o[QStringLiteral("areaY")] = area.y();
    o[QStringLiteral("areaWidth")] = area.width(); o[QStringLiteral("areaHeight")] = area.height();
    o[QStringLiteral("family")] = requestedFamily; o[QStringLiteral("style")] = requestedStyle;
    o[QStringLiteral("weight")] = weight; o[QStringLiteral("italic")] = italic; o[QStringLiteral("fontSize")] = fontSize;
    o[QStringLiteral("colour")] = colour.name(QColor::HexArgb); o[QStringLiteral("opacity")] = opacity;
    o[QStringLiteral("alignment")] = static_cast<int>(horizontalAlignment);
    o[QStringLiteral("tracking")] = tracking; o[QStringLiteral("leading")] = leading;
    o[QStringLiteral("overflow")] = textOverflowModeToString(overflow);
    o[QStringLiteral("revision")] = QString::number(revision);
    return o;
}

TextLayerData TextLayerData::fromJson(const QJsonObject &o, bool *ok)
{
    TextLayerData d;
    bool modeOk = false, overflowOk = false, revisionOk = false;
    d.schema = static_cast<quint32>(o.value(QStringLiteral("schema")).toInt());
    d.text = o.value(QStringLiteral("text")).toString();
    d.mode = textLayoutModeFromString(o.value(QStringLiteral("mode")).toString(), &modeOk);
    d.origin = QPointF(o.value(QStringLiteral("originX")).toDouble(), o.value(QStringLiteral("originY")).toDouble());
    d.area = QRectF(o.value(QStringLiteral("areaX")).toDouble(), o.value(QStringLiteral("areaY")).toDouble(),
                    o.value(QStringLiteral("areaWidth")).toDouble(), o.value(QStringLiteral("areaHeight")).toDouble());
    d.requestedFamily = o.value(QStringLiteral("family")).toString(); d.requestedStyle = o.value(QStringLiteral("style")).toString();
    d.weight = o.value(QStringLiteral("weight")).toInt(400); d.italic = o.value(QStringLiteral("italic")).toBool();
    d.fontSize = o.value(QStringLiteral("fontSize")).toDouble(32.0);
    d.colour = QColor(o.value(QStringLiteral("colour")).toString()); d.opacity = o.value(QStringLiteral("opacity")).toDouble(1.0);
    d.horizontalAlignment = static_cast<Qt::Alignment>(o.value(QStringLiteral("alignment")).toInt(static_cast<int>(Qt::AlignLeft)));
    d.tracking = o.value(QStringLiteral("tracking")).toDouble(); d.leading = o.value(QStringLiteral("leading")).toDouble(1.2);
    d.overflow = textOverflowModeFromString(o.value(QStringLiteral("overflow")).toString(), &overflowOk);
    d.revision = o.value(QStringLiteral("revision")).toString().toULongLong(&revisionOk);
    const bool valid = modeOk && overflowOk && revisionOk && d.isSafe();
    if (ok) *ok = valid;
    return d;
}

} // namespace vfx
