#include "VectorRasterizer.h"

#include <QBuffer>
#include <QDataStream>
#include <QHash>
#include <QIODevice>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace vfx {
namespace {

constexpr qsizetype MaximumCacheBytes = qsizetype(128) * 1024 * 1024;
constexpr qsizetype MaximumCacheEntries = 2048;
constexpr qsizetype MaximumGeometryCacheEntries = 512;
constexpr qsizetype MaximumGeometryCacheElements = 4 * 1024 * 1024;
constexpr qint64 MaximumFeatherWorkingBytes = qint64(512) * 1024 * 1024;

struct CacheEntry {
    QImage image;
    qsizetype bytes = 0;
    quint64 useSerial = 0;
};

struct ResolvedShapeGeometry {
    QPainterPath base;
    QPainterPath strokePath;
    QPainterPath insideOutline;
    QRectF bounds;
    bool visible = false;
};

struct GeometryCacheEntry {
    ResolvedShapeGeometry geometry;
    qsizetype elements = 0;
    quint64 useSerial = 0;
};

QMutex &cacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QByteArray, CacheEntry> &cacheEntries()
{
    static QHash<QByteArray, CacheEntry> entries;
    return entries;
}

QHash<QByteArray, GeometryCacheEntry> &geometryCacheEntries()
{
    static QHash<QByteArray, GeometryCacheEntry> entries;
    return entries;
}

qsizetype &geometryResidentElements()
{
    static qsizetype elements = 0;
    return elements;
}

qsizetype &residentBytes()
{
    static qsizetype bytes = 0;
    return bytes;
}

quint64 &useSerial()
{
    static quint64 serial = 0;
    return serial;
}

qsizetype imageBytes(const QImage &image)
{
    if (image.isNull() || image.sizeInBytes() <= 0) return 0;
    return image.sizeInBytes();
}

QByteArray cacheKey(const LayerNode &layer,
                    const quint64 layerFingerprint,
                    const QSize &previewSize,
                    const QRect &previewRegion,
                    const QSize &documentSize,
                    const QTransform &worldTransform,
                    const QImage::Format format,
                    const QColorSpace &colourSpace,
                    const bool forceOpaquePixelAlpha,
                    const bool grayscaleDocument)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << layer.id
           << std::max<quint64>(1, layer.revision)
           << layerFingerprint
           << previewSize
           << previewRegion
           << documentSize
           << worldTransform
           << static_cast<qint32>(format)
           << colourSpace.iccProfile()
           << forceOpaquePixelAlpha
           << grayscaleDocument;
    return bytes;
}

bool findCached(const QByteArray &key, QImage *image)
{
    QMutexLocker<QMutex> locker(&cacheMutex());
    auto &entries = cacheEntries();
    const auto iterator = entries.find(key);
    if (iterator == entries.end()) return false;
    iterator->useSerial = ++useSerial();
    if (image) *image = iterator->image;
    return true;
}

void evictIfNeededLocked()
{
    auto &entries = cacheEntries();
    while ((residentBytes() > MaximumCacheBytes
            || entries.size() > MaximumCacheEntries)
           && !entries.isEmpty()) {
        auto oldest = entries.begin();
        for (auto iterator = entries.begin(); iterator != entries.end(); ++iterator) {
            if (iterator->useSerial < oldest->useSerial) oldest = iterator;
        }
        residentBytes() = std::max<qsizetype>(0, residentBytes() - oldest->bytes);
        entries.erase(oldest);
    }
}

void insertCached(const QByteArray &key, const QImage &image)
{
    const qsizetype bytes = imageBytes(image);
    if (bytes <= 0 || bytes > MaximumCacheBytes) return;
    QMutexLocker<QMutex> locker(&cacheMutex());
    auto &entries = cacheEntries();
    const auto existing = entries.find(key);
    if (existing != entries.end()) {
        residentBytes() = std::max<qsizetype>(0, residentBytes() - existing->bytes);
        entries.erase(existing);
    }
    entries.insert(key, CacheEntry {image, bytes, ++useSerial()});
    residentBytes() += bytes;
    evictIfNeededLocked();
}

QByteArray geometryCacheKey(const LayerNode &layer,
                            const quint64 layerFingerprint,
                            const VectorShape &shape,
                            const QTransform &worldTransform)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << layer.id
           << std::max<quint64>(1, layer.revision)
           << layerFingerprint
           << shape.id
           << std::max<quint64>(1, shape.revision)
           << worldTransform;
    return bytes;
}

void evictGeometryIfNeededLocked()
{
    auto &entries = geometryCacheEntries();
    while ((entries.size() > MaximumGeometryCacheEntries
            || geometryResidentElements() > MaximumGeometryCacheElements)
           && !entries.isEmpty()) {
        auto oldest = entries.begin();
        for (auto iterator = entries.begin(); iterator != entries.end(); ++iterator) {
            if (iterator->useSerial < oldest->useSerial) oldest = iterator;
        }
        geometryResidentElements() = std::max<qsizetype>(
            0, geometryResidentElements() - oldest->elements);
        entries.erase(oldest);
    }
}

ResolvedShapeGeometry resolveShapeGeometry(const LayerNode &layer,
                                           const quint64 layerFingerprint,
                                           const VectorShape &shape,
                                           const QTransform &worldTransform)
{
    const QByteArray key = geometryCacheKey(layer, layerFingerprint,
                                            shape, worldTransform);
    {
        QMutexLocker<QMutex> locker(&cacheMutex());
        auto &entries = geometryCacheEntries();
        const auto iterator = entries.find(key);
        if (iterator != entries.end()) {
            iterator->useSerial = ++useSerial();
            return iterator->geometry;
        }
    }

    ResolvedShapeGeometry resolved;
    resolved.base = shape.pathForWorldTransform(worldTransform);
    const auto includeBounds = [&resolved](const QRectF &candidate) {
        if (candidate.isEmpty()) return;
        resolved.bounds = resolved.bounds.isEmpty()
            ? candidate : resolved.bounds.united(candidate);
        resolved.visible = true;
    };
    if (shape.fill.enabled && !shape.isOpenPath() && !resolved.base.isEmpty()) {
        includeBounds(resolved.base.boundingRect());
    }
    if (shape.stroke.enabled) {
        if (!shape.isOpenPath()
            && shape.stroke.alignment == VectorStrokeAlignment::Inside) {
            resolved.insideOutline = shape.strokeOutlineForWorldTransform(
                worldTransform, 2.0);
            // The visible result is clipped to the semantic path, so its
            // bounding rectangle can never exceed the base geometry. Avoid a
            // costly boolean path union for every revised Bezier curve.
            if (!resolved.base.isEmpty() && !resolved.insideOutline.isEmpty()) {
                includeBounds(resolved.base.boundingRect());
            }
        } else {
            resolved.strokePath = shape.strokePathForWorldTransform(worldTransform);
            if (!resolved.strokePath.isEmpty()) {
                includeBounds(resolved.strokePath.boundingRect());
            }
        }
    }

    const qsizetype elements = resolved.base.elementCount()
        + resolved.strokePath.elementCount()
        + resolved.insideOutline.elementCount();
    if (elements > 0 && elements <= MaximumGeometryCacheElements) {
        QMutexLocker<QMutex> locker(&cacheMutex());
        auto &entries = geometryCacheEntries();
        const auto existing = entries.find(key);
        if (existing != entries.end()) {
            geometryResidentElements() = std::max<qsizetype>(
                0, geometryResidentElements() - existing->elements);
            entries.erase(existing);
        }
        entries.insert(key, GeometryCacheEntry {resolved, elements, ++useSerial()});
        geometryResidentElements() += elements;
        evictGeometryIfNeededLocked();
    }
    return resolved;
}

