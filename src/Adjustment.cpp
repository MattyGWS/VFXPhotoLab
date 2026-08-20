#include "Adjustment.h"
#include "TransformSafety.h"
#include "SpatialFilter.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <type_traits>
#include <utility>

namespace vfx {
namespace {

QString encodeImage(const QImage &image, bool *ok)
{
    if (ok) *ok = true;
    if (image.isNull()) return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    const bool encoded = buffer.open(QIODevice::WriteOnly)
        && image.save(&buffer, "PNG") && !bytes.isEmpty();
    if (ok) *ok = encoded;
    return encoded ? QString::fromLatin1(bytes.toBase64()) : QString();
}

QImage decodeImage(const QString &encoded, bool *ok)
{
    constexpr int MaximumImageExtent = 32768;
    if (encoded.isEmpty()) {
        if (ok) *ok = true;
        return {};
    }

    const QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        if (ok) *ok = false;
        return {};
    }
    QImageReader reader(&buffer, "PNG");
    const QSize declaredSize = reader.size();
    if (declaredSize.isValid()
        && (declaredSize.width() < 1 || declaredSize.height() < 1
            || declaredSize.width() > MaximumImageExtent
            || declaredSize.height() > MaximumImageExtent)) {
        if (ok) *ok = false;
        return {};
    }
    const QImage image = reader.read();
    const bool loaded = !image.isNull()
        && image.width() >= 1 && image.height() >= 1
        && image.width() <= MaximumImageExtent
        && image.height() <= MaximumImageExtent;
    if (ok) *ok = loaded;
    return loaded ? image : QImage();
}

constexpr double MinimumLevelsGap = 1.0 / 65535.0;
constexpr double MinimumCurveGap = 1.0 / 65535.0;
constexpr int MaximumCurvePoints = 64;

quint64 lutTableFingerprint(const LutParameters &parameters)
{
    constexpr quint64 offset = UINT64_C(14695981039346656037);
    constexpr quint64 prime = UINT64_C(1099511628211);
    quint64 hash = offset;
    const auto appendBytes = [&hash](const void *data, const qsizetype size) {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (qsizetype index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= prime;
        }
    };
    appendBytes(&parameters.shaperSize, sizeof(parameters.shaperSize));
    appendBytes(&parameters.cubeSize, sizeof(parameters.cubeSize));
    appendBytes(parameters.shaperDomainMin.data(), sizeof(double) * 3);
    appendBytes(parameters.shaperDomainMax.data(), sizeof(double) * 3);
    appendBytes(parameters.cubeDomainMin.data(), sizeof(double) * 3);
    appendBytes(parameters.cubeDomainMax.data(), sizeof(double) * 3);
    if (!parameters.shaperData.isEmpty()) {
        appendBytes(parameters.shaperData.constData(),
                    parameters.shaperData.size() * qsizetype(sizeof(float)));
    }
    if (!parameters.cubeData.isEmpty()) {
        appendBytes(parameters.cubeData.constData(),
                    parameters.cubeData.size() * qsizetype(sizeof(float)));
    }
    // Zero is reserved for "not calculated".
    return hash == 0 ? 1 : hash;
}

bool readFiniteJsonNumber(const QJsonObject &object,
                          const QString &key,
                          const double fallback,
                          double *value)
{
    const QJsonValue encoded = object.value(key);
    if (!encoded.isDouble()) {
        if (value) *value = fallback;
        return false;
    }
    const double decoded = encoded.toDouble(fallback);
    if (value) *value = decoded;
    return std::isfinite(decoded);
}

QJsonObject levelsChannelToJson(const LevelsChannelParameters &channel)
{
    QJsonObject object;
    object.insert(QStringLiteral("inputBlack"), channel.inputBlack);
    object.insert(QStringLiteral("inputWhite"), channel.inputWhite);
    object.insert(QStringLiteral("gamma"), channel.gamma);
    object.insert(QStringLiteral("outputBlack"), channel.outputBlack);
    object.insert(QStringLiteral("outputWhite"), channel.outputWhite);
    return object;
}

LevelsChannelParameters levelsChannelFromJson(const QJsonObject &object, bool *ok)
{
    LevelsChannelParameters channel;
    const bool finite = readFiniteJsonNumber(object, QStringLiteral("inputBlack"), 0.0, &channel.inputBlack)
        && readFiniteJsonNumber(object, QStringLiteral("inputWhite"), 1.0, &channel.inputWhite)
        && readFiniteJsonNumber(object, QStringLiteral("gamma"), 1.0, &channel.gamma)
        && readFiniteJsonNumber(object, QStringLiteral("outputBlack"), 0.0, &channel.outputBlack)
        && readFiniteJsonNumber(object, QStringLiteral("outputWhite"), 1.0, &channel.outputWhite);
    channel.normalise();
    if (ok) *ok = finite;
    return channel;
}

QString adjustmentChannelKey(const AdjustmentChannel channel)
{
    switch (channel) {
    case AdjustmentChannel::Rgb: return QStringLiteral("rgb");
    case AdjustmentChannel::Red: return QStringLiteral("red");
    case AdjustmentChannel::Green: return QStringLiteral("green");
    case AdjustmentChannel::Blue: return QStringLiteral("blue");
    }
    return QStringLiteral("rgb");
}

QJsonArray curveChannelToJson(const CurveChannelParameters &channel)
{
    QJsonArray points;
    for (const CurvePoint &point : channel.points) {
        QJsonObject encoded;
        encoded.insert(QStringLiteral("input"), point.input);
        encoded.insert(QStringLiteral("output"), point.output);
        points.push_back(encoded);
    }
    return points;
}

CurveChannelParameters curveChannelFromJson(const QJsonArray &array, bool *ok)
{
    CurveChannelParameters channel;
    channel.points.clear();
    bool valid = array.size() >= 2 && array.size() <= MaximumCurvePoints;
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            valid = false;
            continue;
        }
        CurvePoint point;
        const QJsonObject object = value.toObject();
        valid = readFiniteJsonNumber(object, QStringLiteral("input"), 0.0, &point.input)
            && readFiniteJsonNumber(object, QStringLiteral("output"), 0.0, &point.output)
            && valid;
        channel.points.push_back(point);
    }
    channel.normalise();
    if (ok) *ok = valid;
    return channel;
}

QJsonObject hueSaturationRangeToJson(const HueSaturationRangeParameters &range)
{
    QJsonObject object;
    object.insert(QStringLiteral("hue"), range.hue);
    object.insert(QStringLiteral("saturation"), range.saturation);
    object.insert(QStringLiteral("lightness"), range.lightness);
    object.insert(QStringLiteral("centre"), range.centre);
    object.insert(QStringLiteral("width"), range.width);
    object.insert(QStringLiteral("feather"), range.feather);
    return object;
}

HueSaturationRangeParameters hueSaturationRangeFromJson(const QJsonObject &object,
                                                        const HueSaturationRangeParameters &fallback,
                                                        bool *ok)
{
    HueSaturationRangeParameters range = fallback;
    const bool valid = readFiniteJsonNumber(object, QStringLiteral("hue"), 0.0, &range.hue)
        && readFiniteJsonNumber(object, QStringLiteral("saturation"), 0.0, &range.saturation)
        && readFiniteJsonNumber(object, QStringLiteral("lightness"), 0.0, &range.lightness)
        && readFiniteJsonNumber(object, QStringLiteral("centre"), fallback.centre, &range.centre)
        && readFiniteJsonNumber(object, QStringLiteral("width"), fallback.width, &range.width)
        && readFiniteJsonNumber(object, QStringLiteral("feather"), fallback.feather, &range.feather);
    range.normalise();
    if (ok) *ok = valid;
    return range;
}

QJsonObject colourBalanceRangeToJson(const ColourBalanceRangeParameters &range)
{
    QJsonObject object;
    object.insert(QStringLiteral("cyanRed"), range.cyanRed);
    object.insert(QStringLiteral("magentaGreen"), range.magentaGreen);
    object.insert(QStringLiteral("yellowBlue"), range.yellowBlue);
    return object;
}

ColourBalanceRangeParameters colourBalanceRangeFromJson(const QJsonObject &object, bool *ok)
{
    ColourBalanceRangeParameters range;
    const bool valid = readFiniteJsonNumber(object, QStringLiteral("cyanRed"), 0.0, &range.cyanRed)
        && readFiniteJsonNumber(object, QStringLiteral("magentaGreen"), 0.0, &range.magentaGreen)
        && readFiniteJsonNumber(object, QStringLiteral("yellowBlue"), 0.0, &range.yellowBlue);
    range.normalise();
    if (ok) *ok = valid;
    return range;
}

QJsonObject channelMixerChannelToJson(const ChannelMixerChannelParameters &channel)
{
    QJsonObject object;
    object.insert(QStringLiteral("red"), channel.red);
    object.insert(QStringLiteral("green"), channel.green);
    object.insert(QStringLiteral("blue"), channel.blue);
    object.insert(QStringLiteral("constant"), channel.constant);
    return object;
}

ChannelMixerChannelParameters channelMixerChannelFromJson(const QJsonObject &object, bool *ok)
{
    ChannelMixerChannelParameters channel;
    const bool valid = readFiniteJsonNumber(object, QStringLiteral("red"), 0.0, &channel.red)
        && readFiniteJsonNumber(object, QStringLiteral("green"), 0.0, &channel.green)
        && readFiniteJsonNumber(object, QStringLiteral("blue"), 0.0, &channel.blue)
        && readFiniteJsonNumber(object, QStringLiteral("constant"), 0.0, &channel.constant);
    channel.normalise();
    if (ok) *ok = valid;
    return channel;
}

QJsonObject selectiveColourRangeToJson(const SelectiveColourRangeParameters &range)
{
    QJsonObject object;
    object.insert(QStringLiteral("cyan"), range.cyan);
    object.insert(QStringLiteral("magenta"), range.magenta);
    object.insert(QStringLiteral("yellow"), range.yellow);
    object.insert(QStringLiteral("black"), range.black);
    return object;
}

SelectiveColourRangeParameters selectiveColourRangeFromJson(const QJsonObject &object,
                                                             bool *ok)
{
    SelectiveColourRangeParameters range;
    const bool valid = readFiniteJsonNumber(object, QStringLiteral("cyan"), 0.0, &range.cyan)
        && readFiniteJsonNumber(object, QStringLiteral("magenta"), 0.0, &range.magenta)
        && readFiniteJsonNumber(object, QStringLiteral("yellow"), 0.0, &range.yellow)
        && readFiniteJsonNumber(object, QStringLiteral("black"), 0.0, &range.black);
    range.normalise();
    if (ok) *ok = valid;
    return range;
}

QJsonObject gradientStopToJson(const GradientStop &stop)
{
    QJsonObject object;
    object.insert(QStringLiteral("position"), stop.position);
    object.insert(QStringLiteral("colour"), stop.colour.name(QColor::HexRgb));
    return object;
}

GradientStop gradientStopFromJson(const QJsonObject &object, bool *ok)
{
    GradientStop stop;
    const bool positionOk = readFiniteJsonNumber(
        object, QStringLiteral("position"), 0.0, &stop.position);
    const QJsonValue colourValue = object.value(QStringLiteral("colour"));
    const QColor colour(colourValue.toString());
    const bool valid = positionOk && colourValue.isString() && colour.isValid();
    if (colour.isValid()) stop.colour = colour;
    stop.colour.setAlpha(255);
    if (ok) *ok = valid;
    return stop;
}


QJsonArray domainToJson(const std::array<double, 3> &domain)
{
    return {domain[0], domain[1], domain[2]};
}

bool domainFromJson(const QJsonValue &encoded,
                    const std::array<double, 3> &fallback,
                    std::array<double, 3> *domain)
{
    if (!encoded.isArray() || encoded.toArray().size() != 3) {
        if (domain) *domain = fallback;
        return false;
    }
    const QJsonArray values = encoded.toArray();
    std::array<double, 3> decoded = fallback;
    for (int index = 0; index < 3; ++index) {
        if (!values.at(index).isDouble() || !std::isfinite(values.at(index).toDouble())) {
            if (domain) *domain = fallback;
            return false;
        }
        decoded[static_cast<std::size_t>(index)] = values.at(index).toDouble();
    }
    if (domain) *domain = decoded;
    return true;
}

bool domainHasPositiveFiniteSpan(const std::array<double, 3> &minimum,
                                 const std::array<double, 3> &maximum)
{
    for (int channel = 0; channel < 3; ++channel) {
        const double low = minimum[static_cast<std::size_t>(channel)];
        const double high = maximum[static_cast<std::size_t>(channel)];
        const double span = high - low;
        if (!std::isfinite(low) || !std::isfinite(high)
            || !std::isfinite(span) || span <= 0.0) {
            return false;
        }
    }
    return true;
}

QString encodeFloatVector(const QVector<float> &values)
{
    if (values.isEmpty()) return {};
    QByteArray raw;
    raw.reserve(values.size() * static_cast<qsizetype>(sizeof(float)));
    QDataStream stream(&raw, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    for (const float value : values) stream << value;
    if (stream.status() != QDataStream::Ok) return {};
    return QString::fromLatin1(qCompress(raw, 9).toBase64());
}

bool decodeFloatVector(const QJsonValue &encoded,
                       const qsizetype expectedCount,
                       QVector<float> *values)
{
    if (!values) return false;
    values->clear();
    if (expectedCount == 0) return encoded.isString() && encoded.toString().isEmpty();
    constexpr qsizetype MaximumLutFloats = qsizetype(65) * 65 * 65 * 3
        + qsizetype(65'536) * 3;
    if (expectedCount < 0 || expectedCount > MaximumLutFloats || !encoded.isString()) return false;
    const QByteArray compressed = QByteArray::fromBase64(encoded.toString().toLatin1(),
                                                          QByteArray::AbortOnBase64DecodingErrors);
    if (compressed.size() < 4 || compressed.size() > 64 * 1024 * 1024) return false;
    const quint32 declaredBytes = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar *>(compressed.constData()));
    const quint64 expectedBytes = static_cast<quint64>(expectedCount) * sizeof(float);
    if (declaredBytes != expectedBytes || declaredBytes > 4u * 1024u * 1024u) return false;
    QByteArray raw = qUncompress(compressed);
    if (raw.size() != static_cast<qsizetype>(expectedBytes)) return false;
    QBuffer buffer(&raw);
    if (!buffer.open(QIODevice::ReadOnly)) return false;
    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    values->resize(expectedCount);
    for (qsizetype index = 0; index < expectedCount; ++index) {
        stream >> (*values)[index];
        if (stream.status() != QDataStream::Ok || !std::isfinite((*values)[index])) {
            values->clear();
            return false;
        }
    }
    return true;
}

} // namespace

void LevelsChannelParameters::normalise()
{
    inputBlack = std::clamp(std::isfinite(inputBlack) ? inputBlack : 0.0, 0.0, 1.0);
    inputWhite = std::clamp(std::isfinite(inputWhite) ? inputWhite : 1.0, 0.0, 1.0);
    if (inputWhite < inputBlack + MinimumLevelsGap) {
        if (inputBlack >= 1.0 - MinimumLevelsGap) {
            inputBlack = 1.0 - MinimumLevelsGap;
            inputWhite = 1.0;
        } else {
            inputWhite = inputBlack + MinimumLevelsGap;
        }
    }
    gamma = std::clamp(std::isfinite(gamma) ? gamma : 1.0, 0.01, 10.0);
    outputBlack = std::clamp(std::isfinite(outputBlack) ? outputBlack : 0.0, 0.0, 1.0);
    outputWhite = std::clamp(std::isfinite(outputWhite) ? outputWhite : 1.0, 0.0, 1.0);
    if (outputWhite < outputBlack) std::swap(outputBlack, outputWhite);
}

LevelsChannelParameters &LevelsParameters::channel(const AdjustmentChannel selected)
{
    return channels.at(static_cast<std::size_t>(selected));
}

const LevelsChannelParameters &LevelsParameters::channel(const AdjustmentChannel selected) const
{
    return channels.at(static_cast<std::size_t>(selected));
}

void LevelsParameters::normalise()
{
    for (LevelsChannelParameters &value : channels) value.normalise();
    autoClipShadows = std::clamp(std::isfinite(autoClipShadows) ? autoClipShadows : 0.001, 0.0, 0.25);
    autoClipHighlights = std::clamp(std::isfinite(autoClipHighlights) ? autoClipHighlights : 0.001, 0.0, 0.25);
}

void CurveChannelParameters::normalise()
{
    QVector<CurvePoint> safe;
    safe.reserve(std::min(points.size() + 2, static_cast<qsizetype>(MaximumCurvePoints)));
    for (const CurvePoint &point : std::as_const(points)) {
        if (!std::isfinite(point.input) || !std::isfinite(point.output)) continue;
        safe.push_back({std::clamp(point.input, 0.0, 1.0),
                        std::clamp(point.output, 0.0, 1.0)});
    }
    std::sort(safe.begin(), safe.end(), [](const CurvePoint &left, const CurvePoint &right) {
        return left.input < right.input;
    });
    QVector<CurvePoint> unique;
    unique.reserve(std::min(safe.size() + 2, static_cast<qsizetype>(MaximumCurvePoints)));
    for (const CurvePoint &point : std::as_const(safe)) {
        if (!unique.isEmpty() && std::abs(unique.last().input - point.input) < MinimumCurveGap) {
            unique.last().output = point.output;
        } else if (unique.size() < MaximumCurvePoints) {
            unique.push_back(point);
        }
    }
    if (unique.isEmpty() || unique.first().input > MinimumCurveGap) {
        unique.prepend({0.0, unique.isEmpty() ? 0.0 : unique.first().output});
    } else {
        unique.first().input = 0.0;
    }
    if (unique.size() == 1 || unique.last().input < 1.0 - MinimumCurveGap) {
        unique.push_back({1.0, unique.size() == 1 ? 1.0 : unique.last().output});
    } else {
        unique.last().input = 1.0;
    }
    while (unique.size() > MaximumCurvePoints) unique.remove(unique.size() - 2);
    points = std::move(unique);
}

CurveChannelParameters &CurvesParameters::channel(const AdjustmentChannel selected)
{
    return channels.at(static_cast<std::size_t>(selected));
}

const CurveChannelParameters &CurvesParameters::channel(const AdjustmentChannel selected) const
{
    return channels.at(static_cast<std::size_t>(selected));
}

void CurvesParameters::normalise()
{
    for (CurveChannelParameters &channelValue : channels) channelValue.normalise();
    if (interpolation != CurveInterpolation::Smooth
        && interpolation != CurveInterpolation::Linear) {
        interpolation = CurveInterpolation::Smooth;
    }
}

void HueSaturationRangeParameters::normalise()
{
    hue = std::clamp(std::isfinite(hue) ? hue : 0.0, -180.0, 180.0);
    saturation = std::clamp(std::isfinite(saturation) ? saturation : 0.0, -100.0, 100.0);
    lightness = std::clamp(std::isfinite(lightness) ? lightness : 0.0, -100.0, 100.0);
    centre = std::fmod(std::isfinite(centre) ? centre : 0.0, 360.0);
    if (centre < 0.0) centre += 360.0;
    width = std::clamp(std::isfinite(width) ? width : 60.0, 1.0, 180.0);
    feather = std::clamp(std::isfinite(feather) ? feather : 30.0, 0.0, 90.0);
}

HueSaturationParameters::HueSaturationParameters()
{
    constexpr std::array<double, 6> centres {0.0, 60.0, 120.0, 180.0, 240.0, 300.0};
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        ranges[index].centre = centres[index];
    }
}

HueSaturationRangeParameters &HueSaturationParameters::range(const HueSaturationRange selected)
{
    return ranges.at(static_cast<std::size_t>(selected));
}

const HueSaturationRangeParameters &HueSaturationParameters::range(const HueSaturationRange selected) const
{
    return ranges.at(static_cast<std::size_t>(selected));
}

void HueSaturationParameters::normalise()
{
    hue = std::clamp(std::isfinite(hue) ? hue : 0.0, -180.0, 180.0);
    saturation = std::clamp(std::isfinite(saturation) ? saturation : 0.0, -100.0, 100.0);
    lightness = std::clamp(std::isfinite(lightness) ? lightness : 0.0, -100.0, 100.0);
    for (HueSaturationRangeParameters &value : ranges) value.normalise();
}

void VibranceParameters::normalise()
{
    vibrance = std::clamp(std::isfinite(vibrance) ? vibrance : 0.0, -100.0, 100.0);
    saturation = std::clamp(std::isfinite(saturation) ? saturation : 0.0, -100.0, 100.0);
    skinProtection = std::clamp(std::isfinite(skinProtection) ? skinProtection : 65.0, 0.0, 100.0);
}

void WhiteBalanceParameters::normalise()
{
    temperature = std::clamp(std::isfinite(temperature) ? temperature : 0.0, -100.0, 100.0);
    tint = std::clamp(std::isfinite(tint) ? tint : 0.0, -100.0, 100.0);
}

void ColourBalanceRangeParameters::normalise()
{
    cyanRed = std::clamp(std::isfinite(cyanRed) ? cyanRed : 0.0, -100.0, 100.0);
    magentaGreen = std::clamp(std::isfinite(magentaGreen) ? magentaGreen : 0.0, -100.0, 100.0);
    yellowBlue = std::clamp(std::isfinite(yellowBlue) ? yellowBlue : 0.0, -100.0, 100.0);
}

ColourBalanceRangeParameters &ColourBalanceParameters::range(const ColourBalanceRange selected)
{
    return ranges.at(static_cast<std::size_t>(selected));
}

const ColourBalanceRangeParameters &ColourBalanceParameters::range(const ColourBalanceRange selected) const
{
    return ranges.at(static_cast<std::size_t>(selected));
}

void ColourBalanceParameters::normalise()
{
    for (ColourBalanceRangeParameters &value : ranges) value.normalise();
}

void ChannelMixerChannelParameters::normalise()
{
    red = std::clamp(std::isfinite(red) ? red : 0.0, -200.0, 200.0);
    green = std::clamp(std::isfinite(green) ? green : 0.0, -200.0, 200.0);
    blue = std::clamp(std::isfinite(blue) ? blue : 0.0, -200.0, 200.0);
    constant = std::clamp(std::isfinite(constant) ? constant : 0.0, -100.0, 100.0);
}

ChannelMixerParameters::ChannelMixerParameters()
{
    outputs[0].red = 100.0;
    outputs[1].green = 100.0;
    outputs[2].blue = 100.0;
}

ChannelMixerChannelParameters &ChannelMixerParameters::output(const ChannelMixerOutput selected)
{
    return outputs.at(static_cast<std::size_t>(selected));
}

const ChannelMixerChannelParameters &ChannelMixerParameters::output(const ChannelMixerOutput selected) const
{
    return outputs.at(static_cast<std::size_t>(selected));
}

void ChannelMixerParameters::normalise()
{
    for (ChannelMixerChannelParameters &value : outputs) value.normalise();
    monochrome.normalise();
}

void BlackAndWhiteParameters::normalise()
{
    for (double &value : colourWeights) {
        value = std::clamp(std::isfinite(value) ? value : 100.0, -200.0, 300.0);
    }
    tintHue = std::fmod(std::isfinite(tintHue) ? tintHue : 30.0, 360.0);
    if (tintHue < 0.0) tintHue += 360.0;
    tintSaturation = std::clamp(
        std::isfinite(tintSaturation) ? tintSaturation : 20.0, 0.0, 100.0);
}

void GradientMapParameters::normalise()
{
    QVector<GradientStop> safe;
    safe.reserve(std::min(stops.size() + 2, static_cast<qsizetype>(64)));
    for (GradientStop stop : std::as_const(stops)) {
        if (!std::isfinite(stop.position) || !stop.colour.isValid()) continue;
        stop.position = std::clamp(stop.position, 0.0, 1.0);
        stop.colour.setAlpha(255);
        safe.push_back(stop);
    }
    std::sort(safe.begin(), safe.end(), [](const GradientStop &left, const GradientStop &right) {
        return left.position < right.position;
    });
    QVector<GradientStop> unique;
    unique.reserve(std::min(safe.size() + 2, static_cast<qsizetype>(64)));
    for (const GradientStop &stop : std::as_const(safe)) {
        if (!unique.isEmpty() && std::abs(unique.last().position - stop.position) < MinimumCurveGap) {
            unique.last() = stop;
        } else if (unique.size() < 64) {
            unique.push_back(stop);
        }
    }
    if (unique.isEmpty()) unique = {{0.0, Qt::black}, {1.0, Qt::white}};
    if (unique.first().position > MinimumCurveGap) unique.prepend({0.0, unique.first().colour});
    else unique.first().position = 0.0;
    if (unique.size() == 1 || unique.last().position < 1.0 - MinimumCurveGap) {
        unique.push_back({1.0, unique.last().colour});
    } else {
        unique.last().position = 1.0;
    }
    while (unique.size() > 64) unique.remove(unique.size() - 2);
    stops = std::move(unique);
    if (interpolation != GradientInterpolation::Linear
        && interpolation != GradientInterpolation::Smooth
        && interpolation != GradientInterpolation::Constant) {
        interpolation = GradientInterpolation::Linear;
    }
}

void PosteriseParameters::normalise()
{
    levels = std::clamp(levels, 2, 256);
}

void ThresholdParameters::normalise()
{
    threshold = std::clamp(std::isfinite(threshold) ? threshold : 0.5, 0.0, 1.0);
    if (source != ThresholdSource::Luminance && source != ThresholdSource::Red
        && source != ThresholdSource::Green && source != ThresholdSource::Blue) {
        source = ThresholdSource::Luminance;
    }
}

void PhotoFilterParameters::normalise()
{
    if (!colour.isValid()) colour = QColor(236, 138, 0);
    colour.setAlpha(255);
    density = std::clamp(std::isfinite(density) ? density : 25.0, 0.0, 100.0);
}

void SelectiveColourRangeParameters::normalise()
{
    cyan = std::clamp(std::isfinite(cyan) ? cyan : 0.0, -100.0, 100.0);
    magenta = std::clamp(std::isfinite(magenta) ? magenta : 0.0, -100.0, 100.0);
    yellow = std::clamp(std::isfinite(yellow) ? yellow : 0.0, -100.0, 100.0);
    black = std::clamp(std::isfinite(black) ? black : 0.0, -100.0, 100.0);
}

