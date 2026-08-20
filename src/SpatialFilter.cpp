#include "SpatialFilter.h"

#include <QColorSpace>
#include <QRgba64>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace vfx {
namespace {

constexpr quint64 FnvOffset = UINT64_C(14695981039346656037);
constexpr quint64 FnvPrime = UINT64_C(1099511628211);

void hashBytes(quint64 &hash, const void *data, const qsizetype size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (qsizetype index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= FnvPrime;
    }
}

template <typename T>
void hashValue(quint64 &hash, const T &value)
{
    hashBytes(hash, &value, sizeof(value));
}

quint64 nonZero(const quint64 value)
{
    return value == 0 ? 1 : value;
}

qint64 pixelCount(const QSize &size)
{
    if (size.width() <= 0 || size.height() <= 0) return 0;
    if (size.width() > std::numeric_limits<qint64>::max() / size.height()) {
        return std::numeric_limits<qint64>::max();
    }
    return static_cast<qint64>(size.width()) * size.height();
}

bool cancelled(const std::atomic_bool *cancelRequested)
{
    return cancelRequested
        && cancelRequested->load(std::memory_order_relaxed);
}

qint64 positiveModulo(const qint64 value, const qint64 modulus)
{
    if (modulus <= 0) return 0;
    const qint64 remainder = value % modulus;
    return remainder < 0 ? remainder + modulus : remainder;
}

QVector<QRect> translatedIntersections(const QRect &rect,
                                       const QRect &extentRect,
                                       const QSize &period)
{
    QVector<QRect> result;
    if (rect.isEmpty() || extentRect.isEmpty() || period.isEmpty()) return result;
    // A footprint spanning a complete period can depend on every source pixel.
    // Returning the full extent is both exact for invalidation and bounded.
    if (rect.width() >= period.width() || rect.height() >= period.height()
        || rect.left() < -period.width() || rect.top() < -period.height()
        || rect.right() >= period.width() * 2
        || rect.bottom() >= period.height() * 2) {
        return {extentRect};
    }
    for (const int dy : {-period.height(), 0, period.height()}) {
        for (const int dx : {-period.width(), 0, period.width()}) {
            const QRect candidate = rect.translated(dx, dy).intersected(extentRect);
            if (candidate.isEmpty()) continue;
            bool duplicate = false;
            for (const QRect &existing : result) {
                if (existing == candidate) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) result.push_back(candidate);
        }
    }
    return result;
}

QRect united(const QVector<QRect> &regions)
{
    QRect bounds;
    for (const QRect &region : regions) {
        bounds = bounds.isNull() ? region : bounds.united(region);
    }
    return bounds;
}

struct FloatPlanes {
    QVector<float> values;
    int channels = 0;
    int width = 0;
    int height = 0;