QColor effectiveStyleColour(const QColor &inputColour,
                            const double styleOpacity,
                            const bool forceOpaquePixelAlpha,
                            const bool grayscaleDocument)
{
    QColor colour = inputColour;
    if (grayscaleDocument) {
        const double luminance = std::clamp(
            0.2126 * colour.redF() + 0.7152 * colour.greenF()
                + 0.0722 * colour.blueF(),
            0.0,
            1.0);
        colour.setRgbF(luminance, luminance, luminance, colour.alphaF());
    }
    const double alpha = forceOpaquePixelAlpha
        ? 1.0
        : std::clamp(colour.alphaF() * styleOpacity, 0.0, 1.0);
    colour.setAlphaF(alpha);
    return colour;
}

QImage transparentImage(const QSize &size,
                        const QImage::Format format,
                        const QColorSpace &colourSpace)
{
    if (size.isEmpty()) return {};
    QImage image(size, format);
    if (image.isNull()) return {};
    image.fill(Qt::transparent);
    image.setColorSpace(colourSpace);
    return image;
}

QRectF semanticContentBounds(const LayerNode &layer,
                             const QTransform &worldTransform,
                             const quint64 layerFingerprint)
{
    QRectF result;
    for (const VectorShape &shape : layer.vectorData.objects) {
        if (!shape.isSafe()) continue;
        const QRectF bounds = resolveShapeGeometry(
            layer, layerFingerprint, shape, worldTransform).bounds;
        if (!bounds.isEmpty()) result = result.isEmpty() ? bounds : result.united(bounds);
    }
    return result;
}

QImage renderSemanticRegion(const LayerNode &layer,
                            const quint64 layerFingerprint,
                            const QSize &previewSize,
                            const QRect &previewRegion,
                            const QSize &documentSize,
                            const QTransform &worldTransform,
                            const QImage::Format format,
                            const QColorSpace &colourSpace,
                            const bool forceOpaquePixelAlpha,
                            const bool grayscaleDocument,
                            const bool coverageOnly,
                            const std::atomic_bool *cancelRequested)
{
    QImage result = transparentImage(previewRegion.size(), format, colourSpace);
    if (result.isNull()) return {};

    const QTransform documentToPreview = QTransform::fromScale(
        previewSize.width() / static_cast<double>(std::max(1, documentSize.width())),
        previewSize.height() / static_cast<double>(std::max(1, documentSize.height())));
    const QTransform documentToTile = documentToPreview
        * QTransform::fromTranslate(-previewRegion.left(), -previewRegion.top());

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setPen(Qt::NoPen);
    painter.setTransform(documentToTile);

    // Project data remains semantic. Fill and stroke outlines are resolved only
    // for the requested tile, so editing a point count, join or width never
    // flattens the vector layer or rebuilds the complete document.
    const QRectF tileBounds(QPointF(0.0, 0.0), QSizeF(result.size()));
    for (const VectorShape &shape : layer.vectorData.objects) {
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
            painter.end();
            return {};
        }
        if (!shape.isSafe()) continue;

        const ResolvedShapeGeometry resolved = resolveShapeGeometry(
            layer, layerFingerprint, shape, worldTransform);
        if (!resolved.visible
            || !documentToTile.mapRect(resolved.bounds).intersects(
                tileBounds.adjusted(-2.0, -2.0, 2.0, 2.0))) {
            continue;
        }

        const bool fillContributes = forceOpaquePixelAlpha
            || shape.fill.colour.alphaF() * shape.fill.opacity > 0.0;
        const bool strokeContributes = forceOpaquePixelAlpha
            || shape.stroke.colour.alphaF() * shape.stroke.opacity > 0.0;
        if (!shape.isOpenPath() && shape.fill.enabled
            && (coverageOnly ? fillContributes
                             : (shape.fill.opacity > 0.0 || forceOpaquePixelAlpha))) {
            painter.setBrush(coverageOnly
                ? QColor(Qt::white)
                : effectiveStyleColour(shape.fill.colour,
                                       shape.fill.opacity,
                                       forceOpaquePixelAlpha,
                                       grayscaleDocument));
            painter.drawPath(resolved.base);
        }
        if (shape.stroke.enabled
            && (coverageOnly ? strokeContributes
                             : (shape.stroke.opacity > 0.0 || forceOpaquePixelAlpha))) {
            const QColor strokeColour = coverageOnly
                ? QColor(Qt::white)
                : effectiveStyleColour(shape.stroke.colour,
                                       shape.stroke.opacity,
                                       forceOpaquePixelAlpha,
                                       grayscaleDocument);
            if (!shape.isOpenPath()
                && shape.stroke.alignment == VectorStrokeAlignment::Inside) {
                if (!resolved.base.isEmpty() && !resolved.insideOutline.isEmpty()) {
                    painter.save();
                    painter.setClipPath(resolved.base, Qt::IntersectClip);
                    painter.setBrush(strokeColour);
                    painter.drawPath(resolved.insideOutline);
                    painter.restore();
                }
            } else if (!resolved.strokePath.isEmpty()) {
                painter.setBrush(strokeColour);
                painter.drawPath(resolved.strokePath);
            }
        }
    }
    painter.end();
    return cancelRequested && cancelRequested->load(std::memory_order_acquire)
        ? QImage() : result;
}