SelectiveColourRangeParameters &SelectiveColourParameters::range(
    const SelectiveColourRange selected)
{
    return ranges.at(static_cast<std::size_t>(selected));
}

const SelectiveColourRangeParameters &SelectiveColourParameters::range(
    const SelectiveColourRange selected) const
{
    return ranges.at(static_cast<std::size_t>(selected));
}

void SelectiveColourParameters::normalise()
{
    for (SelectiveColourRangeParameters &value : ranges) value.normalise();
    if (method != SelectiveColourMethod::Relative
        && method != SelectiveColourMethod::Absolute) {
        method = SelectiveColourMethod::Relative;
    }
}

void VignetteParameters::normalise()
{
    amount = std::clamp(std::isfinite(amount) ? amount : 25.0, -100.0, 100.0);
    size = std::clamp(std::isfinite(size) ? size : 100.0, 10.0, 400.0);
    midpoint = std::clamp(std::isfinite(midpoint) ? midpoint : 50.0, 0.0, 100.0);
    roundness = std::clamp(std::isfinite(roundness) ? roundness : 0.0, -100.0, 100.0);
    feather = std::clamp(std::isfinite(feather) ? feather : 50.0, 0.0, 100.0);
    centreX = std::clamp(std::isfinite(centreX) ? centreX : 0.0, -100.0, 100.0);
    centreY = std::clamp(std::isfinite(centreY) ? centreY : 0.0, -100.0, 100.0);
    rotation = std::clamp(std::isfinite(rotation) ? rotation : 0.0, -180.0, 180.0);
    highlightProtection = std::clamp(
        std::isfinite(highlightProtection) ? highlightProtection : 0.0, 0.0, 100.0);
    if (!tint.isValid()) tint = Qt::black;
    tint.setAlpha(255);
}

void RgbSplitParameters::normalise()
{
    redOffsetX = std::clamp(std::isfinite(redOffsetX) ? redOffsetX : -6.0, -200.0, 200.0);
    redOffsetY = std::clamp(std::isfinite(redOffsetY) ? redOffsetY : 0.0, -200.0, 200.0);
    blueOffsetX = std::clamp(std::isfinite(blueOffsetX) ? blueOffsetX : 6.0, -200.0, 200.0);
    blueOffsetY = std::clamp(std::isfinite(blueOffsetY) ? blueOffsetY : 0.0, -200.0, 200.0);
}

void ChromaticAberrationCorrectionParameters::normalise()
{
    redEdgeShift = std::clamp(
        std::isfinite(redEdgeShift) ? redEdgeShift : 0.0, -100.0, 100.0);
    blueEdgeShift = std::clamp(
        std::isfinite(blueEdgeShift) ? blueEdgeShift : 0.0, -100.0, 100.0);
    centreX = std::clamp(std::isfinite(centreX) ? centreX : 0.0, -100.0, 100.0);
    centreY = std::clamp(std::isfinite(centreY) ? centreY : 0.0, -100.0, 100.0);
    falloff = std::clamp(std::isfinite(falloff) ? falloff : 1.0, 0.25, 4.0);
}

void SurfaceBlurParameters::normalise()
{
    radius = std::clamp(std::isfinite(radius) ? radius : 12.0, 0.0, 250.0);
    threshold = std::clamp(std::isfinite(threshold) ? threshold : 20.0, 0.0, 255.0);
}

void MotionBlurParameters::normalise()
{
    distance = std::clamp(std::isfinite(distance) ? distance : 24.0, 0.0, 500.0);
    angle = std::clamp(std::isfinite(angle) ? angle : 0.0, -180.0, 180.0);
    samples = std::clamp(samples, 4, 64);
}

void RadialBlurParameters::normalise()
{
    if (mode != RadialBlurMode::Spin && mode != RadialBlurMode::Zoom) {
        mode = RadialBlurMode::Spin;
    }
    amount = std::clamp(std::isfinite(amount) ? amount : 20.0, 0.0, 250.0);
    centreX = std::clamp(std::isfinite(centreX) ? centreX : 0.0, -100.0, 100.0);
    centreY = std::clamp(std::isfinite(centreY) ? centreY : 0.0, -100.0, 100.0);
    samples = std::clamp(samples, 4, 64);
}

void ShadowsHighlightsParameters::normalise()
{
    shadowAmount = std::clamp(std::isfinite(shadowAmount) ? shadowAmount : 0.0, 0.0, 100.0);
    shadowTonalWidth = std::clamp(std::isfinite(shadowTonalWidth) ? shadowTonalWidth : 50.0, 1.0, 100.0);
    highlightAmount = std::clamp(std::isfinite(highlightAmount) ? highlightAmount : 0.0, 0.0, 100.0);
    highlightTonalWidth = std::clamp(std::isfinite(highlightTonalWidth) ? highlightTonalWidth : 50.0, 1.0, 100.0);
    radius = std::clamp(std::isfinite(radius) ? radius : 80.0, 1.0, 500.0);
    midtoneContrast = std::clamp(std::isfinite(midtoneContrast) ? midtoneContrast : 0.0, -100.0, 100.0);
    colourCorrection = std::clamp(std::isfinite(colourCorrection) ? colourCorrection : 20.0, 0.0, 100.0);
}

void GaussianBlurParameters::normalise()
{
    radius = std::clamp(std::isfinite(radius) ? radius : 8.0, 0.0, 500.0);
}

void BoxBlurParameters::normalise()
{
    radius = std::clamp(std::isfinite(radius) ? radius : 4.0, 0.0, 500.0);
}

void UnsharpMaskParameters::normalise()
{
    radius = std::clamp(std::isfinite(radius) ? radius : 2.0, 0.0, 500.0);
    amount = std::clamp(std::isfinite(amount) ? amount : 100.0, 0.0, 500.0);
    threshold = std::clamp(std::isfinite(threshold) ? threshold : 0.0, 0.0, 255.0);
}

void HighPassParameters::normalise()
{
    radius = std::clamp(std::isfinite(radius) ? radius : 10.0, 0.0, 500.0);
}

bool LutParameters::hasShaper() const
{
    return shaperSize >= 2 && shaperData.size() == qsizetype(shaperSize) * 3;
}

bool LutParameters::hasCube() const
{
    return cubeSize >= 2 && cubeData.size() == qsizetype(cubeSize) * cubeSize * cubeSize * 3;
}

bool LutParameters::hasData() const
{
    return hasShaper() || hasCube();
}

void LutParameters::clear()
{
    title.clear();
    sourceName.clear();
    shaperSize = 0;
    cubeSize = 0;
    shaperData.clear();
    cubeData.clear();
    shaperDomainMin = {0.0, 0.0, 0.0};
    shaperDomainMax = {1.0, 1.0, 1.0};
    cubeDomainMin = {0.0, 0.0, 0.0};
    cubeDomainMax = {1.0, 1.0, 1.0};
    shaperDomainSource = LutDomainSource::DefaultRange;
    cubeDomainSource = LutDomainSource::DefaultRange;
    interpolation = LutInterpolation::Tetrahedral;
    processingMode = LutProcessingMode::EncodedDocument;
    operatorProfile = LutOperatorProfile::Generic;
    strength = 100.0;
    tableFingerprint = 0;
    gpuDisplayRangeCompatible = true;
    gpuTableHalfFloatCompatible = true;
    gpuHalfFloatCompatible = true;
}

void LutParameters::normalise()
{
    title = title.trimmed().left(256);
    sourceName = sourceName.trimmed().left(260);
    strength = std::clamp(std::isfinite(strength) ? strength : 100.0, 0.0, 100.0);
    if (shaperSize < 2 || shaperSize > 65'536
        || shaperData.size() != qsizetype(shaperSize) * 3) {
        shaperSize = 0;
        shaperData.clear();
    }
    const qsizetype cubeCount = cubeSize >= 2 && cubeSize <= 65
        ? qsizetype(cubeSize) * cubeSize * cubeSize * 3 : 0;
    if (cubeCount == 0 || cubeData.size() != cubeCount) {
        cubeSize = 0;
        cubeData.clear();
    }
    if (shaperSize == 0) {
        shaperDomainMin = {0.0, 0.0, 0.0};
        shaperDomainMax = {1.0, 1.0, 1.0};
        shaperDomainSource = LutDomainSource::DefaultRange;
    }
    if (cubeSize == 0) {
        cubeDomainMin = {0.0, 0.0, 0.0};
        cubeDomainMax = {1.0, 1.0, 1.0};
        cubeDomainSource = LutDomainSource::DefaultRange;
    }
    const auto normaliseDomain = [](std::array<double, 3> *minimum,
                                    std::array<double, 3> *maximum,
                                    LutDomainSource *source) {
        bool valid = true;
        for (int channel = 0; channel < 3; ++channel) {
            const double low = (*minimum)[static_cast<std::size_t>(channel)];
            const double high = (*maximum)[static_cast<std::size_t>(channel)];
            const double span = high - low;
            valid = valid && std::isfinite(low) && std::isfinite(high)
                && std::isfinite(span) && span > 0.0;
        }
        if (!valid) {
            *minimum = {0.0, 0.0, 0.0};
            *maximum = {1.0, 1.0, 1.0};
            if (source) *source = LutDomainSource::DefaultRange;
        }
    };
    const auto normaliseDomainSource = [](LutDomainSource *source) {
        switch (*source) {
        case LutDomainSource::DefaultRange:
        case LutDomainSource::DomainDirective:
        case LutDomainSource::InputRangeDirective:
        case LutDomainSource::LegacyPersisted:
            return;
        }
        *source = LutDomainSource::DefaultRange;
    };
    normaliseDomainSource(&shaperDomainSource);
    normaliseDomainSource(&cubeDomainSource);
    switch (interpolation) {
    case LutInterpolation::Trilinear:
    case LutInterpolation::Tetrahedral:
        break;
    default:
        interpolation = LutInterpolation::Trilinear;
        break;
    }
    switch (processingMode) {
    case LutProcessingMode::EncodedDocument:
    case LutProcessingMode::LinearSrgb:
    case LutProcessingMode::RawComponents:
        break;
    default:
        processingMode = LutProcessingMode::EncodedDocument;
        break;
    }
    switch (operatorProfile) {
    case LutOperatorProfile::Generic:
    case LutOperatorProfile::TonyMcMapface:
    case LutOperatorProfile::AgXBaseSrgb:
        break;
    default:
        operatorProfile = LutOperatorProfile::Generic;
        break;
    }
    if (operatorProfile != LutOperatorProfile::Generic && hasCube()) {
        // Both named display transforms define tetrahedral sampling as part of
        // their contract. Persisting the profile is therefore sufficient to
        // reproduce the interpolation requirement after save/reopen.
        interpolation = LutInterpolation::Tetrahedral;
    }
    normaliseDomain(&shaperDomainMin, &shaperDomainMax, &shaperDomainSource);
    normaliseDomain(&cubeDomainMin, &cubeDomainMax, &cubeDomainSource);
    const auto domainsFitGpuUniforms = [](const std::array<double, 3> &minimum,
                                          const std::array<double, 3> &maximum) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float low = static_cast<float>(minimum[channel]);
            const float high = static_cast<float>(maximum[channel]);
            if (!std::isfinite(low) || !std::isfinite(high) || !(high > low)) {
                return false;
            }
        }
        return true;
    };
    const bool gpuDomainsCompatible =
        (!hasShaper() || domainsFitGpuUniforms(shaperDomainMin, shaperDomainMax))
        && (!hasCube() || domainsFitGpuUniforms(cubeDomainMin, cubeDomainMax));
    if (tableFingerprint == 0) {
        // New/imported payloads are validated once. QVector is implicitly
        // shared, so subsequent strength edits and renderer snapshots avoid
        // detaching and rescanning hundreds of thousands of table entries.
        gpuDisplayRangeCompatible = true;
        gpuTableHalfFloatCompatible = true;
        constexpr float MaximumFiniteHalf = 65504.0f;
        const auto validate = [this](QVector<float> *table) {
            for (const float component : std::as_const(*table)) {
                if (!std::isfinite(component)) {
                    gpuDisplayRangeCompatible = false;
                    gpuTableHalfFloatCompatible = false;
                    return false;
                }
                gpuDisplayRangeCompatible = gpuDisplayRangeCompatible
                    && component >= 0.0f && component <= 1.0f;
                gpuTableHalfFloatCompatible = gpuTableHalfFloatCompatible
                    && std::abs(component) <= MaximumFiniteHalf;
            }
            return true;
        };
        if (!validate(&shaperData)) {
            shaperData.clear();
            shaperSize = 0;
        }
        if (!validate(&cubeData)) {
            cubeData.clear();
            cubeSize = 0;
        }
        if (shaperSize == 0) {
            shaperDomainMin = {0.0, 0.0, 0.0};
            shaperDomainMax = {1.0, 1.0, 1.0};
            shaperDomainSource = LutDomainSource::DefaultRange;
        }
        if (cubeSize == 0) {
            cubeDomainMin = {0.0, 0.0, 0.0};
            cubeDomainMax = {1.0, 1.0, 1.0};
            cubeDomainSource = LutDomainSource::DefaultRange;
        }
    }
    if (!hasData()) {
        tableFingerprint = 0;
        gpuDisplayRangeCompatible = true;
        gpuTableHalfFloatCompatible = true;
    } else if (tableFingerprint == 0) {
        tableFingerprint = lutTableFingerprint(*this);
    }
    gpuHalfFloatCompatible = gpuTableHalfFloatCompatible && gpuDomainsCompatible;
}

void AdjustmentData::reset(const AdjustmentType newType)
{
    schema = CurrentSchema;
    type = newType;
    switch (newType) {
    case AdjustmentType::Exposure: parameters = ExposureParameters {}; break;
    case AdjustmentType::Contrast: parameters = ContrastParameters {}; break;
    case AdjustmentType::Saturation: parameters = SaturationParameters {}; break;
    case AdjustmentType::Levels: parameters = LevelsParameters {}; break;
    case AdjustmentType::Curves: parameters = CurvesParameters {}; break;
    case AdjustmentType::HueSaturation: parameters = HueSaturationParameters {}; break;
    case AdjustmentType::Vibrance: parameters = VibranceParameters {}; break;
    case AdjustmentType::WhiteBalance: parameters = WhiteBalanceParameters {}; break;
    case AdjustmentType::ColourBalance: parameters = ColourBalanceParameters {}; break;
    case AdjustmentType::ChannelMixer: parameters = ChannelMixerParameters {}; break;
    case AdjustmentType::BlackAndWhite: parameters = BlackAndWhiteParameters {}; break;
    case AdjustmentType::GradientMap: parameters = GradientMapParameters {}; break;
    case AdjustmentType::Posterise: parameters = PosteriseParameters {}; break;
    case AdjustmentType::Threshold: parameters = ThresholdParameters {}; break;
    case AdjustmentType::Lut: parameters = LutParameters {}; break;
    case AdjustmentType::ShadowsHighlights: parameters = ShadowsHighlightsParameters {}; break;
    case AdjustmentType::GaussianBlur: parameters = GaussianBlurParameters {}; break;
    case AdjustmentType::BoxBlur: parameters = BoxBlurParameters {}; break;
    case AdjustmentType::UnsharpMask: parameters = UnsharpMaskParameters {}; break;
    case AdjustmentType::HighPass: parameters = HighPassParameters {}; break;
    case AdjustmentType::Invert: parameters = InvertParameters {}; break;
    case AdjustmentType::PhotoFilter: parameters = PhotoFilterParameters {}; break;
    case AdjustmentType::SelectiveColour: parameters = SelectiveColourParameters {}; break;
    case AdjustmentType::Vignette: parameters = VignetteParameters {}; break;
    case AdjustmentType::RgbSplit: parameters = RgbSplitParameters {}; break;
    case AdjustmentType::ChromaticAberrationCorrection:
        parameters = ChromaticAberrationCorrectionParameters {};
        break;
    case AdjustmentType::SurfaceBlur: parameters = SurfaceBlurParameters {}; break;
    case AdjustmentType::MotionBlur: parameters = MotionBlurParameters {}; break;
    case AdjustmentType::RadialBlur: parameters = RadialBlurParameters {}; break;
    }
}

