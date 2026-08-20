#include "CloneStamp.h"

#include <QColorSpace>
#include <QHash>
#include <QRgba64>
#include <QSizeF>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace vfx {
namespace {

struct SampledPixel {
    std::array<double, 4> rgba {0.0, 0.0, 0.0, 0.0};
    bool valid = false;
};

SampledPixel sourcePixelAt(const QImage &source,
                           const QPointF &position,
                           const bool sourceIsGrey,
                           const bool sourceIsSixteenBit)
{
    SampledPixel sampled;
    if (source.isNull() || source.width() <= 0 || source.height() <= 0
        || position.x() < 0.0 || position.y() < 0.0
        || position.x() > source.width() - 1.0
        || position.y() > source.height() - 1.0) {
        return sampled;
    }

    const int x0 = std::clamp(static_cast<int>(std::floor(position.x())),
                              0, source.width() - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(position.y())),
                              0, source.height() - 1);
    const int x1 = std::min(x0 + 1, source.width() - 1);
    const int y1 = std::min(y0 + 1, source.height() - 1);
    const double tx = std::clamp(position.x() - x0, 0.0, 1.0);
    const double ty = std::clamp(position.y() - y0, 0.0, 1.0);

    const auto sampleOne = [&](const int x, const int y) {
        std::array<double, 4> value {};
        if (sourceIsGrey) {
            if (source.format() == QImage::Format_Grayscale16) {
                const auto *row = reinterpret_cast<const quint16 *>(source.constScanLine(y));
                const double grey = row[x] / 65535.0;
                value = {grey, grey, grey, 1.0};
            } else {
                const uchar *row = source.constScanLine(y);
                const double grey = row[x] / 255.0;
                value = {grey, grey, grey, 1.0};
            }
        } else if (sourceIsSixteenBit) {
            const auto *row = reinterpret_cast<const QRgba64 *>(source.constScanLine(y));
            const QRgba64 pixel = row[x];
            value = {pixel.red() / 65535.0,
                     pixel.green() / 65535.0,
                     pixel.blue() / 65535.0,
                     pixel.alpha() / 65535.0};
        } else {
            const uchar *row = source.constScanLine(y) + x * 4;
            value = {row[0] / 255.0,
                     row[1] / 255.0,
                     row[2] / 255.0,
                     row[3] / 255.0};
        }
        return value;
    };

    const std::array<double, 4> p00 = sampleOne(x0, y0);
    const std::array<double, 4> p10 = sampleOne(x1, y0);
    const std::array<double, 4> p01 = sampleOne(x0, y1);
    const std::array<double, 4> p11 = sampleOne(x1, y1);
    const double weights[4] {
        (1.0 - tx) * (1.0 - ty),
        tx * (1.0 - ty),
        (1.0 - tx) * ty,
        tx * ty
    };
    const std::array<double, 4> pixels[4] {p00, p10, p01, p11};

    if (sourceIsGrey) {
        for (int component = 0; component < 4; ++component) {
            for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex) {
                sampled.rgba[component] += pixels[sampleIndex][component]
                    * weights[sampleIndex];
            }
        }
    } else {
        // Filter visible colour in associated-alpha space, then return straight RGBA.
        // This prevents transparent neighbouring texels from darkening a sampled edge,
        // while the all-transparent case still interpolates hidden RGB explicitly.
        double alpha = 0.0;
        for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex) {
            alpha += pixels[sampleIndex][3] * weights[sampleIndex];
        }
        sampled.rgba[3] = std::clamp(alpha, 0.0, 1.0);
        if (alpha > 1.0e-12) {
            for (int component = 0; component < 3; ++component) {
                double associated = 0.0;
                for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex) {
                    associated += pixels[sampleIndex][component]
                        * pixels[sampleIndex][3] * weights[sampleIndex];
                }
                sampled.rgba[component] = std::clamp(associated / alpha, 0.0, 1.0);
            }
        } else {
            for (int component = 0; component < 3; ++component) {
                for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex) {
                    sampled.rgba[component] += pixels[sampleIndex][component]
                        * weights[sampleIndex];
                }
            }
        }
    }
    sampled.valid = true;
    return sampled;
}