struct DiscreteThreeBoxKernel {
    int support = 0;
    std::array<int, 3> widths {};
    long double normalisation = 1.0L;

    explicit DiscreteThreeBoxKernel(const int requestedSupport)
        : support(std::max(0, requestedSupport))
    {
        const int base = support / 3;
        const int remainder = support % 3;
        for (int pass = 0; pass < 3; ++pass) {
            const int radius = base + (pass < remainder ? 1 : 0);
            widths[pass] = radius * 2 + 1;
            normalisation *= widths[pass];
        }
    }

    double convolveAt(const int target,
                      const int sourceStart,
                      const int sourceCount,
                      const QVector<double> &prefix0,
                      const QVector<double> &prefix1,
                      const QVector<double> &prefix2) const
    {
        if (sourceCount <= 0) return 0.0;
        if (support == 0) {
            const int local = target - sourceStart;
            if (local < 0 || local >= sourceCount) return 0.0;
            return prefix0[local + 1] - prefix0[local];
        }

        const qint64 supportLower = static_cast<qint64>(target) - support;
        const qint64 supportUpper = static_cast<qint64>(target) + support;
        long double weighted = 0.0L;
        for (int subset = 0; subset < 8; ++subset) {
            int shift = 0;
            int bits = 0;
            for (int pass = 0; pass < 3; ++pass) {
                if ((subset & (1 << pass)) != 0) {
                    shift += widths[pass];
                    ++bits;
                }
            }
            const qint64 a = static_cast<qint64>(target) + support - shift;
            qint64 lower = std::max<qint64>(supportLower, sourceStart);
            qint64 upper = std::min<qint64>(
                std::min<qint64>(supportUpper, a),
                static_cast<qint64>(sourceStart) + sourceCount - 1);
            if (upper < lower) continue;

            const int localLower = static_cast<int>(lower - sourceStart);
            const int localUpper = static_cast<int>(upper - sourceStart);
            const long double s0 = prefix0[localUpper + 1] - prefix0[localLower];
            const long double s1 = prefix1[localUpper + 1] - prefix1[localLower];
            const long double s2 = prefix2[localUpper + 1] - prefix2[localLower];
            const long double aa = static_cast<long double>(a);
            const long double term = 0.5L * (
                (aa + 1.0L) * (aa + 2.0L) * s0
                - (2.0L * aa + 3.0L) * s1
                + s2);
            weighted += (bits % 2 == 0 ? 1.0L : -1.0L) * term;
        }
        return std::clamp(static_cast<double>(weighted / normalisation), 0.0, 1.0);
    }
};

struct FractionalFeatherKernel {
    int support = 0;
    double blend = 0.0;
    DiscreteThreeBoxKernel lower {0};
    DiscreteThreeBoxKernel upper {0};

    explicit FractionalFeatherKernel(const double requestedRadius)
        : support(static_cast<int>(std::ceil(std::max(0.0, requestedRadius)))),
          blend(std::clamp(requestedRadius - std::floor(requestedRadius), 0.0, 1.0)),
          lower(static_cast<int>(std::floor(std::max(0.0, requestedRadius)))),
          upper(support)
    {
    }

    double convolveAt(const int target,
                      const int sourceStart,
                      const int sourceCount,
                      const QVector<double> &prefix0,
                      const QVector<double> &prefix1,
                      const QVector<double> &prefix2) const
    {
        const double low = lower.convolveAt(target, sourceStart, sourceCount,
                                            prefix0, prefix1, prefix2);
        if (blend <= 1.0e-12 || upper.support == lower.support) return low;
        const double high = upper.convolveAt(target, sourceStart, sourceCount,
                                             prefix0, prefix1, prefix2);
        return std::clamp(low + (high - low) * blend, 0.0, 1.0);
    }
};

double sourcePixelAlpha(const QImage &straight, const int x, const int y)
{
    if (straight.depth() > 32) {
        return reinterpret_cast<const QRgba64 *>(straight.constScanLine(y))[x].alpha()
            / 65535.0;
    }
    return straight.constScanLine(y)[x * 4 + 3] / 255.0;
}

QRect expandedRect(const QRect &rect, const int horizontal, const int vertical)
{
    if (rect.isEmpty()) return {};
    const qint64 left = static_cast<qint64>(rect.left()) - horizontal;
    const qint64 top = static_cast<qint64>(rect.top()) - vertical;
    const qint64 right = static_cast<qint64>(rect.right()) + horizontal;
    const qint64 bottom = static_cast<qint64>(rect.bottom()) + vertical;
    const qint64 minimum = std::numeric_limits<int>::min() / 2;
    const qint64 maximum = std::numeric_limits<int>::max() / 2;
    return QRect(QPoint(static_cast<int>(std::clamp(left, minimum, maximum)),
                        static_cast<int>(std::clamp(top, minimum, maximum))),
                 QPoint(static_cast<int>(std::clamp(right, minimum, maximum)),
                        static_cast<int>(std::clamp(bottom, minimum, maximum))));
}