void AdjustmentData::normalise()
{
    schema = CurrentSchema;
    switch (type) {
    case AdjustmentType::Exposure: {
        auto *value = std::get_if<ExposureParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<ExposureParameters>(&parameters); }
        value->exposure = std::clamp(std::isfinite(value->exposure) ? value->exposure : 0.0, -16.0, 16.0);
        value->offset = std::clamp(std::isfinite(value->offset) ? value->offset : 0.0, -1.0, 1.0);
        value->gamma = std::clamp(std::isfinite(value->gamma) ? value->gamma : 1.0, 0.01, 10.0);
        break;
    }
    case AdjustmentType::Contrast: {
        auto *value = std::get_if<ContrastParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<ContrastParameters>(&parameters); }
        value->contrast = std::clamp(std::isfinite(value->contrast) ? value->contrast : 0.0, -100.0, 100.0);
        value->pivot = std::clamp(std::isfinite(value->pivot) ? value->pivot : 0.5, 0.0, 1.0);
        break;
    }
    case AdjustmentType::Saturation: {
        auto *value = std::get_if<SaturationParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<SaturationParameters>(&parameters); }
        value->saturation = std::clamp(std::isfinite(value->saturation) ? value->saturation : 0.0, -100.0, 100.0);
        break;
    }
    case AdjustmentType::Levels: {
        auto *value = std::get_if<LevelsParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<LevelsParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::Curves: {
        auto *value = std::get_if<CurvesParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<CurvesParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::HueSaturation: {
        auto *value = std::get_if<HueSaturationParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<HueSaturationParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::Vibrance: {
        auto *value = std::get_if<VibranceParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<VibranceParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::WhiteBalance: {
        auto *value = std::get_if<WhiteBalanceParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<WhiteBalanceParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::ColourBalance: {
        auto *value = std::get_if<ColourBalanceParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<ColourBalanceParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::ChannelMixer: {
        auto *value = std::get_if<ChannelMixerParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<ChannelMixerParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::BlackAndWhite: {
        auto *value = std::get_if<BlackAndWhiteParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<BlackAndWhiteParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::GradientMap: {
        auto *value = std::get_if<GradientMapParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<GradientMapParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::Posterise: {
        auto *value = std::get_if<PosteriseParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<PosteriseParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::Threshold: {
        auto *value = std::get_if<ThresholdParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<ThresholdParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::Invert: {
        auto *value = std::get_if<InvertParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<InvertParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::PhotoFilter: {
        auto *value = std::get_if<PhotoFilterParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<PhotoFilterParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::SelectiveColour: {
        auto *value = std::get_if<SelectiveColourParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<SelectiveColourParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::Vignette: {
        auto *value = std::get_if<VignetteParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<VignetteParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::RgbSplit: {
        auto *value = std::get_if<RgbSplitParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<RgbSplitParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::ChromaticAberrationCorrection: {
        auto *value = std::get_if<ChromaticAberrationCorrectionParameters>(&parameters);
        if (!value) {
            reset(type);
            value = std::get_if<ChromaticAberrationCorrectionParameters>(&parameters);
        }
        value->normalise();
        break;
    }
    case AdjustmentType::SurfaceBlur: {
        auto *value = std::get_if<SurfaceBlurParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<SurfaceBlurParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::MotionBlur: {
        auto *value = std::get_if<MotionBlurParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<MotionBlurParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::RadialBlur: {
        auto *value = std::get_if<RadialBlurParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<RadialBlurParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::Lut: {
        auto *value = std::get_if<LutParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<LutParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::ShadowsHighlights: {
        auto *value = std::get_if<ShadowsHighlightsParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<ShadowsHighlightsParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::GaussianBlur: {
        auto *value = std::get_if<GaussianBlurParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<GaussianBlurParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::BoxBlur: {
        auto *value = std::get_if<BoxBlurParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<BoxBlurParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::UnsharpMask: {
        auto *value = std::get_if<UnsharpMaskParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<UnsharpMaskParameters>(&parameters); }
        value->normalise();
        break;
    }
    case AdjustmentType::HighPass: {
        auto *value = std::get_if<HighPassParameters>(&parameters);
        if (!value) { reset(type); value = std::get_if<HighPassParameters>(&parameters); }
        value->normalise();
        break;
    }
    }
}

QJsonObject AdjustmentData::toJson(bool *ok) const
{
    AdjustmentData safe = *this;
    safe.normalise();
    QJsonObject object;
    object.insert(QStringLiteral("schema"), static_cast<int>(safe.schema));
    object.insert(QStringLiteral("type"), adjustmentTypeToString(safe.type));
    QJsonObject parametersObject;
    switch (safe.type) {
    case AdjustmentType::Exposure: {
        const auto &value = std::get<ExposureParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("exposure"), value.exposure);
        parametersObject.insert(QStringLiteral("offset"), value.offset);
        parametersObject.insert(QStringLiteral("gamma"), value.gamma);
        break;
    }
    case AdjustmentType::Contrast: {
        const auto &value = std::get<ContrastParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("contrast"), value.contrast);
        parametersObject.insert(QStringLiteral("pivot"), value.pivot);
        break;
    }
    case AdjustmentType::Saturation:
        parametersObject.insert(QStringLiteral("saturation"),
                                std::get<SaturationParameters>(safe.parameters).saturation);
        break;
    case AdjustmentType::Levels: {
        const LevelsParameters &levels = std::get<LevelsParameters>(safe.parameters);
        QJsonObject channelsObject;
        for (int index = 0; index < 4; ++index) {
            const auto channelValue = static_cast<AdjustmentChannel>(index);
            channelsObject.insert(adjustmentChannelKey(channelValue),
                                  levelsChannelToJson(levels.channel(channelValue)));
        }
        parametersObject.insert(QStringLiteral("channels"), channelsObject);
        parametersObject.insert(QStringLiteral("histogramScale"),
                                levels.logarithmicHistogram ? QStringLiteral("logarithmic")
                                                            : QStringLiteral("linear"));
        parametersObject.insert(QStringLiteral("autoClipShadows"), levels.autoClipShadows);
        parametersObject.insert(QStringLiteral("autoClipHighlights"), levels.autoClipHighlights);
        break;
    }
    case AdjustmentType::Curves: {
        const CurvesParameters &curves = std::get<CurvesParameters>(safe.parameters);
        QJsonObject channelsObject;
        for (int index = 0; index < 4; ++index) {
            const auto channelValue = static_cast<AdjustmentChannel>(index);
            channelsObject.insert(adjustmentChannelKey(channelValue),
                                  curveChannelToJson(curves.channel(channelValue)));
        }
        parametersObject.insert(QStringLiteral("channels"), channelsObject);
        parametersObject.insert(QStringLiteral("interpolation"),
                                curveInterpolationToString(curves.interpolation));
        parametersObject.insert(QStringLiteral("histogramScale"),
                                curves.logarithmicHistogram ? QStringLiteral("logarithmic")
                                                            : QStringLiteral("linear"));
        break;
    }
    case AdjustmentType::HueSaturation: {
        const HueSaturationParameters &value = std::get<HueSaturationParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("hue"), value.hue);
        parametersObject.insert(QStringLiteral("saturation"), value.saturation);
        parametersObject.insert(QStringLiteral("lightness"), value.lightness);
        QJsonArray ranges;
        for (const HueSaturationRangeParameters &range : value.ranges) {
            ranges.push_back(hueSaturationRangeToJson(range));
        }
        parametersObject.insert(QStringLiteral("ranges"), ranges);
        break;
    }
    case AdjustmentType::Vibrance: {
        const VibranceParameters &value = std::get<VibranceParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("vibrance"), value.vibrance);
        parametersObject.insert(QStringLiteral("saturation"), value.saturation);
        parametersObject.insert(QStringLiteral("skinProtection"), value.skinProtection);
        break;
    }
    case AdjustmentType::WhiteBalance: {
        const WhiteBalanceParameters &value = std::get<WhiteBalanceParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("temperature"), value.temperature);
        parametersObject.insert(QStringLiteral("tint"), value.tint);
        break;
    }
    case AdjustmentType::ColourBalance: {
        const ColourBalanceParameters &value = std::get<ColourBalanceParameters>(safe.parameters);
        QJsonArray ranges;
        for (const ColourBalanceRangeParameters &range : value.ranges) {
            ranges.push_back(colourBalanceRangeToJson(range));
        }
        parametersObject.insert(QStringLiteral("ranges"), ranges);
        parametersObject.insert(QStringLiteral("preserveLuminosity"), value.preserveLuminosity);
        break;
    }
    case AdjustmentType::ChannelMixer: {
        const ChannelMixerParameters &value = std::get<ChannelMixerParameters>(safe.parameters);
        QJsonArray outputs;
        for (const ChannelMixerChannelParameters &output : value.outputs) {
            outputs.push_back(channelMixerChannelToJson(output));
        }
        parametersObject.insert(QStringLiteral("outputs"), outputs);
        parametersObject.insert(QStringLiteral("monochrome"),
                                channelMixerChannelToJson(value.monochrome));
        parametersObject.insert(QStringLiteral("monochromeEnabled"), value.monochromeEnabled);
        break;
    }
    case AdjustmentType::BlackAndWhite: {
        const BlackAndWhiteParameters &value = std::get<BlackAndWhiteParameters>(safe.parameters);
        QJsonArray weights;
        for (const double weight : value.colourWeights) weights.push_back(weight);
        parametersObject.insert(QStringLiteral("colourWeights"), weights);
        parametersObject.insert(QStringLiteral("tintEnabled"), value.tintEnabled);
        parametersObject.insert(QStringLiteral("tintHue"), value.tintHue);
        parametersObject.insert(QStringLiteral("tintSaturation"), value.tintSaturation);
        break;
    }
    case AdjustmentType::GradientMap: {
        const GradientMapParameters &value = std::get<GradientMapParameters>(safe.parameters);
        QJsonArray stops;
        for (const GradientStop &stop : value.stops) stops.push_back(gradientStopToJson(stop));
        parametersObject.insert(QStringLiteral("stops"), stops);
        parametersObject.insert(QStringLiteral("reverse"), value.reverse);
        parametersObject.insert(QStringLiteral("interpolation"),
                                gradientInterpolationToString(value.interpolation));
        break;
    }
    case AdjustmentType::Posterise: {
        parametersObject.insert(QStringLiteral("levels"),
                                std::get<PosteriseParameters>(safe.parameters).levels);
        break;
    }
    case AdjustmentType::Threshold: {
        const ThresholdParameters &value = std::get<ThresholdParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("threshold"), value.threshold);
        parametersObject.insert(QStringLiteral("source"), thresholdSourceToString(value.source));
        break;
    }
    case AdjustmentType::Invert:
        break;
    case AdjustmentType::PhotoFilter: {
        const PhotoFilterParameters &value = std::get<PhotoFilterParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("colour"), value.colour.name(QColor::HexRgb));
        parametersObject.insert(QStringLiteral("density"), value.density);
        parametersObject.insert(QStringLiteral("preserveLuminosity"), value.preserveLuminosity);
        break;
    }
    case AdjustmentType::SelectiveColour: {
        const SelectiveColourParameters &value =
            std::get<SelectiveColourParameters>(safe.parameters);
        QJsonArray ranges;
        for (const SelectiveColourRangeParameters &range : value.ranges) {
            ranges.push_back(selectiveColourRangeToJson(range));
        }
        parametersObject.insert(QStringLiteral("ranges"), ranges);
        parametersObject.insert(QStringLiteral("method"),
                                value.method == SelectiveColourMethod::Absolute
                                    ? QStringLiteral("absolute")
                                    : QStringLiteral("relative"));
        break;
    }
    case AdjustmentType::Vignette: {
        const VignetteParameters &value = std::get<VignetteParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("amount"), value.amount);
        parametersObject.insert(QStringLiteral("size"), value.size);
        parametersObject.insert(QStringLiteral("midpoint"), value.midpoint);
        parametersObject.insert(QStringLiteral("roundness"), value.roundness);
        parametersObject.insert(QStringLiteral("feather"), value.feather);
        parametersObject.insert(QStringLiteral("centreX"), value.centreX);
        parametersObject.insert(QStringLiteral("centreY"), value.centreY);
        parametersObject.insert(QStringLiteral("rotation"), value.rotation);
        parametersObject.insert(QStringLiteral("highlightProtection"), value.highlightProtection);
        parametersObject.insert(QStringLiteral("inverted"), value.inverted);
        parametersObject.insert(QStringLiteral("tintEnabled"), value.tintEnabled);
        parametersObject.insert(QStringLiteral("tint"), value.tint.name(QColor::HexRgb));
        break;
    }
    case AdjustmentType::RgbSplit: {
        const RgbSplitParameters &value = std::get<RgbSplitParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("redOffsetX"), value.redOffsetX);
        parametersObject.insert(QStringLiteral("redOffsetY"), value.redOffsetY);
        parametersObject.insert(QStringLiteral("blueOffsetX"), value.blueOffsetX);
        parametersObject.insert(QStringLiteral("blueOffsetY"), value.blueOffsetY);
        break;
    }
    case AdjustmentType::ChromaticAberrationCorrection: {
        const ChromaticAberrationCorrectionParameters &value =
            std::get<ChromaticAberrationCorrectionParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("redEdgeShift"), value.redEdgeShift);
        parametersObject.insert(QStringLiteral("blueEdgeShift"), value.blueEdgeShift);
        parametersObject.insert(QStringLiteral("centreX"), value.centreX);
        parametersObject.insert(QStringLiteral("centreY"), value.centreY);
        parametersObject.insert(QStringLiteral("falloff"), value.falloff);
        break;
    }
    case AdjustmentType::SurfaceBlur: {
        const auto &value = std::get<SurfaceBlurParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("radius"), value.radius);
        parametersObject.insert(QStringLiteral("threshold"), value.threshold);
        break;
    }
    case AdjustmentType::MotionBlur: {
        const auto &value = std::get<MotionBlurParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("distance"), value.distance);
        parametersObject.insert(QStringLiteral("angle"), value.angle);
        parametersObject.insert(QStringLiteral("samples"), value.samples);
        parametersObject.insert(QStringLiteral("affectAlpha"), value.affectAlpha);
        break;
    }
    case AdjustmentType::RadialBlur: {
        const auto &value = std::get<RadialBlurParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("mode"),
                                value.mode == RadialBlurMode::Zoom
                                    ? QStringLiteral("zoom") : QStringLiteral("spin"));
        parametersObject.insert(QStringLiteral("amount"), value.amount);
        parametersObject.insert(QStringLiteral("centreX"), value.centreX);
        parametersObject.insert(QStringLiteral("centreY"), value.centreY);
        parametersObject.insert(QStringLiteral("samples"), value.samples);
        parametersObject.insert(QStringLiteral("affectAlpha"), value.affectAlpha);
        break;
    }
    case AdjustmentType::Lut: {
        const LutParameters &value = std::get<LutParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("title"), value.title);
        parametersObject.insert(QStringLiteral("sourceName"), value.sourceName);
        parametersObject.insert(QStringLiteral("strength"), value.strength);
        parametersObject.insert(QStringLiteral("shaperSize"), value.shaperSize);
        parametersObject.insert(QStringLiteral("cubeSize"), value.cubeSize);
        parametersObject.insert(QStringLiteral("shaperDomainMin"), domainToJson(value.shaperDomainMin));
        parametersObject.insert(QStringLiteral("shaperDomainMax"), domainToJson(value.shaperDomainMax));
        parametersObject.insert(QStringLiteral("cubeDomainMin"), domainToJson(value.cubeDomainMin));
        parametersObject.insert(QStringLiteral("cubeDomainMax"), domainToJson(value.cubeDomainMax));
        parametersObject.insert(QStringLiteral("shaperDomainSource"),
                                lutDomainSourceToString(value.shaperDomainSource));
        parametersObject.insert(QStringLiteral("cubeDomainSource"),
                                lutDomainSourceToString(value.cubeDomainSource));
        parametersObject.insert(QStringLiteral("interpolation"),
                                lutInterpolationToString(value.interpolation));
        parametersObject.insert(QStringLiteral("processingMode"),
                                lutProcessingModeToString(value.processingMode));
        parametersObject.insert(QStringLiteral("operatorProfile"),
                                lutOperatorProfileToString(value.operatorProfile));
        parametersObject.insert(QStringLiteral("shaperData"), encodeFloatVector(value.shaperData));
        parametersObject.insert(QStringLiteral("cubeData"), encodeFloatVector(value.cubeData));
        break;
    }
    case AdjustmentType::ShadowsHighlights: {
        const auto &value = std::get<ShadowsHighlightsParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("shadowAmount"), value.shadowAmount);
        parametersObject.insert(QStringLiteral("shadowTonalWidth"), value.shadowTonalWidth);
        parametersObject.insert(QStringLiteral("highlightAmount"), value.highlightAmount);
        parametersObject.insert(QStringLiteral("highlightTonalWidth"), value.highlightTonalWidth);
        parametersObject.insert(QStringLiteral("radius"), value.radius);
        parametersObject.insert(QStringLiteral("midtoneContrast"), value.midtoneContrast);
        parametersObject.insert(QStringLiteral("colourCorrection"), value.colourCorrection);
        break;
    }
    case AdjustmentType::GaussianBlur: {
        const auto &value = std::get<GaussianBlurParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("radius"), value.radius);
        parametersObject.insert(QStringLiteral("affectAlpha"), value.affectAlpha);
        break;
    }
    case AdjustmentType::BoxBlur: {
        const auto &value = std::get<BoxBlurParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("radius"), value.radius);
        parametersObject.insert(QStringLiteral("affectAlpha"), value.affectAlpha);
        break;
    }
    case AdjustmentType::UnsharpMask: {
        const auto &value = std::get<UnsharpMaskParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("radius"), value.radius);
        parametersObject.insert(QStringLiteral("amount"), value.amount);
        parametersObject.insert(QStringLiteral("threshold"), value.threshold);
        break;
    }
    case AdjustmentType::HighPass: {
        const auto &value = std::get<HighPassParameters>(safe.parameters);
        parametersObject.insert(QStringLiteral("radius"), value.radius);
        parametersObject.insert(QStringLiteral("monochrome"), value.monochrome);
        break;
    }
    }
    object.insert(QStringLiteral("parameters"), parametersObject);
    if (ok) *ok = true;
    return object;
}

AdjustmentData AdjustmentData::fromJson(const QJsonObject &object,
                                        const AdjustmentType fallbackType,
                                        bool *ok)
{
    const QJsonValue typeValue = object.value(QStringLiteral("type"));
    bool typeOk = false;
    const AdjustmentType parsedType = adjustmentTypeFromString(
        typeValue.toString(adjustmentTypeToString(fallbackType)), &typeOk);
    const QJsonValue schemaJson = object.value(QStringLiteral("schema"));
    const int schemaValue = schemaJson.toInt(1);
    AdjustmentData result;
    result.reset(typeOk ? parsedType : fallbackType);
    bool valid = typeValue.isString() && typeOk && schemaJson.isDouble()
        && schemaValue >= 1 && schemaValue <= static_cast<int>(CurrentSchema);
    const QJsonValue parametersValue = object.value(QStringLiteral("parameters"));
    valid = valid && parametersValue.isObject();
    const QJsonObject parametersObject = parametersValue.toObject();
    switch (result.type) {
    case AdjustmentType::Exposure: {
        ExposureParameters value;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("exposure"), 0.0, &value.exposure) && valid;
        if (schemaValue >= 2) {
            valid = readFiniteJsonNumber(parametersObject, QStringLiteral("offset"), 0.0, &value.offset) && valid;
            valid = readFiniteJsonNumber(parametersObject, QStringLiteral("gamma"), 1.0, &value.gamma) && valid;
        }
        result.parameters = value;
        break;
    }
    case AdjustmentType::Contrast: {
        ContrastParameters value;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("contrast"), 0.0, &value.contrast) && valid;
        if (schemaValue >= 2) {
            valid = readFiniteJsonNumber(parametersObject, QStringLiteral("pivot"), 0.5, &value.pivot) && valid;
        }
        result.parameters = value;
        break;
    }
    case AdjustmentType::Saturation: {
        SaturationParameters value;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("saturation"), 0.0, &value.saturation) && valid;
        result.parameters = value;
        break;
    }
    case AdjustmentType::Levels: {
        LevelsParameters levels;
        const QJsonValue channelsValue = parametersObject.value(QStringLiteral("channels"));
        valid = valid && channelsValue.isObject();
        const QJsonObject channelsObject = channelsValue.toObject();
        for (int index = 0; index < 4; ++index) {
            const auto channelValue = static_cast<AdjustmentChannel>(index);
            const QJsonValue encoded = channelsObject.value(adjustmentChannelKey(channelValue));
            bool channelOk = false;
            levels.channel(channelValue) = levelsChannelFromJson(encoded.toObject(), &channelOk);
            valid = valid && encoded.isObject() && channelOk;
        }
        const QJsonValue histogramScaleValue = parametersObject.value(QStringLiteral("histogramScale"));
        const QString histogramScale = histogramScaleValue.toString(QStringLiteral("linear"));
        valid = valid && histogramScaleValue.isString()
            && (histogramScale == QStringLiteral("linear") || histogramScale == QStringLiteral("logarithmic"));
        levels.logarithmicHistogram = histogramScale == QStringLiteral("logarithmic");
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("autoClipShadows"), 0.001, &levels.autoClipShadows) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("autoClipHighlights"), 0.001, &levels.autoClipHighlights) && valid;
        result.parameters = levels;
        break;
    }
    case AdjustmentType::Curves: {
        CurvesParameters curves;
        const QJsonValue channelsValue = parametersObject.value(QStringLiteral("channels"));
        valid = valid && schemaValue >= 2 && channelsValue.isObject();
        const QJsonObject channelsObject = channelsValue.toObject();
        for (int index = 0; index < 4; ++index) {
            const auto channelValue = static_cast<AdjustmentChannel>(index);
            const QJsonValue encoded = channelsObject.value(adjustmentChannelKey(channelValue));
            bool channelOk = false;
            curves.channel(channelValue) = curveChannelFromJson(encoded.toArray(), &channelOk);
            valid = valid && encoded.isArray() && channelOk;
        }
        const QJsonValue interpolationValue = parametersObject.value(QStringLiteral("interpolation"));
        bool interpolationOk = false;
        curves.interpolation = curveInterpolationFromString(interpolationValue.toString(), &interpolationOk);
        valid = valid && interpolationValue.isString() && interpolationOk;
        const QJsonValue histogramScaleValue = parametersObject.value(QStringLiteral("histogramScale"));
        const QString histogramScale = histogramScaleValue.toString(QStringLiteral("linear"));
        valid = valid && histogramScaleValue.isString()
            && (histogramScale == QStringLiteral("linear") || histogramScale == QStringLiteral("logarithmic"));
        curves.logarithmicHistogram = histogramScale == QStringLiteral("logarithmic");
        result.parameters = curves;
        break;
    }
    case AdjustmentType::HueSaturation: {
        HueSaturationParameters value;
        valid = valid && schemaValue >= 3;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("hue"), 0.0, &value.hue) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("saturation"), 0.0, &value.saturation) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("lightness"), 0.0, &value.lightness) && valid;
        const QJsonValue rangesValue = parametersObject.value(QStringLiteral("ranges"));
        valid = valid && rangesValue.isArray() && rangesValue.toArray().size() == 6;
        const QJsonArray ranges = rangesValue.toArray();
        for (int index = 0; index < std::min(6, static_cast<int>(ranges.size())); ++index) {
            bool rangeOk = false;
            value.ranges[static_cast<std::size_t>(index)] = hueSaturationRangeFromJson(
                ranges.at(index).toObject(), value.ranges[static_cast<std::size_t>(index)], &rangeOk);
            valid = valid && ranges.at(index).isObject() && rangeOk;
        }
        result.parameters = value;
        break;
    }
    case AdjustmentType::Vibrance: {
        VibranceParameters value;
        valid = valid && schemaValue >= 3;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("vibrance"), 0.0, &value.vibrance) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("saturation"), 0.0, &value.saturation) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("skinProtection"), 65.0, &value.skinProtection) && valid;
        result.parameters = value;
        break;
    }
    case AdjustmentType::WhiteBalance: {
        WhiteBalanceParameters value;
        valid = valid && schemaValue >= 3;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("temperature"), 0.0, &value.temperature) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("tint"), 0.0, &value.tint) && valid;
        result.parameters = value;
        break;
    }
    case AdjustmentType::ColourBalance: {
        ColourBalanceParameters value;
        valid = valid && schemaValue >= 3;
        const QJsonValue rangesValue = parametersObject.value(QStringLiteral("ranges"));
        valid = valid && rangesValue.isArray() && rangesValue.toArray().size() == 3;
        const QJsonArray ranges = rangesValue.toArray();
        for (int index = 0; index < std::min(3, static_cast<int>(ranges.size())); ++index) {
            bool rangeOk = false;
            value.ranges[static_cast<std::size_t>(index)] = colourBalanceRangeFromJson(
                ranges.at(index).toObject(), &rangeOk);
            valid = valid && ranges.at(index).isObject() && rangeOk;
        }
        const QJsonValue preserveValue = parametersObject.value(QStringLiteral("preserveLuminosity"));
        valid = valid && preserveValue.isBool();
        value.preserveLuminosity = preserveValue.toBool(true);
        result.parameters = value;
        break;
    }
    case AdjustmentType::ChannelMixer: {
        ChannelMixerParameters value;
        valid = valid && schemaValue >= 4;
        const QJsonValue outputsValue = parametersObject.value(QStringLiteral("outputs"));
        valid = valid && outputsValue.isArray() && outputsValue.toArray().size() == 3;
        const QJsonArray outputs = outputsValue.toArray();
        for (int index = 0; index < std::min(3, static_cast<int>(outputs.size())); ++index) {
            bool outputOk = false;
            value.outputs[static_cast<std::size_t>(index)] = channelMixerChannelFromJson(
                outputs.at(index).toObject(), &outputOk);
            valid = valid && outputs.at(index).isObject() && outputOk;
        }
        const QJsonValue monochromeValue = parametersObject.value(QStringLiteral("monochrome"));
        bool monochromeOk = false;
        value.monochrome = channelMixerChannelFromJson(monochromeValue.toObject(), &monochromeOk);
        valid = valid && monochromeValue.isObject() && monochromeOk;
        const QJsonValue enabledValue = parametersObject.value(QStringLiteral("monochromeEnabled"));
        valid = valid && enabledValue.isBool();
        value.monochromeEnabled = enabledValue.toBool(false);
        result.parameters = value;
        break;
    }
    case AdjustmentType::BlackAndWhite: {
        BlackAndWhiteParameters value;
        valid = valid && schemaValue >= 4;
        const QJsonValue weightsValue = parametersObject.value(QStringLiteral("colourWeights"));
        valid = valid && weightsValue.isArray() && weightsValue.toArray().size() == 6;
        const QJsonArray weights = weightsValue.toArray();
        for (int index = 0; index < std::min(6, static_cast<int>(weights.size())); ++index) {
            if (!weights.at(index).isDouble() || !std::isfinite(weights.at(index).toDouble())) {
                valid = false;
            } else {
                value.colourWeights[static_cast<std::size_t>(index)] = weights.at(index).toDouble();
            }
        }
        const QJsonValue tintEnabledValue = parametersObject.value(QStringLiteral("tintEnabled"));
        valid = valid && tintEnabledValue.isBool();
        value.tintEnabled = tintEnabledValue.toBool(false);
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("tintHue"), 30.0, &value.tintHue) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("tintSaturation"), 20.0, &value.tintSaturation) && valid;
        result.parameters = value;
        break;
    }
    case AdjustmentType::GradientMap: {
        GradientMapParameters value;
        value.stops.clear();
        valid = valid && schemaValue >= 4;
        const QJsonValue stopsValue = parametersObject.value(QStringLiteral("stops"));
        valid = valid && stopsValue.isArray() && stopsValue.toArray().size() >= 2
            && stopsValue.toArray().size() <= 64;
        const QJsonArray stops = stopsValue.toArray();
        for (const QJsonValue &encoded : stops) {
            bool stopOk = false;
            value.stops.push_back(gradientStopFromJson(encoded.toObject(), &stopOk));
            valid = valid && encoded.isObject() && stopOk;
        }
        const QJsonValue reverseValue = parametersObject.value(QStringLiteral("reverse"));
        valid = valid && reverseValue.isBool();
        value.reverse = reverseValue.toBool(false);
        const QJsonValue interpolationValue = parametersObject.value(QStringLiteral("interpolation"));
        bool interpolationOk = false;
        value.interpolation = gradientInterpolationFromString(
            interpolationValue.toString(), &interpolationOk);
        valid = valid && interpolationValue.isString() && interpolationOk;
        result.parameters = value;
        break;
    }
    case AdjustmentType::Posterise: {
        PosteriseParameters value;
        const QJsonValue levelsValue = parametersObject.value(QStringLiteral("levels"));
        valid = valid && schemaValue >= 5 && levelsValue.isDouble();
        value.levels = levelsValue.toInt(4);
        result.parameters = value;
        break;
    }
    case AdjustmentType::Threshold: {
        ThresholdParameters value;
        valid = valid && schemaValue >= 5;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("threshold"), 0.5,
                                     &value.threshold) && valid;
        const QJsonValue sourceValue = parametersObject.value(QStringLiteral("source"));
        bool sourceOk = false;
        value.source = thresholdSourceFromString(sourceValue.toString(), &sourceOk);
        valid = valid && sourceValue.isString() && sourceOk;
        result.parameters = value;
        break;
    }
    case AdjustmentType::Invert: {
        valid = valid && schemaValue >= 12;
        result.parameters = InvertParameters {};
        break;
    }
    case AdjustmentType::PhotoFilter: {
        PhotoFilterParameters value;
        valid = valid && schemaValue >= 12;
        const QJsonValue colourValue = parametersObject.value(QStringLiteral("colour"));
        const QColor colour(colourValue.toString());
        valid = valid && colourValue.isString() && colour.isValid();
        if (colour.isValid()) value.colour = colour;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("density"), 25.0,
                                     &value.density) && valid;
        const QJsonValue preserveValue = parametersObject.value(QStringLiteral("preserveLuminosity"));
        valid = valid && preserveValue.isBool();
        value.preserveLuminosity = preserveValue.toBool(true);
        result.parameters = value;
        break;
    }
    case AdjustmentType::SelectiveColour: {
        SelectiveColourParameters value;
        valid = valid && schemaValue >= 13;
        const QJsonValue rangesValue = parametersObject.value(QStringLiteral("ranges"));
        valid = valid && rangesValue.isArray() && rangesValue.toArray().size() == 9;
        const QJsonArray ranges = rangesValue.toArray();
        for (int index = 0; index < std::min(9, static_cast<int>(ranges.size())); ++index) {
            bool rangeOk = false;
            value.ranges[static_cast<std::size_t>(index)] = selectiveColourRangeFromJson(
                ranges.at(index).toObject(), &rangeOk);
            valid = valid && ranges.at(index).isObject() && rangeOk;
        }
        const QJsonValue methodValue = parametersObject.value(QStringLiteral("method"));
        const QString method = methodValue.toString().trimmed().toLower();
        valid = valid && methodValue.isString()
            && (method == QStringLiteral("relative")
                || method == QStringLiteral("absolute"));
        value.method = method == QStringLiteral("absolute")
            ? SelectiveColourMethod::Absolute : SelectiveColourMethod::Relative;
        result.parameters = value;
        break;
    }
    case AdjustmentType::Vignette: {
        VignetteParameters value;
        valid = valid && schemaValue >= 14;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("amount"), 25.0, &value.amount) && valid;
        if (schemaValue >= 16) {
            valid = readFiniteJsonNumber(parametersObject, QStringLiteral("size"), 100.0, &value.size) && valid;
        } else {
            value.size = 100.0;
        }
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("midpoint"), 50.0, &value.midpoint) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("roundness"), 0.0, &value.roundness) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("feather"), 50.0, &value.feather) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("centreX"), 0.0, &value.centreX) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("centreY"), 0.0, &value.centreY) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("rotation"), 0.0, &value.rotation) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("highlightProtection"), 0.0, &value.highlightProtection) && valid;
        const QJsonValue invertedValue = parametersObject.value(QStringLiteral("inverted"));
        const QJsonValue tintEnabledValue = parametersObject.value(QStringLiteral("tintEnabled"));
        const QJsonValue tintValue = parametersObject.value(QStringLiteral("tint"));
        const QColor tint(tintValue.toString());
        valid = valid && invertedValue.isBool() && tintEnabledValue.isBool()
            && tintValue.isString() && tint.isValid();
        value.inverted = invertedValue.toBool(false);
        value.tintEnabled = tintEnabledValue.toBool(false);
        if (tint.isValid()) value.tint = tint;
        result.parameters = value;
        break;
    }
    case AdjustmentType::RgbSplit: {
        RgbSplitParameters value;
        valid = valid && schemaValue >= 14;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("redOffsetX"), -6.0, &value.redOffsetX) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("redOffsetY"), 0.0, &value.redOffsetY) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("blueOffsetX"), 6.0, &value.blueOffsetX) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("blueOffsetY"), 0.0, &value.blueOffsetY) && valid;
        result.parameters = value;
        break;
    }
    case AdjustmentType::ChromaticAberrationCorrection: {
        ChromaticAberrationCorrectionParameters value;
        valid = valid && schemaValue >= 14;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("redEdgeShift"), 0.0, &value.redEdgeShift) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("blueEdgeShift"), 0.0, &value.blueEdgeShift) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("centreX"), 0.0, &value.centreX) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("centreY"), 0.0, &value.centreY) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("falloff"), 1.0, &value.falloff) && valid;
        result.parameters = value;
        break;
    }
    case AdjustmentType::SurfaceBlur: {
        SurfaceBlurParameters value;
        valid = valid && schemaValue >= 15;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("radius"), 12.0, &value.radius) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("threshold"), 20.0, &value.threshold) && valid;
        result.parameters = value;
        break;
    }
    case AdjustmentType::MotionBlur: {
        MotionBlurParameters value;
        valid = valid && schemaValue >= 15;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("distance"), 24.0, &value.distance) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("angle"), 0.0, &value.angle) && valid;
        const QJsonValue samplesValue = parametersObject.value(QStringLiteral("samples"));
        const QJsonValue alphaValue = parametersObject.value(QStringLiteral("affectAlpha"));
        valid = valid && samplesValue.isDouble() && alphaValue.isBool();
        value.samples = samplesValue.toInt(24);
        value.affectAlpha = alphaValue.toBool(true);
        result.parameters = value;
        break;
    }
    case AdjustmentType::RadialBlur: {
        RadialBlurParameters value;
        valid = valid && schemaValue >= 15;
        const QJsonValue modeValue = parametersObject.value(QStringLiteral("mode"));
        const QString mode = modeValue.toString().trimmed().toLower();
        valid = valid && modeValue.isString()
            && (mode == QStringLiteral("spin") || mode == QStringLiteral("zoom"));
        value.mode = mode == QStringLiteral("zoom")
            ? RadialBlurMode::Zoom : RadialBlurMode::Spin;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("amount"), 20.0, &value.amount) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("centreX"), 0.0, &value.centreX) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("centreY"), 0.0, &value.centreY) && valid;
        const QJsonValue samplesValue = parametersObject.value(QStringLiteral("samples"));
        const QJsonValue alphaValue = parametersObject.value(QStringLiteral("affectAlpha"));
        valid = valid && samplesValue.isDouble() && alphaValue.isBool();
        value.samples = samplesValue.toInt(24);
        value.affectAlpha = alphaValue.toBool(true);
        result.parameters = value;
        break;
    }
    case AdjustmentType::Lut: {
        LutParameters value;
        valid = valid && schemaValue >= 5;
        const QJsonValue titleValue = parametersObject.value(QStringLiteral("title"));
        const QJsonValue sourceNameValue = parametersObject.value(QStringLiteral("sourceName"));
        valid = valid && titleValue.isString() && sourceNameValue.isString();
        value.title = titleValue.toString();
        value.sourceName = sourceNameValue.toString();
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("strength"), 100.0,
                                     &value.strength) && valid;
        const QJsonValue shaperSizeValue = parametersObject.value(QStringLiteral("shaperSize"));
        const QJsonValue cubeSizeValue = parametersObject.value(QStringLiteral("cubeSize"));
        valid = valid && shaperSizeValue.isDouble() && cubeSizeValue.isDouble();
        value.shaperSize = shaperSizeValue.toInt(0);
        value.cubeSize = cubeSizeValue.toInt(0);
        valid = domainFromJson(parametersObject.value(QStringLiteral("shaperDomainMin")),
                               {0.0, 0.0, 0.0}, &value.shaperDomainMin) && valid;
        valid = domainFromJson(parametersObject.value(QStringLiteral("shaperDomainMax")),
                               {1.0, 1.0, 1.0}, &value.shaperDomainMax) && valid;
        valid = domainFromJson(parametersObject.value(QStringLiteral("cubeDomainMin")),
                               {0.0, 0.0, 0.0}, &value.cubeDomainMin) && valid;
        valid = domainFromJson(parametersObject.value(QStringLiteral("cubeDomainMax")),
                               {1.0, 1.0, 1.0}, &value.cubeDomainMax) && valid;
        valid = domainHasPositiveFiniteSpan(value.shaperDomainMin,
                                            value.shaperDomainMax) && valid;
        valid = domainHasPositiveFiniteSpan(value.cubeDomainMin,
                                            value.cubeDomainMax) && valid;
        if (schemaValue >= 7) {
            const QJsonValue shaperDomainSourceValue = parametersObject.value(
                QStringLiteral("shaperDomainSource"));
            const QJsonValue cubeDomainSourceValue = parametersObject.value(
                QStringLiteral("cubeDomainSource"));
            bool shaperDomainSourceOk = false;
            bool cubeDomainSourceOk = false;
            value.shaperDomainSource = lutDomainSourceFromString(
                shaperDomainSourceValue.toString(), &shaperDomainSourceOk);
            value.cubeDomainSource = lutDomainSourceFromString(
                cubeDomainSourceValue.toString(), &cubeDomainSourceOk);
            valid = valid && shaperDomainSourceValue.isString()
                && cubeDomainSourceValue.isString()
                && shaperDomainSourceOk && cubeDomainSourceOk;
        } else {
            const auto isDefaultDomain = [](const std::array<double, 3> &minimum,
                                            const std::array<double, 3> &maximum) {
                return minimum == std::array<double, 3> {0.0, 0.0, 0.0}
                    && maximum == std::array<double, 3> {1.0, 1.0, 1.0};
            };
            value.shaperDomainSource = isDefaultDomain(value.shaperDomainMin,
                                                        value.shaperDomainMax)
                ? LutDomainSource::DefaultRange : LutDomainSource::LegacyPersisted;
            value.cubeDomainSource = isDefaultDomain(value.cubeDomainMin,
                                                      value.cubeDomainMax)
                ? LutDomainSource::DefaultRange : LutDomainSource::LegacyPersisted;
        }
        if (schemaValue >= 8) {
            const QJsonValue interpolationValue = parametersObject.value(
                QStringLiteral("interpolation"));
            bool interpolationOk = false;
            value.interpolation = lutInterpolationFromString(
                interpolationValue.toString(), &interpolationOk);
            valid = valid && interpolationValue.isString() && interpolationOk;
        } else {
            // Schemas 1-7 were always evaluated trilinearly. Keep their saved
            // appearance stable while new imports default to tetrahedral.
            value.interpolation = LutInterpolation::Trilinear;
        }
        if (schemaValue >= 9) {
            const QJsonValue processingModeValue = parametersObject.value(
                QStringLiteral("processingMode"));
            bool processingModeOk = false;
            value.processingMode = lutProcessingModeFromString(
                processingModeValue.toString(), &processingModeOk);
            valid = valid && processingModeValue.isString() && processingModeOk;
        } else {
            // Every prior schema sampled the document's stored encoded
            // components directly. Persist that historical contract explicitly.
            value.processingMode = LutProcessingMode::EncodedDocument;
        }
        if (schemaValue >= 10) {
            const QJsonValue operatorProfileValue = parametersObject.value(
                QStringLiteral("operatorProfile"));
            bool operatorProfileOk = false;
            value.operatorProfile = lutOperatorProfileFromString(
                operatorProfileValue.toString(), &operatorProfileOk);
            valid = valid && operatorProfileValue.isString() && operatorProfileOk;
        } else {
            // Schemas 1-9 had only generic .cube interpretation. Never infer a
            // named display transform while reopening an existing project.
            value.operatorProfile = LutOperatorProfile::Generic;
        }
        const qsizetype shaperCount = value.shaperSize >= 2 && value.shaperSize <= 65'536
            ? qsizetype(value.shaperSize) * 3 : 0;
        const qsizetype cubeCount = value.cubeSize >= 2 && value.cubeSize <= 65
            ? qsizetype(value.cubeSize) * value.cubeSize * value.cubeSize * 3 : 0;
        valid = decodeFloatVector(parametersObject.value(QStringLiteral("shaperData")),
                                  shaperCount, &value.shaperData) && valid;
        valid = decodeFloatVector(parametersObject.value(QStringLiteral("cubeData")),
                                  cubeCount, &value.cubeData) && valid;
        result.parameters = value;
        break;
    }
    case AdjustmentType::ShadowsHighlights: {
        ShadowsHighlightsParameters value;
        valid = valid && schemaValue >= 6;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("shadowAmount"), 0.0, &value.shadowAmount) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("shadowTonalWidth"), 50.0, &value.shadowTonalWidth) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("highlightAmount"), 0.0, &value.highlightAmount) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("highlightTonalWidth"), 50.0, &value.highlightTonalWidth) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("radius"), 80.0, &value.radius) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("midtoneContrast"), 0.0, &value.midtoneContrast) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("colourCorrection"), 20.0, &value.colourCorrection) && valid;
        result.parameters = value;
        break;
    }
    case AdjustmentType::GaussianBlur: {
        GaussianBlurParameters value;
        valid = valid && schemaValue >= 11;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("radius"), 8.0, &value.radius) && valid;
        const QJsonValue affectAlpha = parametersObject.value(QStringLiteral("affectAlpha"));
        valid = valid && affectAlpha.isBool();
        value.affectAlpha = affectAlpha.toBool(true);
        result.parameters = value;
        break;
    }
    case AdjustmentType::BoxBlur: {
        BoxBlurParameters value;
        valid = valid && schemaValue >= 11;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("radius"), 4.0, &value.radius) && valid;
        const QJsonValue affectAlpha = parametersObject.value(QStringLiteral("affectAlpha"));
        valid = valid && affectAlpha.isBool();
        value.affectAlpha = affectAlpha.toBool(true);
        result.parameters = value;
        break;
    }
    case AdjustmentType::UnsharpMask: {
        UnsharpMaskParameters value;
        valid = valid && schemaValue >= 11;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("radius"), 2.0, &value.radius) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("amount"), 100.0, &value.amount) && valid;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("threshold"), 0.0, &value.threshold) && valid;
        result.parameters = value;
        break;
    }
    case AdjustmentType::HighPass: {
        HighPassParameters value;
        valid = valid && schemaValue >= 11;
        valid = readFiniteJsonNumber(parametersObject, QStringLiteral("radius"), 10.0, &value.radius) && valid;
        const QJsonValue monochrome = parametersObject.value(QStringLiteral("monochrome"));
        valid = valid && monochrome.isBool();
        value.monochrome = monochrome.toBool(false);
        result.parameters = value;
        break;
    }
    }
    result.normalise();
    if (ok) *ok = valid;
    return result;
}