    float &at(const int x, const int y, const int channel)
    {
        return values[(static_cast<qsizetype>(y) * width + x) * channels + channel];
    }
    float at(const int x, const int y, const int channel) const
    {
        return values[(static_cast<qsizetype>(y) * width + x) * channels + channel];
    }
};

bool blurPlanesBox(FloatPlanes *planes,
                   const QSize radius,
                   const SpatialEdgeMode edgeMode,
                   const std::atomic_bool *cancelRequested,
                   const SpatialRowProcessor &rowProcessor)
{
    try {
        if (!planes || planes->width <= 0 || planes->height <= 0
            || planes->channels <= 0 || planes->values.isEmpty()) {
            return false;
        }
        const int radiusX = std::max(0, radius.width());
        const int radiusY = std::max(0, radius.height());
        if (radiusX == 0 && radiusY == 0) return true;

        const qsizetype valueCount = planes->values.size();
        if (valueCount > SpatialFilterFoundation::MaximumReferenceWorkingBytes
                             / qsizetype(sizeof(float))) {
            return false;
        }
        QVector<float> horizontal(valueCount);
        if (horizontal.size() != valueCount) return false;

        // Force both implicitly-shared buffers to detach before workers write
        // disjoint ranges. Calling QVector::operator[] for the first time from
        // several threads can otherwise race the one-time detach bookkeeping.
        float *const destinationValues = planes->values.data();
        const float *const sourceValues = planes->values.constData();
        float *const horizontalValues = horizontal.data();
        const int width = planes->width;
        const int height = planes->height;
        const int channels = planes->channels;
        if (channels > 4) return false;

        const auto runRows = [&](const int rowWidth,
                                 const int rowHeight,
                                 const std::function<void(int)> &processRow) {
            if (rowProcessor) {
                rowProcessor(rowWidth, rowHeight, processRow);
            } else {
                for (int row = 0; row < rowHeight; ++row) processRow(row);
            }
        };

        const int horizontalCount = radiusX * 2 + 1;
        runRows(width, height, [&](const int y) {
            if (cancelled(cancelRequested)) return;
            std::array<double, 4> sums {};
            for (int sample = -radiusX; sample <= radiusX; ++sample) {
                bool inside = true;
                const int mapped = SpatialFilterFoundation::mappedCoordinate(
                    sample, width, edgeMode, &inside);
                if (!inside) continue;
                const qsizetype sampleBase =
                    (static_cast<qsizetype>(y) * width + mapped) * channels;
                for (int channel = 0; channel < channels; ++channel) {
                    sums[channel] += sourceValues[sampleBase + channel];
                }
            }
            for (int x = 0; x < width; ++x) {
                const qsizetype base =
                    (static_cast<qsizetype>(y) * width + x) * channels;
                for (int channel = 0; channel < channels; ++channel) {
                    horizontalValues[base + channel] = static_cast<float>(
                        sums[channel] / horizontalCount);
                }
                if (x + 1 >= width) continue;
                bool removeInside = true;
                bool addInside = true;
                const int removeX = SpatialFilterFoundation::mappedCoordinate(
                    x - radiusX, width, edgeMode, &removeInside);
                const int addX = SpatialFilterFoundation::mappedCoordinate(
                    x + radiusX + 1, width, edgeMode, &addInside);
                const qsizetype removeBase =
                    (static_cast<qsizetype>(y) * width + removeX) * channels;
                const qsizetype addBase =
                    (static_cast<qsizetype>(y) * width + addX) * channels;
                for (int channel = 0; channel < channels; ++channel) {
                    if (removeInside) sums[channel] -= sourceValues[removeBase + channel];
                    if (addInside) sums[channel] += sourceValues[addBase + channel];
                }
            }
        });
        if (cancelled(cancelRequested)) return false;

        // Columns are independent for the vertical sliding pass. Feed them to
        // the same bounded row processor by swapping the logical dimensions;
        // the pixel threshold therefore remains based on the complete image.
        const int verticalCount = radiusY * 2 + 1;
        const float *const horizontalRead = horizontal.constData();
        runRows(height, width, [&](const int x) {
            if (cancelled(cancelRequested)) return;
            std::array<double, 4> sums {};
            for (int sample = -radiusY; sample <= radiusY; ++sample) {
                bool inside = true;
                const int mapped = SpatialFilterFoundation::mappedCoordinate(
                    sample, height, edgeMode, &inside);
                if (!inside) continue;
                const qsizetype base =
                    (static_cast<qsizetype>(mapped) * width + x) * channels;
                for (int channel = 0; channel < channels; ++channel) {
                    sums[channel] += horizontalRead[base + channel];
                }
            }
            for (int y = 0; y < height; ++y) {
                const qsizetype base =
                    (static_cast<qsizetype>(y) * width + x) * channels;
                for (int channel = 0; channel < channels; ++channel) {
                    destinationValues[base + channel] = static_cast<float>(
                        sums[channel] / verticalCount);
                }
                if (y + 1 >= height) continue;
                bool removeInside = true;
                bool addInside = true;
                const int removeY = SpatialFilterFoundation::mappedCoordinate(
                    y - radiusY, height, edgeMode, &removeInside);
                const int addY = SpatialFilterFoundation::mappedCoordinate(
                    y + radiusY + 1, height, edgeMode, &addInside);
                const qsizetype removeBase =
                    (static_cast<qsizetype>(removeY) * width + x) * channels;
                const qsizetype addBase =
                    (static_cast<qsizetype>(addY) * width + x) * channels;
                for (int channel = 0; channel < channels; ++channel) {
                    if (removeInside) sums[channel] -= horizontalRead[removeBase + channel];
                    if (addInside) sums[channel] += horizontalRead[addBase + channel];
                }
            }
        });
        return !cancelled(cancelRequested);
    } catch (const std::bad_alloc &) {
        return false;
    }
}

float clampUnit(const float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace

void SpatialFilterContract::normalise()
{
    version = CurrentVersion;
    maximumRadius = std::clamp(maximumRadius, 0, DefaultMaximumRadius);
    safetyPadding = std::clamp(safetyPadding, 0, 64);
    documentRadius.setWidth(std::clamp(documentRadius.width(), 0, maximumRadius));
    documentRadius.setHeight(std::clamp(documentRadius.height(), 0, maximumRadius));
}

bool SpatialFilterContract::isIdentity() const
{
    return documentRadius.width() <= 0 && documentRadius.height() <= 0;
}

quint64 SpatialFilterContract::fingerprint() const
{
    SpatialFilterContract normalised = *this;
    normalised.normalise();
    quint64 hash = FnvOffset;
    hashValue(hash, normalised.version);
    hashValue(hash, normalised.documentRadius.width());
    hashValue(hash, normalised.documentRadius.height());
    hashValue(hash, normalised.edgeMode);
    hashValue(hash, normalised.alphaMode);
    hashValue(hash, normalised.quality);
    hashValue(hash, normalised.safetyPadding);
    hashValue(hash, normalised.maximumRadius);
    hashValue(hash, normalised.deterministic);
    return nonZero(hash);
}

SpatialFilterGpuContract SpatialFilterTilePlan::gpuContract(
    const SpatialFilterContract &input,
    const QSize &sourceExtent) const
{
    SpatialFilterContract contract = input;
    contract.normalise();
    SpatialFilterGpuContract gpu;
    gpu.sourceWidth = static_cast<quint32>(std::max(0, sourceExtent.width()));
    gpu.sourceHeight = static_cast<quint32>(std::max(0, sourceExtent.height()));
    gpu.outputOriginX = outputRect.x();
    gpu.outputOriginY = outputRect.y();
    gpu.outputWidth = static_cast<quint32>(std::max(0, outputRect.width()));
    gpu.outputHeight = static_cast<quint32>(std::max(0, outputRect.height()));
    gpu.radiusX = static_cast<quint32>(std::max(0, scaledRadius.width()));
    gpu.radiusY = static_cast<quint32>(std::max(0, scaledRadius.height()));
    gpu.edgeMode = static_cast<quint32>(contract.edgeMode);
    gpu.alphaMode = static_cast<quint32>(contract.alphaMode);
    gpu.quality = static_cast<quint32>(contract.quality);
    gpu.flags = (contract.deterministic ? 1u : 0u)
        | (contract.safetyPadding > 0 ? 2u : 0u);
    gpu.samplingOriginX = samplingRect.x();
    gpu.samplingOriginY = samplingRect.y();
    gpu.samplingWidth = static_cast<quint32>(std::max(0, samplingRect.width()));
    gpu.samplingHeight = static_cast<quint32>(std::max(0, samplingRect.height()));
    return gpu;
}

QString SpatialFilterFoundation::edgeModeName(const SpatialEdgeMode mode)
{
    switch (mode) {
    case SpatialEdgeMode::Clamp: return QStringLiteral("Clamp");
    case SpatialEdgeMode::Mirror: return QStringLiteral("Mirror");
    case SpatialEdgeMode::Wrap: return QStringLiteral("Wrap");
    case SpatialEdgeMode::Transparent: return QStringLiteral("Transparent");
    }
    return QStringLiteral("Clamp");
}

QString SpatialFilterFoundation::alphaModeName(const SpatialAlphaMode mode)
{
    switch (mode) {
    case SpatialAlphaMode::StraightRgba: return QStringLiteral("Straight RGBA");
    case SpatialAlphaMode::PreserveSourceAlpha: return QStringLiteral("Preserve source alpha");
    case SpatialAlphaMode::CoverageAwareRgba: return QStringLiteral("Coverage-aware RGBA");
    }
    return QStringLiteral("Preserve source alpha");
}

QSize SpatialFilterFoundation::scaledRadius(const QSize &documentRadius,
                                            const QSize &previewExtent,
                                            const QSize &documentExtent,
                                            const int maximumRadius)
{
    if (previewExtent.isEmpty() || documentExtent.isEmpty() || maximumRadius <= 0) {
        return {};
    }
    const double scaleX = previewExtent.width()
        / static_cast<double>(std::max(1, documentExtent.width()));
    const double scaleY = previewExtent.height()
        / static_cast<double>(std::max(1, documentExtent.height()));
    const int radiusX = std::clamp(
        static_cast<int>(std::ceil(std::max(0, documentRadius.width()) * scaleX)),
        0, maximumRadius);
    const int radiusY = std::clamp(
        static_cast<int>(std::ceil(std::max(0, documentRadius.height()) * scaleY)),
        0, maximumRadius);
    return QSize(radiusX, radiusY);
}

SpatialFilterTilePlan SpatialFilterFoundation::plan(
    const QRect &requestedOutput,
    const QSize &previewExtent,
    const QSize &documentExtent,
    SpatialFilterContract contract)
{
    SpatialFilterTilePlan result;
    const int requestedMaximum = std::clamp(
        contract.maximumRadius, 0, SpatialFilterContract::DefaultMaximumRadius);
    if (contract.documentRadius.width() > requestedMaximum
        || contract.documentRadius.height() > requestedMaximum) {
        result.failureReason = QStringLiteral(
            "The requested spatial radius exceeds the 4096-pixel safety limit");
        return result;
    }
    contract.normalise();
    if (previewExtent.isEmpty()) {
        result.failureReason = QStringLiteral("The preview extent is empty");
        return result;
    }
    const QRect extentRect(QPoint(0, 0), previewExtent);
    result.outputRect = requestedOutput.intersected(extentRect);
    if (result.outputRect.isEmpty()) {
        result.failureReason = QStringLiteral("The output region does not intersect the preview extent");
        return result;
    }
    const QSize reference = documentExtent.isEmpty() ? previewExtent : documentExtent;
    result.scaleX = previewExtent.width()
        / static_cast<double>(std::max(1, reference.width()));
    result.scaleY = previewExtent.height()
        / static_cast<double>(std::max(1, reference.height()));
    result.scaledRadius = scaledRadius(contract.documentRadius,
                                       previewExtent,
                                       reference,
                                       contract.maximumRadius);
    if (result.scaledRadius.width() > 0) {
        result.scaledRadius.rwidth() = std::min(
            contract.maximumRadius, result.scaledRadius.width() + contract.safetyPadding);
    }
    if (result.scaledRadius.height() > 0) {
        result.scaledRadius.rheight() = std::min(
            contract.maximumRadius, result.scaledRadius.height() + contract.safetyPadding);
    }

    const int rx = result.scaledRadius.width();
    const int ry = result.scaledRadius.height();
    result.samplingRect = result.outputRect.adjusted(-rx, -ry, rx, ry);
    result.cropOffset = result.outputRect.topLeft() - result.samplingRect.topLeft();
    result.dependencyRegions = dependencyRegions(result.outputRect,
                                                  previewExtent,
                                                  result.scaledRadius,
                                                  contract.edgeMode);
    result.dependencyBounds = united(result.dependencyRegions);

    const qint64 pixels = pixelCount(result.samplingRect.size());
    const qint64 bytesPerPixel = 8;
    if (pixels <= 0 || pixels > MaximumHaloBytes / bytesPerPixel) {
        result.failureReason = QStringLiteral(
            "The requested spatial halo exceeds the 512 MiB safety limit");
        return result;
    }

    quint64 hash = FnvOffset;
    const quint64 contractFingerprint = contract.fingerprint();
    hashValue(hash, contractFingerprint);
    hashValue(hash, previewExtent.width());
    hashValue(hash, previewExtent.height());
    hashValue(hash, reference.width());
    hashValue(hash, reference.height());
    hashValue(hash, result.outputRect.x());
    hashValue(hash, result.outputRect.y());
    hashValue(hash, result.outputRect.width());
    hashValue(hash, result.outputRect.height());
    hashValue(hash, result.scaledRadius.width());
    hashValue(hash, result.scaledRadius.height());
    for (const QRect &region : result.dependencyRegions) {
        hashValue(hash, region.x());
        hashValue(hash, region.y());
        hashValue(hash, region.width());
        hashValue(hash, region.height());
    }
    result.cacheFingerprint = nonZero(hash);
    result.valid = true;
    return result;
}

QVector<QRect> SpatialFilterFoundation::dependencyRegions(
    const QRect &outputRect,
    const QSize &extent,
    const QSize &radius,
    const SpatialEdgeMode edgeMode)
{
    const QRect extentRect(QPoint(0, 0), extent);
    if (extentRect.isEmpty()) return {};
    const QRect sampling = outputRect.adjusted(-std::max(0, radius.width()),
                                               -std::max(0, radius.height()),
                                               std::max(0, radius.width()),
                                               std::max(0, radius.height()));
    if (sampling.isEmpty()) return {};
    if (edgeMode == SpatialEdgeMode::Wrap) {
        return translatedIntersections(sampling, extentRect, extent);
    }
    if (edgeMode == SpatialEdgeMode::Mirror && !extentRect.contains(sampling)) {
        return {extentRect};
    }
    const QRect clipped = sampling.intersected(extentRect);
    return clipped.isEmpty() ? QVector<QRect>() : QVector<QRect> {clipped};
}

QVector<QRect> SpatialFilterFoundation::affectedOutputRegions(
    const QRect &dirtyInput,
    const QSize &extent,
    const QSize &radius,
    const SpatialEdgeMode edgeMode)
{
    const QRect extentRect(QPoint(0, 0), extent);
    const QRect dirty = dirtyInput.intersected(extentRect);
    if (dirty.isEmpty()) return {};
    const QRect expanded = dirty.adjusted(-std::max(0, radius.width()),
                                          -std::max(0, radius.height()),
                                          std::max(0, radius.width()),
                                          std::max(0, radius.height()));
    if (edgeMode == SpatialEdgeMode::Wrap) {
        return translatedIntersections(expanded, extentRect, extent);
    }
    if (edgeMode == SpatialEdgeMode::Mirror && !extentRect.contains(expanded)) {
        return {extentRect};
    }
    const QRect clipped = expanded.intersected(extentRect);
    return clipped.isEmpty() ? QVector<QRect>() : QVector<QRect> {clipped};
}

int SpatialFilterFoundation::mappedCoordinate(const int coordinate,
                                              const int extent,
                                              const SpatialEdgeMode edgeMode,
                                              bool *inside)
{
    if (inside) *inside = extent > 0;
    if (extent <= 0) return 0;
    if (coordinate >= 0 && coordinate < extent) return coordinate;
    switch (edgeMode) {
    case SpatialEdgeMode::Clamp:
        return std::clamp(coordinate, 0, extent - 1);
    case SpatialEdgeMode::Wrap:
        return static_cast<int>(positiveModulo(coordinate, extent));
    case SpatialEdgeMode::Mirror: {
        if (extent == 1) return 0;
        const qint64 period = static_cast<qint64>(extent) * 2;
        const qint64 folded = positiveModulo(coordinate, period);
        return static_cast<int>(folded < extent ? folded : period - 1 - folded);
    }
    case SpatialEdgeMode::Transparent:
        if (inside) *inside = false;
        return 0;
    }
    if (inside) *inside = false;
    return 0;
}

QImage SpatialFilterFoundation::extractHalo(
    const QImage &source,
    const SpatialFilterTilePlan &plan,
    const SpatialEdgeMode edgeMode,
    const std::atomic_bool *cancelRequested)
{
    if (source.isNull() || !plan.valid || plan.samplingRect.isEmpty()) return {};
    const bool sixteenBit = source.depth() > 32;
    const QImage::Format format = sixteenBit
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    const QImage straight = source.convertToFormat(format);
    QImage halo(plan.samplingRect.size(), format);
    if (straight.isNull() || halo.isNull()
        || halo.sizeInBytes() > MaximumHaloBytes) {
        return {};
    }
    halo.fill(Qt::transparent);
    halo.setColorSpace(source.colorSpace());
    halo.setDevicePixelRatio(source.devicePixelRatio());
    halo.setDotsPerMeterX(source.dotsPerMeterX());
    halo.setDotsPerMeterY(source.dotsPerMeterY());

    if (sixteenBit) {
        for (int y = 0; y < halo.height(); ++y) {
            if (cancelled(cancelRequested)) return {};
            bool insideY = true;
            const int sourceY = mappedCoordinate(plan.samplingRect.y() + y,
                                                 straight.height(), edgeMode, &insideY);
            auto *destination = reinterpret_cast<QRgba64 *>(halo.scanLine(y));
            for (int x = 0; x < halo.width(); ++x) {
                bool insideX = true;
                const int sourceX = mappedCoordinate(plan.samplingRect.x() + x,
                                                     straight.width(), edgeMode, &insideX);
                if (!insideX || !insideY) continue;
                const auto *sourceRow = reinterpret_cast<const QRgba64 *>(
                    straight.constScanLine(sourceY));
                destination[x] = sourceRow[sourceX];
            }
        }
    } else {
        for (int y = 0; y < halo.height(); ++y) {
            if (cancelled(cancelRequested)) return {};
            bool insideY = true;
            const int sourceY = mappedCoordinate(plan.samplingRect.y() + y,
                                                 straight.height(), edgeMode, &insideY);
            uchar *destination = halo.scanLine(y);
            for (int x = 0; x < halo.width(); ++x) {
                bool insideX = true;
                const int sourceX = mappedCoordinate(plan.samplingRect.x() + x,
                                                     straight.width(), edgeMode, &insideX);
                if (!insideX || !insideY) continue;
                const uchar *sourcePixel = straight.constScanLine(sourceY) + sourceX * 4;
                std::memcpy(destination + x * 4, sourcePixel, 4);
            }
        }
    }
    return cancelled(cancelRequested) ? QImage() : halo;
}

bool SpatialFilterFoundation::blurSparseThirteenTap(
    QVector<float> *plane,
    const int width,
    const int height,
    const QSize radius,
    const SpatialEdgeMode edgeMode,
    const bool quantiseHorizontalToEightBit,
    const std::atomic_bool *cancelRequested,
    const SpatialRowProcessor &rowProcessor)
{
    try {
        if (!plane || width <= 0 || height <= 0
            || plane->size() != static_cast<qsizetype>(width) * height) {
            return false;
        }
        if (cancelled(cancelRequested)) return false;

        constexpr std::array<double, 13> weights {
            1.0, 4.0, 11.0, 25.0, 44.0, 58.0, 64.0,
            58.0, 44.0, 25.0, 11.0, 4.0, 1.0
        };
        constexpr double totalWeight = 350.0;
        std::array<int, 13> offsetsX {};
        std::array<int, 13> offsetsY {};
        for (std::size_t index = 0; index < offsetsX.size(); ++index) {
            const double centred = static_cast<double>(index) - 6.0;
            offsetsX[index] = qRound(std::max(0, radius.width()) * centred / 6.0);
            offsetsY[index] = qRound(std::max(0, radius.height()) * centred / 6.0);
        }

        QVector<float> horizontal(plane->size());
        if (horizontal.size() != plane->size()) return false;
        const auto runRows = [&](const std::function<void(int)> &processRow) {
            if (rowProcessor) {
                rowProcessor(width, height, processRow);
            } else {
                for (int y = 0; y < height; ++y) processRow(y);
            }
        };
        runRows([&](const int y) {
            if (cancelled(cancelRequested)) return;
            for (int x = 0; x < width; ++x) {
                double sum = 0.0;
                for (std::size_t index = 0; index < offsetsX.size(); ++index) {
                    bool inside = true;
                    const int sampleX = mappedCoordinate(x + offsetsX[index],
                                                         width, edgeMode, &inside);
                    if (inside) {
                        sum += plane->at(static_cast<qsizetype>(y) * width + sampleX)
                            * weights[index];
                    }
                }
                double value = sum / totalWeight;
                if (quantiseHorizontalToEightBit) {
                    value = std::round(std::clamp(value, 0.0, 1.0) * 255.0) / 255.0;
                }
                horizontal[static_cast<qsizetype>(y) * width + x] =
                    static_cast<float>(value);
            }
        });
        if (cancelled(cancelRequested)) return false;

        runRows([&](const int y) {
            if (cancelled(cancelRequested)) return;
            for (int x = 0; x < width; ++x) {
                double sum = 0.0;
                for (std::size_t index = 0; index < offsetsY.size(); ++index) {
                    bool inside = true;
                    const int sampleY = mappedCoordinate(y + offsetsY[index],
                                                         height, edgeMode, &inside);
                    if (inside) {
                        sum += horizontal[static_cast<qsizetype>(sampleY) * width + x]
                            * weights[index];
                    }
                }
                (*plane)[static_cast<qsizetype>(y) * width + x] =
                    static_cast<float>(sum / totalWeight);
            }
        });
        return !cancelled(cancelRequested);
    } catch (const std::bad_alloc &) {
        return false;
    }
}

QImage SpatialFilterFoundation::boxBlurReference(
    const QImage &source,
    QSize radius,
    const SpatialEdgeMode edgeMode,
    const SpatialAlphaMode alphaMode,
    const std::atomic_bool *cancelRequested,
    const SpatialRowProcessor &rowProcessor)
{
    if (source.isNull() || cancelled(cancelRequested)) return {};
    try {
        radius.setWidth(std::clamp(radius.width(), 0,
                                   SpatialFilterContract::DefaultMaximumRadius));
        radius.setHeight(std::clamp(radius.height(), 0,
                                    SpatialFilterContract::DefaultMaximumRadius));
        const bool sixteenBit = source.depth() > 32;
        const QImage::Format straightFormat = sixteenBit
            ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
        const QImage straight = source.convertToFormat(straightFormat);
        if (straight.isNull()) return {};
        if (radius.isNull()) return straight;

        const qint64 pixels = pixelCount(straight.size());
        // Process one component at a time. Coverage-aware filtering retains one
        // blurred Alpha plane while the current component and its horizontal
        // scratch plane are live. This bounds full-frame working memory to
        // three floats per pixel instead of retaining seven component planes.
        const qint64 floatMultiplier =
            alphaMode == SpatialAlphaMode::CoverageAwareRgba ? 3LL : 2LL;
        if (pixels <= 0
            || pixels > MaximumReferenceWorkingBytes
                            / (qint64(sizeof(float)) * floatMultiplier)) {
            return {};
        }

        QImage output = straight.copy();
        if (output.isNull()) return {};
        output.setColorSpace(source.colorSpace());
        output.setDevicePixelRatio(source.devicePixelRatio());
        output.setDotsPerMeterX(source.dotsPerMeterX());
        output.setDotsPerMeterY(source.dotsPerMeterY());

        FloatPlanes plane;
        plane.width = straight.width();
        plane.height = straight.height();
        plane.channels = 1;
        plane.values.resize(static_cast<qsizetype>(pixels));
        if (plane.values.size() != static_cast<qsizetype>(pixels)) return {};

        // Detach the output once before parallel row writers access disjoint
        // scanlines. The source remains const and can be shared safely.
        uchar *const outputBits = output.bits();
        const uchar *const straightBits = straight.constBits();
        const qsizetype outputStride = output.bytesPerLine();
        const qsizetype straightStride = straight.bytesPerLine();
        const int width = straight.width();
        const int height = straight.height();
        const auto runRows = [&](const std::function<void(int)> &processRow) {
            if (rowProcessor) {
                rowProcessor(width, height, processRow);
            } else {
                for (int y = 0; y < height; ++y) processRow(y);
            }
        };

        const auto fillPlane = [&](const int channel, const bool premultiplyByAlpha) {
            float *const values = plane.values.data();
            if (sixteenBit) {
                runRows([&](const int y) {
                    if (cancelled(cancelRequested)) return;
                    const auto *row = reinterpret_cast<const QRgba64 *>(
                        straightBits + static_cast<qsizetype>(y) * straightStride);
                    float *const destination = values + static_cast<qsizetype>(y) * width;
                    for (int x = 0; x < width; ++x) {
                        const quint16 components[4] {
                            row[x].red(), row[x].green(), row[x].blue(), row[x].alpha()
                        };
                        float value = components[channel] / 65535.0f;
                        if (premultiplyByAlpha) value *= row[x].alpha() / 65535.0f;
                        destination[x] = value;
                    }
                });
            } else {
                runRows([&](const int y) {
                    if (cancelled(cancelRequested)) return;
                    const uchar *row = straightBits + static_cast<qsizetype>(y) * straightStride;
                    float *const destination = values + static_cast<qsizetype>(y) * width;
                    for (int x = 0; x < width; ++x) {
                        const uchar *pixel = row + x * 4;
                        float value = pixel[channel] / 255.0f;
                        if (premultiplyByAlpha) value *= pixel[3] / 255.0f;
                        destination[x] = value;
                    }
                });
            }
            return !cancelled(cancelRequested);
        };

        const auto writeChannel = [&](const int channel,
                                      const FloatPlanes &values,
                                      const FloatPlanes *coverage,
                                      const bool onlyZeroCoverage) {
            const float *const valueData = values.values.constData();
            const float *const coverageData = coverage
                ? coverage->values.constData() : nullptr;
            if (sixteenBit) {
                runRows([&](const int y) {
                    if (cancelled(cancelRequested)) return;
                    auto *row = reinterpret_cast<QRgba64 *>(
                        outputBits + static_cast<qsizetype>(y) * outputStride);
                    const qsizetype rowBase = static_cast<qsizetype>(y) * width;
                    for (int x = 0; x < width; ++x) {
                        const qsizetype index = rowBase + x;
                        const float alpha = coverageData ? coverageData[index] : 1.0f;
                        const bool zeroCoverage = alpha <= 1.0e-8f;
                        if (onlyZeroCoverage != zeroCoverage) continue;
                        float value = valueData[index];
                        if (coverageData && !zeroCoverage && channel < 3) value /= alpha;
                        quint16 components[4] {
                            row[x].red(), row[x].green(), row[x].blue(), row[x].alpha()
                        };
                        components[channel] = static_cast<quint16>(
                            std::lround(clampUnit(value) * 65535.0f));
                        row[x] = QRgba64::fromRgba64(components[0], components[1],
                                                     components[2], components[3]);
                    }
                });
            } else {
                runRows([&](const int y) {
                    if (cancelled(cancelRequested)) return;
                    uchar *row = outputBits + static_cast<qsizetype>(y) * outputStride;
                    const qsizetype rowBase = static_cast<qsizetype>(y) * width;
                    for (int x = 0; x < width; ++x) {
                        const qsizetype index = rowBase + x;
                        const float alpha = coverageData ? coverageData[index] : 1.0f;
                        const bool zeroCoverage = alpha <= 1.0e-8f;
                        if (onlyZeroCoverage != zeroCoverage) continue;
                        float value = valueData[index];
                        if (coverageData && !zeroCoverage && channel < 3) value /= alpha;
                        row[x * 4 + channel] = static_cast<uchar>(
                            std::lround(clampUnit(value) * 255.0f));
                    }
                });
            }
            return !cancelled(cancelRequested);
        };

        FloatPlanes coverage;
        bool coverageHasZero = false;
        if (alphaMode == SpatialAlphaMode::CoverageAwareRgba) {
            if (!fillPlane(3, false)
                || !blurPlanesBox(&plane, radius, edgeMode, cancelRequested, rowProcessor)) {
                return {};
            }
            coverage.width = plane.width;
            coverage.height = plane.height;
            coverage.channels = 1;
            coverage.values = std::move(plane.values);
            for (qsizetype index = 0; index < coverage.values.size(); ++index) {
                if ((index & 0xffff) == 0 && cancelled(cancelRequested)) return {};
                if (coverage.values[index] <= 1.0e-8f) {
                    coverageHasZero = true;
                    break;
                }
            }
            plane.values.resize(static_cast<qsizetype>(pixels));
            if (plane.values.size() != static_cast<qsizetype>(pixels)) return {};
            if (!writeChannel(3, coverage, nullptr, false)) return {};
        }

        for (int channel = 0; channel < 3; ++channel) {
            if (!fillPlane(channel,
                           alphaMode == SpatialAlphaMode::CoverageAwareRgba)
                || !blurPlanesBox(&plane, radius, edgeMode, cancelRequested, rowProcessor)) {
                return {};
            }
            if (alphaMode == SpatialAlphaMode::CoverageAwareRgba) {
                if (!writeChannel(channel, plane, &coverage, false)) return {};
                // Fully transparent output has no premultiplied colour from
                // which to recover straight RGB. Only when such pixels exist,
                // filter hidden colour independently and write those pixels.
                if (coverageHasZero
                    && (!fillPlane(channel, false)
                        || !blurPlanesBox(&plane, radius, edgeMode, cancelRequested, rowProcessor)
                        || !writeChannel(channel, plane, &coverage, true))) {
                    return {};
                }
            } else if (!writeChannel(channel, plane, nullptr, false)) {
                return {};
            }
        }

        if (alphaMode == SpatialAlphaMode::StraightRgba) {
            if (!fillPlane(3, false)
                || !blurPlanesBox(&plane, radius, edgeMode, cancelRequested, rowProcessor)
                || !writeChannel(3, plane, nullptr, false)) {
                return {};
            }
        }
        return cancelled(cancelRequested) ? QImage() : output;
    } catch (const std::bad_alloc &) {
        return {};
    }
}


QImage SpatialFilterFoundation::gaussianBlurReference(
    const QImage &source,
    QSize supportRadius,
    const SpatialEdgeMode edgeMode,
    const SpatialAlphaMode alphaMode,
    const std::atomic_bool *cancelRequested,
    const SpatialRowProcessor &rowProcessor)
{
    if (source.isNull() || cancelled(cancelRequested)) return {};
    supportRadius.setWidth(std::clamp(supportRadius.width(), 0,
                                      SpatialFilterContract::DefaultMaximumRadius));
    supportRadius.setHeight(std::clamp(supportRadius.height(), 0,
                                       SpatialFilterContract::DefaultMaximumRadius));
    if (supportRadius.isNull()) {
        QImage straight = source.convertToFormat(source.depth() > 32
            ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        straight.setColorSpace(source.colorSpace());
        return straight;
    }

    // Distribute the exact dependency support across three box passes. Three
    // equal-width boxes closely approximate a Gaussian while each pass remains
    // a bounded sliding-window operation. Remainders are assigned to the first
    // passes deterministically, so CPU results and cache identities are stable.
    std::array<QSize, 3> passRadii {};
    for (int axis = 0; axis < 2; ++axis) {
        const int total = axis == 0 ? supportRadius.width() : supportRadius.height();
        const int base = total / 3;
        const int remainder = total % 3;
        for (int pass = 0; pass < 3; ++pass) {
            const int value = base + (pass < remainder ? 1 : 0);
            if (axis == 0) passRadii[pass].setWidth(value);
            else passRadii[pass].setHeight(value);
        }
    }

    QImage current = source;
    for (const QSize passRadius : passRadii) {
        if (cancelled(cancelRequested)) return {};
        if (passRadius.isNull()) continue;
        current = boxBlurReference(current, passRadius, edgeMode, alphaMode,
                                   cancelRequested, rowProcessor);
        if (current.isNull()) return {};
    }
    current.setColorSpace(source.colorSpace());
    return cancelled(cancelRequested) ? QImage() : current;
}

} // namespace vfx