void blendStraightRgba(double channels[4],
                       const SampledPixel &sampled,
                       const double coverage)
{
    const double destinationAlpha = std::clamp(channels[3], 0.0, 1.0);
    const double sourceAlpha = std::clamp(sampled.rgba[3], 0.0, 1.0);
    const double inverseCoverage = 1.0 - coverage;
    const double outputAlpha = sourceAlpha * coverage
        + destinationAlpha * inverseCoverage;

    if (outputAlpha > 1.0e-12) {
        for (int component = 0; component < 3; ++component) {
            const double associated = sampled.rgba[component] * sourceAlpha * coverage
                + channels[component] * destinationAlpha * inverseCoverage;
            channels[component] = std::clamp(associated / outputAlpha, 0.0, 1.0);
        }
    } else {
        // No visible colour exists, so retain meaningful straight hidden RGB.
        for (int component = 0; component < 3; ++component) {
            channels[component] = sampled.rgba[component] * coverage
                + channels[component] * inverseCoverage;
        }
    }
    channels[3] = std::clamp(outputAlpha, 0.0, 1.0);
}

double scalarSample(const SampledPixel &sampled,
                    const CloneStampSample mode,
                    const int componentIndex)
{
    switch (mode) {
    case CloneStampSample::Luminance:
        return std::clamp(sampled.rgba[0] * 0.299
                          + sampled.rgba[1] * 0.587
                          + sampled.rgba[2] * 0.114,
                          0.0, 1.0);
    case CloneStampSample::Alpha:
        return std::clamp(sampled.rgba[3], 0.0, 1.0);
    case CloneStampSample::Component:
        return componentIndex >= 0 && componentIndex < 4
            ? std::clamp(sampled.rgba[componentIndex], 0.0, 1.0)
            : 0.0;
    case CloneStampSample::Rgba:
        return std::clamp(sampled.rgba[0], 0.0, 1.0);
    }
    return 0.0;
}

double brushCoverage(const QPointF &pixelCentre,
                     const QPointF &stampCentre,
                     const double radius,
                     const double hardness,
                     const double opacity)
{
    const double dx = pixelCentre.x() - stampCentre.x();
    const double dy = pixelCentre.y() - stampCentre.y();
    const double distance = std::sqrt(dx * dx + dy * dy) / radius;
    if (distance >= 1.0) {
        return 0.0;
    }
    const double safeHardness = std::clamp(hardness, 0.0, 0.9999);
    double smooth = 0.0;
    if (distance > safeHardness) {
        const double t = (distance - safeHardness) / (1.0 - safeHardness);
        smooth = t * t * (3.0 - 2.0 * t);
    }
    return std::clamp((1.0 - smooth) * opacity, 0.0, 1.0);
}

} // namespace