void LiveFilter::normalise()
{
    if (id.isNull()) id = QUuid::createUuid();
    schema = CurrentSchema;
    revision = std::max<quint64>(1, revision);
    adjustment.normalise();
    if (maskImage.isNull()) {
        maskReferenceSize = {};
        maskReferenceOrigin = {};
        maskEnabled = true;
        maskInverted = false;
    } else {
        if (maskImage.format() != QImage::Format_Grayscale8) {
            maskImage = maskImage.convertToFormat(QImage::Format_Grayscale8);
        }
        if (!maskReferenceSize.isValid() || maskReferenceSize.isEmpty()) {
            maskReferenceSize = maskImage.size();
        }
    }
}

bool adjustmentTypeSupportsLiveFilter(const AdjustmentType type)
{
    // 0.14.0f deliberately reuses every existing typed 0.13 operator. The
    // semantic distinction is ownership/execution, not a second algorithm ID
    // namespace. Future operators may opt out here if they require a document
    // stack semantic that cannot be localised to one Smart Layer.
    switch (type) {
    case AdjustmentType::Exposure:
    case AdjustmentType::Contrast:
    case AdjustmentType::Saturation:
    case AdjustmentType::Levels:
    case AdjustmentType::Curves:
    case AdjustmentType::HueSaturation:
    case AdjustmentType::Vibrance:
    case AdjustmentType::WhiteBalance:
    case AdjustmentType::ColourBalance:
    case AdjustmentType::ChannelMixer:
    case AdjustmentType::BlackAndWhite:
    case AdjustmentType::GradientMap:
    case AdjustmentType::Posterise:
    case AdjustmentType::Threshold:
    case AdjustmentType::Lut:
    case AdjustmentType::ShadowsHighlights:
    case AdjustmentType::GaussianBlur:
    case AdjustmentType::BoxBlur:
    case AdjustmentType::UnsharpMask:
    case AdjustmentType::HighPass:
    case AdjustmentType::Invert:
    case AdjustmentType::PhotoFilter:
    case AdjustmentType::SelectiveColour:
    case AdjustmentType::Vignette:
    case AdjustmentType::RgbSplit:
    case AdjustmentType::ChromaticAberrationCorrection:
    case AdjustmentType::SurfaceBlur:
    case AdjustmentType::MotionBlur:
    case AdjustmentType::RadialBlur:
        return true;
    }
    return false;
}

QByteArray adjustmentRenderIdentity(const AdjustmentData &input)
{
    AdjustmentData data = input;
    data.normalise();
    QByteArray semanticBytes;
    if (data.type == AdjustmentType::Lut) {
        const auto &parameters = std::get<LutParameters>(data.parameters);
        QDataStream stream(&semanticBytes, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_0);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << QByteArrayLiteral("adjustment-render-lut-v1")
               << static_cast<qint32>(data.type)
               << parameters.strength
               << static_cast<qint32>(parameters.interpolation)
               << static_cast<qint32>(parameters.processingMode)
               << static_cast<qint32>(parameters.operatorProfile)
               << static_cast<qint32>(parameters.shaperSize)
               << static_cast<qint32>(parameters.cubeSize);
        for (const double value : parameters.shaperDomainMin) stream << value;
        for (const double value : parameters.shaperDomainMax) stream << value;
        for (const double value : parameters.cubeDomainMin) stream << value;
        for (const double value : parameters.cubeDomainMax) stream << value;
        stream << parameters.tableFingerprint;
    } else {
        bool ok = false;
        semanticBytes = QJsonDocument(data.toJson(&ok)).toJson(QJsonDocument::Compact);
        if (!ok) return {};
    }
    return QCryptographicHash::hash(semanticBytes, QCryptographicHash::Sha256);
}

bool LiveFilter::isSafe() const
{
    if (schema != CurrentSchema || id.isNull() || revision < 1
        || !adjustmentTypeSupportsLiveFilter(adjustment.type)) {
        return false;
    }
    bool encodedOk = false;
    const QJsonObject encoded = adjustment.toJson(&encodedOk);
    if (!encodedOk || encoded.isEmpty()) return false;
    if (maskImage.isNull()) {
        return maskReferenceSize.isEmpty()
            && qFuzzyIsNull(maskReferenceOrigin.x())
            && qFuzzyIsNull(maskReferenceOrigin.y())
            && maskEnabled && !maskInverted;
    }
    constexpr int MaximumMaskExtent = 32768;
    return maskImage.width() >= 1 && maskImage.height() >= 1
        && maskImage.width() <= MaximumMaskExtent
        && maskImage.height() <= MaximumMaskExtent
        && maskImage.format() == QImage::Format_Grayscale8
        && maskReferenceSize.isValid() && !maskReferenceSize.isEmpty()
        && maskReferenceSize.width() <= MaximumMaskExtent
        && maskReferenceSize.height() <= MaximumMaskExtent
        && std::isfinite(maskReferenceOrigin.x())
        && std::isfinite(maskReferenceOrigin.y())
        && std::abs(maskReferenceOrigin.x()) <= 1.0e9
        && std::abs(maskReferenceOrigin.y()) <= 1.0e9;
}

QJsonObject LiveFilter::toJson(bool *ok) const
{
    if (ok) *ok = false;
    LiveFilter safe = *this;
    safe.normalise();
    if (!safe.isSafe()) return {};
    bool adjustmentOk = false;
    const QJsonObject adjustmentObject = safe.adjustment.toJson(&adjustmentOk);
    if (!adjustmentOk) return {};
    QJsonObject object;
    object.insert(QStringLiteral("schema"), static_cast<int>(CurrentSchema));
    object.insert(QStringLiteral("id"), safe.id.toString(QUuid::WithoutBraces));
    object.insert(QStringLiteral("enabled"), safe.enabled);
    object.insert(QStringLiteral("revision"), QString::number(safe.revision));
    object.insert(QStringLiteral("adjustment"), adjustmentObject);
    if (!safe.maskImage.isNull()) {
        bool maskOk = false;
        const QString encodedMask = encodeImage(safe.maskImage, &maskOk);
        if (!maskOk || encodedMask.isEmpty()) return {};
        object.insert(QStringLiteral("maskImage"), encodedMask);
        object.insert(QStringLiteral("maskReferenceWidth"), safe.maskReferenceSize.width());
        object.insert(QStringLiteral("maskReferenceHeight"), safe.maskReferenceSize.height());
        object.insert(QStringLiteral("maskReferenceOriginX"), safe.maskReferenceOrigin.x());
        object.insert(QStringLiteral("maskReferenceOriginY"), safe.maskReferenceOrigin.y());
        object.insert(QStringLiteral("maskEnabled"), safe.maskEnabled);
        object.insert(QStringLiteral("maskInverted"), safe.maskInverted);
    }
    if (ok) *ok = true;
    return object;
}

LiveFilter LiveFilter::fromJson(const QJsonObject &object, bool *ok)
{
    if (ok) *ok = false;
    LiveFilter filter;
    const int schemaValue = object.value(QStringLiteral("schema")).toInt(-1);
    const QUuid id(object.value(QStringLiteral("id")).toString());
    const QJsonValue enabledValue = object.value(QStringLiteral("enabled"));
    const QJsonValue revisionValue = object.value(QStringLiteral("revision"));
    bool revisionOk = false;
    const quint64 revision = revisionValue.toString().toULongLong(&revisionOk);
    const QJsonValue adjustmentValue = object.value(QStringLiteral("adjustment"));
    bool adjustmentOk = false;
    const AdjustmentData adjustment = AdjustmentData::fromJson(
        adjustmentValue.toObject(), AdjustmentType::Exposure, &adjustmentOk);
    bool valid = (schemaValue == 1 || schemaValue == static_cast<int>(CurrentSchema))
        && !id.isNull() && enabledValue.isBool() && revisionValue.isString()
        && revisionOk && revision >= 1 && adjustmentValue.isObject()
        && adjustmentOk && adjustmentTypeSupportsLiveFilter(adjustment.type);
    if (!valid) return filter;
    filter.schema = CurrentSchema;
    filter.id = id;
    filter.enabled = enabledValue.toBool(true);
    filter.revision = revision;
    filter.adjustment = adjustment;

    const QStringList maskFields {
        QStringLiteral("maskImage"), QStringLiteral("maskReferenceWidth"),
        QStringLiteral("maskReferenceHeight"), QStringLiteral("maskReferenceOriginX"),
        QStringLiteral("maskReferenceOriginY"), QStringLiteral("maskEnabled"),
        QStringLiteral("maskInverted")};
    if (schemaValue == 1) {
        for (const QString &field : maskFields) {
            valid = valid && !object.contains(field);
        }
    }

    if (schemaValue >= 2 && object.contains(QStringLiteral("maskImage"))) {
        const QJsonValue maskValue = object.value(QStringLiteral("maskImage"));
        const QJsonValue widthValue = object.value(QStringLiteral("maskReferenceWidth"));
        const QJsonValue heightValue = object.value(QStringLiteral("maskReferenceHeight"));
        const QJsonValue originXValue = object.value(QStringLiteral("maskReferenceOriginX"));
        const QJsonValue originYValue = object.value(QStringLiteral("maskReferenceOriginY"));
        const QJsonValue maskEnabledValue = object.value(QStringLiteral("maskEnabled"));
        const QJsonValue maskInvertedValue = object.value(QStringLiteral("maskInverted"));
        bool maskOk = false;
        filter.maskImage = maskValue.isString()
            ? decodeImage(maskValue.toString(), &maskOk) : QImage();
        valid = valid && maskValue.isString() && maskOk && !filter.maskImage.isNull()
            && widthValue.isDouble() && heightValue.isDouble()
            && originXValue.isDouble() && originYValue.isDouble()
            && maskEnabledValue.isBool() && maskInvertedValue.isBool();
        if (valid) {
            filter.maskReferenceSize = QSize(widthValue.toInt(), heightValue.toInt());
            filter.maskReferenceOrigin = QPointF(originXValue.toDouble(), originYValue.toDouble());
            filter.maskEnabled = maskEnabledValue.toBool(true);
            filter.maskInverted = maskInvertedValue.toBool(false);
        }
    } else if (schemaValue >= 2) {
        for (const QString &field : maskFields) {
            if (field == QStringLiteral("maskImage")) continue;
            valid = valid && !object.contains(field);
        }
    }
    if (!valid) return LiveFilter();
    filter.normalise();
    if (ok) *ok = filter.isSafe();
    return filter;
}

QSize liveFilterStackSpatialRadius2D(const QVector<LiveFilter> &filters)
{
    qint64 x = 0;
    qint64 y = 0;
    for (const LiveFilter &filter : filters) {
        if (!filter.enabled) continue;
        const QSize radius = adjustmentSpatialRadius2D(filter.adjustment);
        x += radius.width();
        y += radius.height();
        x = std::min<qint64>(x, std::numeric_limits<int>::max() / 4);
        y = std::min<qint64>(y, std::numeric_limits<int>::max() / 4);
    }
    return QSize(static_cast<int>(x), static_cast<int>(y));
}

int liveFilterStackSpatialRadius(const QVector<LiveFilter> &filters)
{
    const QSize radius = liveFilterStackSpatialRadius2D(filters);
    return std::max(radius.width(), radius.height());
}

qint64 liveFilterStackEstimatedBytes(const QVector<LiveFilter> &filters)
{
    qint64 total = static_cast<qint64>(filters.size()) * static_cast<qint64>(sizeof(LiveFilter));
    const auto add = [&](const qint64 bytes) {
        if (bytes <= 0) return;
        const qint64 maximum = std::numeric_limits<qint64>::max();
        total = total > maximum - bytes ? maximum : total + bytes;
    };
    for (const LiveFilter &filter : filters) {
        add(filter.maskImage.sizeInBytes());
        std::visit([&](const auto &parameters) {
            using Parameters = std::decay_t<decltype(parameters)>;
            if constexpr (std::is_same_v<Parameters, LutParameters>) {
                add(static_cast<qint64>(parameters.title.size() + parameters.sourceName.size())
                    * static_cast<qint64>(sizeof(QChar)));
                add(static_cast<qint64>(parameters.shaperData.capacity())
                    * static_cast<qint64>(sizeof(float)));
                add(static_cast<qint64>(parameters.cubeData.capacity())
                    * static_cast<qint64>(sizeof(float)));
            } else if constexpr (std::is_same_v<Parameters, CurvesParameters>) {
                for (const CurveChannelParameters &channel : parameters.channels) {
                    add(static_cast<qint64>(channel.points.capacity())
                        * static_cast<qint64>(sizeof(CurvePoint)));
                }
            } else if constexpr (std::is_same_v<Parameters, GradientMapParameters>) {
                add(static_cast<qint64>(parameters.stops.capacity())
                    * static_cast<qint64>(sizeof(GradientStop)));
            }
        }, filter.adjustment.parameters);
    }
    return total;
}

QString layerEffectTypeToString(const LayerEffectType type)
{
    switch (type) {
    case LayerEffectType::DropShadow: return QStringLiteral("drop-shadow");
    case LayerEffectType::InnerShadow: return QStringLiteral("inner-shadow");
    case LayerEffectType::OuterGlow: return QStringLiteral("outer-glow");
    case LayerEffectType::InnerGlow: return QStringLiteral("inner-glow");
    case LayerEffectType::Stroke: return QStringLiteral("stroke");
    case LayerEffectType::ColourOverlay: return QStringLiteral("colour-overlay");
    case LayerEffectType::GradientOverlay: return QStringLiteral("gradient-overlay");
    case LayerEffectType::BevelEmboss: return QStringLiteral("bevel-emboss");
    }
    return QStringLiteral("drop-shadow");
}

QString layerEffectTypeDisplayName(const LayerEffectType type)
{
    switch (type) {
    case LayerEffectType::DropShadow: return QStringLiteral("Drop Shadow");
    case LayerEffectType::InnerShadow: return QStringLiteral("Inner Shadow");
    case LayerEffectType::OuterGlow: return QStringLiteral("Outer Glow");
    case LayerEffectType::InnerGlow: return QStringLiteral("Inner Glow");
    case LayerEffectType::Stroke: return QStringLiteral("Stroke");
    case LayerEffectType::ColourOverlay: return QStringLiteral("Colour Overlay");
    case LayerEffectType::GradientOverlay: return QStringLiteral("Gradient Overlay");
    case LayerEffectType::BevelEmboss: return QStringLiteral("Bevel & Emboss");
    }
    return QStringLiteral("Layer Effect");
}

LayerEffectType layerEffectTypeFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("drop-shadow")) return LayerEffectType::DropShadow;
    if (normalised == QStringLiteral("inner-shadow")) return LayerEffectType::InnerShadow;
    if (normalised == QStringLiteral("outer-glow")) return LayerEffectType::OuterGlow;
    if (normalised == QStringLiteral("inner-glow")) return LayerEffectType::InnerGlow;
    if (normalised == QStringLiteral("stroke")) return LayerEffectType::Stroke;
    if (normalised == QStringLiteral("colour-overlay")
        || normalised == QStringLiteral("color-overlay")) return LayerEffectType::ColourOverlay;
    if (normalised == QStringLiteral("gradient-overlay")) return LayerEffectType::GradientOverlay;
    if (normalised == QStringLiteral("bevel-emboss")
        || normalised == QStringLiteral("bevel-and-emboss")) return LayerEffectType::BevelEmboss;
    if (ok) *ok = false;
    return LayerEffectType::DropShadow;
}

bool layerEffectTypeHasRenderer(const LayerEffectType type)
{
    switch (type) {
    case LayerEffectType::DropShadow:
    case LayerEffectType::InnerShadow:
    case LayerEffectType::OuterGlow:
    case LayerEffectType::InnerGlow:
    case LayerEffectType::Stroke:
    case LayerEffectType::ColourOverlay:
    case LayerEffectType::GradientOverlay:
    case LayerEffectType::BevelEmboss:
        return true;
    }
    return false;
}

QString layerEffectImplementationRevision(const LayerEffectType type)
{
    switch (type) {
    case LayerEffectType::DropShadow:
    case LayerEffectType::InnerShadow:
    case LayerEffectType::OuterGlow:
    case LayerEffectType::InnerGlow:
        return QStringLiteral("0.14.0i");
    case LayerEffectType::Stroke:
    case LayerEffectType::ColourOverlay:
    case LayerEffectType::GradientOverlay:
        return QStringLiteral("0.14.0j");
    case LayerEffectType::BevelEmboss:
        return QStringLiteral("0.14.0k");
    }
    return QStringLiteral("0.14.0i");
}

bool layerTypeSupportsLayerEffects(const LayerType type)
{
    switch (type) {
    case LayerType::BaseImage:
    case LayerType::Raster:
    case LayerType::Vector:
    case LayerType::Text:
    case LayerType::Smart:
        return true;
    case LayerType::Adjustment:
    case LayerType::Group:
        return false;
    }
    return false;
}

namespace {

QString layerEffectStrokePositionToString(const LayerEffectStrokePosition position)
{
    switch (position) {
    case LayerEffectStrokePosition::Inside: return QStringLiteral("inside");
    case LayerEffectStrokePosition::Centre: return QStringLiteral("centre");
    case LayerEffectStrokePosition::Outside: return QStringLiteral("outside");
    }
    return QStringLiteral("outside");
}

LayerEffectStrokePosition layerEffectStrokePositionFromString(const QString &value,
                                                              bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("inside")) return LayerEffectStrokePosition::Inside;
    if (normalised == QStringLiteral("centre")
        || normalised == QStringLiteral("center")) return LayerEffectStrokePosition::Centre;
    if (normalised == QStringLiteral("outside")) return LayerEffectStrokePosition::Outside;
    if (ok) *ok = false;
    return LayerEffectStrokePosition::Outside;
}

QString layerEffectGradientStyleToString(const LayerEffectGradientStyle style)
{
    switch (style) {
    case LayerEffectGradientStyle::Linear: return QStringLiteral("linear");
    case LayerEffectGradientStyle::Radial: return QStringLiteral("radial");
    }
    return QStringLiteral("linear");
}

LayerEffectGradientStyle layerEffectGradientStyleFromString(const QString &value,
                                                            bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("linear")) return LayerEffectGradientStyle::Linear;
    if (normalised == QStringLiteral("radial")) return LayerEffectGradientStyle::Radial;
    if (ok) *ok = false;
    return LayerEffectGradientStyle::Linear;
}

QString layerEffectBevelStyleToString(const LayerEffectBevelStyle style)
{
    switch (style) {
    case LayerEffectBevelStyle::InnerBevel: return QStringLiteral("inner-bevel");
    case LayerEffectBevelStyle::OuterBevel: return QStringLiteral("outer-bevel");
    case LayerEffectBevelStyle::Emboss: return QStringLiteral("emboss");
    case LayerEffectBevelStyle::PillowEmboss: return QStringLiteral("pillow-emboss");
    }
    return QStringLiteral("inner-bevel");
}