bool buildExactColourCarrier(const QImage &straight,
                             const QImage &coverageStraight,
                             const QRect &sourceRect,
                             const QRect &outputRect,
                             const int supportX,
                             const int supportY,
                             QImage *carrier,
                             const std::atomic_bool *cancelRequested)
{
    if (!carrier || straight.isNull() || coverageStraight.isNull()
        || straight.size() != sourceRect.size()
        || coverageStraight.size() != sourceRect.size()
        || outputRect.isEmpty() || supportX < 0 || supportY < 0) {
        return false;
    }
    const QImage semantic = straight.convertToFormat(QImage::Format_RGBA8888);
    const QImage coverage = coverageStraight.convertToFormat(QImage::Format_RGBA8888);
    if (semantic.isNull() || coverage.isNull()) return false;

    const int sourceWidth = sourceRect.width();
    const int sourceHeight = sourceRect.height();
    const int outputWidth = outputRect.width();
    const int outputHeight = outputRect.height();
    const qint64 nearestCount = static_cast<qint64>(sourceHeight) * outputWidth;
    if (nearestCount <= 0
        || nearestCount > std::numeric_limits<qsizetype>::max()) {
        return false;
    }

    QVector<int> nearestX(static_cast<qsizetype>(nearestCount), -1);
    QVector<int> visibleXs;
    visibleXs.reserve(sourceWidth);
    bool anyVisible = false;
    for (int sy = 0; sy < sourceHeight; ++sy) {
        if (cancelRequested
            && cancelRequested->load(std::memory_order_acquire)) {
            return false;
        }
        visibleXs.clear();
        const uchar *semanticRow = semantic.constScanLine(sy);
        for (int sx = 0; sx < sourceWidth; ++sx) {
            if (semanticRow[sx * 4 + 3] > 0) visibleXs.push_back(sx);
        }
        anyVisible = anyVisible || !visibleXs.isEmpty();
        qsizetype rightIndex = 0;
        for (int ox = 0; ox < outputWidth; ++ox) {
            const int globalX = outputRect.x() + ox;
            while (rightIndex < visibleXs.size()
                   && sourceRect.x() + visibleXs[rightIndex] < globalX) {
                ++rightIndex;
            }
            int candidate = -1;
            if (rightIndex < visibleXs.size()) candidate = visibleXs[rightIndex];
            if (rightIndex > 0) {
                const int leftCandidate = visibleXs[rightIndex - 1];
                if (candidate < 0
                    || std::abs(sourceRect.x() + leftCandidate - globalX)
                        <= std::abs(sourceRect.x() + candidate - globalX)) {
                    candidate = leftCandidate;
                }
            }
            if (candidate >= 0
                && std::abs(sourceRect.x() + candidate - globalX) <= supportX) {
                nearestX[static_cast<qsizetype>(sy) * outputWidth + ox] = candidate;
            }
        }
    }

    QImage output(outputRect.size(), QImage::Format_RGBA8888);
    if (output.isNull()) return false;
    output.fill(Qt::transparent);
    output.setColorSpace(straight.colorSpace());
    if (!anyVisible) {
        *carrier = std::move(output);
        return true;
    }

    QVector<int> envelopeRows(sourceHeight);
    QVector<double> envelopeBoundaries(sourceHeight + 1);
    for (int ox = 0; ox < outputWidth; ++ox) {
        if (cancelRequested
            && cancelRequested->load(std::memory_order_acquire)) {
            return false;
        }
        int envelopeSize = 0;
        const int globalX = outputRect.x() + ox;
        for (int sy = 0; sy < sourceHeight; ++sy) {
            const int sx = nearestX[static_cast<qsizetype>(sy) * outputWidth + ox];
            if (sx < 0) continue;
            const double dx = sourceRect.x() + sx - globalX;
            const double weight = dx * dx;
            if (envelopeSize == 0) {
                envelopeRows[0] = sy;
                envelopeBoundaries[0] = -std::numeric_limits<double>::infinity();
                envelopeBoundaries[1] = std::numeric_limits<double>::infinity();
                envelopeSize = 1;
                continue;
            }
            double boundary = 0.0;
            while (envelopeSize > 0) {
                const int previous = envelopeRows[envelopeSize - 1];
                const int previousX = nearestX[
                    static_cast<qsizetype>(previous) * outputWidth + ox];
                const double previousDx = sourceRect.x() + previousX - globalX;
                const double previousWeight = previousDx * previousDx;
                boundary = ((weight + double(sy) * sy)
                            - (previousWeight + double(previous) * previous))
                    / (2.0 * (sy - previous));
                if (envelopeSize == 1
                    || boundary > envelopeBoundaries[envelopeSize - 1]) {
                    break;
                }
                --envelopeSize;
            }
            if (envelopeSize == 0) {
                envelopeRows[0] = sy;
                envelopeBoundaries[0] = -std::numeric_limits<double>::infinity();
                envelopeBoundaries[1] = std::numeric_limits<double>::infinity();
                envelopeSize = 1;
            } else {
                envelopeRows[envelopeSize] = sy;
                envelopeBoundaries[envelopeSize] = boundary;
                ++envelopeSize;
                envelopeBoundaries[envelopeSize] =
                    std::numeric_limits<double>::infinity();
            }
        }
        if (envelopeSize == 0) continue;

        int selected = 0;
        for (int oy = 0; oy < outputHeight; ++oy) {
            const double queryY = outputRect.y() + oy - sourceRect.y();
            while (selected + 1 < envelopeSize
                   && queryY > envelopeBoundaries[selected + 1]) {
                ++selected;
            }
            int sy = envelopeRows[selected];
            int sx = nearestX[static_cast<qsizetype>(sy) * outputWidth + ox];
            const int queryRow = outputRect.y() + oy - sourceRect.y();
            if (std::abs(sy - queryRow) > supportY) {
                double bestDistance = std::numeric_limits<double>::infinity();
                int bestRow = -1;
                int bestX = -1;
                const int firstRow = std::max(0, queryRow - supportY);
                const int lastRow = std::min(sourceHeight - 1,
                                             queryRow + supportY);
                for (int candidateRow = firstRow; candidateRow <= lastRow;
                     ++candidateRow) {
                    const int candidateX = nearestX[
                        static_cast<qsizetype>(candidateRow) * outputWidth + ox];
                    if (candidateX < 0) continue;
                    const double dx = sourceRect.x() + candidateX - globalX;
                    const double dy = candidateRow - queryRow;
                    const double distance = dx * dx + dy * dy;
                    if (distance < bestDistance
                        || (qFuzzyCompare(distance + 1.0, bestDistance + 1.0)
                            && (candidateRow < bestRow
                                || (candidateRow == bestRow
                                    && candidateX < bestX)))) {
                        bestDistance = distance;
                        bestRow = candidateRow;
                        bestX = candidateX;
                    }
                }
                sy = bestRow;
                sx = bestX;
            }
            if (sy < 0 || sx < 0) continue;

            const uchar *sourcePixel = semantic.constScanLine(sy) + sx * 4;
            const uchar *coveragePixel = coverage.constScanLine(sy) + sx * 4;
            const double sourceCoverage = coveragePixel[3] / 255.0;
            const double styleAlpha = sourceCoverage > 0.0
                ? std::clamp((sourcePixel[3] / 255.0) / sourceCoverage,
                             0.0, 1.0)
                : 0.0;
            uchar *target = output.scanLine(oy) + ox * 4;
            target[0] = sourcePixel[0];
            target[1] = sourcePixel[1];
            target[2] = sourcePixel[2];
            target[3] = static_cast<uchar>(std::lround(styleAlpha * 255.0));
        }
    }
    *carrier = std::move(output);
    return true;
}

