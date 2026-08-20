#include "HealingBrush.h"

#include <QColorSpace>
#include <QHash>
#include <QRgba64>
#include <QSizeF>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace vfx {
namespace {

// The seamless-cloning formulation follows the same underlying method used by
// GIMP's GPLv3 Healing tool (app/paint/gimpheal.c), itself based on Todor
// Georgiev's "Photoshop Healing Brush: a Tool for Seamless Cloning".  This is
// an independent sparse implementation adapted to VFX Photo Lab's immutable
// sampled-retouch transaction, transformed coordinates and straight RGBA data.

struct SampledPixel {
    std::array<double, 4> rgba {0.0, 0.0, 0.0, 0.0};
    bool valid = false;
};

SampledPixel samplePixel(const QImage &source,
                         const QPointF &position,
                         const bool sourceIsSixteenBit)
{
    SampledPixel sampled;
    if (source.isNull() || source.width() <= 0 || source.height() <= 0
        || !std::isfinite(position.x()) || !std::isfinite(position.y())
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

    const auto readOne = [&](const int x, const int y) {
        std::array<double, 4> value {};
        if (sourceIsSixteenBit) {
            const auto *row = reinterpret_cast<const QRgba64 *>(source.constScanLine(y));
            const QRgba64 pixel = row[x];
            value = {pixel.red() / 65535.0,
                     pixel.green() / 65535.0,
                     pixel.blue() / 65535.0,
                     pixel.alpha() / 65535.0};
        } else {
            const uchar *pixel = source.constScanLine(y) + x * 4;
            value = {pixel[0] / 255.0,
                     pixel[1] / 255.0,
                     pixel[2] / 255.0,
                     pixel[3] / 255.0};
        }
        return value;
    };

    const std::array<double, 4> pixels[4] {
        readOne(x0, y0), readOne(x1, y0), readOne(x0, y1), readOne(x1, y1)
    };
    const double weights[4] {
        (1.0 - tx) * (1.0 - ty),
        tx * (1.0 - ty),
        (1.0 - tx) * ty,
        tx * ty
    };

    double alpha = 0.0;
    for (int index = 0; index < 4; ++index) {
        alpha += pixels[index][3] * weights[index];
    }
    sampled.rgba[3] = std::clamp(alpha, 0.0, 1.0);
    if (alpha > 1.0e-12) {
        for (int component = 0; component < 3; ++component) {
            double associated = 0.0;
            for (int index = 0; index < 4; ++index) {
                associated += pixels[index][component]
                    * pixels[index][3] * weights[index];
            }
            sampled.rgba[component] = std::clamp(associated / alpha, 0.0, 1.0);
        }
    } else {
        // Preserve useful straight hidden colour when every contributing texel
        // is transparent rather than manufacturing black around alpha edges.
        for (int component = 0; component < 3; ++component) {
            for (int index = 0; index < 4; ++index) {
                sampled.rgba[component] += pixels[index][component] * weights[index];
            }
        }
    }
    sampled.valid = true;
    return sampled;
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

std::array<double, 4> destinationPixel(const QImage &image,
                                       const int x,
                                       const int y,
                                       const bool sixteenBit)
{
    if (sixteenBit) {
        const auto *row = reinterpret_cast<const QRgba64 *>(image.constScanLine(y));
        const QRgba64 pixel = row[x];
        return {pixel.red() / 65535.0,
                pixel.green() / 65535.0,
                pixel.blue() / 65535.0,
                pixel.alpha() / 65535.0};
    }
    const uchar *pixel = image.constScanLine(y) + x * 4;
    return {pixel[0] / 255.0,
            pixel[1] / 255.0,
            pixel[2] / 255.0,
            pixel[3] / 255.0};
}

void writeDestinationPixel(QImage *image,
                           const int x,
                           const int y,
                           const bool sixteenBit,
                           const std::array<double, 4> &rgba)
{
    if (sixteenBit) {
        auto *row = reinterpret_cast<QRgba64 *>(image->scanLine(y));
        row[x] = QRgba64::fromRgba64(
            static_cast<quint16>(std::clamp(qRound(rgba[0] * 65535.0), 0, 65535)),
            static_cast<quint16>(std::clamp(qRound(rgba[1] * 65535.0), 0, 65535)),
            static_cast<quint16>(std::clamp(qRound(rgba[2] * 65535.0), 0, 65535)),
            static_cast<quint16>(std::clamp(qRound(rgba[3] * 65535.0), 0, 65535)));
        return;
    }
    uchar *pixel = image->scanLine(y) + x * 4;
    for (int component = 0; component < 4; ++component) {
        pixel[component] = static_cast<uchar>(std::clamp(
            qRound(rgba[component] * 255.0), 0, 255));
    }
}

struct CoverageTile {
    QRect rect;
    QVector<float> remaining;
    QVector<int> activeIndices;
};

struct ActivePixel {
    int x = 0;
    int y = 0;
    float coverage = 0.0f;
    std::array<float, 3> source {0.0f, 0.0f, 0.0f};
    std::array<float, 3> correction {0.0f, 0.0f, 0.0f};
    std::array<int, 4> neighbours {
        std::numeric_limits<int>::min(),
        std::numeric_limits<int>::min(),
        std::numeric_limits<int>::min(),
        std::numeric_limits<int>::min()
    };
};

constexpr int OmittedNeighbour = std::numeric_limits<int>::min();

quint64 coordinateKey(const int x, const int y)
{
    return (static_cast<quint64>(static_cast<quint32>(y)) << 32)
        | static_cast<quint32>(x);
}

int encodeBoundary(const int boundaryIndex)
{
    return -boundaryIndex - 1;
}

int decodeBoundary(const int encoded)
{
    return -encoded - 1;
}

} // namespace

HealingBrushResult applyHealingBrush(const HealingBrushRequest &request)
{
    HealingBrushResult result;
    const auto cancellationRequested = [&request] {
        return request.cancelRequested
            && request.cancelRequested->load(std::memory_order_acquire);
    };
    const auto cancelledResult = [&result]() -> HealingBrushResult {
        result.cancelled = true;
        result.image = {};
        result.affectedRect = {};
        result.error.clear();
        return result;
    };
    if (cancellationRequested()) {
        return cancelledResult();
    }
    if (request.destination.isNull()) {
        result.error = QStringLiteral("Healing Brush destination is empty.");
        return result;
    }
    if (request.source.isNull()) {
        result.error = QStringLiteral("Healing Brush source is empty.");
        return result;
    }
    if (request.targetSegments.isEmpty()) {
        result.image = request.destination;
        return result;
    }

    const bool destinationSixteenBit = request.destination.depth() > 32;
    const bool sourceSixteenBit = request.source.depth() > 32;
    QImage destination = request.destination.convertToFormat(
        destinationSixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    QImage source = request.source.convertToFormat(
        sourceSixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (destination.isNull() || source.isNull()) {
        result.error = QStringLiteral("Healing Brush could not prepare its images.");
        return result;
    }
    destination.detach();
    const QImage immutableDestination = destination;

    const double radius = std::max(0.5, request.diameterPixels * 0.5);
    const double spacing = std::max(1.0, radius * 0.22);
    const double opacity = std::clamp(request.opacity, 0.0, 1.0);

    QVector<QPointF> stamps;
    int segmentIndex = 0;
    for (const QLineF &segment : request.targetSegments) {
        if ((segmentIndex++ & 31) == 0 && cancellationRequested()) {
            return cancelledResult();
        }
        const double length = segment.length();
        if (length <= 1.0e-6) {
            stamps.push_back(segment.p2());
            continue;
        }
        const int steps = std::max(1, static_cast<int>(std::ceil(length / spacing)));
        stamps.reserve(stamps.size() + steps);
        for (int step = 1; step <= steps; ++step) {
            if ((step & 255) == 0 && cancellationRequested()) {
                return cancelledResult();
            }
            const double amount = step / static_cast<double>(steps);
            stamps.push_back(segment.p1() + (segment.p2() - segment.p1()) * amount);
        }
    }
    if (stamps.isEmpty() || opacity <= 0.0) {
        result.image = destination;
        return result;
    }

    constexpr int coverageTileSize = 256;
    QVector<CoverageTile> coverageTiles;
    QHash<quint64, int> coverageTileLookup;

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
        tile.activeIndices.fill(-1, tileRect.width() * tileRect.height());
        const int index = coverageTiles.size();
        coverageTiles.push_back(std::move(tile));
        coverageTileLookup.insert(key, index);
        return coverageTiles[index];
    };

    int stampIndex = 0;
    for (const QPointF &stamp : stamps) {
        if ((stampIndex++ & 15) == 0 && cancellationRequested()) {
            return cancelledResult();
        }
        const QRect affected = QRectF(stamp - QPointF(radius, radius),
                                      QSizeF(radius * 2.0, radius * 2.0))
                                   .adjusted(-1.0, -1.0, 1.0, 1.0)
                                   .toAlignedRect()
                                   .intersected(destination.rect());
        if (affected.isEmpty()) {
            continue;
        }
        const int firstTileX = affected.left() / coverageTileSize;
        const int lastTileX = affected.right() / coverageTileSize;
        const int firstTileY = affected.top() / coverageTileSize;
        const int lastTileY = affected.bottom() / coverageTileSize;
        for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
            for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
                CoverageTile &tile = ensureCoverageTile(tileX, tileY);
                const QRect localAffected = affected.intersected(tile.rect);
                for (int y = localAffected.top(); y <= localAffected.bottom(); ++y) {
                    if ((y & 31) == 0 && cancellationRequested()) {
                        return cancelledResult();
                    }
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

    const auto sourceAtDestinationPixel = [&](const int x, const int y) {
        const QPointF targetPixelCentre(x + 0.5, y + 0.5);
        const QPointF targetLayerPoint = request.targetPixelToLayer.map(
            targetPixelCentre);
        const QPointF destinationDocumentPoint = request.targetLayerToDocument.map(
            targetLayerPoint);
        const QPointF sourceDocumentPoint = destinationDocumentPoint
            + request.sourceOffsetDocument;
        const QPointF sourceLayerPoint = request.sourceDocumentToLayer.map(
            sourceDocumentPoint);
        const QPointF sourcePixelPoint = request.sourceLayerToPixel.map(
            sourceLayerPoint) - QPointF(0.5, 0.5);
        return samplePixel(source, sourcePixelPoint, sourceSixteenBit);
    };

    QVector<ActivePixel> activePixels;
    int affectedMinX = destination.width();
    int affectedMinY = destination.height();
    int affectedMaxX = -1;
    int affectedMaxY = -1;
    constexpr double minimumCoverage = 1.0e-7;
    constexpr int maximumActivePixels = 1500000;

    // Build only the pixels touched by the stroke.  Unlike a full bounding-box
    // Poisson solve, this remains sparse for long or diagonal gestures.
    int inspectedTile = 0;
    for (CoverageTile &tile : coverageTiles) {
        if ((inspectedTile++ & 7) == 0 && cancellationRequested()) {
            return cancelledResult();
        }
        for (int localY = 0; localY < tile.rect.height(); ++localY) {
            if ((localY & 31) == 0 && cancellationRequested()) {
                return cancelledResult();
            }
            const int y = tile.rect.y() + localY;
            for (int localX = 0; localX < tile.rect.width(); ++localX) {
                const int storageIndex = localY * tile.rect.width() + localX;
                const double coverage = std::clamp(
                    1.0 - static_cast<double>(tile.remaining[storageIndex]),
                    0.0,
                    1.0);
                if (coverage <= minimumCoverage) {
                    continue;
                }

                const int x = tile.rect.x() + localX;
                const SampledPixel sampled = sourceAtDestinationPixel(x, y);
                if (!sampled.valid) {
                    continue;
                }
                const std::array<double, 4> before = destinationPixel(
                    immutableDestination, x, y, destinationSixteenBit);

                ActivePixel pixel;
                pixel.x = x;
                pixel.y = y;
                pixel.coverage = static_cast<float>(coverage);
                for (int component = 0; component < 3; ++component) {
                    pixel.source[component] = static_cast<float>(sampled.rgba[component]);
                    pixel.correction[component] = static_cast<float>(
                        before[component] - sampled.rgba[component]);
                }
                tile.activeIndices[storageIndex] = activePixels.size();
                activePixels.push_back(pixel);
                if (activePixels.size() > maximumActivePixels) {
                    result.error = QStringLiteral(
                        "Healing Brush stroke is too large for the current seamless solver."
                    );
                    return result;
                }
                affectedMinX = std::min(affectedMinX, x);
                affectedMinY = std::min(affectedMinY, y);
                affectedMaxX = std::max(affectedMaxX, x);
                affectedMaxY = std::max(affectedMaxY, y);
            }
        }
        tile.remaining.clear();
        tile.remaining.squeeze();
    }

    if (activePixels.isEmpty()) {
        result.image = destination;
        return result;
    }
    const QRect actualAffected(affectedMinX,
                               affectedMinY,
                               affectedMaxX - affectedMinX + 1,
                               affectedMaxY - affectedMinY + 1);

    const auto activeIndexAt = [&](const int x, const int y) -> int {
        if (x < 0 || y < 0 || x >= destination.width() || y >= destination.height()) {
            return -1;
        }
        const int tileX = x / coverageTileSize;
        const int tileY = y / coverageTileSize;
        const auto found = coverageTileLookup.constFind(tileKey(tileX, tileY));
        if (found == coverageTileLookup.cend()) {
            return -1;
        }
        const CoverageTile &tile = coverageTiles[found.value()];
        const int localX = x - tile.rect.x();
        const int localY = y - tile.rect.y();
        if (localX < 0 || localY < 0
            || localX >= tile.rect.width() || localY >= tile.rect.height()) {
            return -1;
        }
        return tile.activeIndices[localY * tile.rect.width() + localX];
    };

    QVector<std::array<float, 3>> boundaryCorrections;
    QHash<quint64, int> boundaryLookup;
    const auto boundaryAt = [&](const int x, const int y) -> int {
        if (x < 0 || y < 0 || x >= destination.width() || y >= destination.height()) {
            return OmittedNeighbour;
        }
        const quint64 key = coordinateKey(x, y);
        const auto found = boundaryLookup.constFind(key);
        if (found != boundaryLookup.cend()) {
            return encodeBoundary(found.value());
        }
        const SampledPixel sampled = sourceAtDestinationPixel(x, y);
        if (!sampled.valid) {
            // Match GIMP's edge behaviour: a neighbour outside the available
            // source/canvas does not become an artificial black boundary.
            return OmittedNeighbour;
        }
        const std::array<double, 4> before = destinationPixel(
            immutableDestination, x, y, destinationSixteenBit);
        std::array<float, 3> correction {};
        for (int component = 0; component < 3; ++component) {
            correction[component] = static_cast<float>(
                before[component] - sampled.rgba[component]);
        }
        const int index = boundaryCorrections.size();
        boundaryCorrections.push_back(correction);
        boundaryLookup.insert(key, index);
        return encodeBoundary(index);
    };

    static constexpr int neighbourDx[4] {1, 0, -1, 0};
    static constexpr int neighbourDy[4] {0, 1, 0, -1};
    QVector<int> redPixels;
    QVector<int> blackPixels;
    redPixels.reserve((activePixels.size() + 1) / 2);
    blackPixels.reserve(activePixels.size() / 2);

    for (int index = 0; index < activePixels.size(); ++index) {
        if ((index & 16383) == 0 && cancellationRequested()) {
            return cancelledResult();
        }
        ActivePixel &pixel = activePixels[index];
        for (int neighbour = 0; neighbour < 4; ++neighbour) {
            const int nx = pixel.x + neighbourDx[neighbour];
            const int ny = pixel.y + neighbourDy[neighbour];
            const int activeIndex = activeIndexAt(nx, ny);
            pixel.neighbours[neighbour] = activeIndex >= 0
                ? activeIndex : boundaryAt(nx, ny);
        }
        if (((pixel.x + pixel.y) & 1) == 0) {
            redPixels.push_back(index);
        } else {
            blackPixels.push_back(index);
        }
    }

    // Solve Laplace(correction) = 0 over the complete stroke mask.  The fixed
    // destination-minus-source values immediately outside the mask are the
    // Dirichlet boundary condition.  Consequently the source supplies the
    // interior gradients/structure, while the correction smoothly matches the
    // destination's colour and illumination at the edge.
    const double activeCount = static_cast<double>(activePixels.size());
    const double omegaEstimate = 2.0
        - 1.0 / (0.1575 * std::sqrt(activeCount) + 0.8);
    const double omega = std::clamp(omegaEstimate, 1.0, 1.95);
    // As in GIMP, 0.1 of an 8-bit perceptual code value is already below
    // visible precision and avoids wasting hundreds of iterations merely
    // because the destination container happens to be RGBA64.
    const double epsilon = 0.1 / 255.0;
    const double convergenceThreshold = epsilon * epsilon * activeCount * 3.0;
    constexpr int maximumIterations = 500;

    bool cancelledDuringSolve = false;
    const auto relaxParity = [&](const QVector<int> &indices) {
        double squaredUpdate = 0.0;
        int processed = 0;
        for (const int index : indices) {
            if ((processed++ & 16383) == 0 && cancellationRequested()) {
                cancelledDuringSolve = true;
                break;
            }
            ActivePixel &pixel = activePixels[index];
            std::array<double, 3> sum {0.0, 0.0, 0.0};
            int neighbourCount = 0;
            for (const int reference : pixel.neighbours) {
                if (reference == OmittedNeighbour) {
                    continue;
                }
                const std::array<float, 3> &value = reference >= 0
                    ? activePixels[reference].correction
                    : boundaryCorrections[decodeBoundary(reference)];
                for (int component = 0; component < 3; ++component) {
                    sum[component] += value[component];
                }
                ++neighbourCount;
            }
            if (neighbourCount == 0) {
                continue;
            }
            const double inverseCount = 1.0 / neighbourCount;
            for (int component = 0; component < 3; ++component) {
                const double target = sum[component] * inverseCount;
                const double delta = omega
                    * (target - pixel.correction[component]);
                pixel.correction[component] = static_cast<float>(
                    pixel.correction[component] + delta);
                squaredUpdate += delta * delta;
            }
        }
        return squaredUpdate;
    };

    for (int iteration = 0; iteration < maximumIterations; ++iteration) {
        if (cancellationRequested()) {
            return cancelledResult();
        }
        const double squaredUpdate = relaxParity(redPixels)
            + relaxParity(blackPixels);
        if (cancelledDuringSolve) {
            return cancelledResult();
        }
        if (squaredUpdate <= convergenceThreshold) {
            break;
        }
    }

    int writeIndex = 0;
    for (const ActivePixel &pixel : std::as_const(activePixels)) {
        if ((writeIndex++ & 16383) == 0 && cancellationRequested()) {
            return cancelledResult();
        }
        const std::array<double, 4> before = destinationPixel(
            immutableDestination, pixel.x, pixel.y, destinationSixteenBit);
        std::array<double, 4> after = before;
        const double coverage = std::clamp(static_cast<double>(pixel.coverage),
                                           0.0,
                                           1.0);
        for (int component = 0; component < 3; ++component) {
            const double seamless = std::clamp(
                static_cast<double>(pixel.source[component])
                    + static_cast<double>(pixel.correction[component]),
                0.0,
                1.0);
            after[component] = before[component] * (1.0 - coverage)
                + seamless * coverage;
        }
        // Healing adapts RGB only. Alpha, including zero-alpha pixels, remains
        // bit-for-bit unchanged while straight hidden RGB can still be repaired.
        after[3] = before[3];
        writeDestinationPixel(&destination,
                              pixel.x,
                              pixel.y,
                              destinationSixteenBit,
                              after);
    }

    destination.setColorSpace(request.destination.colorSpace());
    result.image = destination;
    result.affectedRect = actualAffected;
    return result;
}

} // namespace vfx