LayerEffectBevelStyle layerEffectBevelStyleFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("inner-bevel")) return LayerEffectBevelStyle::InnerBevel;
    if (normalised == QStringLiteral("outer-bevel")) return LayerEffectBevelStyle::OuterBevel;
    if (normalised == QStringLiteral("emboss")) return LayerEffectBevelStyle::Emboss;
    if (normalised == QStringLiteral("pillow-emboss")) return LayerEffectBevelStyle::PillowEmboss;
    if (ok) *ok = false;
    return LayerEffectBevelStyle::InnerBevel;
}

QString layerEffectBevelDirectionToString(const LayerEffectBevelDirection direction)
{
    switch (direction) {
    case LayerEffectBevelDirection::Up: return QStringLiteral("up");
    case LayerEffectBevelDirection::Down: return QStringLiteral("down");
    }
    return QStringLiteral("up");
}

LayerEffectBevelDirection layerEffectBevelDirectionFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("up")) return LayerEffectBevelDirection::Up;
    if (normalised == QStringLiteral("down")) return LayerEffectBevelDirection::Down;
    if (ok) *ok = false;
    return LayerEffectBevelDirection::Up;
}

void normaliseLayerEffectGradientStops(QVector<GradientStop> *stops)
{
    if (!stops) return;
    GradientMapParameters parameters;
    parameters.stops = *stops;
    parameters.normalise();
    *stops = std::move(parameters.stops);
}

bool layerEffectGradientStopsAreSafe(const QVector<GradientStop> &stops)
{
    if (stops.size() < 2 || stops.size() > 64
        || std::abs(stops.constFirst().position) > 1.0e-9
        || std::abs(stops.constLast().position - 1.0) > 1.0e-9) return false;
    double previous = -1.0;
    for (const GradientStop &stop : stops) {
        if (!std::isfinite(stop.position) || stop.position < 0.0 || stop.position > 1.0
            || !stop.colour.isValid() || stop.colour.alpha() != 255
            || stop.position <= previous) {
            return false;
        }
        previous = stop.position;
    }
    return true;
}

bool layerEffectBlendModeIsSafe(const BlendMode mode)
{
    switch (mode) {
    case BlendMode::Copy:
    case BlendMode::Multiply:
    case BlendMode::Screen:
    case BlendMode::Overlay:
    case BlendMode::Darken:
    case BlendMode::Lighten:
    case BlendMode::ColourDodge:
    case BlendMode::ColourBurn:
    case BlendMode::Add:
    case BlendMode::Subtract:
    case BlendMode::Difference:
    case BlendMode::Exclusion:
        return true;
    }
    return false;
}

void applyLayerEffectTypeDefaults(LayerEffect *effect)
{
    if (!effect) return;
    effect->effectOpacity = 0.75;
    effect->angleDegrees = 135.0;
    effect->spread = 0.0;
    switch (effect->type) {
    case LayerEffectType::DropShadow:
        effect->colour = QColor(0, 0, 0);
        effect->effectBlendMode = BlendMode::Multiply;
        effect->distance = 10.0;
        effect->size = 10.0;
        break;
    case LayerEffectType::InnerShadow:
        effect->colour = QColor(0, 0, 0);
        effect->effectBlendMode = BlendMode::Multiply;
        effect->distance = 5.0;
        effect->size = 5.0;
        break;
    case LayerEffectType::OuterGlow:
        effect->colour = QColor(255, 246, 190);
        effect->effectBlendMode = BlendMode::Screen;
        effect->distance = 0.0;
        effect->size = 10.0;
        break;
    case LayerEffectType::InnerGlow:
        effect->colour = QColor(255, 246, 190);
        effect->effectBlendMode = BlendMode::Screen;
        effect->distance = 0.0;
        effect->size = 10.0;
        break;
    case LayerEffectType::Stroke:
        effect->colour = QColor(0, 0, 0);
        effect->effectOpacity = 1.0;
        effect->effectBlendMode = BlendMode::Copy;
        effect->distance = 0.0;
        effect->size = 3.0;
        effect->strokePosition = LayerEffectStrokePosition::Outside;
        break;
    case LayerEffectType::ColourOverlay:
        effect->colour = QColor(255, 0, 0);
        effect->effectOpacity = 1.0;
        effect->effectBlendMode = BlendMode::Copy;
        effect->distance = 0.0;
        effect->size = 0.0;
        break;
    case LayerEffectType::GradientOverlay:
        effect->colour = QColor(0, 0, 0);
        effect->effectOpacity = 1.0;
        effect->effectBlendMode = BlendMode::Copy;
        effect->distance = 0.0;
        effect->size = 0.0;
        effect->gradientStops = {{0.0, Qt::black}, {1.0, Qt::white}};
        effect->gradientInterpolation = GradientInterpolation::Linear;
        effect->gradientStyle = LayerEffectGradientStyle::Linear;
        effect->gradientAngleDegrees = 90.0;
        effect->gradientScale = 100.0;
        effect->gradientReverse = false;
        break;
    case LayerEffectType::BevelEmboss:
        effect->colour = QColor(0, 0, 0);
        effect->effectOpacity = 1.0;
        effect->effectBlendMode = BlendMode::Copy;
        effect->distance = 0.0;
        effect->spread = 0.0;
        effect->size = 8.0;
        effect->angleDegrees = 135.0;
        effect->bevelStyle = LayerEffectBevelStyle::InnerBevel;
        effect->bevelDirection = LayerEffectBevelDirection::Up;
        effect->bevelDepth = 100.0;
        effect->bevelSoften = 0.0;
        effect->bevelAltitudeDegrees = 30.0;
        effect->bevelHighlightColour = QColor(255, 255, 255);
        effect->bevelHighlightBlendMode = BlendMode::Screen;
        effect->bevelHighlightOpacity = 0.75;
        effect->bevelShadowColour = QColor(0, 0, 0);
        effect->bevelShadowBlendMode = BlendMode::Multiply;
        effect->bevelShadowOpacity = 0.75;
        break;
    }
}

} // namespace

void LayerEffect::normalise()
{
    schema = CurrentSchema;
    if (id.isNull()) id = QUuid::createUuid();
    revision = std::max<quint64>(1, revision);
    if (!colour.isValid()) colour = QColor(0, 0, 0);
    colour.setAlpha(255);
    effectOpacity = std::clamp(std::isfinite(effectOpacity) ? effectOpacity : 0.75,
                               0.0, 1.0);
    angleDegrees = std::isfinite(angleDegrees) ? angleDegrees : 135.0;
    angleDegrees = std::remainder(angleDegrees, 360.0);
    distance = std::clamp(std::isfinite(distance) ? distance : 0.0, 0.0, 4096.0);
    spread = std::clamp(std::isfinite(spread) ? spread : 0.0, 0.0, 100.0);
    size = std::clamp(std::isfinite(size) ? size : 0.0, 0.0, 4096.0);
    if (type == LayerEffectType::BevelEmboss) size = std::max(1.0, size);
    switch (strokePosition) {
    case LayerEffectStrokePosition::Inside:
    case LayerEffectStrokePosition::Centre:
    case LayerEffectStrokePosition::Outside:
        break;
    default:
        strokePosition = LayerEffectStrokePosition::Outside;
        break;
    }
    normaliseLayerEffectGradientStops(&gradientStops);
    if (gradientInterpolation != GradientInterpolation::Linear
        && gradientInterpolation != GradientInterpolation::Smooth
        && gradientInterpolation != GradientInterpolation::Constant) {
        gradientInterpolation = GradientInterpolation::Linear;
    }
    if (gradientStyle != LayerEffectGradientStyle::Linear
        && gradientStyle != LayerEffectGradientStyle::Radial) {
        gradientStyle = LayerEffectGradientStyle::Linear;
    }
    gradientAngleDegrees = std::isfinite(gradientAngleDegrees)
        ? std::remainder(gradientAngleDegrees, 360.0) : 90.0;
    gradientScale = std::clamp(std::isfinite(gradientScale) ? gradientScale : 100.0,
                               10.0, 1000.0);
    switch (bevelStyle) {
    case LayerEffectBevelStyle::InnerBevel:
    case LayerEffectBevelStyle::OuterBevel:
    case LayerEffectBevelStyle::Emboss:
    case LayerEffectBevelStyle::PillowEmboss:
        break;
    default:
        bevelStyle = LayerEffectBevelStyle::InnerBevel;
        break;
    }
    if (bevelDirection != LayerEffectBevelDirection::Up
        && bevelDirection != LayerEffectBevelDirection::Down) {
        bevelDirection = LayerEffectBevelDirection::Up;
    }
    bevelDepth = std::clamp(std::isfinite(bevelDepth) ? bevelDepth : 100.0, 1.0, 1000.0);
    bevelSoften = std::clamp(std::isfinite(bevelSoften) ? bevelSoften : 0.0, 0.0, 250.0);
    bevelAltitudeDegrees = std::clamp(
        std::isfinite(bevelAltitudeDegrees) ? bevelAltitudeDegrees : 30.0, 0.0, 90.0);
    if (!bevelHighlightColour.isValid()) bevelHighlightColour = QColor(255, 255, 255);
    bevelHighlightColour.setAlpha(255);
    if (!bevelShadowColour.isValid()) bevelShadowColour = QColor(0, 0, 0);
    bevelShadowColour.setAlpha(255);
    if (!layerEffectBlendModeIsSafe(bevelHighlightBlendMode))
        bevelHighlightBlendMode = BlendMode::Screen;
    if (!layerEffectBlendModeIsSafe(bevelShadowBlendMode))
        bevelShadowBlendMode = BlendMode::Multiply;
    bevelHighlightOpacity = std::clamp(
        std::isfinite(bevelHighlightOpacity) ? bevelHighlightOpacity : 0.75, 0.0, 1.0);
    bevelShadowOpacity = std::clamp(
        std::isfinite(bevelShadowOpacity) ? bevelShadowOpacity : 0.75, 0.0, 1.0);
    if (!layerEffectBlendModeIsSafe(effectBlendMode)) effectBlendMode = BlendMode::Copy;
    if (!layerEffectTypeHasRenderer(type)) enabled = false;
}

bool LayerEffect::isSafe() const
{
    switch (type) {
    case LayerEffectType::DropShadow:
    case LayerEffectType::InnerShadow:
    case LayerEffectType::OuterGlow:
    case LayerEffectType::InnerGlow:
    case LayerEffectType::Stroke:
    case LayerEffectType::ColourOverlay:
    case LayerEffectType::GradientOverlay:
    case LayerEffectType::BevelEmboss:
        break;
    default:
        return false;
    }
    return schema == CurrentSchema && !id.isNull() && revision >= 1
        && colour.isValid() && colour.alpha() == 255
        && std::isfinite(effectOpacity) && effectOpacity >= 0.0 && effectOpacity <= 1.0
        && layerEffectBlendModeIsSafe(effectBlendMode)
        && std::isfinite(angleDegrees) && std::abs(angleDegrees) <= 360.0
        && std::isfinite(distance) && distance >= 0.0 && distance <= 4096.0
        && std::isfinite(spread) && spread >= 0.0 && spread <= 100.0
        && std::isfinite(size)
        && size >= (type == LayerEffectType::BevelEmboss ? 1.0 : 0.0)
        && size <= 4096.0
        && (strokePosition == LayerEffectStrokePosition::Inside
            || strokePosition == LayerEffectStrokePosition::Centre
            || strokePosition == LayerEffectStrokePosition::Outside)
        && layerEffectGradientStopsAreSafe(gradientStops)
        && (gradientInterpolation == GradientInterpolation::Linear
            || gradientInterpolation == GradientInterpolation::Smooth
            || gradientInterpolation == GradientInterpolation::Constant)
        && (gradientStyle == LayerEffectGradientStyle::Linear
            || gradientStyle == LayerEffectGradientStyle::Radial)
        && std::isfinite(gradientAngleDegrees) && std::abs(gradientAngleDegrees) <= 360.0
        && std::isfinite(gradientScale) && gradientScale >= 10.0 && gradientScale <= 1000.0
        && (bevelStyle == LayerEffectBevelStyle::InnerBevel
            || bevelStyle == LayerEffectBevelStyle::OuterBevel
            || bevelStyle == LayerEffectBevelStyle::Emboss
            || bevelStyle == LayerEffectBevelStyle::PillowEmboss)
        && (bevelDirection == LayerEffectBevelDirection::Up
            || bevelDirection == LayerEffectBevelDirection::Down)
        && std::isfinite(bevelDepth) && bevelDepth >= 1.0 && bevelDepth <= 1000.0
        && std::isfinite(bevelSoften) && bevelSoften >= 0.0 && bevelSoften <= 250.0
        && std::isfinite(bevelAltitudeDegrees) && bevelAltitudeDegrees >= 0.0
        && bevelAltitudeDegrees <= 90.0
        && bevelHighlightColour.isValid() && bevelHighlightColour.alpha() == 255
        && layerEffectBlendModeIsSafe(bevelHighlightBlendMode)
        && std::isfinite(bevelHighlightOpacity) && bevelHighlightOpacity >= 0.0
        && bevelHighlightOpacity <= 1.0
        && bevelShadowColour.isValid() && bevelShadowColour.alpha() == 255
        && layerEffectBlendModeIsSafe(bevelShadowBlendMode)
        && std::isfinite(bevelShadowOpacity) && bevelShadowOpacity >= 0.0
        && bevelShadowOpacity <= 1.0
        && (!enabled || layerEffectTypeHasRenderer(type));
}

QJsonObject LayerEffect::toJson(bool *ok) const
{
    if (ok) *ok = false;
    if (!isSafe()) return {};
    QJsonObject object;
    object.insert(QStringLiteral("schema"), static_cast<int>(CurrentSchema));
    object.insert(QStringLiteral("id"), id.toString(QUuid::WithoutBraces));
    object.insert(QStringLiteral("type"), layerEffectTypeToString(type));
    object.insert(QStringLiteral("enabled"), enabled);
    object.insert(QStringLiteral("revision"), QString::number(revision));
    object.insert(QStringLiteral("colour"), colour.name(QColor::HexArgb));
    object.insert(QStringLiteral("opacity"), effectOpacity);
    object.insert(QStringLiteral("blendMode"), blendModeToString(effectBlendMode));
    object.insert(QStringLiteral("angle"), angleDegrees);
    object.insert(QStringLiteral("distance"), distance);
    object.insert(QStringLiteral("spread"), spread);
    object.insert(QStringLiteral("size"), size);
    object.insert(QStringLiteral("strokePosition"),
                  layerEffectStrokePositionToString(strokePosition));
    QJsonArray gradientArray;
    for (const GradientStop &stop : gradientStops) {
        gradientArray.append(gradientStopToJson(stop));
    }
    object.insert(QStringLiteral("gradientStops"), gradientArray);
    object.insert(QStringLiteral("gradientInterpolation"),
                  gradientInterpolationToString(gradientInterpolation));
    object.insert(QStringLiteral("gradientStyle"),
                  layerEffectGradientStyleToString(gradientStyle));
    object.insert(QStringLiteral("gradientAngle"), gradientAngleDegrees);
    object.insert(QStringLiteral("gradientScale"), gradientScale);
    object.insert(QStringLiteral("gradientReverse"), gradientReverse);
    object.insert(QStringLiteral("bevelStyle"), layerEffectBevelStyleToString(bevelStyle));
    object.insert(QStringLiteral("bevelDirection"),
                  layerEffectBevelDirectionToString(bevelDirection));
    object.insert(QStringLiteral("bevelDepth"), bevelDepth);
    object.insert(QStringLiteral("bevelSoften"), bevelSoften);
    object.insert(QStringLiteral("bevelAltitude"), bevelAltitudeDegrees);
    object.insert(QStringLiteral("bevelHighlightColour"),
                  bevelHighlightColour.name(QColor::HexArgb));
    object.insert(QStringLiteral("bevelHighlightBlendMode"),
                  blendModeToString(bevelHighlightBlendMode));
    object.insert(QStringLiteral("bevelHighlightOpacity"), bevelHighlightOpacity);
    object.insert(QStringLiteral("bevelShadowColour"),
                  bevelShadowColour.name(QColor::HexArgb));
    object.insert(QStringLiteral("bevelShadowBlendMode"),
                  blendModeToString(bevelShadowBlendMode));
    object.insert(QStringLiteral("bevelShadowOpacity"), bevelShadowOpacity);
    if (ok) *ok = true;
    return object;
}

LayerEffect LayerEffect::fromJson(const QJsonObject &object, bool *ok)
{
    if (ok) *ok = false;
    LayerEffect effect;
    const int schemaValue = object.value(QStringLiteral("schema")).toInt(-1);
    const QUuid id(object.value(QStringLiteral("id")).toString());
    const QJsonValue typeValue = object.value(QStringLiteral("type"));
    const QJsonValue enabledValue = object.value(QStringLiteral("enabled"));
    const QJsonValue revisionValue = object.value(QStringLiteral("revision"));
    bool typeOk = false;
    const LayerEffectType type = layerEffectTypeFromString(typeValue.toString(), &typeOk);
    bool revisionOk = false;
    const quint64 revision = revisionValue.toString().toULongLong(&revisionOk);
    if ((schemaValue < 1 || schemaValue > static_cast<int>(CurrentSchema))
        || id.isNull() || !typeValue.isString() || !typeOk || !enabledValue.isBool()
        || !revisionValue.isString() || !revisionOk || revision < 1) {
        return effect;
    }
    effect.schema = CurrentSchema;
    effect.id = id;
    effect.type = type;
    effect.enabled = enabledValue.toBool(false);
    effect.revision = revision;
    applyLayerEffectTypeDefaults(&effect);

    if (schemaValue >= 2) {
        const QJsonValue colourValue = object.value(QStringLiteral("colour"));
        const QJsonValue opacityValue = object.value(QStringLiteral("opacity"));
        const QJsonValue blendValue = object.value(QStringLiteral("blendMode"));
        const QJsonValue angleValue = object.value(QStringLiteral("angle"));
        const QJsonValue distanceValue = object.value(QStringLiteral("distance"));
        const QJsonValue spreadValue = object.value(QStringLiteral("spread"));
        const QJsonValue sizeValue = object.value(QStringLiteral("size"));
        bool blendOk = false;
        const BlendMode blend = blendModeFromString(blendValue.toString(), &blendOk);
        const QColor colour(colourValue.toString());
        if (!colourValue.isString() || !colour.isValid() || !opacityValue.isDouble()
            || !blendValue.isString() || !blendOk || !angleValue.isDouble()
            || !distanceValue.isDouble() || !spreadValue.isDouble() || !sizeValue.isDouble()) {
            return LayerEffect();
        }
        effect.colour = colour;
        effect.colour.setAlpha(255);
        effect.effectOpacity = opacityValue.toDouble();
        effect.effectBlendMode = blend;
        effect.angleDegrees = angleValue.toDouble();
        effect.distance = distanceValue.toDouble();
        effect.spread = spreadValue.toDouble();
        effect.size = sizeValue.toDouble();
    } else {
        // Schema-1 0.14.0h entries were definition-only and always disabled.
        // They gain deterministic type defaults when migrated but remain disabled.
        effect.enabled = false;
    }
    // 0.14.0i serialised future Stroke/Overlay definitions through schema 2,
    // but their generic colour/size fields were placeholders rather than
    // authored renderer state. On migration to the first real renderer schema,
    // give those disabled definitions the deterministic 0.14.0j defaults.
    if (schemaValue < 3
        && (effect.type == LayerEffectType::Stroke
            || effect.type == LayerEffectType::ColourOverlay
            || effect.type == LayerEffectType::GradientOverlay)) {
        const bool wasEnabled = effect.enabled;
        applyLayerEffectTypeDefaults(&effect);
        effect.enabled = wasEnabled && layerEffectTypeHasRenderer(effect.type);
    }
    if (schemaValue >= 3) {
        const QJsonValue strokePositionValue = object.value(QStringLiteral("strokePosition"));
        const QJsonValue gradientStopsValue = object.value(QStringLiteral("gradientStops"));
        const QJsonValue gradientInterpolationValue = object.value(
            QStringLiteral("gradientInterpolation"));
        const QJsonValue gradientStyleValue = object.value(QStringLiteral("gradientStyle"));
        const QJsonValue gradientAngleValue = object.value(QStringLiteral("gradientAngle"));
        const QJsonValue gradientScaleValue = object.value(QStringLiteral("gradientScale"));
        const QJsonValue gradientReverseValue = object.value(QStringLiteral("gradientReverse"));
        bool strokePositionOk = false;
        bool gradientInterpolationOk = false;
        bool gradientStyleOk = false;
        const LayerEffectStrokePosition strokePosition = layerEffectStrokePositionFromString(
            strokePositionValue.toString(), &strokePositionOk);
        const GradientInterpolation gradientInterpolation = gradientInterpolationFromString(
            gradientInterpolationValue.toString(), &gradientInterpolationOk);
        const LayerEffectGradientStyle gradientStyle = layerEffectGradientStyleFromString(
            gradientStyleValue.toString(), &gradientStyleOk);
        if (!strokePositionValue.isString() || !strokePositionOk
            || !gradientStopsValue.isArray() || !gradientInterpolationValue.isString()
            || !gradientInterpolationOk || !gradientStyleValue.isString() || !gradientStyleOk
            || !gradientAngleValue.isDouble() || !gradientScaleValue.isDouble()
            || !gradientReverseValue.isBool()) {
            return LayerEffect();
        }
        QVector<GradientStop> stops;
        const QJsonArray array = gradientStopsValue.toArray();
        if (array.size() < 2 || array.size() > 64) return LayerEffect();
        stops.reserve(array.size());
        for (const QJsonValue &value : array) {
            if (!value.isObject()) return LayerEffect();
            bool stopOk = false;
            GradientStop stop = gradientStopFromJson(value.toObject(), &stopOk);
            if (!stopOk) return LayerEffect();
            stops.push_back(std::move(stop));
        }
        effect.strokePosition = strokePosition;
        effect.gradientStops = std::move(stops);
        effect.gradientInterpolation = gradientInterpolation;
        effect.gradientStyle = gradientStyle;
        effect.gradientAngleDegrees = gradientAngleValue.toDouble();
        effect.gradientScale = gradientScaleValue.toDouble();
        effect.gradientReverse = gradientReverseValue.toBool(false);
    }
    // Schema-3 Bevel & Emboss was a disabled definition-only placeholder.
    // Schema 4 gives it deterministic production defaults while preserving
    // disabled appearance on migration.
    if (schemaValue < 4 && effect.type == LayerEffectType::BevelEmboss) {
        applyLayerEffectTypeDefaults(&effect);
        effect.enabled = false;
    }
    if (schemaValue >= 4) {
        const QJsonValue styleValue = object.value(QStringLiteral("bevelStyle"));
        const QJsonValue directionValue = object.value(QStringLiteral("bevelDirection"));
        const QJsonValue depthValue = object.value(QStringLiteral("bevelDepth"));
        const QJsonValue softenValue = object.value(QStringLiteral("bevelSoften"));
        const QJsonValue altitudeValue = object.value(QStringLiteral("bevelAltitude"));
        const QJsonValue highlightColourValue = object.value(QStringLiteral("bevelHighlightColour"));
        const QJsonValue highlightBlendValue = object.value(QStringLiteral("bevelHighlightBlendMode"));
        const QJsonValue highlightOpacityValue = object.value(QStringLiteral("bevelHighlightOpacity"));
        const QJsonValue shadowColourValue = object.value(QStringLiteral("bevelShadowColour"));
        const QJsonValue shadowBlendValue = object.value(QStringLiteral("bevelShadowBlendMode"));
        const QJsonValue shadowOpacityValue = object.value(QStringLiteral("bevelShadowOpacity"));
        bool styleOk = false, directionOk = false, highlightBlendOk = false, shadowBlendOk = false;
        const auto style = layerEffectBevelStyleFromString(styleValue.toString(), &styleOk);
        const auto direction = layerEffectBevelDirectionFromString(directionValue.toString(), &directionOk);
        const BlendMode highlightBlend = blendModeFromString(highlightBlendValue.toString(), &highlightBlendOk);
        const BlendMode shadowBlend = blendModeFromString(shadowBlendValue.toString(), &shadowBlendOk);
        const QColor highlightColour(highlightColourValue.toString());
        const QColor shadowColour(shadowColourValue.toString());
        if (!styleValue.isString() || !styleOk || !directionValue.isString() || !directionOk
            || !depthValue.isDouble() || !softenValue.isDouble() || !altitudeValue.isDouble()
            || !highlightColourValue.isString() || !highlightColour.isValid()
            || !highlightBlendValue.isString() || !highlightBlendOk
            || !highlightOpacityValue.isDouble() || !shadowColourValue.isString()
            || !shadowColour.isValid() || !shadowBlendValue.isString() || !shadowBlendOk
            || !shadowOpacityValue.isDouble()) return LayerEffect();
        effect.bevelStyle = style;
        effect.bevelDirection = direction;
        effect.bevelDepth = depthValue.toDouble();
        effect.bevelSoften = softenValue.toDouble();
        effect.bevelAltitudeDegrees = altitudeValue.toDouble();
        effect.bevelHighlightColour = highlightColour;
        effect.bevelHighlightColour.setAlpha(255);
        effect.bevelHighlightBlendMode = highlightBlend;
        effect.bevelHighlightOpacity = highlightOpacityValue.toDouble();
        effect.bevelShadowColour = shadowColour;
        effect.bevelShadowColour.setAlpha(255);
        effect.bevelShadowBlendMode = shadowBlend;
        effect.bevelShadowOpacity = shadowOpacityValue.toDouble();
    }
    effect.normalise();
    if (!effect.isSafe()) return LayerEffect();
    if (ok) *ok = true;
    return effect;
}

qint64 layerEffectStackEstimatedBytes(const QVector<LayerEffect> &effects)
{
    qint64 bytes = static_cast<qint64>(effects.capacity())
        * static_cast<qint64>(sizeof(LayerEffect));
    for (const LayerEffect &effect : effects) {
        bytes += static_cast<qint64>(effect.gradientStops.capacity())
            * static_cast<qint64>(sizeof(GradientStop));
    }
    return bytes;
}