QImage featherSemanticCoverage(const LayerNode &layer,
                               const quint64 layerFingerprint,
                               const QSize &previewSize,
                               const QRect &previewRegion,
                               const QSize &documentSize,
                               const QTransform &worldTransform,
                               const QImage::Format format,
                               const QColorSpace &colourSpace,
                               const bool forceOpaquePixelAlpha,
                               const bool grayscaleDocument,
                               const std::atomic_bool *cancelRequested)
{
    try {
        const double scaleX = previewSize.width()
            / static_cast<double>(std::max(1, documentSize.width()));
        const double scaleY = previewSize.height()
            / static_cast<double>(std::max(1, documentSize.height()));
        const double supportX = layer.vectorData.featherRadius * std::abs(scaleX);
        const double supportY = layer.vectorData.featherRadius * std::abs(scaleY);
        const double maximumIntegerSupport =
            std::numeric_limits<int>::max() / 4.0 - 2.0;
        if (!std::isfinite(supportX) || !std::isfinite(supportY)
            || supportX > maximumIntegerSupport
            || supportY > maximumIntegerSupport) {
            return {};
        }
        const FractionalFeatherKernel kernelX(supportX);
        const FractionalFeatherKernel kernelY(supportY);
        const int radiusX = std::min(kernelX.support + 2,
            std::numeric_limits<int>::max() / 4);
        const int radiusY = std::min(kernelY.support + 2,
            std::numeric_limits<int>::max() / 4);
        const QRect supportRect = expandedRect(previewRegion, radiusX, radiusY);

        const QTransform documentToPreview = QTransform::fromScale(scaleX, scaleY);
        const QRectF semanticDocumentBounds = semanticContentBounds(
            layer, worldTransform, layerFingerprint);
        const QRectF clippedBounds = documentToPreview.mapRect(semanticDocumentBounds)
            .adjusted(-2.0, -2.0, 2.0, 2.0)
            .intersected(QRectF(supportRect));
        const QRect sourceRect = clippedBounds.isEmpty()
            ? QRect() : clippedBounds.toAlignedRect().intersected(supportRect);
        if (sourceRect.isEmpty()) {
            return transparentImage(previewRegion.size(), format, colourSpace);
        }

        const qint64 sourcePixels = static_cast<qint64>(sourceRect.width())
            * sourceRect.height();
        const qint64 outputPixels = static_cast<qint64>(previewRegion.width())
            * previewRegion.height();
        const int sourceWidth = sourceRect.width();
        const int sourceHeight = sourceRect.height();
        const int outputWidth = previewRegion.width();
        const int outputHeight = previewRegion.height();
        const qint64 horizontalCount = static_cast<qint64>(sourceHeight) * outputWidth;
        if (sourcePixels <= 0 || outputPixels <= 0 || horizontalCount <= 0
            || sourcePixels > std::numeric_limits<qsizetype>::max()
            || outputPixels > std::numeric_limits<qsizetype>::max()
            || horizontalCount > std::numeric_limits<qsizetype>::max()) {
            return {};
        }

        QImage formatProbe(1, 1, format);
        if (formatProbe.isNull()) return {};
        const bool sixteenBit = formatProbe.depth() > 32;
        const QImage::Format straightFormat = sixteenBit
            ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
        const qint64 pixelBytes = sixteenBit ? 8 : 4;
        const long double estimatedWorkingBytes =
            static_cast<long double>(sourcePixels) * pixelBytes * 2.0L
            + static_cast<long double>(horizontalCount)
                * (sizeof(double) + sizeof(int))
            + static_cast<long double>(outputPixels)
                * (sizeof(double) + pixelBytes * 2)
            + static_cast<long double>(sourceWidth + sourceHeight) * 40.0L;
        if (estimatedWorkingBytes > MaximumFeatherWorkingBytes) {
            // Full-document callers are allowed, but the CPU reference remains
            // bounded by recursively evaluating exact independent subregions.
            // Ordinary canvas/export composition already requests tiles, so this
            // path is mainly for clipboard and direct regression renders.
            if (outputWidth <= 64 && outputHeight <= 64) return {};
            QVector<QRect> pieces;
            if (outputWidth >= outputHeight && outputWidth > 1) {
                const int leftWidth = outputWidth / 2;
                pieces = {
                    QRect(previewRegion.x(), previewRegion.y(), leftWidth, outputHeight),
                    QRect(previewRegion.x() + leftWidth, previewRegion.y(),
                          outputWidth - leftWidth, outputHeight)
                };
            } else if (outputHeight > 1) {
                const int topHeight = outputHeight / 2;
                pieces = {
                    QRect(previewRegion.x(), previewRegion.y(), outputWidth, topHeight),
                    QRect(previewRegion.x(), previewRegion.y() + topHeight,
                          outputWidth, outputHeight - topHeight)
                };
            } else {
                return {};
            }
            QImage combined = transparentImage(previewRegion.size(), format, colourSpace);
            if (combined.isNull()) return {};
            const int outputBytesPerPixel = std::max(1, formatProbe.depth() / 8);
            for (const QRect &piece : pieces) {
                if (cancelRequested
                    && cancelRequested->load(std::memory_order_acquire)) {
                    return {};
                }
                const QImage rendered = featherSemanticCoverage(
                    layer, layerFingerprint, previewSize, piece, documentSize,
                    worldTransform, format, colourSpace, forceOpaquePixelAlpha,
                    grayscaleDocument, cancelRequested);
                if (rendered.isNull() || rendered.format() != combined.format()) return {};
                const QPoint offset = piece.topLeft() - previewRegion.topLeft();
                const size_t rowBytes = static_cast<size_t>(rendered.width())
                    * outputBytesPerPixel;
                for (int row = 0; row < rendered.height(); ++row) {
                    std::memcpy(combined.scanLine(offset.y() + row)
                                    + offset.x() * outputBytesPerPixel,
                                rendered.constScanLine(row), rowBytes);
                }
            }
            return combined;
        }

        const QImage semantic = renderSemanticRegion(
            layer, layerFingerprint, previewSize, sourceRect, documentSize,
            worldTransform, straightFormat, colourSpace, forceOpaquePixelAlpha,
            grayscaleDocument, false, cancelRequested);
        if (semantic.isNull()) return {};
        const QImage silhouette = renderSemanticRegion(
            layer, layerFingerprint, previewSize, sourceRect, documentSize,
            worldTransform, straightFormat, colourSpace, forceOpaquePixelAlpha,
            grayscaleDocument, true, cancelRequested);
        if (silhouette.isNull()) return {};
        const QImage straight = semantic.convertToFormat(straightFormat);
        const QImage coverageStraight = silhouette.convertToFormat(straightFormat);
        if (straight.isNull() || coverageStraight.isNull()) return {};

        QVector<double> horizontal(static_cast<qsizetype>(horizontalCount));
        QVector<int> nearestX(static_cast<qsizetype>(horizontalCount), -1);
        QVector<double> prefix0(sourceWidth + 1);
        QVector<double> prefix1(sourceWidth + 1);
        QVector<double> prefix2(sourceWidth + 1);
        QVector<int> visibleXs;
        visibleXs.reserve(sourceWidth);

        bool anyVisible = false;
        for (int sy = 0; sy < sourceHeight; ++sy) {
            if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) return {};
            prefix0[0] = prefix1[0] = prefix2[0] = 0.0;
            visibleXs.clear();
            for (int sx = 0; sx < sourceWidth; ++sx) {
                const double alpha = sourcePixelAlpha(coverageStraight, sx, sy);
                const double localX = sx;
                prefix0[sx + 1] = prefix0[sx] + alpha;
                prefix1[sx + 1] = prefix1[sx] + alpha * localX;
                prefix2[sx + 1] = prefix2[sx] + alpha * localX * localX;
                if (sourcePixelAlpha(straight, sx, sy) > 0.0) {
                    visibleXs.push_back(sx);
                }
            }
            anyVisible = anyVisible || !visibleXs.isEmpty();
            qsizetype rightIndex = 0;
            for (int ox = 0; ox < outputWidth; ++ox) {
                const int globalX = previewRegion.x() + ox;
                horizontal[static_cast<qsizetype>(sy) * outputWidth + ox] =
                    kernelX.convolveAt(globalX - sourceRect.x(), 0, sourceWidth,
                                       prefix0, prefix1, prefix2);
                if (visibleXs.isEmpty()) continue;
                while (rightIndex < visibleXs.size()
                       && sourceRect.x() + visibleXs[rightIndex] < globalX) {
                    ++rightIndex;
                }
                int candidate = -1;
                if (rightIndex < visibleXs.size()) candidate = visibleXs[rightIndex];
                if (rightIndex > 0) {
                    const int leftCandidate = visibleXs[rightIndex - 1];
                    if (candidate < 0
                        || std::abs(sourceRect.x() + leftCandidate - globalX)
                            <= std::abs(sourceRect.x() + candidate - globalX)) {
                        candidate = leftCandidate;
                    }
                }
                if (candidate >= 0
                    && std::abs(sourceRect.x() + candidate - globalX)
                        <= kernelX.support) {
                    nearestX[static_cast<qsizetype>(sy) * outputWidth + ox] = candidate;
                }
            }
        }
        if (!anyVisible) {
            return transparentImage(previewRegion.size(), format, colourSpace);
        }

        QVector<double> outputAlpha(static_cast<qsizetype>(outputPixels));
        QVector<double> vertical0(sourceHeight + 1);
        QVector<double> vertical1(sourceHeight + 1);
        QVector<double> vertical2(sourceHeight + 1);
        for (int ox = 0; ox < outputWidth; ++ox) {
            vertical0[0] = vertical1[0] = vertical2[0] = 0.0;
            for (int sy = 0; sy < sourceHeight; ++sy) {
                const double value = horizontal[static_cast<qsizetype>(sy) * outputWidth + ox];
                const double localY = sy;
                vertical0[sy + 1] = vertical0[sy] + value;
                vertical1[sy + 1] = vertical1[sy] + value * localY;
                vertical2[sy + 1] = vertical2[sy] + value * localY * localY;
            }
            for (int oy = 0; oy < outputHeight; ++oy) {
                const int globalY = previewRegion.y() + oy;
                outputAlpha[static_cast<qsizetype>(oy) * outputWidth + ox] =
                    kernelY.convolveAt(globalY - sourceRect.y(), 0, sourceHeight,
                                       vertical0, vertical1, vertical2);
            }
        }

        QImage output(previewRegion.size(), straightFormat);
        if (output.isNull()) return {};
        output.fill(Qt::transparent);
        output.setColorSpace(colourSpace);

        QVector<int> envelopeRows(sourceHeight);
        QVector<double> envelopeBoundaries(sourceHeight + 1);
        for (int ox = 0; ox < outputWidth; ++ox) {
            if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) return {};
            int envelopeSize = 0;
            const int globalX = previewRegion.x() + ox;
            for (int sy = 0; sy < sourceHeight; ++sy) {
                const int sx = nearestX[static_cast<qsizetype>(sy) * outputWidth + ox];
                if (sx < 0) continue;
                const double dx = sourceRect.x() + sx - globalX;
                const double weight = dx * dx;
                if (envelopeSize == 0) {
                    envelopeRows[0] = sy;
                    envelopeBoundaries[0] = -std::numeric_limits<double>::infinity();
                    envelopeBoundaries[1] = std::numeric_limits<double>::infinity();
                    envelopeSize = 1;
                    continue;
                }
                double boundary = 0.0;
                while (envelopeSize > 0) {
                    const int previous = envelopeRows[envelopeSize - 1];
                    const int previousX = nearestX[
                        static_cast<qsizetype>(previous) * outputWidth + ox];
                    const double previousDx = sourceRect.x() + previousX - globalX;
                    const double previousWeight = previousDx * previousDx;
                    boundary = ((weight + double(sy) * sy)
                                - (previousWeight + double(previous) * previous))
                        / (2.0 * (sy - previous));
                    if (envelopeSize == 1
                        || boundary > envelopeBoundaries[envelopeSize - 1]) {
                        break;
                    }
                    --envelopeSize;
                }
                if (envelopeSize == 0) {
                    envelopeRows[0] = sy;
                    envelopeBoundaries[0] = -std::numeric_limits<double>::infinity();
                    envelopeBoundaries[1] = std::numeric_limits<double>::infinity();
                    envelopeSize = 1;
                } else {
                    envelopeRows[envelopeSize] = sy;
                    envelopeBoundaries[envelopeSize] = boundary;
                    ++envelopeSize;
                    envelopeBoundaries[envelopeSize] =
                        std::numeric_limits<double>::infinity();
                }
            }
            if (envelopeSize == 0) continue;

            int selected = 0;
            for (int oy = 0; oy < outputHeight; ++oy) {
                const double queryY = previewRegion.y() + oy - sourceRect.y();
                while (selected + 1 < envelopeSize
                       && queryY > envelopeBoundaries[selected + 1]) {
                    ++selected;
                }
                int sy = envelopeRows[selected];
                int sx = nearestX[static_cast<qsizetype>(sy) * outputWidth + ox];
                const int queryRow = previewRegion.y() + oy - sourceRect.y();
                if (std::abs(sy - queryRow) > kernelY.support) {
                    double bestDistance = std::numeric_limits<double>::infinity();
                    int bestRow = -1;
                    int bestX = -1;
                    const int firstRow = std::max(0, queryRow - kernelY.support);
                    const int lastRow = std::min(sourceHeight - 1,
                                                 queryRow + kernelY.support);
                    for (int candidateRow = firstRow; candidateRow <= lastRow;
                         ++candidateRow) {
                        const int candidateX = nearestX[
                            static_cast<qsizetype>(candidateRow) * outputWidth + ox];
                        if (candidateX < 0) continue;
                        const double dx = sourceRect.x() + candidateX - globalX;
                        const double dy = candidateRow - queryRow;
                        const double distance = dx * dx + dy * dy;
                        if (distance < bestDistance
                            || (qFuzzyCompare(distance + 1.0, bestDistance + 1.0)
                                && (candidateRow < bestRow
                                    || (candidateRow == bestRow
                                        && candidateX < bestX)))) {
                            bestDistance = distance;
                            bestRow = candidateRow;
                            bestX = candidateX;
                        }
                    }
                    sy = bestRow;
                    sx = bestX;
                }
                if (sy < 0 || sx < 0) continue;

                const qsizetype outputIndex =
                    static_cast<qsizetype>(oy) * outputWidth + ox;
                if (sixteenBit) {
                    const QRgba64 sourcePixel = reinterpret_cast<const QRgba64 *>(
                        straight.constScanLine(sy))[sx];
                    const QRgba64 coveragePixel = reinterpret_cast<const QRgba64 *>(
                        coverageStraight.constScanLine(sy))[sx];
                    const double sourceCoverage = coveragePixel.alpha() / 65535.0;
                    const double styleAlpha = sourceCoverage > 0.0
                        ? std::clamp((sourcePixel.alpha() / 65535.0) / sourceCoverage,
                                     0.0, 1.0)
                        : 0.0;
                    const quint16 alpha = static_cast<quint16>(std::lround(
                        std::clamp(outputAlpha[outputIndex] * styleAlpha, 0.0, 1.0)
                        * 65535.0));
                    reinterpret_cast<QRgba64 *>(output.scanLine(oy))[ox] =
                        QRgba64::fromRgba64(sourcePixel.red(), sourcePixel.green(),
                                           sourcePixel.blue(), alpha);
                } else {
                    const uchar *sourcePixel = straight.constScanLine(sy) + sx * 4;
                    const uchar *coveragePixel =
                        coverageStraight.constScanLine(sy) + sx * 4;
                    const double sourceCoverage = coveragePixel[3] / 255.0;
                    const double styleAlpha = sourceCoverage > 0.0
                        ? std::clamp((sourcePixel[3] / 255.0) / sourceCoverage,
                                     0.0, 1.0)
                        : 0.0;
                    uchar *target = output.scanLine(oy) + ox * 4;
                    target[0] = sourcePixel[0];
                    target[1] = sourcePixel[1];
                    target[2] = sourcePixel[2];
                    target[3] = static_cast<uchar>(std::lround(
                        std::clamp(outputAlpha[outputIndex] * styleAlpha, 0.0, 1.0)
                        * 255.0));
                }
            }
        }
        QImage converted = output.convertToFormat(format);
        converted.setColorSpace(colourSpace);
        return cancelRequested && cancelRequested->load(std::memory_order_acquire)
            ? QImage() : converted;
    } catch (const std::bad_alloc &) {
        return {};
    }
}

} // namespace

