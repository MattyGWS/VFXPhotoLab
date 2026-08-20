#include "ImageSizeOperations.h"

#include <QByteArray>
#include <QRgba64>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace vfx {
namespace {

constexpr double Pi = 3.1415926535897932384626433832795;

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

bool cancelled(const std::atomic_bool *cancelRequested)
{
    return cancelRequested
        && cancelRequested->load(std::memory_order_acquire);
}

int dotsPerMetreFromDpi(const double dpi)
{
    return std::max(1, qRound(std::clamp(dpi, 1.0, 9600.0) / 0.0254));
}

bool validResolution(const double value)
{
    return std::isfinite(value) && value >= 1.0 && value <= 9600.0;
}

bool validResampleMethod(const ImageResampleMethod method)
{
    switch (method) {
    case ImageResampleMethod::NearestNeighbour:
    case ImageResampleMethod::Bilinear:
    case ImageResampleMethod::Bicubic:
    case ImageResampleMethod::Lanczos3:
    case ImageResampleMethod::Area:
        return true;
    }
    return false;
}

bool finiteTransform(const QTransform &transform)
{
    return std::isfinite(transform.m11())
        && std::isfinite(transform.m12())
        && std::isfinite(transform.m13())
        && std::isfinite(transform.m21())
        && std::isfinite(transform.m22())
        && std::isfinite(transform.m23())
        && std::isfinite(transform.m31())
        && std::isfinite(transform.m32())
        && std::isfinite(transform.m33());
}

bool finitePoint(const QPointF &point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

bool checkedAddBytes(quint64 *total,
                     const quint64 bytes,
                     const quint64 budget,
                     QString *errorMessage)
{
    if (!total || *total > std::numeric_limits<quint64>::max() - bytes) {
        setError(errorMessage,
                 QStringLiteral("The resized document byte estimate overflowed."));
        return false;
    }
    *total += bytes;
    if (budget > 0 && *total > budget) {
        constexpr quint64 MiB = 1024uLL * 1024uLL;
        const quint64 requiredMiB = *total / MiB + ((*total % MiB) != 0u);
        const quint64 budgetMiB = budget / MiB + ((budget % MiB) != 0u);
        setError(errorMessage,
                 QStringLiteral("The resized editable document would require approximately "
                                "%1 MiB, exceeding the current safe preparation budget of %2 MiB.")
                     .arg(requiredMiB)
                     .arg(budgetMiB));
        return false;
    }
    return true;
}

bool checkedImageBytes(const QSize &size,
                       const quint64 bytesPerPixel,
                       quint64 *total,
                       const quint64 budget,
                       QString *errorMessage)
{
    if (size.isEmpty() || bytesPerPixel == 0) {
        setError(errorMessage,
                 QStringLiteral("A resized editable payload has invalid dimensions."));
        return false;
    }
    const quint64 width = static_cast<quint64>(size.width());
    const quint64 height = static_cast<quint64>(size.height());
    if (width > std::numeric_limits<quint64>::max() / bytesPerPixel) {
        setError(errorMessage,
                 QStringLiteral("A resized editable payload byte estimate overflowed."));
        return false;
    }
    const quint64 activeRowBytes = width * bytesPerPixel;
    if (activeRowBytes > std::numeric_limits<quint64>::max() - 3u) {
        setError(errorMessage,
                 QStringLiteral("A resized editable payload byte estimate overflowed."));
        return false;
    }
    // QImage scan lines are at least 32-bit aligned, including Grayscale8
    // masks and the conservative dense-selection estimate.
    const quint64 rowBytes = (activeRowBytes + 3u) & ~quint64(3u);
    if (rowBytes > std::numeric_limits<quint64>::max() / height) {
        setError(errorMessage,
                 QStringLiteral("A resized editable payload byte estimate overflowed."));
        return false;
    }
    return checkedAddBytes(total,
                           rowBytes * height,
                           budget,
                           errorMessage);
}

bool persistentImageSizeIsSafe(const QSize &size, const int depth)
{
    constexpr quint64 MaximumPersistentImageBytes = 0xfffffffeULL;
    if (size.isEmpty() || size.width() > 32768 || size.height() > 32768) {
        return false;
    }
    const quint64 bytesPerPixel = depth > 32 ? 8u : 4u;
    const quint64 pixels = static_cast<quint64>(size.width())
        * static_cast<quint64>(size.height());
    return pixels <= MaximumPersistentImageBytes / bytesPerPixel;
}

int scaledExtent(const int oldExtent, const double scale)
{
    if (oldExtent < 1 || !std::isfinite(scale) || scale <= 0.0) {
        return 0;
    }
    const double value = oldExtent * scale;
    if (value > 32768.0 + 0.5) {
        return 0;
    }
    return std::clamp(qRound(value), 1, 32768);
}

bool scaleVectorLayerData(VectorLayerData *data,
                          const double scaleX,
                          const double scaleY)
{
    if (!data || !data->isSafe() || !std::isfinite(scaleX)
        || !std::isfinite(scaleY) || scaleX <= 0.0 || scaleY <= 0.0) {
        return false;
    }
    const QTransform documentScale = QTransform::fromScale(scaleX, scaleY);
    const QTransform inverseDocumentScale = QTransform::fromScale(
        1.0 / scaleX, 1.0 / scaleY);
    const double radiusScale = std::min(scaleX, scaleY);
    const double scaledFeather = data->featherRadius * radiusScale;
    if (!std::isfinite(scaledFeather)
        || scaledFeather > VectorLayerData::MaximumFeatherRadius) {
        return false;
    }
    data->featherRadius = scaledFeather;
    for (VectorShape &shape : data->objects) {
        shape.bounds = QRectF(shape.bounds.x() * scaleX,
                              shape.bounds.y() * scaleY,
                              shape.bounds.width() * scaleX,
                              shape.bounds.height() * scaleY);
        shape.lineStart = QPointF(shape.lineStart.x() * scaleX,
                                  shape.lineStart.y() * scaleY);
        shape.lineEnd = QPointF(shape.lineEnd.x() * scaleX,
                                shape.lineEnd.y() * scaleY);
        if (shape.type == VectorShapeType::Path) {
            const auto scalePath = [scaleX, scaleY, radiusScale](
                                       VectorBezierPath &path) {
                for (VectorPathNode &node : path.nodes) {
                    node.anchor = QPointF(node.anchor.x() * scaleX,
                                          node.anchor.y() * scaleY);
                    node.inHandle = QPointF(node.inHandle.x() * scaleX,
                                            node.inHandle.y() * scaleY);
                    node.outHandle = QPointF(node.outHandle.x() * scaleX,
                                             node.outHandle.y() * scaleY);
                    node.cornerRadius *= radiusScale;
                }
            };
            scalePath(shape.bezierPath);
            for (VectorBezierPath &path : shape.additionalBezierPaths) {
                scalePath(path);
            }
        }
        shape.cornerRadii.topLeft *= radiusScale;
        shape.cornerRadii.topRight *= radiusScale;
        shape.cornerRadii.bottomRight *= radiusScale;
        shape.cornerRadii.bottomLeft *= radiusScale;
        shape.stroke.width *= radiusScale;
        shape.stroke.dashLength *= radiusScale;
        shape.stroke.gapLength *= radiusScale;
        shape.stroke.dashOffset *= radiusScale;
        shape.transform = inverseDocumentScale
            * shape.transform * documentScale;
        ++shape.revision;
        shape.normalise();
        if (!shape.isSafe()) return false;
    }
    data->normalise();
    return data->isSafe();
}

bool scaleTextLayerData(TextLayerData *data, const double scaleX, const double scaleY)
{
    if (!data || !data->isSafe()) return false;
    data->origin = QPointF(data->origin.x() * scaleX, data->origin.y() * scaleY);
    data->area = QRectF(data->area.x() * scaleX, data->area.y() * scaleY,
                        data->area.width() * scaleX, data->area.height() * scaleY);
    data->fontSize *= std::sqrt(std::abs(scaleX * scaleY));
    data->tracking *= std::sqrt(std::abs(scaleX * scaleY));
    ++data->revision; data->normalise(); return data->isSafe();
}

bool preflightLayerTree(const QVector<LayerNode> &layers,
                        const QSize &oldDocumentSize,
                        const double scaleX,
                        const double scaleY,
                        const quint64 budget,
                        quint64 *estimatedBytes,
                        const std::atomic_bool *cancelRequested,
                        QString *errorMessage)
{
    for (const LayerNode &layer : layers) {
        if (cancelled(cancelRequested)) {
            setError(errorMessage, QStringLiteral("Image resize cancelled."));
            return false;
        }
        if (layer.id.isNull() || !finiteTransform(layer.transform)
            || !finitePoint(layer.rasterReferenceOrigin)
            || !finitePoint(layer.maskReferenceOrigin)) {
            setError(errorMessage,
                     QStringLiteral("A layer contains invalid coordinates or transform data."));
            return false;
        }

        if (layer.type == LayerType::Vector) {
            VectorLayerData scaled = layer.vectorData;
            if (!scaleVectorLayerData(&scaled, scaleX, scaleY)) { setError(errorMessage, QStringLiteral("A vector shape would exceed safe coordinates after Image Size.")); return false; }
        }
        if (layer.type == LayerType::Text) {
            TextLayerData scaled = layer.textData;
            if (!scaleTextLayerData(&scaled, scaleX, scaleY)) { setError(errorMessage, QStringLiteral("A text layer would exceed safe coordinates after Image Size.")); return false; }
        }

        if ((layer.type == LayerType::Raster || layer.type == LayerType::BaseImage)
            && !layer.rasterImage.isNull()) {
            const QSize outputSize(scaledExtent(layer.rasterImage.width(), scaleX),
                                   scaledExtent(layer.rasterImage.height(), scaleY));
            if (outputSize.isEmpty()
                || !persistentImageSizeIsSafe(outputSize, layer.rasterImage.depth())) {
                setError(errorMessage,
                         QStringLiteral("A raster layer would exceed the supported "
                                        "32768-pixel or exact snapshot limit."));
                return false;
            }
            const quint64 bytesPerPixel = layer.rasterImage.depth() > 32 ? 8u : 4u;
            if (!checkedImageBytes(outputSize,
                                   bytesPerPixel,
                                   estimatedBytes,
                                   budget,
                                   errorMessage)) {
                return false;
            }
        }

        if (!layer.maskImage.isNull()) {
            const QSize outputSize = layer.maskImage.size() == QSize(1, 1)
                ? QSize(1, 1)
                : QSize(scaledExtent(layer.maskImage.width(), scaleX),
                        scaledExtent(layer.maskImage.height(), scaleY));
            if (outputSize.isEmpty()
                || !persistentImageSizeIsSafe(outputSize, 8)) {
                setError(errorMessage,
                         QStringLiteral("A layer mask would exceed the supported "
                                        "32768-pixel or exact snapshot limit."));
                return false;
            }
            if (!checkedImageBytes(outputSize,
                                   1u,
                                   estimatedBytes,
                                   budget,
                                   errorMessage)) {
                return false;
            }
        }

        const QSize rasterReference = layer.rasterReferenceSize.isValid()
                && !layer.rasterReferenceSize.isEmpty()
            ? layer.rasterReferenceSize : oldDocumentSize;
        if ((layer.type == LayerType::Raster || layer.type == LayerType::BaseImage)
            && (scaledExtent(rasterReference.width(), scaleX) < 1
                || scaledExtent(rasterReference.height(), scaleY) < 1)) {
            setError(errorMessage,
                     QStringLiteral("A raster reference extent would exceed the supported limit."));
            return false;
        }
        const QSize maskReference = layer.maskReferenceSize.isValid()
                && !layer.maskReferenceSize.isEmpty()
            ? layer.maskReferenceSize : oldDocumentSize;
        if (!layer.maskImage.isNull()
            && (scaledExtent(maskReference.width(), scaleX) < 1
                || scaledExtent(maskReference.height(), scaleY) < 1)) {
            setError(errorMessage,
                     QStringLiteral("A mask reference extent would exceed the supported limit."));
            return false;
        }

        if (!preflightLayerTree(layer.children,
                                oldDocumentSize,
                                scaleX,
                                scaleY,
                                budget,
                                estimatedBytes,
                                cancelRequested,
                                errorMessage)) {
            return false;
        }
    }
    return true;
}

struct AxisSample {
    int first = 0;
    int second = 0;
    double fraction = 0.0;
};

QVector<AxisSample> samplingAxis(const int sourceExtent,
                                 const int destinationExtent,
                                 const ImageResampleMethod method)
{
    QVector<AxisSample> samples;
    if (sourceExtent < 1 || destinationExtent < 1) {
        return samples;
    }
    samples.resize(destinationExtent);
    const qint64 doubledDestinationExtent = qint64(destinationExtent) * 2;
    for (int destination = 0; destination < destinationExtent; ++destination) {
        const qint64 doubledCentre = qint64(destination) * 2 + 1;
        if (method == ImageResampleMethod::NearestNeighbour) {
            // floor(sourcePosition + 0.5) expressed as exact integer arithmetic.
            // This avoids CPU/GPU phase disagreements at rational half-pixel ties.
            const int nearest = std::clamp(
                static_cast<int>((doubledCentre * sourceExtent)
                                 / doubledDestinationExtent),
                0,
                sourceExtent - 1);
            samples[destination] = {nearest, nearest, 0.0};
            continue;
        }

        // sourcePosition = ((2d + 1)S - D) / (2D). C++ signed
        // division truncates toward zero, so normalise the remainder to obtain
        // mathematical floor and an exact non-negative interpolation fraction.
        const qint64 numerator = doubledCentre * sourceExtent - destinationExtent;
        qint64 base = numerator / doubledDestinationExtent;
        qint64 remainder = numerator % doubledDestinationExtent;
        if (remainder < 0) {
            --base;
            remainder += doubledDestinationExtent;
        }
        samples[destination] = {
            std::clamp(static_cast<int>(base), 0, sourceExtent - 1),
            std::clamp(static_cast<int>(base + 1), 0, sourceExtent - 1),
            remainder / static_cast<double>(doubledDestinationExtent)
        };
    }
    return samples;
}

quint16 interpolate16(const quint16 a,
                      const quint16 b,
                      const quint16 c,
                      const quint16 d,
                      const double tx,
                      const double ty)
{
    const double top = a + (b - static_cast<double>(a)) * tx;
    const double bottom = c + (d - static_cast<double>(c)) * tx;
    return static_cast<quint16>(std::clamp(
        qRound(top + (bottom - top) * ty), 0, 65535));
}

uchar interpolate8(const uchar a,
                   const uchar b,
                   const uchar c,
                   const uchar d,
                   const double tx,
                   const double ty)
{
    const double top = a + (b - static_cast<double>(a)) * tx;
    const double bottom = c + (d - static_cast<double>(c)) * tx;
    return static_cast<uchar>(std::clamp(
        qRound(top + (bottom - top) * ty), 0, 255));
}

struct FilterTap {
    int index = 0;
    double weight = 0.0;
};

using AxisFilter = QVector<QVector<FilterTap>>;

constexpr qsizetype MaximumIntermediateFilterBytes = 16 * 1024 * 1024;

int maximumTapCount(const AxisFilter &filters)
{
    int maximum = 1;
    for (const QVector<FilterTap> &filter : filters) {
        maximum = std::max(maximum, static_cast<int>(filter.size()));
    }
    return maximum;
}

int filteredBlockWidth(const int destinationWidth,
                       const AxisFilter &verticalFilters,
                       const int componentCount)
{
    if (destinationWidth < 1 || componentCount < 1) {
        return 1;
    }
    const qsizetype bytesPerOutputColumn = std::max<qsizetype>(
        1,
        qsizetype(maximumTapCount(verticalFilters))
            * componentCount * qsizetype(sizeof(double)));
    const qsizetype budgeted = std::max<qsizetype>(
        1, MaximumIntermediateFilterBytes / bytesPerOutputColumn);
    return std::clamp<int>(static_cast<int>(std::min<qsizetype>(
                               destinationWidth, budgeted)),
                           1,
                           destinationWidth);
}

void appendTap(QVector<FilterTap> *taps, const int index, const double weight)
{
    if (!taps || std::abs(weight) < 1.0e-15) {
        return;
    }
    for (FilterTap &tap : *taps) {
        if (tap.index == index) {
            tap.weight += weight;
            return;
        }
    }
    taps->push_back({index, weight});
}

double cubicKernel(const double value)
{
    constexpr double A = -0.5; // Catmull-Rom cubic convolution.
    const double x = std::abs(value);
    if (x < 1.0) {
        return (A + 2.0) * x * x * x
            - (A + 3.0) * x * x + 1.0;
    }
    if (x < 2.0) {
        return A * x * x * x - 5.0 * A * x * x
            + 8.0 * A * x - 4.0 * A;
    }
    return 0.0;
}

double sinc(const double value)
{
    if (std::abs(value) < 1.0e-12) {
        return 1.0;
    }
    const double radians = Pi * value;
    return std::sin(radians) / radians;
}

double lanczos3Kernel(const double value)
{
    const double x = std::abs(value);
    return x < 3.0 ? sinc(value) * sinc(value / 3.0) : 0.0;
}

AxisFilter filterAxis(const int sourceExtent,
                      const int destinationExtent,
                      const ImageResampleMethod method)
{
    AxisFilter filters;
    if (sourceExtent < 1 || destinationExtent < 1) {
        return filters;
    }
    filters.resize(destinationExtent);

    if (method == ImageResampleMethod::NearestNeighbour
        || method == ImageResampleMethod::Bilinear) {
        const QVector<AxisSample> samples = samplingAxis(
            sourceExtent, destinationExtent, method);
        if (samples.size() != destinationExtent) {
            return {};
        }
        for (int destination = 0; destination < destinationExtent; ++destination) {
            const AxisSample sample = samples.at(destination);
            appendTap(&filters[destination], sample.first, 1.0 - sample.fraction);
            appendTap(&filters[destination], sample.second, sample.fraction);
        }
        return filters;
    }

    if (method == ImageResampleMethod::Area) {
        if (destinationExtent >= sourceExtent) {
            // Area integration is a reduction filter. On an expanding axis use
            // bilinear interpolation while any reducing axis still receives the
            // exact box average.
            return filterAxis(sourceExtent,
                              destinationExtent,
                              ImageResampleMethod::Bilinear);
        }
        const double sourcePerDestination = sourceExtent
            / static_cast<double>(destinationExtent);
        for (int destination = 0; destination < destinationExtent; ++destination) {
            const double start = destination * sourcePerDestination;
            const double end = (destination + 1) * sourcePerDestination;
            const int first = std::max(0, static_cast<int>(std::floor(start)));
            const int last = std::min(sourceExtent - 1,
                                      static_cast<int>(std::ceil(end)) - 1);
            for (int source = first; source <= last; ++source) {
                const double overlap = std::max(
                    0.0,
                    std::min(end, source + 1.0) - std::max(start, double(source)));
                appendTap(&filters[destination],
                          source,
                          overlap / sourcePerDestination);
            }
        }
        return filters;
    }

    const double destinationPerSource = destinationExtent
        / static_cast<double>(sourceExtent);
    const double antialiasScale = std::min(1.0, destinationPerSource);
    const double radius = method == ImageResampleMethod::Lanczos3 ? 3.0 : 2.0;
    const double support = radius / antialiasScale;
    const auto kernel = method == ImageResampleMethod::Lanczos3
        ? lanczos3Kernel : cubicKernel;

    for (int destination = 0; destination < destinationExtent; ++destination) {
        const double centre = (destination + 0.5)
            * sourceExtent / static_cast<double>(destinationExtent) - 0.5;
        const int first = static_cast<int>(std::ceil(centre - support));
        const int last = static_cast<int>(std::floor(centre + support));
        double total = 0.0;
        for (int source = first; source <= last; ++source) {
            const double weight = kernel((source - centre) * antialiasScale)
                * antialiasScale;
            const int clamped = std::clamp(source, 0, sourceExtent - 1);
            appendTap(&filters[destination], clamped, weight);
            total += weight;
        }
        if (std::abs(total) < 1.0e-12 || filters[destination].isEmpty()) {
            const int nearest = std::clamp(qRound(centre), 0, sourceExtent - 1);
            filters[destination] = {{nearest, 1.0}};
            continue;
        }
        // Normalise after edge-clamped duplicate taps have been combined.
        double combinedTotal = 0.0;
        for (const FilterTap &tap : std::as_const(filters[destination])) {
            combinedTotal += tap.weight;
        }
        if (std::abs(combinedTotal) < 1.0e-12) {
            const int nearest = std::clamp(qRound(centre), 0, sourceExtent - 1);
            filters[destination] = {{nearest, 1.0}};
            continue;
        }
        for (FilterTap &tap : filters[destination]) {
            tap.weight /= combinedTotal;
        }
    }
    return filters;
}

bool isAdvancedMethod(const ImageResampleMethod method)
{
    return method == ImageResampleMethod::Bicubic
        || method == ImageResampleMethod::Lanczos3
        || method == ImageResampleMethod::Area;
}

bool gpuEligibleMethod(const ImageResampleMethod method)
{
    return method == ImageResampleMethod::NearestNeighbour
        || method == ImageResampleMethod::Bilinear;
}

QImage resampleStraightRgbaSimple(const QImage &source,
                                  const QSize &destinationSize,
                                  const ImageResampleMethod method,
                                  const std::atomic_bool *cancelRequested)
{
    if (source.isNull() || destinationSize.isEmpty()) {
        return {};
    }
    const bool sixteenBit = source.depth() > 32;
    const QImage::Format format = sixteenBit
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    const QImage straight = source.convertToFormat(format);
    if (straight.isNull()) {
        return {};
    }
    if (straight.size() == destinationSize) {
        return straight;
    }

    QImage output(destinationSize, format);
    if (output.isNull()) {
        return {};
    }
    output.setColorSpace(source.colorSpace());
    output.setDevicePixelRatio(source.devicePixelRatio());
    output.setDotsPerMeterX(source.dotsPerMeterX());
    output.setDotsPerMeterY(source.dotsPerMeterY());

    const QVector<AxisSample> xSamples = samplingAxis(
        straight.width(), output.width(), method);
    const QVector<AxisSample> ySamples = samplingAxis(
        straight.height(), output.height(), method);
    if (xSamples.size() != output.width() || ySamples.size() != output.height()) {
        return {};
    }

    if (sixteenBit) {
        for (int y = 0; y < output.height(); ++y) {
            if (cancelled(cancelRequested)) {
                return {};
            }
            const AxisSample ys = ySamples.at(y);
            const auto *row0 = reinterpret_cast<const QRgba64 *>(
                straight.constScanLine(ys.first));
            const auto *row1 = reinterpret_cast<const QRgba64 *>(
                straight.constScanLine(ys.second));
            auto *destination = reinterpret_cast<QRgba64 *>(output.scanLine(y));
            for (int x = 0; x < output.width(); ++x) {
                const AxisSample xs = xSamples.at(x);
                const QRgba64 p00 = row0[xs.first];
                const QRgba64 p10 = row0[xs.second];
                const QRgba64 p01 = row1[xs.first];
                const QRgba64 p11 = row1[xs.second];
                destination[x] = QRgba64::fromRgba64(
                    interpolate16(p00.red(), p10.red(), p01.red(), p11.red(),
                                  xs.fraction, ys.fraction),
                    interpolate16(p00.green(), p10.green(), p01.green(), p11.green(),
                                  xs.fraction, ys.fraction),
                    interpolate16(p00.blue(), p10.blue(), p01.blue(), p11.blue(),
                                  xs.fraction, ys.fraction),
                    interpolate16(p00.alpha(), p10.alpha(), p01.alpha(), p11.alpha(),
                                  xs.fraction, ys.fraction));
            }
        }
        return output;
    }

    for (int y = 0; y < output.height(); ++y) {
        if (cancelled(cancelRequested)) {
            return {};
        }
        const AxisSample ys = ySamples.at(y);
        const uchar *row0 = straight.constScanLine(ys.first);
        const uchar *row1 = straight.constScanLine(ys.second);
        uchar *destination = output.scanLine(y);
        for (int x = 0; x < output.width(); ++x) {
            const AxisSample xs = xSamples.at(x);
            for (int channel = 0; channel < 4; ++channel) {
                destination[x * 4 + channel] = interpolate8(
                    row0[xs.first * 4 + channel],
                    row0[xs.second * 4 + channel],
                    row1[xs.first * 4 + channel],
                    row1[xs.second * 4 + channel],
                    xs.fraction,
                    ys.fraction);
            }
        }
    }
    return output;
}

QVector<double> horizontallyFilteredRgba8(
    const uchar *source,
    const AxisFilter &xFilters,
    const int startX,
    const int count,
    const std::atomic_bool *cancelRequested)
{
    if (!source || startX < 0 || count < 1 || startX + count > xFilters.size()) {
        return {};
    }
    QVector<double> row(count * 4, 0.0);
    for (int localX = 0; localX < count; ++localX) {
        if ((localX & 255) == 0 && cancelled(cancelRequested)) {
            return {};
        }
        const QVector<FilterTap> &taps = xFilters.at(startX + localX);
        for (const FilterTap &tap : taps) {
            const uchar *pixel = source + tap.index * 4;
            for (int channel = 0; channel < 4; ++channel) {
                row[localX * 4 + channel] += pixel[channel] * tap.weight;
            }
        }
    }
    return row;
}

QVector<double> horizontallyFilteredRgba16(
    const QRgba64 *source,
    const AxisFilter &xFilters,
    const int startX,
    const int count,
    const std::atomic_bool *cancelRequested)
{
    if (!source || startX < 0 || count < 1 || startX + count > xFilters.size()) {
        return {};
    }
    QVector<double> row(count * 4, 0.0);
    for (int localX = 0; localX < count; ++localX) {
        if ((localX & 255) == 0 && cancelled(cancelRequested)) {
            return {};
        }
        for (const FilterTap &tap : xFilters.at(startX + localX)) {
            const QRgba64 pixel = source[tap.index];
            row[localX * 4] += pixel.red() * tap.weight;
            row[localX * 4 + 1] += pixel.green() * tap.weight;
            row[localX * 4 + 2] += pixel.blue() * tap.weight;
            row[localX * 4 + 3] += pixel.alpha() * tap.weight;
        }
    }
    return row;
}

QImage resampleStraightRgbaAdvanced(const QImage &source,
                                    const QSize &destinationSize,
                                    const ImageResampleMethod method,
                                    const std::atomic_bool *cancelRequested)
{
    if (source.isNull() || destinationSize.isEmpty() || !isAdvancedMethod(method)) {
        return {};
    }
    const bool sixteenBit = source.depth() > 32;
    const QImage::Format format = sixteenBit
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    const QImage straight = source.convertToFormat(format);
    if (straight.isNull()) {
        return {};
    }
    if (straight.size() == destinationSize) {
        return straight;
    }

    const AxisFilter xFilters = filterAxis(straight.width(), destinationSize.width(), method);
    const AxisFilter yFilters = filterAxis(straight.height(), destinationSize.height(), method);
    if (xFilters.size() != destinationSize.width()
        || yFilters.size() != destinationSize.height()) {
        return {};
    }

    QImage output(destinationSize, format);
    if (output.isNull()) {
        return {};
    }
    output.setColorSpace(source.colorSpace());
    output.setDevicePixelRatio(source.devicePixelRatio());
    output.setDotsPerMeterX(source.dotsPerMeterX());
    output.setDotsPerMeterY(source.dotsPerMeterY());

    // Block the horizontal intermediate so an extreme one-axis reduction never
    // retains sourceRows × fullDestinationWidth doubles. The exact filter taps
    // and output rounding are unchanged; only provisional memory is bounded.
    const int blockWidth = filteredBlockWidth(output.width(), yFilters, 4);
    for (int startX = 0; startX < output.width(); startX += blockWidth) {
        if (cancelled(cancelRequested)) {
            return {};
        }
        const int count = std::min(blockWidth, output.width() - startX);
        std::map<int, QVector<double>> horizontalRows;
        for (int y = 0; y < output.height(); ++y) {
            if (cancelled(cancelRequested)) {
                return {};
            }
            std::set<int> neededRows;
            for (const FilterTap &tap : yFilters.at(y)) {
                neededRows.insert(tap.index);
                if (horizontalRows.find(tap.index) == horizontalRows.end()) {
                    QVector<double> filtered = sixteenBit
                        ? horizontallyFilteredRgba16(
                              reinterpret_cast<const QRgba64 *>(
                                  straight.constScanLine(tap.index)),
                              xFilters,
                              startX,
                              count,
                              cancelRequested)
                        : horizontallyFilteredRgba8(
                              straight.constScanLine(tap.index),
                              xFilters,
                              startX,
                              count,
                              cancelRequested);
                    if (filtered.size() != count * 4) {
                        return {};
                    }
                    horizontalRows.emplace(tap.index, std::move(filtered));
                }
            }
            for (auto iterator = horizontalRows.begin();
                 iterator != horizontalRows.end();) {
                if (neededRows.find(iterator->first) == neededRows.end()) {
                    iterator = horizontalRows.erase(iterator);
                } else {
                    ++iterator;
                }
            }

            if (sixteenBit) {
                auto *destination = reinterpret_cast<QRgba64 *>(output.scanLine(y));
                for (int localX = 0; localX < count; ++localX) {
                    double components[4] = {0.0, 0.0, 0.0, 0.0};
                    for (const FilterTap &tap : yFilters.at(y)) {
                        const QVector<double> &row = horizontalRows.at(tap.index);
                        for (int channel = 0; channel < 4; ++channel) {
                            components[channel] += row[localX * 4 + channel]
                                * tap.weight;
                        }
                    }
                    destination[startX + localX] = QRgba64::fromRgba64(
                        static_cast<quint16>(std::clamp(qRound(components[0]), 0, 65535)),
                        static_cast<quint16>(std::clamp(qRound(components[1]), 0, 65535)),
                        static_cast<quint16>(std::clamp(qRound(components[2]), 0, 65535)),
                        static_cast<quint16>(std::clamp(qRound(components[3]), 0, 65535)));
                }
            } else {
                uchar *destination = output.scanLine(y) + startX * 4;
                for (int localX = 0; localX < count; ++localX) {
                    for (int channel = 0; channel < 4; ++channel) {
                        double component = 0.0;
                        for (const FilterTap &tap : yFilters.at(y)) {
                            component += horizontalRows.at(tap.index)
                                             [localX * 4 + channel]
                                * tap.weight;
                        }
                        destination[localX * 4 + channel] = static_cast<uchar>(
                            std::clamp(qRound(component), 0, 255));
                    }
                }
            }
        }
    }
    return output;
}

QImage resampleStraightRgba(const QImage &source,
                            const QSize &destinationSize,
                            const ImageResampleMethod method,
                            const std::atomic_bool *cancelRequested)
{
    return isAdvancedMethod(method)
        ? resampleStraightRgbaAdvanced(source,
                                      destinationSize,
                                      method,
                                      cancelRequested)
        : resampleStraightRgbaSimple(source,
                                    destinationSize,
                                    method,
                                    cancelRequested);
}

QImage resampleGrayscaleSimple(const QImage &source,
                               const QSize &destinationSize,
                               const ImageResampleMethod method,
                               const std::atomic_bool *cancelRequested)
{
    if (source.isNull() || destinationSize.isEmpty()) {
        return {};
    }
    const QImage grayscale = source.convertToFormat(QImage::Format_Grayscale8);
    if (grayscale.isNull()) {
        return {};
    }
    if (grayscale.size() == QSize(1, 1)) {
        return grayscale;
    }
    if (grayscale.size() == destinationSize) {
        return grayscale;
    }

    QImage output(destinationSize, QImage::Format_Grayscale8);
    if (output.isNull()) {
        return {};
    }
    output.fill(0);
    const QVector<AxisSample> xSamples = samplingAxis(
        grayscale.width(), output.width(), method);
    const QVector<AxisSample> ySamples = samplingAxis(
        grayscale.height(), output.height(), method);
    if (xSamples.size() != output.width() || ySamples.size() != output.height()) {
        return {};
    }
    for (int y = 0; y < output.height(); ++y) {
        if (cancelled(cancelRequested)) {
            return {};
        }
        const AxisSample ys = ySamples.at(y);
        const uchar *row0 = grayscale.constScanLine(ys.first);
        const uchar *row1 = grayscale.constScanLine(ys.second);
        uchar *destination = output.scanLine(y);
        for (int x = 0; x < output.width(); ++x) {
            const AxisSample xs = xSamples.at(x);
            destination[x] = interpolate8(
                row0[xs.first], row0[xs.second],
                row1[xs.first], row1[xs.second],
                xs.fraction, ys.fraction);
        }
    }
    output.setColorSpace(source.colorSpace());
    output.setDevicePixelRatio(source.devicePixelRatio());
    output.setDotsPerMeterX(source.dotsPerMeterX());
    output.setDotsPerMeterY(source.dotsPerMeterY());
    return output;
}

QVector<double> horizontallyFilteredGray(
    const uchar *source,
    const AxisFilter &xFilters,
    const int startX,
    const int count,
    const std::atomic_bool *cancelRequested)
{
    if (!source || startX < 0 || count < 1 || startX + count > xFilters.size()) {
        return {};
    }
    QVector<double> row(count, 0.0);
    for (int localX = 0; localX < count; ++localX) {
        if ((localX & 255) == 0 && cancelled(cancelRequested)) {
            return {};
        }
        for (const FilterTap &tap : xFilters.at(startX + localX)) {
            row[localX] += source[tap.index] * tap.weight;
        }
    }
    return row;
}

QImage resampleGrayscaleAdvanced(const QImage &source,
                                 const QSize &destinationSize,
                                 const ImageResampleMethod method,
                                 const std::atomic_bool *cancelRequested)
{
    if (source.isNull() || destinationSize.isEmpty() || !isAdvancedMethod(method)) {
        return {};
    }
    const QImage grayscale = source.convertToFormat(QImage::Format_Grayscale8);
    if (grayscale.isNull()) {
        return {};
    }
    if (grayscale.size() == QSize(1, 1) || grayscale.size() == destinationSize) {
        return grayscale;
    }
    const AxisFilter xFilters = filterAxis(grayscale.width(), destinationSize.width(), method);
    const AxisFilter yFilters = filterAxis(grayscale.height(), destinationSize.height(), method);
    if (xFilters.size() != destinationSize.width()
        || yFilters.size() != destinationSize.height()) {
        return {};
    }

    QImage output(destinationSize, QImage::Format_Grayscale8);
    if (output.isNull()) {
        return {};
    }
    output.fill(0);
    const int blockWidth = filteredBlockWidth(output.width(), yFilters, 1);
    for (int startX = 0; startX < output.width(); startX += blockWidth) {
        if (cancelled(cancelRequested)) {
            return {};
        }
        const int count = std::min(blockWidth, output.width() - startX);
        std::map<int, QVector<double>> horizontalRows;
        for (int y = 0; y < output.height(); ++y) {
            if (cancelled(cancelRequested)) {
                return {};
            }
            std::set<int> neededRows;
            for (const FilterTap &tap : yFilters.at(y)) {
                neededRows.insert(tap.index);
                if (horizontalRows.find(tap.index) == horizontalRows.end()) {
                    QVector<double> filtered = horizontallyFilteredGray(
                        grayscale.constScanLine(tap.index),
                        xFilters,
                        startX,
                        count,
                        cancelRequested);
                    if (filtered.size() != count) {
                        return {};
                    }
                    horizontalRows.emplace(tap.index, std::move(filtered));
                }
            }
            for (auto iterator = horizontalRows.begin();
                 iterator != horizontalRows.end();) {
                if (neededRows.find(iterator->first) == neededRows.end()) {
                    iterator = horizontalRows.erase(iterator);
                } else {
                    ++iterator;
                }
            }
            uchar *destination = output.scanLine(y) + startX;
            for (int localX = 0; localX < count; ++localX) {
                double value = 0.0;
                for (const FilterTap &tap : yFilters.at(y)) {
                    value += horizontalRows.at(tap.index)[localX] * tap.weight;
                }
                destination[localX] = static_cast<uchar>(
                    std::clamp(qRound(value), 0, 255));
            }
        }
    }
    output.setColorSpace(source.colorSpace());
    output.setDevicePixelRatio(source.devicePixelRatio());
    output.setDotsPerMeterX(source.dotsPerMeterX());
    output.setDotsPerMeterY(source.dotsPerMeterY());
    return output;
}

QImage resampleGrayscale(const QImage &source,
                         const QSize &destinationSize,
                         const ImageResampleMethod method,
                         const std::atomic_bool *cancelRequested)
{
    return isAdvancedMethod(method)
        ? resampleGrayscaleAdvanced(source,
                                   destinationSize,
                                   method,
                                   cancelRequested)
        : resampleGrayscaleSimple(source,
                                 destinationSize,
                                 method,
                                 cancelRequested);
}

QImage resamplePayload(const QImage &source,
                       const QSize &destinationSize,
                       const ImageResampleMethod method,
                       const bool grayscale,
                       const ImageResampleAccelerator &accelerator,
                       const std::atomic_bool *cancelRequested,
                       int *gpuPayloads,
                       int *cpuPayloads,
                       QString *firstGpuFallbackReason)
{
    if (source.isNull() || destinationSize.isEmpty()) {
        return {};
    }
    const bool compactUniformMask = grayscale && source.size() == QSize(1, 1);
    const bool actualResize = source.size() != destinationSize && !compactUniformMask;
    if (actualResize && gpuEligibleMethod(method)
        && accelerator && source.depth() <= 32) {
        QString gpuError;
        QImage accelerated = accelerator(source,
                                         destinationSize,
                                         method,
                                         cancelRequested,
                                         &gpuError);
        if (cancelled(cancelRequested)) {
            return {};
        }
        if (!accelerated.isNull()) {
            const QImage::Format expectedFormat = grayscale
                ? QImage::Format_Grayscale8
                : QImage::Format_RGBA8888;
            if (accelerated.size() == destinationSize
                && accelerated.format() == expectedFormat) {
                // The accelerator owns only pixel production. Normalise the
                // non-pixel metadata here so GPU and CPU payloads remain
                // interchangeable through history, residency and export.
                accelerated.setColorSpace(source.colorSpace());
                accelerated.setDevicePixelRatio(source.devicePixelRatio());
                accelerated.setDotsPerMeterX(source.dotsPerMeterX());
                accelerated.setDotsPerMeterY(source.dotsPerMeterY());
                if (gpuPayloads) {
                    ++*gpuPayloads;
                }
                return accelerated;
            }
            gpuError = QStringLiteral(
                "Native tiled GPU resampling returned an incompatible payload; "
                "using the exact CPU reference");
        }
        if (cancelled(cancelRequested)) {
            return {};
        }
        if (firstGpuFallbackReason && firstGpuFallbackReason->isEmpty()) {
            *firstGpuFallbackReason = gpuError.isEmpty()
                ? QStringLiteral("Native tiled GPU resampling was unavailable.")
                : gpuError;
        }
    }
    if (actualResize && cpuPayloads) {
        ++*cpuPayloads;
    }
    return grayscale
        ? resampleGrayscale(source, destinationSize, method, cancelRequested)
        : resampleStraightRgba(source, destinationSize, method, cancelRequested);
}

void initialiseReferenceExtents(QVector<LayerNode> *layers,
                                const QSize &oldDocumentSize)
{
    if (!layers) {
        return;
    }
    for (LayerNode &layer : *layers) {
        bool changed = false;
        if ((layer.type == LayerType::Raster || layer.type == LayerType::BaseImage)
            && (layer.rasterReferenceSize.isEmpty()
                || !layer.rasterReferenceSize.isValid())) {
            layer.rasterReferenceSize = oldDocumentSize;
            changed = true;
        }
        if (!layer.maskImage.isNull()
            && (layer.maskReferenceSize.isEmpty()
                || !layer.maskReferenceSize.isValid())) {
            layer.maskReferenceSize = oldDocumentSize;
            changed = true;
        }
        initialiseReferenceExtents(&layer.children, oldDocumentSize);
        if (changed) {
            ++layer.revision;
        }
    }
}

bool resampleLayerTree(QVector<LayerNode> *layers,
                       const double scaleX,
                       const double scaleY,
                       const ImageResampleMethod method,
                       const ImageResampleAccelerator &accelerator,
                       const std::atomic_bool *cancelRequested,
                       int *gpuPayloads,
                       int *cpuPayloads,
                       QString *firstGpuFallbackReason,
                       QString *errorMessage)
{
    if (!layers) {
        return false;
    }
    for (LayerNode &layer : *layers) {
        if (cancelled(cancelRequested)) {
            setError(errorMessage, QStringLiteral("Image resize cancelled."));
            return false;
        }
        bool changed = false;
        if ((layer.type == LayerType::Raster || layer.type == LayerType::BaseImage)
            && !layer.rasterImage.isNull()) {
            const QSize outputSize(
                scaledExtent(layer.rasterImage.width(), scaleX),
                scaledExtent(layer.rasterImage.height(), scaleY));
            if (outputSize.isEmpty()
                || !persistentImageSizeIsSafe(outputSize, layer.rasterImage.depth())) {
                setError(errorMessage,
                         QStringLiteral("A raster layer would exceed the supported "
                                        "32768-pixel or exact snapshot limit."));
                return false;
            }
            QImage resized = resamplePayload(
                layer.rasterImage,
                outputSize,
                method,
                false,
                accelerator,
                cancelRequested,
                gpuPayloads,
                cpuPayloads,
                firstGpuFallbackReason);
            if (resized.isNull()) {
                setError(errorMessage,
                         cancelled(cancelRequested)
                             ? QStringLiteral("Image resize cancelled.")
                             : QStringLiteral("Could not resample a raster layer."));
                return false;
            }
            layer.rasterImage = std::move(resized);
            changed = true;
        }
        if (!layer.maskImage.isNull()) {
            const QSize outputSize = layer.maskImage.size() == QSize(1, 1)
                ? QSize(1, 1)
                : QSize(scaledExtent(layer.maskImage.width(), scaleX),
                        scaledExtent(layer.maskImage.height(), scaleY));
            if (outputSize.isEmpty()
                || !persistentImageSizeIsSafe(outputSize, 8)) {
                setError(errorMessage,
                         QStringLiteral("A layer mask would exceed the supported "
                                        "32768-pixel or exact snapshot limit."));
                return false;
            }
            QImage resized = resamplePayload(
                layer.maskImage,
                outputSize,
                method,
                true,
                accelerator,
                cancelRequested,
                gpuPayloads,
                cpuPayloads,
                firstGpuFallbackReason);
            if (resized.isNull()) {
                setError(errorMessage,
                         cancelled(cancelRequested)
                             ? QStringLiteral("Image resize cancelled.")
                             : QStringLiteral("Could not resample a layer mask."));
                return false;
            }
            layer.maskImage = std::move(resized);
            changed = true;
        }
        if (!resampleLayerTree(&layer.children,
                               scaleX,
                               scaleY,
                               method,
                               accelerator,
                               cancelRequested,
                               gpuPayloads,
                               cpuPayloads,
                               firstGpuFallbackReason,
                               errorMessage)) {
            return false;
        }
        if (changed) {
            ++layer.revision;
        }
    }
    return true;
}

bool scaleLayerCoordinateSystems(QVector<LayerNode> *layers,
                                 const double scaleX,
                                 const double scaleY,
                                 const std::atomic_bool *cancelRequested,
                                 QString *errorMessage)
{
    if (!layers || !std::isfinite(scaleX) || !std::isfinite(scaleY)
        || scaleX <= 0.0 || scaleY <= 0.0) {
        setError(errorMessage, QStringLiteral("The layer scale is invalid."));
        return false;
    }
    const QTransform documentScale = QTransform::fromScale(scaleX, scaleY);
    const QTransform inverseDocumentScale = QTransform::fromScale(
        1.0 / scaleX, 1.0 / scaleY);
    for (LayerNode &layer : *layers) {
        if (cancelled(cancelRequested)) {
            setError(errorMessage, QStringLiteral("Image resize cancelled."));
            return false;
        }
        bool changed = false;
        if ((layer.type == LayerType::Raster || layer.type == LayerType::BaseImage)
            && layer.rasterReferenceSize.isValid()
            && !layer.rasterReferenceSize.isEmpty()) {
            const QSize scaledReference(
                scaledExtent(layer.rasterReferenceSize.width(), scaleX),
                scaledExtent(layer.rasterReferenceSize.height(), scaleY));
            if (scaledReference.isEmpty()) {
                setError(errorMessage,
                         QStringLiteral("A raster reference extent would exceed "
                                        "the supported 32768-pixel limit."));
                return false;
            }
            layer.rasterReferenceSize = scaledReference;
            layer.rasterReferenceOrigin = QPointF(
                layer.rasterReferenceOrigin.x() * scaleX,
                layer.rasterReferenceOrigin.y() * scaleY);
            changed = true;
        }
        if (!layer.maskImage.isNull()
            && layer.maskReferenceSize.isValid()
            && !layer.maskReferenceSize.isEmpty()) {
            const QSize scaledReference(
                scaledExtent(layer.maskReferenceSize.width(), scaleX),
                scaledExtent(layer.maskReferenceSize.height(), scaleY));
            if (scaledReference.isEmpty()) {
                setError(errorMessage,
                         QStringLiteral("A mask reference extent would exceed "
                                        "the supported 32768-pixel limit."));
                return false;
            }
            layer.maskReferenceSize = scaledReference;
            layer.maskReferenceOrigin = QPointF(
                layer.maskReferenceOrigin.x() * scaleX,
                layer.maskReferenceOrigin.y() * scaleY);
            changed = true;
        }

        if (layer.type == LayerType::Vector) {
            if (!scaleVectorLayerData(&layer.vectorData, scaleX, scaleY)) { setError(errorMessage, QStringLiteral("A vector shape could not be scaled safely.")); return false; }
            changed = true;
        }
        if (layer.type == LayerType::Text) {
            if (!scaleTextLayerData(&layer.textData, scaleX, scaleY)) { setError(errorMessage, QStringLiteral("A text layer could not be scaled safely.")); return false; }
            changed = true;
        }

        layer.transform = inverseDocumentScale
            * layer.transform * documentScale;
        changed = true;
        if (!scaleLayerCoordinateSystems(&layer.children,
                                         scaleX,
                                         scaleY,
                                         cancelRequested,
                                         errorMessage)) {
            return false;
        }
        if (changed) {
            ++layer.revision;
        }
    }
    return true;
}

QByteArray materialiseSelectionRow(const SelectionMask::Snapshot &snapshot,
                                   const int y)
{
    QByteArray row(snapshot.size.width(),
                   static_cast<char>(snapshot.active
                                         ? snapshot.implicitCoverage : 0));
    if (!snapshot.active || y < 0 || y >= snapshot.size.height()) {
        return row;
    }
    const int tileY = y / SelectionMask::TileSize;
    const int localY = y - tileY * SelectionMask::TileSize;
    const int tileColumns = (snapshot.size.width()
                             + SelectionMask::TileSize - 1)
        / SelectionMask::TileSize;
    for (int tileX = 0; tileX < tileColumns; ++tileX) {
        const QPoint tileIndex(tileX, tileY);
        const auto found = snapshot.tiles.constFind(
            SelectionMask::tileKey(tileIndex));
        if (found == snapshot.tiles.cend()) {
            continue;
        }
        const int tileWidth = std::min(
            SelectionMask::TileSize,
            snapshot.size.width() - tileX * SelectionMask::TileSize);
        const int tileHeight = std::min(
            SelectionMask::TileSize,
            snapshot.size.height() - tileY * SelectionMask::TileSize);
        if (localY >= tileHeight || found->size() < tileWidth * tileHeight) {
            continue;
        }
        std::memcpy(row.data() + tileX * SelectionMask::TileSize,
                    found->constData() + localY * tileWidth,
                    static_cast<size_t>(tileWidth));
    }
    return row;
}

SelectionMask::Snapshot resampleSelection(
    const SelectionMask &source,
    const QSize &destinationSize,
    const ImageResampleMethod method,
    const std::atomic_bool *cancelRequested)
{
    SelectionMask destination(destinationSize);
    if (!source.isActive() || source.isEmpty()) {
        destination.deactivate();
        return destination.snapshot();
    }
    if (source.isFull()) {
        destination.selectAll();
        return destination.snapshot();
    }

    const AxisFilter xFilters = filterAxis(
        source.size().width(), destinationSize.width(), method);
    const AxisFilter yFilters = filterAxis(
        source.size().height(), destinationSize.height(), method);
    if (xFilters.size() != destinationSize.width()
        || yFilters.size() != destinationSize.height()) {
        return {};
    }

    const SelectionMask::Snapshot snapshot = source.snapshot();
    destination.selectNone();
    constexpr int StripHeight = SelectionMask::TileSize;
    const int blockWidth = filteredBlockWidth(destinationSize.width(), yFilters, 1);
    for (int startY = 0; startY < destinationSize.height(); startY += StripHeight) {
        if (cancelled(cancelRequested)) {
            return {};
        }
        const int height = std::min(
            StripHeight, destinationSize.height() - startY);
        QImage strip(QSize(destinationSize.width(), height),
                     QImage::Format_Grayscale8);
        if (strip.isNull()) {
            return {};
        }
        strip.fill(0);

        for (int startX = 0; startX < destinationSize.width(); startX += blockWidth) {
            const int count = std::min(
                blockWidth, destinationSize.width() - startX);
            std::map<int, QVector<double>> horizontalRows;
            for (int localY = 0; localY < height; ++localY) {
                const int destinationY = startY + localY;
                if (cancelled(cancelRequested)) {
                    return {};
                }
                std::set<int> neededRows;
                for (const FilterTap &tap : yFilters.at(destinationY)) {
                    neededRows.insert(tap.index);
                    if (horizontalRows.find(tap.index) == horizontalRows.end()) {
                        const QByteArray sourceRow = materialiseSelectionRow(
                            snapshot, tap.index);
                        QVector<double> filtered = horizontallyFilteredGray(
                            reinterpret_cast<const uchar *>(sourceRow.constData()),
                            xFilters,
                            startX,
                            count,
                            cancelRequested);
                        if (filtered.size() != count) {
                            return {};
                        }
                        horizontalRows.emplace(tap.index, std::move(filtered));
                    }
                }
                for (auto iterator = horizontalRows.begin();
                     iterator != horizontalRows.end();) {
                    if (neededRows.find(iterator->first) == neededRows.end()) {
                        iterator = horizontalRows.erase(iterator);
                    } else {
                        ++iterator;
                    }
                }
                uchar *row = strip.scanLine(localY) + startX;
                for (int localX = 0; localX < count; ++localX) {
                    double value = 0.0;
                    for (const FilterTap &tap : yFilters.at(destinationY)) {
                        value += horizontalRows.at(tap.index)[localX] * tap.weight;
                    }
                    row[localX] = static_cast<uchar>(
                        std::clamp(qRound(value), 0, 255));
                }
            }
        }
        if (!destination.setCoverageImage(
                QRect(0, startY, destinationSize.width(), height), strip)) {
            return {};
        }
    }
    if (destination.isEmpty()) {
        destination.deactivate();
    }
    return destination.snapshot();
}

QVector<double> scaledGuides(const QVector<double> &guides,
                             const double scale,
                             const double maximum)
{
    QVector<double> result;
    result.reserve(guides.size());
    for (const double guide : guides) {
        const double transformed = guide * scale;
        if (std::isfinite(transformed)
            && transformed >= 0.0 && transformed <= maximum) {
            result.push_back(transformed);
        }
    }
    return result;
}

bool transformsNearlyEqual(const QTransform &left, const QTransform &right)
{
    constexpr double Epsilon = 1.0e-9;
    const auto close = [=](const double a, const double b) {
        return std::abs(a - b) <= Epsilon
            * std::max({1.0, std::abs(a), std::abs(b)});
    };
    return close(left.m11(), right.m11())
        && close(left.m12(), right.m12())
        && close(left.m13(), right.m13())
        && close(left.m21(), right.m21())
        && close(left.m22(), right.m22())
        && close(left.m23(), right.m23())
        && close(left.m31(), right.m31())
        && close(left.m32(), right.m32())
        && close(left.m33(), right.m33());
}

bool pointsNearlyEqual(const QPointF &left, const QPointF &right)
{
    constexpr double Epsilon = 1.0e-9;
    const auto close = [=](const double a, const double b) {
        return std::abs(a - b) <= Epsilon
            * std::max({1.0, std::abs(a), std::abs(b)});
    };
    return close(left.x(), right.x()) && close(left.y(), right.y());
}

bool imageMetadataMatches(const QImage &prepared, const QImage &source)
{
    return prepared.colorSpace() == source.colorSpace()
        && prepared.devicePixelRatio() == source.devicePixelRatio()
        && prepared.dotsPerMeterX() == source.dotsPerMeterX()
        && prepared.dotsPerMeterY() == source.dotsPerMeterY();
}

bool validatePreparedLayerTree(const QVector<LayerNode> &sourceLayers,
                               const QVector<LayerNode> &preparedLayers,
                               const QSize &oldDocumentSize,
                               const double scaleX,
                               const double scaleY,
                               QString *errorMessage)
{
    if (sourceLayers.size() != preparedLayers.size()) {
        setError(errorMessage,
                 QStringLiteral("Image Size changed the layer-tree structure unexpectedly."));
        return false;
    }
    const QTransform documentScale = QTransform::fromScale(scaleX, scaleY);
    const QTransform inverseDocumentScale = QTransform::fromScale(
        1.0 / scaleX, 1.0 / scaleY);
    for (qsizetype index = 0; index < sourceLayers.size(); ++index) {
        const LayerNode &source = sourceLayers.at(index);
        const LayerNode &prepared = preparedLayers.at(index);
        if (source.id != prepared.id || source.type != prepared.type
            || !finiteTransform(prepared.transform)
            || !finitePoint(prepared.rasterReferenceOrigin)
            || !finitePoint(prepared.maskReferenceOrigin)) {
            setError(errorMessage,
                     QStringLiteral("Image Size produced invalid layer identity or coordinate data."));
            return false;
        }

        const QTransform expectedTransform = inverseDocumentScale
            * source.transform * documentScale;
        if (!transformsNearlyEqual(prepared.transform, expectedTransform)) {
            setError(errorMessage,
                     QStringLiteral("Image Size produced an inconsistent layer transform."));
            return false;
        }

        if (source.type == LayerType::Vector) {
            VectorLayerData expectedVector = source.vectorData;
            if (!scaleVectorLayerData(&expectedVector, scaleX, scaleY)
                || prepared.vectorData != expectedVector) {
                setError(errorMessage,
                         QStringLiteral("Image Size produced inconsistent vector geometry."));
                return false;
            }
        } else if (prepared.vectorData != source.vectorData) {
            setError(errorMessage,
                     QStringLiteral("Image Size changed unrelated vector payload data."));
            return false;
        }

        if ((source.type == LayerType::Raster || source.type == LayerType::BaseImage)
            && !source.rasterImage.isNull()) {
            const QSize expectedSize(scaledExtent(source.rasterImage.width(), scaleX),
                                     scaledExtent(source.rasterImage.height(), scaleY));
            const QImage::Format expectedFormat = source.rasterImage.depth() > 32
                ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
            if (prepared.rasterImage.isNull()
                || prepared.rasterImage.size() != expectedSize
                || prepared.rasterImage.format() != expectedFormat
                || !imageMetadataMatches(prepared.rasterImage,
                                         source.rasterImage)) {
                setError(errorMessage,
                         QStringLiteral("Image Size produced an incompatible raster payload."));
                return false;
            }
            const QSize sourceReference = source.rasterReferenceSize.isValid()
                    && !source.rasterReferenceSize.isEmpty()
                ? source.rasterReferenceSize : oldDocumentSize;
            const QSize expectedReference(
                scaledExtent(sourceReference.width(), scaleX),
                scaledExtent(sourceReference.height(), scaleY));
            const QPointF expectedOrigin(source.rasterReferenceOrigin.x() * scaleX,
                                         source.rasterReferenceOrigin.y() * scaleY);
            if (prepared.rasterReferenceSize != expectedReference
                || !pointsNearlyEqual(prepared.rasterReferenceOrigin, expectedOrigin)) {
                setError(errorMessage,
                         QStringLiteral("Image Size produced inconsistent raster coordinates."));
                return false;
            }
        } else if (!prepared.rasterImage.isNull()) {
            setError(errorMessage,
                     QStringLiteral("Image Size introduced an unexpected raster payload."));
            return false;
        }

        if (!source.maskImage.isNull()) {
            const QSize expectedSize = source.maskImage.size() == QSize(1, 1)
                ? QSize(1, 1)
                : QSize(scaledExtent(source.maskImage.width(), scaleX),
                        scaledExtent(source.maskImage.height(), scaleY));
            if (prepared.maskImage.isNull()
                || prepared.maskImage.size() != expectedSize
                || prepared.maskImage.format() != QImage::Format_Grayscale8
                || !imageMetadataMatches(prepared.maskImage,
                                         source.maskImage)) {
                setError(errorMessage,
                         QStringLiteral("Image Size produced an incompatible mask payload."));
                return false;
            }
            const QSize sourceReference = source.maskReferenceSize.isValid()
                    && !source.maskReferenceSize.isEmpty()
                ? source.maskReferenceSize : oldDocumentSize;
            const QSize expectedReference(
                scaledExtent(sourceReference.width(), scaleX),
                scaledExtent(sourceReference.height(), scaleY));
            const QPointF expectedOrigin(source.maskReferenceOrigin.x() * scaleX,
                                         source.maskReferenceOrigin.y() * scaleY);
            if (prepared.maskReferenceSize != expectedReference
                || !pointsNearlyEqual(prepared.maskReferenceOrigin, expectedOrigin)) {
                setError(errorMessage,
                         QStringLiteral("Image Size produced inconsistent mask coordinates."));
                return false;
            }
        } else if (!prepared.maskImage.isNull()) {
            setError(errorMessage,
                     QStringLiteral("Image Size introduced an unexpected mask payload."));
            return false;
        }

        if (!validatePreparedLayerTree(source.children,
                                       prepared.children,
                                       oldDocumentSize,
                                       scaleX,
                                       scaleY,
                                       errorMessage)) {
            return false;
        }
    }
    return true;
}

bool validatePreparedResult(const PhotoDocument &document,
                            const ImageSizeRequest &request,
                            const ImageSizeResult &prepared,
                            QString *errorMessage)
{
    const QSize oldSize = document.sourceImage().size();
    if (prepared.canvasImage.isNull()
        || prepared.canvasImage.size() != request.pixelSize
        || prepared.selection.size != request.pixelSize
        || !validResolution(prepared.resolutionX)
        || !validResolution(prepared.resolutionY)
        || !std::isfinite(prepared.scaleX)
        || !std::isfinite(prepared.scaleY)
        || prepared.scaleX <= 0.0 || prepared.scaleY <= 0.0) {
        setError(errorMessage,
                 QStringLiteral("Image Size produced an invalid prepared document."));
        return false;
    }
    const QImage::Format expectedCanvasFormat = document.sourceImage().depth() > 32
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    if (prepared.canvasImage.format() != expectedCanvasFormat
        || prepared.canvasImage.colorSpace()
            != document.sourceImage().colorSpace()
        || prepared.canvasImage.devicePixelRatio()
            != document.sourceImage().devicePixelRatio()
        || prepared.canvasImage.dotsPerMeterX()
            != dotsPerMetreFromDpi(prepared.resolutionX)
        || prepared.canvasImage.dotsPerMeterY()
            != dotsPerMetreFromDpi(prepared.resolutionY)) {
        setError(errorMessage,
                 QStringLiteral("Image Size produced incompatible canvas metadata."));
        return false;
    }
    const auto guidesValid = [](const QVector<double> &guides,
                                const double maximum) {
        return std::all_of(guides.cbegin(), guides.cend(),
                           [maximum](const double guide) {
                               return std::isfinite(guide)
                                   && guide >= 0.0 && guide <= maximum;
                           });
    };
    if (!guidesValid(prepared.horizontalGuides, request.pixelSize.height())
        || !guidesValid(prepared.verticalGuides, request.pixelSize.width())) {
        setError(errorMessage,
                 QStringLiteral("Image Size produced invalid guide coordinates."));
        return false;
    }
    if (!validatePreparedLayerTree(document.layers(),
                                   prepared.layers,
                                   oldSize,
                                   prepared.scaleX,
                                   prepared.scaleY,
                                   errorMessage)) {
        return false;
    }
    return true;
}

} // namespace

QImage resampleStraightRgbaCpuReference(
    const QImage &source,
    const QSize &destinationSize,
    const ImageResampleMethod method,
    const std::atomic_bool *cancelRequested)
{
    return resampleStraightRgba(source, destinationSize, method, cancelRequested);
}

QImage resampleGrayscaleCpuReference(
    const QImage &source,
    const QSize &destinationSize,
    const ImageResampleMethod method,
    const std::atomic_bool *cancelRequested)
{
    return resampleGrayscale(source, destinationSize, method, cancelRequested);
}

bool buildImageSizeResult(const PhotoDocument &document,
                          const ImageSizeRequest &request,
                          ImageSizeResult *result,
                          const std::atomic_bool *cancelRequested,
                          QString *errorMessage)
{
    if (!result || !document.hasImage()) {
        setError(errorMessage, QStringLiteral("There is no document image to resize."));
        return false;
    }
    const double requestedResolutionX = request.resolutionX > 0.0
        ? request.resolutionX : document.resolutionX();
    const double requestedResolutionY = request.resolutionY > 0.0
        ? request.resolutionY : document.resolutionY();
    if (!validResolution(requestedResolutionX)
        || !validResolution(requestedResolutionY)) {
        setError(errorMessage,
                 QStringLiteral("Image resolution must be between 1 and 9600 ppi."));
        return false;
    }
    if (!validResampleMethod(request.method)) {
        setError(errorMessage,
                 QStringLiteral("The requested image resampling method is invalid."));
        return false;
    }
    if (request.pixelSize.width() < 1 || request.pixelSize.height() < 1
        || request.pixelSize.width() > 32768
        || request.pixelSize.height() > 32768) {
        setError(errorMessage,
                 QStringLiteral("Image dimensions must be between 1 and 32768 pixels."));
        return false;
    }
    if (cancelled(cancelRequested)) {
        setError(errorMessage, QStringLiteral("Image resize cancelled."));
        return false;
    }

    const QSize oldSize = document.sourceImage().size();
    if (oldSize.isEmpty()) {
        setError(errorMessage,
                 QStringLiteral("The current document dimensions are invalid."));
        return false;
    }
    if (!request.resamplePixels && request.pixelSize != oldSize) {
        setError(errorMessage,
                 QStringLiteral("Pixel dimensions cannot change while resampling is disabled."));
        return false;
    }

    ImageSizeResult prepared;
    prepared.resolutionX = requestedResolutionX;
    prepared.resolutionY = requestedResolutionY;
    const bool pixelsResampled = request.resamplePixels && request.pixelSize != oldSize;
    prepared.pixelsResampled = pixelsResampled;

    if (!pixelsResampled) {
        if (cancelled(cancelRequested)) {
            setError(errorMessage, QStringLiteral("Image resize cancelled."));
            return false;
        }
        prepared.canvasImage = document.sourceImage();
        prepared.canvasImage.setDotsPerMeterX(dotsPerMetreFromDpi(requestedResolutionX));
        prepared.canvasImage.setDotsPerMeterY(dotsPerMetreFromDpi(requestedResolutionY));
        if (cancelled(cancelRequested)) {
            setError(errorMessage, QStringLiteral("Image resize cancelled."));
            return false;
        }
        prepared.layers = document.layers();
        prepared.selection = document.selectionMask().snapshot();
        prepared.horizontalGuides = document.horizontalGuides();
        prepared.verticalGuides = document.verticalGuides();
        if (cancelled(cancelRequested)) {
            setError(errorMessage, QStringLiteral("Image resize cancelled."));
            return false;
        }
        if (!validatePreparedResult(document, request, prepared, errorMessage)) {
            return false;
        }
        *result = std::move(prepared);
        return true;
    }

    if (!persistentImageSizeIsSafe(
            request.pixelSize, document.sourceImage().depth())) {
        setError(errorMessage,
                 QStringLiteral("The resized image would exceed the exact "
                                "Hot/Warm/Cold snapshot image limit."));
        return false;
    }

    const double scaleX = request.pixelSize.width()
        / static_cast<double>(oldSize.width());
    const double scaleY = request.pixelSize.height()
        / static_cast<double>(oldSize.height());
    prepared.scaleX = scaleX;
    prepared.scaleY = scaleY;

    quint64 estimatedPreparedBytes = 0;
    const quint64 canvasBytesPerPixel = document.sourceImage().depth() > 32
        ? 8u : 4u;
    if (!checkedImageBytes(request.pixelSize,
                           canvasBytesPerPixel,
                           &estimatedPreparedBytes,
                           request.maximumPreparedBytes,
                           errorMessage)
        || !preflightLayerTree(document.layers(),
                               oldSize,
                               scaleX,
                               scaleY,
                               request.maximumPreparedBytes,
                               &estimatedPreparedBytes,
                               cancelRequested,
                               errorMessage)) {
        return false;
    }
    if (document.selectionMask().isActive()
        && !document.selectionMask().isEmpty()
        && !document.selectionMask().isFull()
        && !checkedImageBytes(request.pixelSize,
                              1u,
                              &estimatedPreparedBytes,
                              request.maximumPreparedBytes,
                              errorMessage)) {
        return false;
    }
    prepared.estimatedPreparedBytes = estimatedPreparedBytes;
    if (cancelled(cancelRequested)) {
        setError(errorMessage, QStringLiteral("Image resize cancelled."));
        return false;
    }

    prepared.canvasImage = resamplePayload(
        document.sourceImage(),
        request.pixelSize,
        request.method,
        false,
        request.accelerator,
        cancelRequested,
        &prepared.gpuPayloads,
        &prepared.cpuPayloads,
        &prepared.firstGpuFallbackReason);
    if (prepared.canvasImage.isNull()) {
        setError(errorMessage,
                 cancelled(cancelRequested)
                     ? QStringLiteral("Image resize cancelled.")
                     : QStringLiteral("Could not resample the document canvas."));
        return false;
    }
    prepared.canvasImage.setDotsPerMeterX(dotsPerMetreFromDpi(requestedResolutionX));
    prepared.canvasImage.setDotsPerMeterY(dotsPerMetreFromDpi(requestedResolutionY));

    prepared.layers = document.layers();
    initialiseReferenceExtents(&prepared.layers, oldSize);
    if (!resampleLayerTree(&prepared.layers,
                           scaleX,
                           scaleY,
                           request.method,
                           request.accelerator,
                           cancelRequested,
                           &prepared.gpuPayloads,
                           &prepared.cpuPayloads,
                           &prepared.firstGpuFallbackReason,
                           errorMessage)) {
        return false;
    }
    if (!scaleLayerCoordinateSystems(&prepared.layers,
                                     scaleX,
                                     scaleY,
                                     cancelRequested,
                                     errorMessage)) {
        return false;
    }

    prepared.selection = resampleSelection(
        document.selectionMask(), request.pixelSize,
        request.method, cancelRequested);
    if (prepared.selection.size != request.pixelSize) {
        setError(errorMessage,
                 cancelled(cancelRequested)
                     ? QStringLiteral("Image resize cancelled.")
                     : QStringLiteral("Could not resample the document selection."));
        return false;
    }
    prepared.horizontalGuides = scaledGuides(
        document.horizontalGuides(), scaleY, request.pixelSize.height());
    prepared.verticalGuides = scaledGuides(
        document.verticalGuides(), scaleX, request.pixelSize.width());

    if (cancelled(cancelRequested)) {
        setError(errorMessage, QStringLiteral("Image resize cancelled."));
        return false;
    }
    if (!validatePreparedResult(document, request, prepared, errorMessage)) {
        return false;
    }
    *result = std::move(prepared);
    return true;
}

} // namespace vfx