QByteArray layerEffectStackRenderIdentity(const QVector<LayerEffect> &effects)
{
    QByteArray semantic;
    QDataStream stream(&semantic, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << QByteArrayLiteral("layer-effect-stack-render-v4");
    bool hasRenderable = false;
    for (const LayerEffect &effect : effects) {
        if (!effect.enabled || !layerEffectTypeHasRenderer(effect.type)) continue;
        hasRenderable = true;
        // Render identity deliberately excludes UUID/revision. Undo branches or
        // metadata-only edits that return to identical authored parameters can
        // therefore reuse the same generated fx tile safely. Authored ordering
        // is retained by serialising enabled effects in vector order.
        stream << static_cast<qint32>(effect.type)
               << static_cast<quint32>(effect.colour.rgba())
               << effect.effectOpacity
               << static_cast<qint32>(effect.effectBlendMode)
               << effect.angleDegrees << effect.distance
               << effect.spread << effect.size;
        if (effect.type == LayerEffectType::Stroke) {
            stream << static_cast<qint32>(effect.strokePosition);
        } else if (effect.type == LayerEffectType::GradientOverlay) {
            stream << static_cast<qint32>(effect.gradientStyle)
                   << static_cast<qint32>(effect.gradientInterpolation)
                   << effect.gradientAngleDegrees << effect.gradientScale
                   << effect.gradientReverse
                   << static_cast<qint32>(effect.gradientStops.size());
            for (const GradientStop &stop : effect.gradientStops) {
                stream << stop.position << static_cast<quint32>(stop.colour.rgba());
            }
        } else if (effect.type == LayerEffectType::BevelEmboss) {
            stream << static_cast<qint32>(effect.bevelStyle)
                   << static_cast<qint32>(effect.bevelDirection)
                   << effect.bevelDepth << effect.bevelSoften << effect.bevelAltitudeDegrees
                   << static_cast<quint32>(effect.bevelHighlightColour.rgba())
                   << static_cast<qint32>(effect.bevelHighlightBlendMode)
                   << effect.bevelHighlightOpacity
                   << static_cast<quint32>(effect.bevelShadowColour.rgba())
                   << static_cast<qint32>(effect.bevelShadowBlendMode)
                   << effect.bevelShadowOpacity;
        }
    }
    // Disabled definitions and future renderer placeholders are authored state,
    // but they do not change pixels and must not evict tiled render caches.
    if (!hasRenderable) return {};
    return QCryptographicHash::hash(semantic, QCryptographicHash::Sha256);
}

QSize layerEffectStackSpatialRadius2D(const QVector<LayerEffect> &effects)
{
    qint64 radiusX = 0;
    qint64 radiusY = 0;
    for (const LayerEffect &effect : effects) {
        if (!effect.enabled || !layerEffectTypeHasRenderer(effect.type)) continue;
        if (effect.type == LayerEffectType::ColourOverlay
            || effect.type == LayerEffectType::GradientOverlay) {
            continue;
        }
        if (effect.type == LayerEffectType::Stroke) {
            const int strokeRadius = std::clamp(qCeil(effect.size), 0, 4096);
            radiusX = std::max<qint64>(radiusX, strokeRadius);
            radiusY = std::max<qint64>(radiusY, strokeRadius);
            continue;
        }
        if (effect.type == LayerEffectType::BevelEmboss) {
            const int bevelRadius = std::clamp(
                qCeil(effect.size + effect.bevelSoften)
                    + SpatialFilterContract::DefaultSafetyPadding, 0, 4096);
            radiusX = std::max<qint64>(radiusX, bevelRadius);
            radiusY = std::max<qint64>(radiusY, bevelRadius);
            continue;
        }
        const int spreadRadius = std::clamp(qCeil(effect.size * effect.spread / 100.0), 0, 4096);
        const int blurRadius = std::clamp(qCeil(effect.size), 0, 4096);
        int x = blurRadius + spreadRadius + SpatialFilterContract::DefaultSafetyPadding;
        int y = x;
        if (effect.type == LayerEffectType::DropShadow
            || effect.type == LayerEffectType::InnerShadow) {
            const double radians = effect.angleDegrees * std::numbers::pi / 180.0;
            x += qCeil(std::abs(std::cos(radians) * effect.distance));
            y += qCeil(std::abs(std::sin(radians) * effect.distance));
        }
        radiusX = std::max<qint64>(radiusX, x);
        radiusY = std::max<qint64>(radiusY, y);
    }
    return QSize(static_cast<int>(std::min<qint64>(radiusX, std::numeric_limits<int>::max() / 4)),
                 static_cast<int>(std::min<qint64>(radiusY, std::numeric_limits<int>::max() / 4)));
}

AdjustmentData LayerNode::effectiveAdjustmentData() const
{
    AdjustmentData result = adjustment;
    if (result.type != adjustmentType) result.reset(adjustmentType);
    switch (adjustmentType) {
    case AdjustmentType::Exposure: {
        ExposureParameters value = std::holds_alternative<ExposureParameters>(result.parameters)
            ? std::get<ExposureParameters>(result.parameters) : ExposureParameters {};
        value.exposure = exposure;
        result.parameters = value;
        break;
    }
    case AdjustmentType::Contrast: {
        ContrastParameters value = std::holds_alternative<ContrastParameters>(result.parameters)
            ? std::get<ContrastParameters>(result.parameters) : ContrastParameters {};
        value.contrast = contrast;
        result.parameters = value;
        break;
    }
    case AdjustmentType::Saturation: {
        SaturationParameters value = std::holds_alternative<SaturationParameters>(result.parameters)
            ? std::get<SaturationParameters>(result.parameters) : SaturationParameters {};
        value.saturation = saturation;
        result.parameters = value;
        break;
    }
    case AdjustmentType::Levels: {
        LevelsParameters levels = std::holds_alternative<LevelsParameters>(result.parameters)
            ? std::get<LevelsParameters>(result.parameters) : LevelsParameters {};
        LevelsChannelParameters &rgb = levels.channel(AdjustmentChannel::Rgb);
        rgb.inputBlack = blackPoint;
        rgb.inputWhite = whitePoint;
        rgb.gamma = gamma;
        result.parameters = levels;
        break;
    }
    case AdjustmentType::Curves:
        if (!std::holds_alternative<CurvesParameters>(result.parameters)) result.parameters = CurvesParameters {};
        break;
    case AdjustmentType::HueSaturation:
        if (!std::holds_alternative<HueSaturationParameters>(result.parameters)) result.parameters = HueSaturationParameters {};
        break;
    case AdjustmentType::Vibrance:
        if (!std::holds_alternative<VibranceParameters>(result.parameters)) result.parameters = VibranceParameters {};
        break;
    case AdjustmentType::WhiteBalance:
        if (!std::holds_alternative<WhiteBalanceParameters>(result.parameters)) result.parameters = WhiteBalanceParameters {};
        break;
    case AdjustmentType::ColourBalance:
        if (!std::holds_alternative<ColourBalanceParameters>(result.parameters)) result.parameters = ColourBalanceParameters {};
        break;
    case AdjustmentType::ChannelMixer:
        if (!std::holds_alternative<ChannelMixerParameters>(result.parameters)) result.parameters = ChannelMixerParameters {};
        break;
    case AdjustmentType::BlackAndWhite:
        if (!std::holds_alternative<BlackAndWhiteParameters>(result.parameters)) result.parameters = BlackAndWhiteParameters {};
        break;
    case AdjustmentType::GradientMap:
        if (!std::holds_alternative<GradientMapParameters>(result.parameters)) result.parameters = GradientMapParameters {};
        break;
    case AdjustmentType::Posterise:
        if (!std::holds_alternative<PosteriseParameters>(result.parameters)) result.parameters = PosteriseParameters {};
        break;
    case AdjustmentType::Threshold:
        if (!std::holds_alternative<ThresholdParameters>(result.parameters)) result.parameters = ThresholdParameters {};
        break;
    case AdjustmentType::Invert:
        if (!std::holds_alternative<InvertParameters>(result.parameters)) result.parameters = InvertParameters {};
        break;
    case AdjustmentType::PhotoFilter:
        if (!std::holds_alternative<PhotoFilterParameters>(result.parameters)) result.parameters = PhotoFilterParameters {};
        break;
    case AdjustmentType::SelectiveColour:
        if (!std::holds_alternative<SelectiveColourParameters>(result.parameters)) result.parameters = SelectiveColourParameters {};
        break;
    case AdjustmentType::Vignette:
        if (!std::holds_alternative<VignetteParameters>(result.parameters)) result.parameters = VignetteParameters {};
        break;
    case AdjustmentType::RgbSplit:
        if (!std::holds_alternative<RgbSplitParameters>(result.parameters)) result.parameters = RgbSplitParameters {};
        break;
    case AdjustmentType::ChromaticAberrationCorrection:
        if (!std::holds_alternative<ChromaticAberrationCorrectionParameters>(result.parameters)) {
            result.parameters = ChromaticAberrationCorrectionParameters {};
        }
        break;
    case AdjustmentType::SurfaceBlur:
        if (!std::holds_alternative<SurfaceBlurParameters>(result.parameters)) result.parameters = SurfaceBlurParameters {};
        break;
    case AdjustmentType::MotionBlur:
        if (!std::holds_alternative<MotionBlurParameters>(result.parameters)) result.parameters = MotionBlurParameters {};
        break;
    case AdjustmentType::RadialBlur:
        if (!std::holds_alternative<RadialBlurParameters>(result.parameters)) result.parameters = RadialBlurParameters {};
        break;
    case AdjustmentType::Lut:
        if (!std::holds_alternative<LutParameters>(result.parameters)) result.parameters = LutParameters {};
        break;
    case AdjustmentType::ShadowsHighlights:
        if (!std::holds_alternative<ShadowsHighlightsParameters>(result.parameters)) result.parameters = ShadowsHighlightsParameters {};
        break;
    case AdjustmentType::GaussianBlur:
        if (!std::holds_alternative<GaussianBlurParameters>(result.parameters)) result.parameters = GaussianBlurParameters {};
        break;
    case AdjustmentType::BoxBlur:
        if (!std::holds_alternative<BoxBlurParameters>(result.parameters)) result.parameters = BoxBlurParameters {};
        break;
    case AdjustmentType::UnsharpMask:
        if (!std::holds_alternative<UnsharpMaskParameters>(result.parameters)) result.parameters = UnsharpMaskParameters {};
        break;
    case AdjustmentType::HighPass:
        if (!std::holds_alternative<HighPassParameters>(result.parameters)) result.parameters = HighPassParameters {};
        break;
    }
    result.normalise();
    return result;
}

LevelsParameters LayerNode::effectiveLevelsParameters() const
{
    const AdjustmentData data = effectiveAdjustmentData();
    return data.type == AdjustmentType::Levels
        ? std::get<LevelsParameters>(data.parameters) : LevelsParameters {};
}

CurvesParameters LayerNode::effectiveCurvesParameters() const
{
    const AdjustmentData data = effectiveAdjustmentData();
    return data.type == AdjustmentType::Curves
        ? std::get<CurvesParameters>(data.parameters) : CurvesParameters {};
}

void LayerNode::setAdjustmentData(const AdjustmentData &data)
{
    adjustment = data;
    adjustment.normalise();
    adjustmentType = adjustment.type;
    switch (adjustmentType) {
    case AdjustmentType::Exposure:
        exposure = std::get<ExposureParameters>(adjustment.parameters).exposure;
        break;
    case AdjustmentType::Contrast:
        contrast = std::get<ContrastParameters>(adjustment.parameters).contrast;
        break;
    case AdjustmentType::Saturation:
        saturation = std::get<SaturationParameters>(adjustment.parameters).saturation;
        break;
    case AdjustmentType::Levels: {
        const auto &rgb = std::get<LevelsParameters>(adjustment.parameters).channel(AdjustmentChannel::Rgb);
        blackPoint = rgb.inputBlack;
        whitePoint = rgb.inputWhite;
        gamma = rgb.gamma;
        break;
    }
    case AdjustmentType::Curves:
    case AdjustmentType::HueSaturation:
    case AdjustmentType::Vibrance:
    case AdjustmentType::WhiteBalance:
    case AdjustmentType::ColourBalance:
    case AdjustmentType::ChannelMixer:
    case AdjustmentType::BlackAndWhite:
    case AdjustmentType::GradientMap:
    case AdjustmentType::Posterise:
    case AdjustmentType::Threshold:
    case AdjustmentType::Lut:
    case AdjustmentType::ShadowsHighlights:
    case AdjustmentType::GaussianBlur:
    case AdjustmentType::BoxBlur:
    case AdjustmentType::UnsharpMask:
    case AdjustmentType::HighPass:
    case AdjustmentType::Invert:
    case AdjustmentType::PhotoFilter:
    case AdjustmentType::SelectiveColour:
    case AdjustmentType::Vignette:
    case AdjustmentType::RgbSplit:
    case AdjustmentType::ChromaticAberrationCorrection:
    case AdjustmentType::SurfaceBlur:
    case AdjustmentType::MotionBlur:
    case AdjustmentType::RadialBlur:
        break;
    }
}

void LayerNode::resetAdjustmentParameters(const AdjustmentType type)
{
    AdjustmentData data;
    data.reset(type);
    setAdjustmentData(data);
}

void LayerNode::setExposure(const double value)
{
    AdjustmentData data = effectiveAdjustmentData();
    if (data.type != AdjustmentType::Exposure) data.reset(AdjustmentType::Exposure);
    std::get<ExposureParameters>(data.parameters).exposure = value;
    setAdjustmentData(data);
}

void LayerNode::setExposureParameters(const ExposureParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::Exposure);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setContrast(const double value)
{
    AdjustmentData data = effectiveAdjustmentData();
    if (data.type != AdjustmentType::Contrast) data.reset(AdjustmentType::Contrast);
    std::get<ContrastParameters>(data.parameters).contrast = value;
    setAdjustmentData(data);
}

void LayerNode::setContrastParameters(const ContrastParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::Contrast);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setSaturation(const double value)
{
    AdjustmentData data = effectiveAdjustmentData();
    if (data.type != AdjustmentType::Saturation) data.reset(AdjustmentType::Saturation);
    std::get<SaturationParameters>(data.parameters).saturation = value;
    setAdjustmentData(data);
}

void LayerNode::setSaturationParameters(const SaturationParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::Saturation);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setLevelsParameters(const LevelsParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::Levels);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setCurvesParameters(const CurvesParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::Curves);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setHueSaturationParameters(const HueSaturationParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::HueSaturation);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setVibranceParameters(const VibranceParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::Vibrance);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setWhiteBalanceParameters(const WhiteBalanceParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::WhiteBalance);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setColourBalanceParameters(const ColourBalanceParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::ColourBalance);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setChannelMixerParameters(const ChannelMixerParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::ChannelMixer);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setBlackAndWhiteParameters(const BlackAndWhiteParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::BlackAndWhite);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setGradientMapParameters(const GradientMapParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::GradientMap);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setPosteriseParameters(const PosteriseParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::Posterise);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setThresholdParameters(const ThresholdParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::Threshold);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setInvertParameters(const InvertParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::Invert);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setPhotoFilterParameters(const PhotoFilterParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::PhotoFilter);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setSelectiveColourParameters(const SelectiveColourParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::SelectiveColour);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setVignetteParameters(const VignetteParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::Vignette);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setRgbSplitParameters(const RgbSplitParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::RgbSplit);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setChromaticAberrationCorrectionParameters(
    const ChromaticAberrationCorrectionParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::ChromaticAberrationCorrection);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setSurfaceBlurParameters(const SurfaceBlurParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::SurfaceBlur);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setMotionBlurParameters(const MotionBlurParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::MotionBlur);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setRadialBlurParameters(const RadialBlurParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::RadialBlur);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setLutParameters(const LutParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::Lut);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setShadowsHighlightsParameters(const ShadowsHighlightsParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::ShadowsHighlights);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setGaussianBlurParameters(const GaussianBlurParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::GaussianBlur);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setBoxBlurParameters(const BoxBlurParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::BoxBlur);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setUnsharpMaskParameters(const UnsharpMaskParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::UnsharpMask);
    data.parameters = parameters;
    setAdjustmentData(data);
}

void LayerNode::setHighPassParameters(const HighPassParameters &parameters)
{
    AdjustmentData data;
    data.reset(AdjustmentType::HighPass);
    data.parameters = parameters;
    setAdjustmentData(data);
}

QString adjustmentTypeToString(const AdjustmentType type)
{
    switch (type) {
    case AdjustmentType::Exposure: return QStringLiteral("exposure");
    case AdjustmentType::Contrast: return QStringLiteral("contrast");
    case AdjustmentType::Saturation: return QStringLiteral("saturation");
    case AdjustmentType::Levels: return QStringLiteral("levels");
    case AdjustmentType::Curves: return QStringLiteral("curves");
    case AdjustmentType::HueSaturation: return QStringLiteral("hue-saturation");
    case AdjustmentType::Vibrance: return QStringLiteral("vibrance");
    case AdjustmentType::WhiteBalance: return QStringLiteral("white-balance");
    case AdjustmentType::ColourBalance: return QStringLiteral("colour-balance");
    case AdjustmentType::ChannelMixer: return QStringLiteral("channel-mixer");
    case AdjustmentType::BlackAndWhite: return QStringLiteral("black-and-white");
    case AdjustmentType::GradientMap: return QStringLiteral("gradient-map");
    case AdjustmentType::Posterise: return QStringLiteral("posterise");
    case AdjustmentType::Threshold: return QStringLiteral("threshold");
    case AdjustmentType::Lut: return QStringLiteral("lut");
    case AdjustmentType::ShadowsHighlights: return QStringLiteral("shadows-highlights");
    case AdjustmentType::GaussianBlur: return QStringLiteral("gaussian-blur");
    case AdjustmentType::BoxBlur: return QStringLiteral("box-blur");
    case AdjustmentType::UnsharpMask: return QStringLiteral("unsharp-mask");
    case AdjustmentType::HighPass: return QStringLiteral("high-pass");
    case AdjustmentType::Invert: return QStringLiteral("invert");
    case AdjustmentType::PhotoFilter: return QStringLiteral("photo-filter");
    case AdjustmentType::SelectiveColour: return QStringLiteral("selective-colour");
    case AdjustmentType::Vignette: return QStringLiteral("vignette");
    case AdjustmentType::RgbSplit: return QStringLiteral("rgb-split");
    case AdjustmentType::ChromaticAberrationCorrection:
        return QStringLiteral("chromatic-aberration-correction");
    case AdjustmentType::SurfaceBlur: return QStringLiteral("surface-blur");
    case AdjustmentType::MotionBlur: return QStringLiteral("motion-blur");
    case AdjustmentType::RadialBlur: return QStringLiteral("radial-blur");
    }
    return QStringLiteral("exposure");
}

AdjustmentProcessingDomain adjustmentProcessingDomain(const AdjustmentData &input)
{
    AdjustmentData data = input;
    data.normalise();
    switch (data.type) {
    case AdjustmentType::Exposure:
        // Exposure is a scene-referred scale and therefore belongs in the
        // current working primaries with a linear transfer function.
        return AdjustmentProcessingDomain::LinearWorking;
    case AdjustmentType::Contrast:
    case AdjustmentType::Saturation:
    case AdjustmentType::HueSaturation:
    case AdjustmentType::Vibrance:
    case AdjustmentType::WhiteBalance:
    case AdjustmentType::ColourBalance:
    case AdjustmentType::BlackAndWhite:
    case AdjustmentType::GradientMap:
    case AdjustmentType::PhotoFilter:
    case AdjustmentType::SelectiveColour:
    case AdjustmentType::Vignette:
    case AdjustmentType::ShadowsHighlights:
        // These established operators use sRGB/Rec.709 luminance, HSL or
        // Oklab contracts. Managed documents explicitly enter that domain and
        // return to the document working space afterwards.
        return AdjustmentProcessingDomain::EncodedSrgb;
    case AdjustmentType::Threshold: {
        const auto &parameters = std::get<ThresholdParameters>(data.parameters);
        return parameters.source == ThresholdSource::Luminance
            ? AdjustmentProcessingDomain::EncodedSrgb
            : AdjustmentProcessingDomain::RawComponents;
    }
    case AdjustmentType::ChannelMixer:
        return AdjustmentProcessingDomain::RawComponents;
    case AdjustmentType::Lut:
        return AdjustmentProcessingDomain::LutContract;
    case AdjustmentType::Levels:
    case AdjustmentType::Curves:
    case AdjustmentType::Posterise:
    case AdjustmentType::Invert:
    case AdjustmentType::GaussianBlur:
    case AdjustmentType::BoxBlur:
    case AdjustmentType::UnsharpMask:
    case AdjustmentType::HighPass:
    case AdjustmentType::RgbSplit:
    case AdjustmentType::ChromaticAberrationCorrection:
    case AdjustmentType::SurfaceBlur:
    case AdjustmentType::MotionBlur:
    case AdjustmentType::RadialBlur:
        return AdjustmentProcessingDomain::EncodedWorking;
    }
    return AdjustmentProcessingDomain::EncodedWorking;
}

QString adjustmentProcessingDomainName(const AdjustmentProcessingDomain domain)
{
    switch (domain) {
    case AdjustmentProcessingDomain::EncodedWorking:
        return QStringLiteral("Encoded working space");
    case AdjustmentProcessingDomain::LinearWorking:
        return QStringLiteral("Linear working space");
    case AdjustmentProcessingDomain::EncodedSrgb:
        return QStringLiteral("Encoded sRGB / Rec.709");
    case AdjustmentProcessingDomain::RawComponents:
        return QStringLiteral("Raw components");
    case AdjustmentProcessingDomain::LutContract:
        return QStringLiteral("LUT processing contract");
    }
    return QStringLiteral("Encoded working space");
}

bool adjustmentRequiresManagedDomainTransform(const AdjustmentData &data)
{
    const AdjustmentProcessingDomain domain = adjustmentProcessingDomain(data);
    return domain == AdjustmentProcessingDomain::LinearWorking
        || domain == AdjustmentProcessingDomain::EncodedSrgb;
}

bool layerTreeRequiresManagedDomainTransform(const QVector<LayerNode> &layers)
{
    for (const LayerNode &layer : layers) {
        if (!layer.visible || layer.opacity <= 0.0) continue;
        if (layer.type == LayerType::Adjustment
            && adjustmentRequiresManagedDomainTransform(layer.effectiveAdjustmentData())) {
            return true;
        }
        if (layer.type == LayerType::Group
            && layerTreeRequiresManagedDomainTransform(layer.children)) {
            return true;
        }
    }
    return false;
}

AdjustmentType adjustmentTypeFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("exposure")) return AdjustmentType::Exposure;
    if (normalised == QStringLiteral("contrast")) return AdjustmentType::Contrast;
    if (normalised == QStringLiteral("saturation")) return AdjustmentType::Saturation;
    if (normalised == QStringLiteral("levels")) return AdjustmentType::Levels;
    if (normalised == QStringLiteral("curves")) return AdjustmentType::Curves;
    if (normalised == QStringLiteral("hue-saturation")) return AdjustmentType::HueSaturation;
    if (normalised == QStringLiteral("vibrance")) return AdjustmentType::Vibrance;
    if (normalised == QStringLiteral("white-balance")) return AdjustmentType::WhiteBalance;
    if (normalised == QStringLiteral("colour-balance") || normalised == QStringLiteral("color-balance")) return AdjustmentType::ColourBalance;
    if (normalised == QStringLiteral("channel-mixer")) return AdjustmentType::ChannelMixer;
    if (normalised == QStringLiteral("black-and-white") || normalised == QStringLiteral("black-white")) return AdjustmentType::BlackAndWhite;
    if (normalised == QStringLiteral("gradient-map")) return AdjustmentType::GradientMap;
    if (normalised == QStringLiteral("posterise") || normalised == QStringLiteral("posterize")) return AdjustmentType::Posterise;
    if (normalised == QStringLiteral("threshold")) return AdjustmentType::Threshold;
    if (normalised == QStringLiteral("lut")) return AdjustmentType::Lut;
    if (normalised == QStringLiteral("shadows-highlights") || normalised == QStringLiteral("shadows/highlights")) return AdjustmentType::ShadowsHighlights;
    if (normalised == QStringLiteral("gaussian-blur")) return AdjustmentType::GaussianBlur;
    if (normalised == QStringLiteral("box-blur")) return AdjustmentType::BoxBlur;
    if (normalised == QStringLiteral("unsharp-mask")) return AdjustmentType::UnsharpMask;
    if (normalised == QStringLiteral("high-pass")) return AdjustmentType::HighPass;
    if (normalised == QStringLiteral("invert")) return AdjustmentType::Invert;
    if (normalised == QStringLiteral("photo-filter") || normalised == QStringLiteral("photo filter")) return AdjustmentType::PhotoFilter;
    if (normalised == QStringLiteral("selective-colour") || normalised == QStringLiteral("selective-color")
        || normalised == QStringLiteral("selective colour") || normalised == QStringLiteral("selective color")) {
        return AdjustmentType::SelectiveColour;
    }
    if (normalised == QStringLiteral("vignette")) return AdjustmentType::Vignette;
    if (normalised == QStringLiteral("rgb-split")
        || normalised == QStringLiteral("rgb split")
        || normalised == QStringLiteral("creative-rgb-split")) {
        return AdjustmentType::RgbSplit;
    }
    if (normalised == QStringLiteral("chromatic-aberration-correction")
        || normalised == QStringLiteral("chromatic aberration correction")
        || normalised == QStringLiteral("ca-correction")) {
        return AdjustmentType::ChromaticAberrationCorrection;
    }
    if (normalised == QStringLiteral("surface-blur") || normalised == QStringLiteral("surface blur")) return AdjustmentType::SurfaceBlur;
    if (normalised == QStringLiteral("motion-blur") || normalised == QStringLiteral("motion blur")) return AdjustmentType::MotionBlur;
    if (normalised == QStringLiteral("radial-blur") || normalised == QStringLiteral("radial blur")) return AdjustmentType::RadialBlur;
    if (ok) *ok = false;
    return AdjustmentType::Exposure;
}