QImage VectorRasterizer::renderLayerRegion(
    const LayerNode &layer,
    const QSize &previewSize,
    const QRect &previewRegion,
    const QSize &documentSize,
    const QTransform &worldTransform,
    const QImage::Format format,
    const QColorSpace &colourSpace,
    const bool forceOpaquePixelAlpha,
    const bool grayscaleDocument,
    const std::atomic_bool *cancelRequested)
{
    if (layer.type != LayerType::Vector
        || !layer.vectorData.isSafe()
        || previewSize.isEmpty()
        || previewRegion.isEmpty()
        || documentSize.isEmpty()
        || !worldTransform.isInvertible()) {
        return {};
    }
    if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
        return {};
    }

    const quint64 layerFingerprint = layer.vectorData.fingerprint();
    const QByteArray key = cacheKey(layer,
                                    layerFingerprint,
                                    previewSize,
                                    previewRegion,
                                    documentSize,
                                    worldTransform,
                                    format,
                                    colourSpace,
                                    forceOpaquePixelAlpha,
                                    grayscaleDocument);
    QImage cached;
    if (findCached(key, &cached)) {
        return cached;
    }

    // Preserve the accepted raster equation byte-for-byte at exactly 0 px.
    // Non-zero Feather uses a separate CPU coverage path so editable fill and
    // stroke colours are never blurred together.
    QImage result = layer.vectorData.featherRadius <= 0.0
        ? renderSemanticRegion(layer, layerFingerprint, previewSize, previewRegion,
                               documentSize, worldTransform, format, colourSpace,
                               forceOpaquePixelAlpha, grayscaleDocument, false,
                               cancelRequested)
        : featherSemanticCoverage(layer, layerFingerprint, previewSize, previewRegion,
                                  documentSize, worldTransform, format, colourSpace,
                                  forceOpaquePixelAlpha, grayscaleDocument,
                                  cancelRequested);
    if (result.isNull()) return {};
    if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
        return {};
    }
    insertCached(key, result);
    return result;
}

