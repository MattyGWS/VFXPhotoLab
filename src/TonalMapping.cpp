#include "TonalMapping.h"

#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>

namespace vfx {
namespace {

double clamp01(const double value)
{
    return std::clamp(value, 0.0, 1.0);
}

double applyLevels(const double input, const LevelsChannelParameters &parameters)
{
    const double black = clamp01(parameters.inputBlack);
    const double white = std::max(black + 1.0e-12, clamp01(parameters.inputWhite));
    const double normalised = clamp01((input - black) / (white - black));
    const double corrected = clamp01(std::pow(normalised, 1.0 / std::max(0.01, parameters.gamma)));
    return clamp01(clamp01(parameters.outputBlack)
                   + corrected * (clamp01(parameters.outputWhite)
                                  - clamp01(parameters.outputBlack)));
}

QVector<double> curveTangents(const QVector<CurvePoint> &points)
{
    QVector<double> tangent(points.size(), 0.0);
    if (points.size() < 2) return tangent;
    QVector<double> secant(points.size() - 1, 0.0);
    QVector<double> width(points.size() - 1, 0.0);
    for (int index = 0; index + 1 < points.size(); ++index) {
        width[index] = std::max(1.0e-12, points[index + 1].input - points[index].input);
        secant[index] = (points[index + 1].output - points[index].output) / width[index];
    }
    tangent[0] = secant[0];
    tangent[tangent.size() - 1] = secant[secant.size() - 1];
    for (int index = 1; index + 1 < points.size(); ++index) {
        const double left = secant[index - 1];
        const double right = secant[index];
        if (left == 0.0 || right == 0.0 || (left < 0.0) != (right < 0.0)) {
            tangent[index] = 0.0;
            continue;
        }
        const double w1 = 2.0 * width[index] + width[index - 1];
        const double w2 = width[index] + 2.0 * width[index - 1];
        tangent[index] = (w1 + w2) / (w1 / left + w2 / right);
    }
    return tangent;
}

struct PreparedCurve {
    QVector<CurvePoint> points;
    QVector<double> tangents;
};

PreparedCurve prepareCurve(const CurveChannelParameters &input)
{
    CurveChannelParameters safe = input;
    safe.normalise();
    return {safe.points, curveTangents(safe.points)};
}

std::array<double, 3> evaluatePreparedGradient(const GradientMapParameters &parameters,
                                               const double input)
{
    const double source = parameters.reverse ? 1.0 - clamp01(input) : clamp01(input);
    const QVector<GradientStop> &stops = parameters.stops;
    const auto components = [](const QColor &colour) {
        return std::array<double, 3> {colour.redF(), colour.greenF(), colour.blueF()};
    };
    if (source <= stops.first().position) return components(stops.first().colour);
    if (source >= stops.last().position) return components(stops.last().colour);
    int rightIndex = 1;
    while (rightIndex < static_cast<int>(stops.size()) && stops[rightIndex].position < source) ++rightIndex;
    const GradientStop &left = stops[std::max(0, rightIndex - 1)];
    const GradientStop &right = stops[std::min(rightIndex, static_cast<int>(stops.size()) - 1)];
    const double width = std::max(1.0e-12, right.position - left.position);
    double t = clamp01((source - left.position) / width);
    if (parameters.interpolation == GradientInterpolation::Constant) {
        // A constant segment keeps the left colour until the next stop and
        // switches to that stop's colour exactly at its position.
        t = source >= right.position ? 1.0 : 0.0;
    }
    else if (parameters.interpolation == GradientInterpolation::Smooth) t = t * t * (3.0 - 2.0 * t);
    const auto a = components(left.colour);
    const auto b = components(right.colour);
    return {a[0] + (b[0] - a[0]) * t,
            a[1] + (b[1] - a[1]) * t,
            a[2] + (b[2] - a[2]) * t};
}

double evaluatePreparedCurve(const PreparedCurve &curve,
                             const CurveInterpolation interpolation,
                             const double input)
{
    const QVector<CurvePoint> &points = curve.points;
    const double x = clamp01(input);
    if (x <= points.first().input) return points.first().output;
    if (x >= points.last().input) return points.last().output;
    int rightIndex = 1;
    while (rightIndex < points.size() && points[rightIndex].input < x) ++rightIndex;
    const int leftIndex = std::max(0, rightIndex - 1);
    const CurvePoint &left = points[leftIndex];
    const CurvePoint &right = points[rightIndex];
    const double width = std::max(1.0e-12, right.input - left.input);
    const double t = clamp01((x - left.input) / width);
    if (interpolation == CurveInterpolation::Linear) {
        return clamp01(left.output + (right.output - left.output) * t);
    }
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10 = t3 - 2.0 * t2 + t;
    const double h01 = -2.0 * t3 + 3.0 * t2;
    const double h11 = t3 - t2;
    return clamp01(h00 * left.output
                   + h10 * width * curve.tangents[leftIndex]
                   + h01 * right.output
                   + h11 * width * curve.tangents[rightIndex]);
}

struct TonalCacheEntry {
    AdjustmentData adjustment;
    int bitDepth = 8;
    TonalLookupTable table;
    quint64 lastUse = 0;
};

QMutex &tonalCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QVector<TonalCacheEntry> &tonalCacheEntries()
{
    static QVector<TonalCacheEntry> entries;
    return entries;
}

quint64 &tonalCacheUseSerial()
{
    static quint64 serial = 0;
    return serial;
}

constexpr qsizetype MaximumTonalCacheBytes = 16 * 1024 * 1024;
constexpr qsizetype MaximumTonalCacheEntries = 32;

TonalLookupTable buildUncachedTonalLookup(const AdjustmentData &adjustment,
                                          const int bitDepth)
{
    TonalLookupTable table;
    table.maximumValue = bitDepth > 8 ? 65535 : 255;
    const int count = table.maximumValue + 1;
    for (QVector<quint16> &channel : table.channels) channel.resize(count);

    if (!adjustmentUsesTonalLookup(adjustment.type)
        && !adjustmentUsesLuminanceLookup(adjustment.type)) {
        for (int value = 0; value < count; ++value) {
            for (QVector<quint16> &channel : table.channels) {
                channel[value] = static_cast<quint16>(value);
            }
        }
        if (table.maximumValue == 255) (void)table.toRgba8Image();
        return table;
    }

    std::array<PreparedCurve, 4> preparedCurves;
    GradientMapParameters preparedGradient;
    if (adjustment.type == AdjustmentType::Curves) {
        const CurvesParameters &curves = std::get<CurvesParameters>(adjustment.parameters);
        for (int index = 0; index < 4; ++index) {
            preparedCurves[static_cast<std::size_t>(index)] = prepareCurve(
                curves.channel(static_cast<AdjustmentChannel>(index)));
        }
    } else if (adjustment.type == AdjustmentType::GradientMap) {
        preparedGradient = std::get<GradientMapParameters>(adjustment.parameters);
        preparedGradient.normalise();
    }

    for (int value = 0; value < count; ++value) {
        const double input = value / static_cast<double>(table.maximumValue);
        if (adjustment.type == AdjustmentType::Levels) {
            const LevelsParameters &levels = std::get<LevelsParameters>(adjustment.parameters);
            const double master = applyLevels(input, levels.channel(AdjustmentChannel::Rgb));
            table.channels[0][value] = static_cast<quint16>(std::lround(
                applyLevels(master, levels.channel(AdjustmentChannel::Red)) * table.maximumValue));
            table.channels[1][value] = static_cast<quint16>(std::lround(
                applyLevels(master, levels.channel(AdjustmentChannel::Green)) * table.maximumValue));
            table.channels[2][value] = static_cast<quint16>(std::lround(
                applyLevels(master, levels.channel(AdjustmentChannel::Blue)) * table.maximumValue));
        } else if (adjustment.type == AdjustmentType::Curves) {
            const CurvesParameters &curves = std::get<CurvesParameters>(adjustment.parameters);
            const double master = evaluatePreparedCurve(
                preparedCurves[0], curves.interpolation, input);
            table.channels[0][value] = static_cast<quint16>(std::lround(
                evaluatePreparedCurve(preparedCurves[1], curves.interpolation, master)
                    * table.maximumValue));
            table.channels[1][value] = static_cast<quint16>(std::lround(
                evaluatePreparedCurve(preparedCurves[2], curves.interpolation, master)
                    * table.maximumValue));
            table.channels[2][value] = static_cast<quint16>(std::lround(
                evaluatePreparedCurve(preparedCurves[3], curves.interpolation, master)
                    * table.maximumValue));
        } else {
            const auto colour = evaluatePreparedGradient(preparedGradient, input);
            table.channels[0][value] = static_cast<quint16>(std::lround(
                clamp01(colour[0]) * table.maximumValue));
            table.channels[1][value] = static_cast<quint16>(std::lround(
                clamp01(colour[1]) * table.maximumValue));
            table.channels[2][value] = static_cast<quint16>(std::lround(
                clamp01(colour[2]) * table.maximumValue));
        }
    }
    if (table.maximumValue == 255) (void)table.toRgba8Image();
    return table;
}

} // namespace

bool TonalLookupTable::isValid() const
{
    if (maximumValue <= 0) return false;
    const qsizetype expected = static_cast<qsizetype>(maximumValue) + 1;
    return channels[0].size() == expected
        && channels[1].size() == expected
        && channels[2].size() == expected;
}

quint16 TonalLookupTable::map(const int component, const int value) const
{
    if (!isValid() || component < 0 || component > 2) return 0;
    return channels[static_cast<std::size_t>(component)].at(
        std::clamp(value, 0, maximumValue));
}

QImage TonalLookupTable::toRgba8Image() const
{
    if (!isValid() || maximumValue != 255) return {};
    if (!m_rgba8Image.isNull()) return m_rgba8Image;
    QImage image(256, 1, QImage::Format_RGBA8888);
    if (image.isNull()) return {};
    uchar *row = image.scanLine(0);
    for (int value = 0; value < 256; ++value) {
        row[value * 4] = static_cast<uchar>(channels[0].at(value));
        row[value * 4 + 1] = static_cast<uchar>(channels[1].at(value));
        row[value * 4 + 2] = static_cast<uchar>(channels[2].at(value));
        row[value * 4 + 3] = 255;
    }
    m_rgba8Image = image;
    return m_rgba8Image;
}

qsizetype TonalLookupTable::retainedBytes() const
{
    qsizetype bytes = 0;
    for (const QVector<quint16> &channel : channels) {
        bytes += channel.size() * static_cast<qsizetype>(sizeof(quint16));
    }
    if (!m_rgba8Image.isNull()) bytes += m_rgba8Image.sizeInBytes();
    return bytes;
}

double evaluateCurveChannel(const CurveChannelParameters &channel,
                            const CurveInterpolation interpolation,
                            const double input)
{
    return evaluatePreparedCurve(prepareCurve(channel), interpolation, input);
}

bool adjustmentUsesTonalLookup(const AdjustmentType type)
{
    return type == AdjustmentType::Levels || type == AdjustmentType::Curves;
}

bool adjustmentUsesLuminanceLookup(const AdjustmentType type)
{
    return type == AdjustmentType::GradientMap;
}

TonalLookupTable buildTonalLookup(const AdjustmentData &inputAdjustment,
                                  const int bitDepth)
{
    AdjustmentData adjustment = inputAdjustment;
    adjustment.normalise();
    const int normalisedBitDepth = bitDepth > 8 ? 16 : 8;

    {
        QMutexLocker locker(&tonalCacheMutex());
        QVector<TonalCacheEntry> &entries = tonalCacheEntries();
        for (TonalCacheEntry &entry : entries) {
            if (entry.bitDepth == normalisedBitDepth
                && entry.adjustment == adjustment) {
                entry.lastUse = ++tonalCacheUseSerial();
                return entry.table;
            }
        }
    }

    TonalLookupTable built = buildUncachedTonalLookup(adjustment, normalisedBitDepth);
    if (!built.isValid()) return built;

    QMutexLocker locker(&tonalCacheMutex());
    QVector<TonalCacheEntry> &entries = tonalCacheEntries();
    for (TonalCacheEntry &entry : entries) {
        if (entry.bitDepth == normalisedBitDepth
            && entry.adjustment == adjustment) {
            entry.lastUse = ++tonalCacheUseSerial();
            return entry.table;
        }
    }
    entries.push_back({adjustment, normalisedBitDepth, built, ++tonalCacheUseSerial()});

    auto retainedBytes = [&entries] {
        qsizetype bytes = 0;
        for (const TonalCacheEntry &entry : entries) bytes += entry.table.retainedBytes();
        return bytes;
    };
    while (entries.size() > MaximumTonalCacheEntries
           || retainedBytes() > MaximumTonalCacheBytes) {
        auto oldest = std::min_element(entries.begin(), entries.end(),
                                       [](const TonalCacheEntry &left,
                                          const TonalCacheEntry &right) {
            return left.lastUse < right.lastUse;
        });
        if (oldest == entries.end()) break;
        entries.erase(oldest);
    }
    return built;
}

} // namespace vfx