QString defaultAdjustmentName(const AdjustmentType type)
{
    switch (type) {
    case AdjustmentType::Exposure: return QStringLiteral("Exposure");
    case AdjustmentType::Contrast: return QStringLiteral("Contrast");
    case AdjustmentType::Saturation: return QStringLiteral("Saturation");
    case AdjustmentType::Levels: return QStringLiteral("Levels");
    case AdjustmentType::Curves: return QStringLiteral("Curves");
    case AdjustmentType::HueSaturation: return QStringLiteral("Hue/Saturation");
    case AdjustmentType::Vibrance: return QStringLiteral("Vibrance");
    case AdjustmentType::WhiteBalance: return QStringLiteral("White Balance");
    case AdjustmentType::ColourBalance: return QStringLiteral("Colour Balance");
    case AdjustmentType::ChannelMixer: return QStringLiteral("Channel Mixer");
    case AdjustmentType::BlackAndWhite: return QStringLiteral("Black and White");
    case AdjustmentType::GradientMap: return QStringLiteral("Gradient Map");
    case AdjustmentType::Posterise: return QStringLiteral("Posterise");
    case AdjustmentType::Threshold: return QStringLiteral("Threshold");
    case AdjustmentType::Lut: return QStringLiteral("LUT");
    case AdjustmentType::ShadowsHighlights: return QStringLiteral("Shadows/Highlights");
    case AdjustmentType::GaussianBlur: return QStringLiteral("Gaussian Blur");
    case AdjustmentType::BoxBlur: return QStringLiteral("Box Blur");
    case AdjustmentType::UnsharpMask: return QStringLiteral("Unsharp Mask");
    case AdjustmentType::HighPass: return QStringLiteral("High Pass");
    case AdjustmentType::Invert: return QStringLiteral("Invert");
    case AdjustmentType::PhotoFilter: return QStringLiteral("Photo Filter");
    case AdjustmentType::SelectiveColour: return QStringLiteral("Selective Colour");
    case AdjustmentType::Vignette: return QStringLiteral("Vignette");
    case AdjustmentType::RgbSplit: return QStringLiteral("RGB Split");
    case AdjustmentType::ChromaticAberrationCorrection:
        return QStringLiteral("Chromatic Aberration Correction");
    case AdjustmentType::SurfaceBlur: return QStringLiteral("Surface Blur");
    case AdjustmentType::MotionBlur: return QStringLiteral("Motion Blur");
    case AdjustmentType::RadialBlur: return QStringLiteral("Radial Blur");
    }
    return QStringLiteral("Adjustment");
}

bool adjustmentIsSpatial(const AdjustmentType type)
{
    return type == AdjustmentType::ShadowsHighlights
        || type == AdjustmentType::GaussianBlur
        || type == AdjustmentType::BoxBlur
        || type == AdjustmentType::UnsharpMask
        || type == AdjustmentType::HighPass
        || type == AdjustmentType::RgbSplit
        || type == AdjustmentType::ChromaticAberrationCorrection
        || type == AdjustmentType::SurfaceBlur
        || type == AdjustmentType::MotionBlur
        || type == AdjustmentType::RadialBlur;
}

QSize adjustmentSpatialRadius2D(const AdjustmentData &input)
{
    AdjustmentData data = input;
    data.normalise();
    const auto square = [](const int radius) { return QSize(radius, radius); };
    switch (data.type) {
    case AdjustmentType::ShadowsHighlights: {
        const auto &parameters = std::get<ShadowsHighlightsParameters>(data.parameters);
        if (std::abs(parameters.shadowAmount) <= 1.0e-12
            && std::abs(parameters.highlightAmount) <= 1.0e-12
            && std::abs(parameters.midtoneContrast) <= 1.0e-12) return {};
        return square(std::clamp(qCeil(parameters.radius), 1, 500));
    }
    case AdjustmentType::GaussianBlur:
        return square(std::clamp(qCeil(std::get<GaussianBlurParameters>(data.parameters).radius), 0, 500));
    case AdjustmentType::BoxBlur:
        return square(std::clamp(qCeil(std::get<BoxBlurParameters>(data.parameters).radius), 0, 500));
    case AdjustmentType::UnsharpMask: {
        const auto &parameters = std::get<UnsharpMaskParameters>(data.parameters);
        return parameters.amount <= 1.0e-12 ? QSize()
            : square(std::clamp(qCeil(parameters.radius), 0, 500));
    }
    case AdjustmentType::HighPass:
        return square(std::clamp(qCeil(std::get<HighPassParameters>(data.parameters).radius), 0, 500));
    case AdjustmentType::RgbSplit: {
        const auto &p = std::get<RgbSplitParameters>(data.parameters);
        const double x = std::max(std::abs(p.redOffsetX), std::abs(p.blueOffsetX));
        const double y = std::max(std::abs(p.redOffsetY), std::abs(p.blueOffsetY));
        return QSize(std::clamp(qCeil(x) + (x > 0.0 ? 1 : 0), 0, 201),
                     std::clamp(qCeil(y) + (y > 0.0 ? 1 : 0), 0, 201));
    }
    case AdjustmentType::ChromaticAberrationCorrection: {
        const auto &p = std::get<ChromaticAberrationCorrectionParameters>(data.parameters);
        const double maximum = std::max(std::abs(p.redEdgeShift), std::abs(p.blueEdgeShift));
        return square(std::clamp(qCeil(maximum) + (maximum > 0.0 ? 1 : 0), 0, 101));
    }
    case AdjustmentType::SurfaceBlur:
        return square(std::clamp(qCeil(std::get<SurfaceBlurParameters>(data.parameters).radius), 0, 250));
    case AdjustmentType::MotionBlur: {
        const auto &p = std::get<MotionBlurParameters>(data.parameters);
        if (p.distance <= 1.0e-12) return {};
        const double radians = p.angle * std::numbers::pi / 180.0;
        const double halfDistance = p.distance * 0.5;
        const auto componentRadius = [](const double component) {
            return component <= 1.0e-9
                ? 0 : std::clamp(qCeil(component) + 1, 0, 251);
        };
        return QSize(componentRadius(std::abs(std::cos(radians)) * halfDistance),
                     componentRadius(std::abs(std::sin(radians)) * halfDistance));
    }
    case AdjustmentType::RadialBlur: {
        const auto &p = std::get<RadialBlurParameters>(data.parameters);
        return square(std::clamp(qCeil(p.amount * 0.5) + (p.amount > 0.0 ? 1 : 0), 0, 126));
    }
    default:
        return {};
    }
}

int adjustmentSpatialRadius(const AdjustmentData &data)
{
    const QSize radius = adjustmentSpatialRadius2D(data);
    return std::max(radius.width(), radius.height());
}

QSize maximumSpatialAdjustmentRadius2D(const QVector<LayerNode> &layers)
{
    // Return the dependency halo of the complete stack, not merely the
    // largest individual radius. Consecutive spatial adjustments are
    // compositional: a radius-80 adjustment above another radius-80
    // adjustment can depend on source pixels 160 pixels away. Isolated groups
    // form independent branches and therefore take the maximum, while Pass
    // Through groups transform the parent accumulator and conservatively add
    // their child dependency.
    qint64 dependencyX = 0;
    qint64 dependencyY = 0;
    for (auto iterator = layers.crbegin(); iterator != layers.crend(); ++iterator) {
        const LayerNode &layer = *iterator;
        if (!layer.visible || layer.opacity <= 0.0) continue;
        if (layer.type == LayerType::Adjustment) {
            const QSize radius = adjustmentSpatialRadius2D(layer.effectiveAdjustmentData());
            dependencyX += radius.width();
            dependencyY += radius.height();
        } else if (layer.type == LayerType::Group) {
            const QSize child = maximumSpatialAdjustmentRadius2D(layer.children);
            if (layer.groupCompositeMode == GroupCompositeMode::PassThrough) {
                dependencyX += child.width();
                dependencyY += child.height();
            } else {
                dependencyX = std::max<qint64>(dependencyX, child.width());
                dependencyY = std::max<qint64>(dependencyY, child.height());
            }
        }
        // Layer Effects own their tile-local coverage halo and crop the generated
        // pass back to the caller's requested region. Do not add that halo here:
        // doing so would expand once at the document stack and a second time in
        // renderLayerEffectPasses(), doubling memory/work for large shadows.
        dependencyX = std::min<qint64>(dependencyX, std::numeric_limits<int>::max() / 4);
        dependencyY = std::min<qint64>(dependencyY, std::numeric_limits<int>::max() / 4);
    }
    return QSize(static_cast<int>(dependencyX), static_cast<int>(dependencyY));
}

int maximumSpatialAdjustmentRadius(const QVector<LayerNode> &layers)
{
    const QSize radius = maximumSpatialAdjustmentRadius2D(layers);
    return std::max(radius.width(), radius.height());
}

QString curveInterpolationToString(const CurveInterpolation interpolation)
{
    return interpolation == CurveInterpolation::Linear
        ? QStringLiteral("linear") : QStringLiteral("smooth");
}

CurveInterpolation curveInterpolationFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("smooth")) return CurveInterpolation::Smooth;
    if (normalised == QStringLiteral("linear")) return CurveInterpolation::Linear;
    if (ok) *ok = false;
    return CurveInterpolation::Smooth;
}

QString hueSaturationRangeDisplayName(const HueSaturationRange range)
{
    switch (range) {
    case HueSaturationRange::Reds: return QStringLiteral("Reds");
    case HueSaturationRange::Yellows: return QStringLiteral("Yellows");
    case HueSaturationRange::Greens: return QStringLiteral("Greens");
    case HueSaturationRange::Cyans: return QStringLiteral("Cyans");
    case HueSaturationRange::Blues: return QStringLiteral("Blues");
    case HueSaturationRange::Magentas: return QStringLiteral("Magentas");
    }
    return QStringLiteral("Reds");
}

QString colourBalanceRangeDisplayName(const ColourBalanceRange range)
{
    switch (range) {
    case ColourBalanceRange::Shadows: return QStringLiteral("Shadows");
    case ColourBalanceRange::Midtones: return QStringLiteral("Midtones");
    case ColourBalanceRange::Highlights: return QStringLiteral("Highlights");
    }
    return QStringLiteral("Midtones");
}

QString channelMixerOutputDisplayName(const ChannelMixerOutput output)
{
    switch (output) {
    case ChannelMixerOutput::Red: return QStringLiteral("Red");
    case ChannelMixerOutput::Green: return QStringLiteral("Green");
    case ChannelMixerOutput::Blue: return QStringLiteral("Blue");
    }
    return QStringLiteral("Red");
}

QString gradientInterpolationToString(const GradientInterpolation interpolation)
{
    switch (interpolation) {
    case GradientInterpolation::Linear: return QStringLiteral("linear");
    case GradientInterpolation::Smooth: return QStringLiteral("smooth");
    case GradientInterpolation::Constant: return QStringLiteral("constant");
    }
    return QStringLiteral("linear");
}

GradientInterpolation gradientInterpolationFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("linear")) return GradientInterpolation::Linear;
    if (normalised == QStringLiteral("smooth")) return GradientInterpolation::Smooth;
    if (normalised == QStringLiteral("constant")) return GradientInterpolation::Constant;
    if (ok) *ok = false;
    return GradientInterpolation::Linear;
}

QString thresholdSourceToString(const ThresholdSource source)
{
    switch (source) {
    case ThresholdSource::Luminance: return QStringLiteral("luminance");
    case ThresholdSource::Red: return QStringLiteral("red");
    case ThresholdSource::Green: return QStringLiteral("green");
    case ThresholdSource::Blue: return QStringLiteral("blue");
    }
    return QStringLiteral("luminance");
}

QString thresholdSourceDisplayName(const ThresholdSource source)
{
    switch (source) {
    case ThresholdSource::Luminance: return QStringLiteral("Luminance");
    case ThresholdSource::Red: return QStringLiteral("Red");
    case ThresholdSource::Green: return QStringLiteral("Green");
    case ThresholdSource::Blue: return QStringLiteral("Blue");
    }
    return QStringLiteral("Luminance");
}

ThresholdSource thresholdSourceFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("luminance")) return ThresholdSource::Luminance;
    if (normalised == QStringLiteral("red")) return ThresholdSource::Red;
    if (normalised == QStringLiteral("green")) return ThresholdSource::Green;
    if (normalised == QStringLiteral("blue")) return ThresholdSource::Blue;
    if (ok) *ok = false;
    return ThresholdSource::Luminance;
}

QString lutDomainSourceToString(const LutDomainSource source)
{
    switch (source) {
    case LutDomainSource::DefaultRange: return QStringLiteral("default");
    case LutDomainSource::DomainDirective: return QStringLiteral("domain-directive");
    case LutDomainSource::InputRangeDirective: return QStringLiteral("input-range-directive");
    case LutDomainSource::LegacyPersisted: return QStringLiteral("legacy-persisted");
    }
    return QStringLiteral("default");
}

QString lutDomainSourceDisplayName(const LutDomainSource source)
{
    switch (source) {
    case LutDomainSource::DefaultRange: return QStringLiteral("Default 0–1 range");
    case LutDomainSource::DomainDirective: return QStringLiteral("DOMAIN_MIN / DOMAIN_MAX");
    case LutDomainSource::InputRangeDirective: return QStringLiteral("LUT input range");
    case LutDomainSource::LegacyPersisted: return QStringLiteral("Legacy persisted range");
    }
    return QStringLiteral("Default 0–1 range");
}

LutDomainSource lutDomainSourceFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("default")) return LutDomainSource::DefaultRange;
    if (normalised == QStringLiteral("domain-directive")) return LutDomainSource::DomainDirective;
    if (normalised == QStringLiteral("input-range-directive")) return LutDomainSource::InputRangeDirective;
    if (normalised == QStringLiteral("legacy-persisted")) return LutDomainSource::LegacyPersisted;
    if (ok) *ok = false;
    return LutDomainSource::DefaultRange;
}

QString lutInterpolationToString(const LutInterpolation interpolation)
{
    switch (interpolation) {
    case LutInterpolation::Trilinear: return QStringLiteral("trilinear");
    case LutInterpolation::Tetrahedral: return QStringLiteral("tetrahedral");
    }
    return QStringLiteral("trilinear");
}

QString lutInterpolationDisplayName(const LutInterpolation interpolation)
{
    switch (interpolation) {
    case LutInterpolation::Trilinear: return QStringLiteral("Trilinear");
    case LutInterpolation::Tetrahedral: return QStringLiteral("Tetrahedral");
    }
    return QStringLiteral("Trilinear");
}

LutInterpolation lutInterpolationFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("trilinear")) return LutInterpolation::Trilinear;
    if (normalised == QStringLiteral("tetrahedral")) return LutInterpolation::Tetrahedral;
    if (ok) *ok = false;
    return LutInterpolation::Trilinear;
}

QString lutProcessingModeToString(const LutProcessingMode mode)
{
    switch (mode) {
    case LutProcessingMode::EncodedDocument: return QStringLiteral("encoded-document");
    case LutProcessingMode::LinearSrgb: return QStringLiteral("linear-srgb");
    case LutProcessingMode::RawComponents: return QStringLiteral("raw-components");
    }
    return QStringLiteral("encoded-document");
}

QString lutProcessingModeDisplayName(const LutProcessingMode mode)
{
    switch (mode) {
    case LutProcessingMode::EncodedDocument:
        return QStringLiteral("Encoded document values");
    case LutProcessingMode::LinearSrgb:
        return QStringLiteral("Linear sRGB / Rec.709");
    case LutProcessingMode::RawComponents:
        return QStringLiteral("Raw component values");
    }
    return QStringLiteral("Encoded document values");
}

LutProcessingMode lutProcessingModeFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("encoded-document")) {
        return LutProcessingMode::EncodedDocument;
    }
    if (normalised == QStringLiteral("linear-srgb")) {
        return LutProcessingMode::LinearSrgb;
    }
    if (normalised == QStringLiteral("raw-components")) {
        return LutProcessingMode::RawComponents;
    }
    if (ok) *ok = false;
    return LutProcessingMode::EncodedDocument;
}

QString lutOperatorProfileToString(const LutOperatorProfile profile)
{
    switch (profile) {
    case LutOperatorProfile::Generic: return QStringLiteral("generic");
    case LutOperatorProfile::TonyMcMapface: return QStringLiteral("tony-mc-mapface");
    case LutOperatorProfile::AgXBaseSrgb: return QStringLiteral("agx-base-srgb");
    }
    return QStringLiteral("generic");
}

QString lutOperatorProfileDisplayName(const LutOperatorProfile profile)
{
    switch (profile) {
    case LutOperatorProfile::Generic: return QStringLiteral("Generic .cube");
    case LutOperatorProfile::TonyMcMapface: return QStringLiteral("Tony McMapface");
    case LutOperatorProfile::AgXBaseSrgb: return QStringLiteral("AgX Base sRGB");
    }
    return QStringLiteral("Generic .cube");
}

LutOperatorProfile lutOperatorProfileFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("generic")) return LutOperatorProfile::Generic;
    if (normalised == QStringLiteral("tony-mc-mapface")) {
        return LutOperatorProfile::TonyMcMapface;
    }
    if (normalised == QStringLiteral("agx-base-srgb")) {
        return LutOperatorProfile::AgXBaseSrgb;
    }
    if (ok) *ok = false;
    return LutOperatorProfile::Generic;
}

QString layerTypeToString(const LayerType type)
{
    switch (type) {
    case LayerType::BaseImage:
        return QStringLiteral("base-image");
    case LayerType::Raster:
        return QStringLiteral("raster");
    case LayerType::Adjustment:
        return QStringLiteral("adjustment");
    case LayerType::Group:
        return QStringLiteral("group");
    case LayerType::Vector:
        return QStringLiteral("vector");
    case LayerType::Text:
        return QStringLiteral("text");
    case LayerType::Smart:
        return QStringLiteral("smart");
    }
    return QStringLiteral("raster");
}

LayerType layerTypeFromString(const QString &value, bool *ok)
{
    if (ok) {
        *ok = true;
    }
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("base-image")) {
        return LayerType::BaseImage;
    }
    if (normalised == QStringLiteral("raster")) {
        return LayerType::Raster;
    }
    if (normalised == QStringLiteral("adjustment")) {
        return LayerType::Adjustment;
    }
    if (normalised == QStringLiteral("group")) {
        return LayerType::Group;
    }
    if (normalised == QStringLiteral("vector")) {
        return LayerType::Vector;
    }
    if (normalised == QStringLiteral("text")) {
        return LayerType::Text;
    }
    if (normalised == QStringLiteral("smart")) {
        return LayerType::Smart;
    }
    if (ok) {
        *ok = false;
    }
    return LayerType::Raster;
}

QString defaultLayerName(const LayerType type, const AdjustmentType adjustmentType)
{
    switch (type) {
    case LayerType::BaseImage:
        return QStringLiteral("Base Image");
    case LayerType::Raster:
        return QStringLiteral("Raster Layer");
    case LayerType::Adjustment:
        return defaultAdjustmentName(adjustmentType);
    case LayerType::Group:
        return QStringLiteral("Group");
    case LayerType::Vector:
        return QStringLiteral("Vector Shape");
    case LayerType::Text:
        return QStringLiteral("Text");
    case LayerType::Smart:
        return QStringLiteral("Smart Layer");
    }
    return QStringLiteral("Layer");
}

QString blendModeToString(const BlendMode mode)
{
    switch (mode) {
    case BlendMode::Copy:
        return QStringLiteral("copy");
    case BlendMode::Multiply:
        return QStringLiteral("multiply");
    case BlendMode::Screen:
        return QStringLiteral("screen");
    case BlendMode::Overlay:
        return QStringLiteral("overlay");
    case BlendMode::Darken:
        return QStringLiteral("darken");
    case BlendMode::Lighten:
        return QStringLiteral("lighten");
    case BlendMode::ColourDodge:
        return QStringLiteral("colour-dodge");
    case BlendMode::ColourBurn:
        return QStringLiteral("colour-burn");
    case BlendMode::Add:
        return QStringLiteral("add");
    case BlendMode::Subtract:
        return QStringLiteral("subtract");
    case BlendMode::Difference:
        return QStringLiteral("difference");
    case BlendMode::Exclusion:
        return QStringLiteral("exclusion");
    }
    return QStringLiteral("copy");
}

QString blendModeDisplayName(const BlendMode mode)
{
    switch (mode) {
    case BlendMode::Copy:
        return QStringLiteral("Copy / Replace");
    case BlendMode::Multiply:
        return QStringLiteral("Multiply");
    case BlendMode::Screen:
        return QStringLiteral("Screen");
    case BlendMode::Overlay:
        return QStringLiteral("Overlay");
    case BlendMode::Darken:
        return QStringLiteral("Darken");
    case BlendMode::Lighten:
        return QStringLiteral("Lighten");
    case BlendMode::ColourDodge:
        return QStringLiteral("Colour Dodge");
    case BlendMode::ColourBurn:
        return QStringLiteral("Colour Burn");
    case BlendMode::Add:
        return QStringLiteral("Add");
    case BlendMode::Subtract:
        return QStringLiteral("Subtract");
    case BlendMode::Difference:
        return QStringLiteral("Difference");
    case BlendMode::Exclusion:
        return QStringLiteral("Exclusion");
    }
    return QStringLiteral("Copy / Replace");
}

BlendMode blendModeFromString(const QString &value, bool *ok)
{
    if (ok) {
        *ok = true;
    }
    const QString normalised = value.trimmed().toLower();
    const QVector<BlendMode> serialisedModes {
        BlendMode::Copy,
        BlendMode::Multiply,
        BlendMode::Screen,
        BlendMode::Overlay,
        BlendMode::Darken,
        BlendMode::Lighten,
        BlendMode::ColourDodge,
        BlendMode::ColourBurn,
        BlendMode::Add,
        BlendMode::Subtract,
        BlendMode::Difference,
        BlendMode::Exclusion
    };
    for (const BlendMode mode : serialisedModes) {
        if (normalised == blendModeToString(mode)) {
            return mode;
        }
    }
    if (ok) {
        *ok = false;
    }
    return BlendMode::Copy;
}


QString groupCompositeModeToString(const GroupCompositeMode mode)
{
    switch (mode) {
    case GroupCompositeMode::Isolated:
        return QStringLiteral("isolated");
    case GroupCompositeMode::PassThrough:
        return QStringLiteral("pass-through");
    }
    return QStringLiteral("isolated");
}

QString groupCompositeModeDisplayName(const GroupCompositeMode mode)
{
    switch (mode) {
    case GroupCompositeMode::Isolated:
        return QStringLiteral("Isolated / Normal");
    case GroupCompositeMode::PassThrough:
        return QStringLiteral("Pass Through");
    }
    return QStringLiteral("Isolated / Normal");
}

GroupCompositeMode groupCompositeModeFromString(const QString &value, bool *ok)
{
    if (ok) {
        *ok = true;
    }
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("isolated")
        || normalised == QStringLiteral("normal")) {
        return GroupCompositeMode::Isolated;
    }
    if (normalised == QStringLiteral("pass-through")
        || normalised == QStringLiteral("passthrough")) {
        return GroupCompositeMode::PassThrough;
    }
    if (ok) {
        *ok = false;
    }
    return GroupCompositeMode::Isolated;
}

QVector<BlendMode> availableBlendModes()
{
    return {
        BlendMode::Copy,
        BlendMode::Multiply,
        BlendMode::Screen,
        BlendMode::Overlay,
        BlendMode::Darken,
        BlendMode::Lighten,
        BlendMode::ColourDodge,
        BlendMode::ColourBurn,
        BlendMode::Add,
        BlendMode::Difference,
        BlendMode::Exclusion
    };
}

bool LayerNode::operator==(const LayerNode &other) const
{
    const auto imagesMatch = [](const QImage &left, const QImage &right) {
        if (left.isNull() || right.isNull()) {
            return left.isNull() == right.isNull();
        }

        // QImage is implicitly shared but is not directly equality-comparable
        // for Qt container traits. cacheKey() changes whenever either image is
        // detached or modified, making it a constant-time revision identity for
        // the document snapshots used by Undo/Redo.
        return left.cacheKey() == right.cacheKey()
            && left.size() == right.size()
            && left.format() == right.format()
            && left.devicePixelRatio() == right.devicePixelRatio();
    };

    return id == other.id
        && type == other.type
        && name == other.name
        && visible == other.visible
        && opacity == other.opacity
        && blendMode == other.blendMode
        && groupCompositeMode == other.groupCompositeMode
        && transform == other.transform
        && revision == other.revision
        && adjustmentType == other.adjustmentType
        && effectiveAdjustmentData() == other.effectiveAdjustmentData()
        && exposure == other.exposure
        && contrast == other.contrast
        && saturation == other.saturation
        && blackPoint == other.blackPoint
        && whitePoint == other.whitePoint
        && gamma == other.gamma
        && vectorData == other.vectorData
        && textData == other.textData
        && smartSource == other.smartSource
        && smartTransform == other.smartTransform
        && liveFilters == other.liveFilters
        && layerEffects == other.layerEffects
        && imagesMatch(rasterImage, other.rasterImage)
        && rasterReferenceSize == other.rasterReferenceSize
        && rasterReferenceOrigin == other.rasterReferenceOrigin
        && imagesMatch(maskImage, other.maskImage)
        && maskReferenceSize == other.maskReferenceSize
        && maskReferenceOrigin == other.maskReferenceOrigin
        && maskEnabled == other.maskEnabled
        && maskInverted == other.maskInverted
        && [&] {
            if (children.size() != other.children.size()) {
                return false;
            }
            for (qsizetype index = 0; index < children.size(); ++index) {
                if (!(children.at(index) == other.children.at(index))) {
                    return false;
                }
            }
            return true;
        }();
}