CloneStampResult applyCloneStamp(const CloneStampRequest &request)
{
    CloneStampResult result;
    if (request.destination.isNull()) {
        result.error = QStringLiteral("Clone Stamp destination is empty.");
        return result;
    }
    if (request.source.isNull()) {
        result.error = QStringLiteral("Clone Stamp source is empty.");
        return result;
    }
    if (request.targetSegments.isEmpty()) {
        result.image = request.destination;
        return result;
    }
    if (request.target == CloneStampTarget::ComponentChannel
        && (request.componentIndex < 0 || request.componentIndex > 3)) {
        result.error = QStringLiteral("Clone Stamp component target is invalid.");
        return result;
    }

    const bool targetMask = request.target == CloneStampTarget::Mask;
    const bool targetSixteenBit = !targetMask && request.destination.depth() > 32;
    QImage destination = request.destination.convertToFormat(
        targetMask ? QImage::Format_Grayscale8
                   : (targetSixteenBit ? QImage::Format_RGBA64
                                       : QImage::Format_RGBA8888));
    if (destination.isNull()) {
        result.error = QStringLiteral("Clone Stamp could not prepare the destination image.");
        return result;
    }
    destination.detach();

    const bool sourceIsGrey = request.source.format() == QImage::Format_Grayscale8
        || request.source.format() == QImage::Format_Grayscale16;
    const bool sourceIsSixteenBit = !sourceIsGrey && request.source.depth() > 32;
    QImage source = request.source.convertToFormat(
        sourceIsGrey
            ? (request.source.format() == QImage::Format_Grayscale16
                   ? QImage::Format_Grayscale16
                   : QImage::Format_Grayscale8)
            : (sourceIsSixteenBit ? QImage::Format_RGBA64
                                  : QImage::Format_RGBA8888));
    if (source.isNull()) {
        result.error = QStringLiteral("Clone Stamp could not prepare the source image.");
        return result;
    }

    const double radius = std::max(0.5, request.diameterPixels * 0.5);
    const double spacing = std::max(1.0, radius * 0.22);
    const double opacity = std::clamp(request.opacity, 0.0, 1.0);

    QVector<QPointF> stamps;
    for (const QLineF &segment : request.targetSegments) {
        const double length = segment.length();
        if (length <= 1.0e-6) {
            stamps.push_back(segment.p2());
            continue;
        }
        const int steps = std::max(1, static_cast<int>(std::ceil(length / spacing)));
        stamps.reserve(stamps.size() + steps);
        for (int step = 1; step <= steps; ++step) {
            const double amount = step / static_cast<double>(steps);
            stamps.push_back(segment.p1() + (segment.p2() - segment.p1()) * amount);
        }
    }
    if (stamps.isEmpty()) {
        result.image = destination;
        return result;
    }

    // Accumulate the complete stroke as a single floating-point coverage field.
    // Quantising every low-opacity dab independently lets different RGB channels
    // cross their 8-bit thresholds at different times, creating coloured contour
    // bands around very soft strokes. For a fixed Clone source at each target
    // pixel, sequential source-over dabs are exactly equivalent to one blend with
    // coverage 1 - product(1 - dabCoverage), so only the final pixel is quantised.
    constexpr int coverageTileSize = 256;
    struct CoverageTile {
        QRect rect;
        QVector<float> remaining;
    };
    QVector<CoverageTile> coverageTiles;
    QHash<quint64, int> coverageTileLookup;
    QRect affectedTotal;

    const auto tileKey = [](const int tileX, const int tileY) {
        return (static_cast<quint64>(static_cast<quint32>(tileY)) << 32)
            | static_cast<quint32>(tileX);
    };
    auto ensureCoverageTile = [&](const int tileX, const int tileY) -> CoverageTile & {
        const quint64 key = tileKey(tileX, tileY);
        const auto found = coverageTileLookup.constFind(key);
        if (found != coverageTileLookup.cend()) {
            return coverageTiles[found.value()];
        }
        const QRect tileRect(tileX * coverageTileSize,
                             tileY * coverageTileSize,
                             std::min(coverageTileSize,
                                      destination.width() - tileX * coverageTileSize),
                             std::min(coverageTileSize,
                                      destination.height() - tileY * coverageTileSize));
        CoverageTile tile;
        tile.rect = tileRect;
        tile.remaining.fill(1.0f, tileRect.width() * tileRect.height());
        const int index = coverageTiles.size();
        coverageTiles.push_back(std::move(tile));
        coverageTileLookup.insert(key, index);
        return coverageTiles[index];
    };

    for (const QPointF &stamp : stamps) {
        const QRect affected = QRectF(stamp - QPointF(radius, radius),
                                      QSizeF(radius * 2.0, radius * 2.0))
                                   .adjusted(-1.0, -1.0, 1.0, 1.0)
                                   .toAlignedRect()
                                   .intersected(destination.rect());
        if (affected.isEmpty()) {
            continue;
        }
        affectedTotal = affectedTotal.isEmpty()
            ? affected : affectedTotal.united(affected);

        const int firstTileX = affected.left() / coverageTileSize;
        const int lastTileX = affected.right() / coverageTileSize;
        const int firstTileY = affected.top() / coverageTileSize;
        const int lastTileY = affected.bottom() / coverageTileSize;
        for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
            for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
                CoverageTile &tile = ensureCoverageTile(tileX, tileY);
                const QRect localAffected = affected.intersected(tile.rect);
                for (int y = localAffected.top(); y <= localAffected.bottom(); ++y) {
                    const int localY = y - tile.rect.y();
                    for (int x = localAffected.left(); x <= localAffected.right(); ++x) {
                        const double amount = brushCoverage(QPointF(x + 0.5, y + 0.5),
                                                            stamp,
                                                            radius,
                                                            request.hardness,
                                                            opacity);
                        if (amount <= 0.0) {
                            continue;
                        }
                        const int index = localY * tile.rect.width()
                            + (x - tile.rect.x());
                        tile.remaining[index] *= static_cast<float>(1.0 - amount);
                    }
                }
            }
        }
    }

    for (const CoverageTile &tile : coverageTiles) {
        for (int localY = 0; localY < tile.rect.height(); ++localY) {
            const int y = tile.rect.y() + localY;
            for (int localX = 0; localX < tile.rect.width(); ++localX) {
                const int x = tile.rect.x() + localX;
                const double coverage = std::clamp(
                    1.0 - static_cast<double>(
                              tile.remaining[localY * tile.rect.width() + localX]),
                    0.0, 1.0);
                if (coverage <= 0.0) {
                    continue;
                }

                const QPointF targetPixelCentre(x + 0.5, y + 0.5);
                const QPointF targetLayerPoint = request.targetPixelToLayer.map(
                    targetPixelCentre);
                const QPointF destinationDocumentPoint =
                    request.targetLayerToDocument.map(targetLayerPoint);
                const QPointF sourceDocumentPoint = destinationDocumentPoint
                    + request.sourceOffsetDocument;
                const QPointF sourceLayerPoint = request.sourceDocumentToLayer.map(
                    sourceDocumentPoint);
                const QPointF sourcePixelPoint = request.sourceLayerToPixel.map(
                    sourceLayerPoint) - QPointF(0.5, 0.5);
                const SampledPixel sampled = sourcePixelAt(source,
                                                           sourcePixelPoint,
                                                           sourceIsGrey,
                                                           sourceIsSixteenBit);
                if (!sampled.valid) {
                    continue;
                }

                if (targetMask) {
                    uchar *row = destination.scanLine(y);
                    const double value = scalarSample(sampled,
                                                      request.sample,
                                                      request.componentIndex);
                    row[x] = static_cast<uchar>(std::clamp(
                        qRound((value * coverage
                                + row[x] / 255.0 * (1.0 - coverage)) * 255.0),
                        0, 255));
                    continue;
                }

                if (targetSixteenBit) {
                    auto *row = reinterpret_cast<QRgba64 *>(destination.scanLine(y));
                    const QRgba64 original = row[x];
                    double channels[4] {original.red() / 65535.0,
                                        original.green() / 65535.0,
                                        original.blue() / 65535.0,
                                        original.alpha() / 65535.0};
                    if (request.target == CloneStampTarget::RasterPixels) {
                        blendStraightRgba(channels, sampled, coverage);
                    } else if (request.target == CloneStampTarget::GreyChannel) {
                        const double value = scalarSample(sampled,
                                                          request.sample,
                                                          request.componentIndex);
                        for (int component = 0; component < 3; ++component) {
                            channels[component] = value * coverage
                                + channels[component] * (1.0 - coverage);
                        }
                    } else {
                        const double value = scalarSample(sampled,
                                                          request.sample,
                                                          request.componentIndex);
                        channels[request.componentIndex] = value * coverage
                            + channels[request.componentIndex] * (1.0 - coverage);
                    }
                    row[x] = QRgba64::fromRgba64(
                        static_cast<quint16>(std::clamp(qRound(channels[0] * 65535.0), 0, 65535)),
                        static_cast<quint16>(std::clamp(qRound(channels[1] * 65535.0), 0, 65535)),
                        static_cast<quint16>(std::clamp(qRound(channels[2] * 65535.0), 0, 65535)),
                        static_cast<quint16>(std::clamp(qRound(channels[3] * 65535.0), 0, 65535)));
                } else {
                    uchar *pixel = destination.scanLine(y) + x * 4;
                    if (request.target == CloneStampTarget::RasterPixels) {
                        double channels[4] {pixel[0] / 255.0,
                                            pixel[1] / 255.0,
                                            pixel[2] / 255.0,
                                            pixel[3] / 255.0};
                        blendStraightRgba(channels, sampled, coverage);
                        for (int component = 0; component < 4; ++component) {
                            pixel[component] = static_cast<uchar>(std::clamp(
                                qRound(channels[component] * 255.0), 0, 255));
                        }
                    } else if (request.target == CloneStampTarget::GreyChannel) {
                        const double value = scalarSample(sampled,
                                                          request.sample,
                                                          request.componentIndex);
                        for (int component = 0; component < 3; ++component) {
                            pixel[component] = static_cast<uchar>(std::clamp(
                                qRound((value * coverage
                                        + pixel[component] / 255.0 * (1.0 - coverage)) * 255.0),
                                0, 255));
                        }
                    } else {
                        const double value = scalarSample(sampled,
                                                          request.sample,
                                                          request.componentIndex);
                        uchar &component = pixel[request.componentIndex];
                        component = static_cast<uchar>(std::clamp(
                            qRound((value * coverage
                                    + component / 255.0 * (1.0 - coverage)) * 255.0),
                            0, 255));
                    }
                }
            }
        }
    }

    destination.setColorSpace(request.destination.colorSpace());
    result.image = destination;
    result.affectedRect = affectedTotal;
    return result;
}

} // namespace vfx