bool VectorRasterizer::prepareGpuFeatherTile(
    const LayerNode &layer,
    const QSize &previewSize,
    const QRect &previewRegion,
    const QSize &documentSize,
    const QTransform &worldTransform,
    const QColorSpace &colourSpace,
    const bool forceOpaquePixelAlpha,
    const bool grayscaleDocument,
    VectorFeatherGpuTileData *prepared,
    QString *errorMessage,
    const std::atomic_bool *cancelRequested)
{
    if (errorMessage) errorMessage->clear();
    if (prepared) *prepared = {};
    if (!prepared || layer.type != LayerType::Vector
        || !layer.vectorData.isSafe()
        || layer.vectorData.featherRadius <= 0.0
        || previewSize.isEmpty() || previewRegion.isEmpty()
        || documentSize.isEmpty() || !worldTransform.isInvertible()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The vector Feather GPU preparation request is invalid");
        }
        return false;
    }
    if (cancelRequested
        && cancelRequested->load(std::memory_order_acquire)) {
        if (errorMessage) *errorMessage = QStringLiteral("Vector Feather preparation cancelled");
        return false;
    }

    try {
        const double scaleX = previewSize.width()
            / static_cast<double>(std::max(1, documentSize.width()));
        const double scaleY = previewSize.height()
            / static_cast<double>(std::max(1, documentSize.height()));
        const double supportX = layer.vectorData.featherRadius * std::abs(scaleX);
        const double supportY = layer.vectorData.featherRadius * std::abs(scaleY);
        const double maximumIntegerSupport =
            std::numeric_limits<int>::max() / 4.0 - 2.0;
        if (!std::isfinite(supportX) || !std::isfinite(supportY)
            || supportX <= 0.0 || supportY <= 0.0
            || supportX > maximumIntegerSupport
            || supportY > maximumIntegerSupport) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "The vector Feather radius is outside the safe tiled range");
            }
            return false;
        }
        const FractionalFeatherKernel kernelX(supportX);
        const FractionalFeatherKernel kernelY(supportY);
        const int radiusX = std::min(kernelX.support + 2,
            std::numeric_limits<int>::max() / 4);
        const int radiusY = std::min(kernelY.support + 2,
            std::numeric_limits<int>::max() / 4);
        const QRect supportRect = expandedRect(previewRegion, radiusX, radiusY);

        const quint64 layerFingerprint = layer.vectorData.fingerprint();
        const QTransform documentToPreview = QTransform::fromScale(scaleX, scaleY);
        const QRectF semanticDocumentBounds = semanticContentBounds(
            layer, worldTransform, layerFingerprint);
        const QRectF clippedBounds = documentToPreview.mapRect(semanticDocumentBounds)
            .adjusted(-2.0, -2.0, 2.0, 2.0)
            .intersected(QRectF(supportRect));
        const QRect sourceRect = clippedBounds.isEmpty()
            ? QRect() : clippedBounds.toAlignedRect().intersected(supportRect);
        if (sourceRect.isEmpty()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "The vector layer does not contribute to this Feather tile");
            }
            return false;
        }

        constexpr qint64 MaximumGpuPreparationBytes = qint64(256) * 1024 * 1024;
        const qint64 sourcePixels = static_cast<qint64>(sourceRect.width())
            * sourceRect.height();
        const qint64 outputPixels = static_cast<qint64>(previewRegion.width())
            * previewRegion.height();
        // Account conservatively for the two semantic source images, the
        // temporary straight-format carrier inputs that may detach during
        // conversion, the prepared coverage image, nearest-X scratch and the
        // output colour carrier. Keep the declared 256 MiB guard honest even
        // for unusual direct callers with output regions much larger than a
        // normal 256x256 compositor tile.
        const long double estimatedBytes =
            static_cast<long double>(sourcePixels) * 20.0L
            + static_cast<long double>(sourceRect.height())
                * previewRegion.width() * sizeof(int)
            + static_cast<long double>(outputPixels) * 8.0L
            + (static_cast<long double>(sourceRect.width())
               + static_cast<long double>(sourceRect.height())) * 16.0L;
        if (sourcePixels <= 0 || outputPixels <= 0
            || estimatedBytes > MaximumGpuPreparationBytes) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "The vector Feather GPU preparation working set is too large");
            }
            return false;
        }

        const QImage semantic = renderSemanticRegion(
            layer, layerFingerprint, previewSize, sourceRect, documentSize,
            worldTransform, QImage::Format_RGBA8888, colourSpace,
            forceOpaquePixelAlpha, grayscaleDocument, false, cancelRequested);
        const QImage coverage = renderSemanticRegion(
            layer, layerFingerprint, previewSize, sourceRect, documentSize,
            worldTransform, QImage::Format_RGBA8888, colourSpace,
            forceOpaquePixelAlpha, grayscaleDocument, true, cancelRequested);
        if (semantic.isNull() || coverage.isNull()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "The semantic vector Feather inputs could not be rasterised");
            }
            return false;
        }

        QImage carrier;
        if (!buildExactColourCarrier(semantic, coverage, sourceRect, previewRegion,
                                     kernelX.support, kernelY.support, &carrier,
                                     cancelRequested)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "The exact vector Feather colour carrier could not be prepared");
            }
            return false;
        }
        prepared->coverage = coverage.convertToFormat(QImage::Format_RGBA8888);
        prepared->coverage.setColorSpace(colourSpace);
        prepared->colourCarrier = std::move(carrier);
        prepared->colourCarrier.setColorSpace(colourSpace);
        prepared->sourceRect = sourceRect;
        prepared->outputRect = previewRegion;
        prepared->radiusX = supportX;
        prepared->radiusY = supportY;
        if (!prepared->isValid()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "The prepared vector Feather GPU tile is malformed");
            }
            *prepared = {};
            return false;
        }
        return true;
    } catch (const std::bad_alloc &) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Vector Feather GPU preparation exceeded available memory");
        }
        *prepared = {};
        return false;
    }
}

QRectF VectorRasterizer::contentBounds(const LayerNode &layer,
                                       const QTransform &worldTransform)
{
    if (layer.type != LayerType::Vector || !layer.vectorData.isSafe()) return {};
    const quint64 layerFingerprint = layer.vectorData.fingerprint();
    QRectF result = semanticContentBounds(layer, worldTransform, layerFingerprint);
    if (!result.isEmpty() && layer.vectorData.featherRadius > 0.0) {
        const double radius = layer.vectorData.featherRadius;
        result = result.adjusted(-radius, -radius, radius, radius);
    }
    return result;
}

void VectorRasterizer::clearCache()
{
    QMutexLocker<QMutex> locker(&cacheMutex());
    cacheEntries().clear();
    geometryCacheEntries().clear();
    geometryResidentElements() = 0;
    residentBytes() = 0;
    useSerial() = 0;
}

qsizetype VectorRasterizer::cacheBytes()
{
    QMutexLocker<QMutex> locker(&cacheMutex());
    return residentBytes();
}

qsizetype VectorRasterizer::cacheEntryCount()
{
    QMutexLocker<QMutex> locker(&cacheMutex());
    return cacheEntries().size();
}

} // namespace vfx