QJsonObject LayerNode::toJson(bool *ok) const
{
    if (ok) {
        *ok = true;
    }
    QJsonObject object;
    object.insert(QStringLiteral("id"), id.toString(QUuid::WithoutBraces));
    object.insert(QStringLiteral("kind"), layerTypeToString(type));
    object.insert(QStringLiteral("name"), name);
    object.insert(QStringLiteral("visible"), visible);
    object.insert(QStringLiteral("opacity"), std::clamp(opacity, 0.0, 1.0));
    object.insert(QStringLiteral("blendMode"), blendModeToString(blendMode));
    if (type == LayerType::Group) {
        object.insert(QStringLiteral("compositingMode"),
                      groupCompositeModeToString(groupCompositeMode));
    }
    object.insert(QStringLiteral("revision"), QString::number(std::max<quint64>(1, revision)));
    if (!transform.isInvertible() || !transformMatrixIsFiniteAndBounded(transform)) {
        if (ok) {
            *ok = false;
        }
        return {};
    }
    QJsonObject transformObject;
    transformObject.insert(QStringLiteral("m11"), transform.m11());
    transformObject.insert(QStringLiteral("m12"), transform.m12());
    transformObject.insert(QStringLiteral("m13"), transform.m13());
    transformObject.insert(QStringLiteral("m21"), transform.m21());
    transformObject.insert(QStringLiteral("m22"), transform.m22());
    transformObject.insert(QStringLiteral("m23"), transform.m23());
    transformObject.insert(QStringLiteral("dx"), transform.dx());
    transformObject.insert(QStringLiteral("dy"), transform.dy());
    transformObject.insert(QStringLiteral("m33"), transform.m33());
    object.insert(QStringLiteral("transform"), transformObject);

    if (type == LayerType::Adjustment) {
        object.insert(QStringLiteral("adjustmentType"), adjustmentTypeToString(adjustmentType));
        object.insert(QStringLiteral("exposure"), exposure);
        object.insert(QStringLiteral("contrast"), contrast);
        object.insert(QStringLiteral("saturation"), saturation);
        object.insert(QStringLiteral("blackPoint"), blackPoint);
        object.insert(QStringLiteral("whitePoint"), whitePoint);
        object.insert(QStringLiteral("gamma"), gamma);
        bool adjustmentOk = false;
        object.insert(QStringLiteral("adjustment"),
                      effectiveAdjustmentData().toJson(&adjustmentOk));
        if (!adjustmentOk) {
            if (ok) {
                *ok = false;
            }
            return {};
        }
    }

    if (type == LayerType::Vector) {
        bool vectorOk = false;
        object.insert(QStringLiteral("vector"), vectorData.toJson(&vectorOk));
        if (!vectorOk) {
            if (ok) *ok = false;
            return {};
        }
    }
    if (type == LayerType::Text) {
        bool textOk = false;
        object.insert(QStringLiteral("text"), textData.toJson(&textOk));
        if (!textOk) {
            if (ok) *ok = false;
            return {};
        }
    }
    if (type == LayerType::Smart) {
        bool smartOk = false;
        object.insert(QStringLiteral("smartSource"), smartSource.toJson(&smartOk));
        bool transformOk = false;
        object.insert(QStringLiteral("smartTransform"),
                      smartTransform.toJson(&transformOk));
        if (!smartOk || !transformOk
            || liveFilters.size() > MaximumLiveFilterCount) {
            if (ok) *ok = false;
            return {};
        }
        if (!liveFilters.isEmpty()) {
            QJsonArray filters;
            for (const LiveFilter &filter : liveFilters) {
                bool filterOk = false;
                const QJsonObject encoded = filter.toJson(&filterOk);
                if (!filterOk) {
                    if (ok) *ok = false;
                    return {};
                }
                filters.append(encoded);
            }
            object.insert(QStringLiteral("liveFilters"), filters);
        }
    } else if (!liveFilters.isEmpty()) {
        if (ok) *ok = false;
        return {};
    }

    if (!layerEffects.isEmpty()) {
        if (!layerTypeSupportsLayerEffects(type)
            || layerEffects.size() > MaximumLayerEffectCount) {
            if (ok) *ok = false;
            return {};
        }
        QSet<QUuid> effectIds;
        QJsonArray effects;
        for (const LayerEffect &effect : layerEffects) {
            if (effectIds.contains(effect.id)) {
                if (ok) *ok = false;
                return {};
            }
            effectIds.insert(effect.id);
            bool effectOk = false;
            const QJsonObject encoded = effect.toJson(&effectOk);
            if (!effectOk) {
                if (ok) *ok = false;
                return {};
            }
            effects.append(encoded);
        }
        object.insert(QStringLiteral("layerEffects"), effects);
    }

    if ((type == LayerType::Raster || type == LayerType::BaseImage)
        && rasterReferenceSize.isValid() && !rasterReferenceSize.isEmpty()) {
        QJsonObject reference;
        reference.insert(QStringLiteral("width"), rasterReferenceSize.width());
        reference.insert(QStringLiteral("height"), rasterReferenceSize.height());
        object.insert(QStringLiteral("rasterReferenceSize"), reference);
    }
    if (maskReferenceSize.isValid() && !maskReferenceSize.isEmpty()) {
        QJsonObject reference;
        reference.insert(QStringLiteral("width"), maskReferenceSize.width());
        reference.insert(QStringLiteral("height"), maskReferenceSize.height());
        object.insert(QStringLiteral("maskReferenceSize"), reference);
    }
    const auto validReferenceOrigin = [](const QPointF &origin) {
        constexpr double MaximumCoordinate = 1.0e9;
        return std::isfinite(origin.x()) && std::isfinite(origin.y())
            && std::abs(origin.x()) <= MaximumCoordinate
            && std::abs(origin.y()) <= MaximumCoordinate;
    };
    if (!validReferenceOrigin(rasterReferenceOrigin)
        || !validReferenceOrigin(maskReferenceOrigin)) {
        if (ok) {
            *ok = false;
        }
        return {};
    }
    const auto insertReferenceOrigin = [&object](const QString &key,
                                                 const QPointF &origin) {
        if (qFuzzyIsNull(origin.x()) && qFuzzyIsNull(origin.y())) {
            return;
        }
        QJsonObject value;
        value.insert(QStringLiteral("x"), origin.x());
        value.insert(QStringLiteral("y"), origin.y());
        object.insert(key, value);
    };
    if (type == LayerType::Raster || type == LayerType::BaseImage) {
        insertReferenceOrigin(QStringLiteral("rasterReferenceOrigin"),
                              rasterReferenceOrigin);
    }
    insertReferenceOrigin(QStringLiteral("maskReferenceOrigin"),
                          maskReferenceOrigin);

    if ((type == LayerType::Raster || type == LayerType::BaseImage) && !rasterImage.isNull()) {
        bool imageOk = false;
        const QString encoded = encodeImage(rasterImage, &imageOk);
        if (!imageOk) {
            if (ok) {
                *ok = false;
            }
            return {};
        }
        object.insert(QStringLiteral("rasterEncoding"), QStringLiteral("png-base64"));
        object.insert(QStringLiteral("rasterData"), encoded);
    }
    if (!maskImage.isNull()) {
        bool maskOk = false;
        const QString encoded = encodeImage(maskImage, &maskOk);
        if (!maskOk) {
            if (ok) {
                *ok = false;
            }
            return {};
        }
        object.insert(QStringLiteral("maskEncoding"), QStringLiteral("png-base64"));
        object.insert(QStringLiteral("maskData"), encoded);
        object.insert(QStringLiteral("maskEnabled"), maskEnabled);
        object.insert(QStringLiteral("maskInverted"), maskInverted);
    }

    if (type == LayerType::Group) {
        QJsonArray childArray;
        for (const LayerNode &child : children) {
            bool childOk = false;
            const QJsonObject childObject = child.toJson(&childOk);
            if (!childOk) {
                if (ok) {
                    *ok = false;
                }
                return {};
            }
            childArray.append(childObject);
        }
        object.insert(QStringLiteral("children"), childArray);
    }
    return object;
}

static LayerNode layerNodeFromJson(const QJsonObject &object,
                                   bool *ok,
                                   QStringList *warnings,
                                   const int depth,
                                   int *remainingLayers)
{
    if (!remainingLayers || depth < 0 || depth >= LayerNode::MaximumTreeDepth
        || *remainingLayers <= 0) {
        if (ok) *ok = false;
        return {};
    }
    --(*remainingLayers);
    bool typeOk = false;
    LayerNode layer;
    layer.type = layerTypeFromString(object.value(QStringLiteral("kind")).toString(), &typeOk);
    if (!typeOk) {
        if (ok) {
            *ok = false;
        }
        return layer;
    }

    const QUuid parsedId(object.value(QStringLiteral("id")).toString());
    if (!parsedId.isNull()) {
        layer.id = parsedId;
    }
    layer.name = object.value(QStringLiteral("name")).toString(defaultLayerName(layer.type));
    layer.visible = object.value(QStringLiteral("visible")).toBool(true);
    const QJsonValue opacityValue = object.value(QStringLiteral("opacity"));
    const double decodedOpacity = opacityValue.toDouble(1.0);
    if ((!opacityValue.isUndefined() && !opacityValue.isDouble())
        || !std::isfinite(decodedOpacity)) {
        if (ok) *ok = false;
        return layer;
    }
    layer.opacity = std::clamp(decodedOpacity, 0.0, 1.0);
    bool revisionOk = false;
    layer.revision = object.value(QStringLiteral("revision")).toString().toULongLong(&revisionOk);
    if (!revisionOk) {
        layer.revision = static_cast<quint64>(object.value(QStringLiteral("revision")).toDouble(1.0));
    }
    layer.revision = std::max<quint64>(1, layer.revision);

    bool blendOk = false;
    layer.blendMode = blendModeFromString(
        object.value(QStringLiteral("blendMode")).toString(QStringLiteral("copy")), &blendOk);
    if (!blendOk) {
        layer.blendMode = BlendMode::Copy;
    }

    if (layer.type == LayerType::Group) {
        const QJsonValue modeValue = object.value(QStringLiteral("compositingMode"));
        bool modeOk = false;
        layer.groupCompositeMode = groupCompositeModeFromString(
            modeValue.toString(QStringLiteral("isolated")), &modeOk);
        if ((!modeValue.isUndefined() && !modeValue.isString()) || !modeOk) {
            layer.groupCompositeMode = GroupCompositeMode::Isolated;
            if (warnings) {
                warnings->push_back(
                    QStringLiteral("The compositing mode for group ‘%1’ was invalid and was reset to Isolated / Normal.")
                        .arg(layer.name));
            }
        }
    } else {
        layer.groupCompositeMode = GroupCompositeMode::Isolated;
    }

    const QJsonObject transformObject = object.value(QStringLiteral("transform")).toObject();
    if (!transformObject.isEmpty()) {
        // m13/m23/m33 were added as optional fields during the Transform
        // Expansion milestone. Their defaults reproduce the affine matrix
        // written by earlier version-6 projects.
        layer.transform = QTransform(
            transformObject.value(QStringLiteral("m11")).toDouble(1.0),
            transformObject.value(QStringLiteral("m12")).toDouble(0.0),
            transformObject.value(QStringLiteral("m13")).toDouble(0.0),
            transformObject.value(QStringLiteral("m21")).toDouble(0.0),
            transformObject.value(QStringLiteral("m22")).toDouble(1.0),
            transformObject.value(QStringLiteral("m23")).toDouble(0.0),
            transformObject.value(QStringLiteral("dx")).toDouble(0.0),
            transformObject.value(QStringLiteral("dy")).toDouble(0.0),
            transformObject.value(QStringLiteral("m33")).toDouble(1.0));
        if (!transformMatrixIsFiniteAndBounded(layer.transform)
            || !layer.transform.isInvertible()) {
            layer.transform.reset();
            if (warnings) {
                warnings->push_back(
                    QStringLiteral("The transform on layer ‘%1’ was invalid and was reset.")
                        .arg(layer.name));
            }
        }
    } else {
        // Project formats 1-4 stored translation separately. Import it as an
        // affine transform so older documents remain editable.
        const QJsonObject offsetObject = object.value(QStringLiteral("offset")).toObject();
        layer.transform = QTransform::fromTranslate(
            offsetObject.value(QStringLiteral("x")).toDouble(0.0),
            offsetObject.value(QStringLiteral("y")).toDouble(0.0));
    }

    const auto parseReferenceSize = [](const QJsonValue &value) {
        const QJsonObject reference = value.toObject();
        const int width = reference.value(QStringLiteral("width")).toInt(0);
        const int height = reference.value(QStringLiteral("height")).toInt(0);
        return width > 0 && height > 0 && width <= 32768 && height <= 32768
            ? QSize(width, height) : QSize();
    };
    layer.rasterReferenceSize = parseReferenceSize(
        object.value(QStringLiteral("rasterReferenceSize")));
    layer.maskReferenceSize = parseReferenceSize(
        object.value(QStringLiteral("maskReferenceSize")));
    const auto parseReferenceOrigin = [](const QJsonValue &value) {
        const QJsonObject reference = value.toObject();
        const double x = reference.value(QStringLiteral("x")).toDouble(0.0);
        const double y = reference.value(QStringLiteral("y")).toDouble(0.0);
        constexpr double MaximumCoordinate = 1.0e9;
        return std::isfinite(x) && std::isfinite(y)
                && std::abs(x) <= MaximumCoordinate
                && std::abs(y) <= MaximumCoordinate
            ? QPointF(x, y) : QPointF();
    };
    layer.rasterReferenceOrigin = parseReferenceOrigin(
        object.value(QStringLiteral("rasterReferenceOrigin")));
    layer.maskReferenceOrigin = parseReferenceOrigin(
        object.value(QStringLiteral("maskReferenceOrigin")));

    if (layer.type == LayerType::Adjustment) {
        bool adjustmentOk = false;
        layer.adjustmentType = adjustmentTypeFromString(
            object.value(QStringLiteral("adjustmentType")).toString(), &adjustmentOk);
        if (!adjustmentOk) {
            if (ok) {
                *ok = false;
            }
            return layer;
        }
        layer.exposure = object.value(QStringLiteral("exposure")).toDouble(0.0);
        layer.contrast = object.value(QStringLiteral("contrast")).toDouble(0.0);
        layer.saturation = object.value(QStringLiteral("saturation")).toDouble(0.0);
        layer.blackPoint = object.value(QStringLiteral("blackPoint")).toDouble(0.0);
        layer.whitePoint = object.value(QStringLiteral("whitePoint")).toDouble(1.0);
        layer.gamma = object.value(QStringLiteral("gamma")).toDouble(1.0);
        if (!std::isfinite(layer.exposure) || !std::isfinite(layer.contrast)
            || !std::isfinite(layer.saturation) || !std::isfinite(layer.blackPoint)
            || !std::isfinite(layer.whitePoint) || !std::isfinite(layer.gamma)) {
            if (ok) *ok = false;
            return layer;
        }

        const QJsonValue typedValue = object.value(QStringLiteral("adjustment"));
        if (!typedValue.isUndefined()) {
            bool typedOk = false;
            const AdjustmentData typed = AdjustmentData::fromJson(
                typedValue.toObject(), layer.adjustmentType, &typedOk);
            if (!typedValue.isObject() || !typedOk || typed.type != layer.adjustmentType) {
                if (warnings) {
                    warnings->push_back(
                        QStringLiteral("The typed parameters for adjustment ‘%1’ were invalid; legacy version-6 values were restored.")
                            .arg(layer.name));
                }
                layer.adjustment.reset(layer.adjustmentType);
            } else {
                layer.setAdjustmentData(typed);
            }
        } else {
            AdjustmentData migrated;
            migrated.reset(layer.adjustmentType);
            switch (layer.adjustmentType) {
            case AdjustmentType::Exposure:
                migrated.parameters = ExposureParameters {layer.exposure};
                break;
            case AdjustmentType::Contrast:
                migrated.parameters = ContrastParameters {layer.contrast};
                break;
            case AdjustmentType::Saturation:
                migrated.parameters = SaturationParameters {layer.saturation};
                break;
            case AdjustmentType::Levels: {
                LevelsParameters levels;
                auto &rgb = levels.channel(AdjustmentChannel::Rgb);
                rgb.inputBlack = layer.blackPoint;
                rgb.inputWhite = layer.whitePoint;
                rgb.gamma = layer.gamma;
                migrated.parameters = levels;
                break;
            }
            case AdjustmentType::Curves:
                migrated.parameters = CurvesParameters {};
                break;
            case AdjustmentType::HueSaturation:
                migrated.parameters = HueSaturationParameters {};
                break;
            case AdjustmentType::Vibrance:
                migrated.parameters = VibranceParameters {};
                break;
            case AdjustmentType::WhiteBalance:
                migrated.parameters = WhiteBalanceParameters {};
                break;
            case AdjustmentType::ColourBalance:
                migrated.parameters = ColourBalanceParameters {};
                break;
            case AdjustmentType::ChannelMixer:
                migrated.parameters = ChannelMixerParameters {};
                break;
            case AdjustmentType::BlackAndWhite:
                migrated.parameters = BlackAndWhiteParameters {};
                break;
            case AdjustmentType::GradientMap:
                migrated.parameters = GradientMapParameters {};
                break;
            case AdjustmentType::Posterise:
                migrated.parameters = PosteriseParameters {};
                break;
            case AdjustmentType::Threshold:
                migrated.parameters = ThresholdParameters {};
                break;
            case AdjustmentType::Lut:
                migrated.parameters = LutParameters {};
                break;
            case AdjustmentType::ShadowsHighlights:
                migrated.parameters = ShadowsHighlightsParameters {};
                break;
            case AdjustmentType::GaussianBlur:
                migrated.parameters = GaussianBlurParameters {};
                break;
            case AdjustmentType::BoxBlur:
                migrated.parameters = BoxBlurParameters {};
                break;
            case AdjustmentType::UnsharpMask:
                migrated.parameters = UnsharpMaskParameters {};
                break;
            case AdjustmentType::HighPass:
                migrated.parameters = HighPassParameters {};
                break;
            case AdjustmentType::Invert:
                migrated.parameters = InvertParameters {};
                break;
            case AdjustmentType::PhotoFilter:
                migrated.parameters = PhotoFilterParameters {};
                break;
            case AdjustmentType::SelectiveColour:
                migrated.parameters = SelectiveColourParameters {};
                break;
            case AdjustmentType::Vignette:
                migrated.parameters = VignetteParameters {};
                break;
            case AdjustmentType::RgbSplit:
                migrated.parameters = RgbSplitParameters {};
                break;
            case AdjustmentType::ChromaticAberrationCorrection:
                migrated.parameters = ChromaticAberrationCorrectionParameters {};
                break;
            case AdjustmentType::SurfaceBlur:
                migrated.parameters = SurfaceBlurParameters {};
                break;
            case AdjustmentType::MotionBlur:
                migrated.parameters = MotionBlurParameters {};
                break;
            case AdjustmentType::RadialBlur:
                migrated.parameters = RadialBlurParameters {};
                break;
            }
            layer.setAdjustmentData(migrated);
        }
    }

    if (layer.type == LayerType::Vector) {
        const QJsonValue vectorValue = object.value(QStringLiteral("vector"));
        bool vectorOk = false;
        layer.vectorData = VectorLayerData::fromJson(vectorValue.toObject(), &vectorOk);
        if (!vectorValue.isObject() || !vectorOk) {
            if (ok) *ok = false;
            return layer;
        }
    }

    if (layer.type == LayerType::Text) {
        const QJsonValue textValue = object.value(QStringLiteral("text"));
        bool textOk = false;
        layer.textData = TextLayerData::fromJson(textValue.toObject(), &textOk);
        if (!textValue.isObject() || !textOk) {
            if (ok) *ok = false;
            return layer;
        }
    }

    if (layer.type == LayerType::Smart) {
        const QJsonValue smartValue = object.value(QStringLiteral("smartSource"));
        bool smartOk = false;
        layer.smartSource = SmartLayerReference::fromJson(smartValue.toObject(), &smartOk);
        if (!smartValue.isObject() || !smartOk) {
            if (ok) *ok = false;
            return layer;
        }
        const QJsonValue transformValue = object.value(QStringLiteral("smartTransform"));
        if (transformValue.isUndefined()) {
            // 0.14.0a-c migration: those releases always sampled Smart
            // presentations with the compositor's bilinear path.
            layer.smartTransform = SmartTransformState {};
        } else {
            bool transformOk = false;
            layer.smartTransform = SmartTransformState::fromJson(
                transformValue.toObject(), &transformOk);
            if (!transformValue.isObject() || !transformOk) {
                if (ok) *ok = false;
                return layer;
            }
        }
        const QJsonValue filtersValue = object.value(QStringLiteral("liveFilters"));
        if (!filtersValue.isUndefined()) {
            if (!filtersValue.isArray()
                || filtersValue.toArray().size() > LayerNode::MaximumLiveFilterCount) {
                if (ok) *ok = false;
                return layer;
            }
            QSet<QUuid> filterIds;
            for (const QJsonValue &value : filtersValue.toArray()) {
                if (!value.isObject()) {
                    if (ok) *ok = false;
                    return layer;
                }
                bool filterOk = false;
                LiveFilter filter = LiveFilter::fromJson(value.toObject(), &filterOk);
                if (!filterOk || filterIds.contains(filter.id)) {
                    if (ok) *ok = false;
                    return layer;
                }
                filterIds.insert(filter.id);
                layer.liveFilters.push_back(std::move(filter));
            }
        }
    } else if (!object.value(QStringLiteral("smartTransform")).isUndefined()
               || !object.value(QStringLiteral("liveFilters")).isUndefined()) {
        if (ok) *ok = false;
        return layer;
    }

    const QJsonValue effectsValue = object.value(QStringLiteral("layerEffects"));
    if (!effectsValue.isUndefined()) {
        if (!layerTypeSupportsLayerEffects(layer.type) || !effectsValue.isArray()
            || effectsValue.toArray().size() > LayerNode::MaximumLayerEffectCount) {
            if (ok) *ok = false;
            return layer;
        }
        QSet<QUuid> effectIds;
        for (const QJsonValue &value : effectsValue.toArray()) {
            if (!value.isObject()) {
                if (ok) *ok = false;
                return layer;
            }
            bool effectOk = false;
            LayerEffect effect = LayerEffect::fromJson(value.toObject(), &effectOk);
            if (!effectOk || effectIds.contains(effect.id)) {
                if (ok) *ok = false;
                return layer;
            }
            effectIds.insert(effect.id);
            layer.layerEffects.push_back(std::move(effect));
        }
    }

    if (layer.type == LayerType::Raster || layer.type == LayerType::BaseImage) {
        const QJsonValue rasterDataValue = object.value(QStringLiteral("rasterData"));
        const QJsonValue rasterEncodingValue = object.value(QStringLiteral("rasterEncoding"));
        const QString rasterEncoding = rasterEncodingValue.toString();
        bool imageOk = (rasterDataValue.isUndefined() || rasterDataValue.isString())
            && (rasterEncodingValue.isUndefined() || rasterEncodingValue.isString());
        if (!rasterEncoding.isEmpty()
            && rasterEncoding.compare(QStringLiteral("png-base64"), Qt::CaseInsensitive) != 0) {
            imageOk = false;
        }
        layer.rasterImage = imageOk
            ? decodeImage(rasterDataValue.toString(), &imageOk)
            : QImage();
        if (!imageOk) {
            if (ok) {
                *ok = false;
            }
            return layer;
        }
    }

    const QJsonValue maskDataValue = object.value(QStringLiteral("maskData"));
    const QJsonValue maskEncodingValue = object.value(QStringLiteral("maskEncoding"));
    const QString maskEncoding = maskEncodingValue.toString();
    bool maskOk = (maskDataValue.isUndefined() || maskDataValue.isString())
        && (maskEncodingValue.isUndefined() || maskEncodingValue.isString());
    if (!maskEncoding.isEmpty()
        && maskEncoding.compare(QStringLiteral("png-base64"), Qt::CaseInsensitive) != 0) {
        maskOk = false;
    }
    layer.maskImage = maskOk
        ? decodeImage(maskDataValue.toString(), &maskOk)
        : QImage();
    if (!maskOk) {
        // A damaged optional mask should not make the entire project
        // unrecoverable. Discard only that mask and allow the caller to show
        // one aggregated warning after the rest of the layer tree has loaded.
        layer.maskImage = {};
        if (warnings) {
            warnings->push_back(
                QStringLiteral("The mask on layer ‘%1’ was damaged and was discarded.")
                    .arg(layer.name));
        }
    }
    if (!layer.maskImage.isNull()) {
        const QJsonValue enabledValue = object.value(QStringLiteral("maskEnabled"));
        const QJsonValue invertedValue = object.value(QStringLiteral("maskInverted"));
        if (!enabledValue.isUndefined() && !enabledValue.isBool() && warnings) {
            warnings->push_back(
                QStringLiteral("The enabled state for the mask on layer ‘%1’ was invalid and was reset.")
                    .arg(layer.name));
        }
        if (!invertedValue.isUndefined() && !invertedValue.isBool() && warnings) {
            warnings->push_back(
                QStringLiteral("The inverted state for the mask on layer ‘%1’ was invalid and was reset.")
                    .arg(layer.name));
        }
        layer.maskEnabled = enabledValue.toBool(true);
        layer.maskInverted = invertedValue.toBool(false);
    } else {
        layer.maskEnabled = true;
        layer.maskInverted = false;
    }

    if (layer.type == LayerType::Group) {
        const QJsonValue childrenValue = object.value(QStringLiteral("children"));
        if (!childrenValue.isUndefined() && !childrenValue.isArray()) {
            if (ok) *ok = false;
            return layer;
        }
        const QJsonArray childArray = childrenValue.toArray();
        if (childArray.size() > *remainingLayers) {
            if (ok) *ok = false;
            return layer;
        }
        layer.children.reserve(childArray.size());
        for (const QJsonValue &value : childArray) {
            if (!value.isObject()) {
                if (ok) {
                    *ok = false;
                }
                return layer;
            }
            bool childOk = false;
            LayerNode child = layerNodeFromJson(value.toObject(), &childOk, warnings,
                                                    depth + 1, remainingLayers);
            if (!childOk) {
                if (ok) {
                    *ok = false;
                }
                return layer;
            }
            layer.children.push_back(std::move(child));
        }
    }

    if (ok) {
        *ok = true;
    }
    return layer;
}

LayerNode LayerNode::fromJson(const QJsonObject &object,
                              bool *ok,
                              QStringList *warnings)
{
    int remainingLayers = MaximumTreeLayerCount;
    return layerNodeFromJson(object, ok, warnings, 0, &remainingLayers);
}

} // namespace vfx
