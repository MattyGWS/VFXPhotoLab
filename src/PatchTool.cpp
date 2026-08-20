#include "PatchTool.h"

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
        for (int component = 0; component < 3; ++component) {
            for (int index = 0; index < 4; ++index) {
                sampled.rgba[component] += pixels[index][component] * weights[index];
            }
        }
    }
    sampled.valid = true;
    return sampled;
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

double selectionCoverageAtDocument(const SelectionMask &selection,
                                   const QPointF &documentPoint)
{
    const double sampleX = documentPoint.x() - 0.5;
    const double sampleY = documentPoint.y() - 0.5;
    const int x0 = static_cast<int>(std::floor(sampleX));
    const int y0 = static_cast<int>(std::floor(sampleY));
    const double fx = sampleX - x0;
    const double fy = sampleY - y0;
    const double c00 = selection.coverageAt(x0, y0) / 255.0;
    const double c10 = selection.coverageAt(x0 + 1, y0) / 255.0;
    const double c01 = selection.coverageAt(x0, y0 + 1) / 255.0;
    const double c11 = selection.coverageAt(x0 + 1, y0 + 1) / 255.0;
    const double top = c00 + (c10 - c00) * fx;
    const double bottom = c01 + (c11 - c01) * fx;
    return std::clamp(top + (bottom - top) * fy, 0.0, 1.0);
}

struct CoverageTile {
    QRect rect;
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

PatchToolResult applyPatchTool(const PatchToolRequest &request)
{
    PatchToolResult result;
    const auto cancellationRequested = [&request] {
        return request.cancelRequested
            && request.cancelRequested->load(std::memory_order_acquire);
    };
    const auto cancelledResult = [&result]() -> PatchToolResult {
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
        result.error = QStringLiteral("Patch Tool destination is empty.");
        return result;
    }
    if (request.source.isNull()) {
        result.error = QStringLiteral("Patch Tool source is empty.");
        return result;
    }
    if (!request.selection.active) {
        result.error = QStringLiteral("Patch Tool requires an active selection.");
        return result;
    }

    SelectionMask selection(request.selection.size);
    if (!selection.restoreSnapshot(request.selection, false) || selection.isEmpty()) {
        result.error = QStringLiteral("Patch Tool selection is empty or invalid.");
        return result;
    }

    const bool destinationSixteenBit = request.destination.depth() > 32;
    const bool sourceSixteenBit = request.source.depth() > 32;
    QImage destination = request.destination.convertToFormat(
        destinationSixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    QImage source = request.source.convertToFormat(
        sourceSixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (destination.isNull() || source.isNull()) {
        result.error = QStringLiteral("Patch Tool could not prepare its images.");
        return result;
    }
    destination.detach();
    const QImage immutableDestination = destination;
    const double opacity = std::clamp(request.opacity, 0.0, 1.0);
    if (opacity <= 0.0) {
        result.image = destination;
        return result;
    }

    const QTransform targetPixelToDocument = request.targetLayerToDocument
        * request.targetPixelToLayer;
    bool targetDocumentInvertible = false;
    const QTransform documentToTargetPixel = targetPixelToDocument.inverted(
        &targetDocumentInvertible);
    if (!targetDocumentInvertible) {
        result.error = QStringLiteral("Patch Tool target transform is not invertible.");
        return result;
    }

    QRectF selectionDocumentBounds = selection.isFull()
        ? QRectF(QPointF(), QSizeF(selection.size()))
        : QRectF(selection.nonZeroBounds());
    selectionDocumentBounds.translate(request.destinationOffsetDocument);
    const QRect targetBounds = documentToTargetPixel.mapRect(selectionDocumentBounds)
                                   .adjusted(-2.0, -2.0, 2.0, 2.0)
                                   .toAlignedRect()
                                   .intersected(destination.rect());
    if (targetBounds.isEmpty()) {
        result.image = destination;
        return result;
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
        tile.activeIndices.fill(-1, tileRect.width() * tileRect.height());
        const int index = coverageTiles.size();
        coverageTiles.push_back(std::move(tile));
        coverageTileLookup.insert(key, index);
        return coverageTiles[index];
    };

    const int firstTileX = targetBounds.left() / coverageTileSize;
    const int lastTileX = targetBounds.right() / coverageTileSize;
    const int firstTileY = targetBounds.top() / coverageTileSize;
    const int lastTileY = targetBounds.bottom() / coverageTileSize;
    for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
        for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
            ensureCoverageTile(tileX, tileY);
        }
    }

    QVector<ActivePixel> activePixels;
    int affectedMinX = destination.width();
    int affectedMinY = destination.height();
    int affectedMaxX = -1;
    int affectedMaxY = -1;
    constexpr double minimumCoverage = 1.0e-7;
    constexpr int maximumActivePixels = 1500000;

    int inspectedTile = 0;
    for (CoverageTile &tile : coverageTiles) {
        if ((inspectedTile++ & 7) == 0 && cancellationRequested()) {
            return cancelledResult();
        }
        const QRect inspect = tile.rect.intersected(targetBounds);
        for (int y = inspect.top(); y <= inspect.bottom(); ++y) {
            if ((y & 31) == 0 && cancellationRequested()) {
                return cancelledResult();
            }
            for (int x = inspect.left(); x <= inspect.right(); ++x) {
                const QPointF targetPixelCentre(x + 0.5, y + 0.5);
                const QPointF targetLayerPoint = request.targetPixelToLayer.map(
                    targetPixelCentre);
                const QPointF destinationDocumentPoint = request.targetLayerToDocument.map(
                    targetLayerPoint);
                const QPointF selectionDocumentPoint = destinationDocumentPoint
                    - request.destinationOffsetDocument;
                const double coverage = selectionCoverageAtDocument(
                    selection, selectionDocumentPoint) * opacity;
                if (coverage <= minimumCoverage) {
                    continue;
                }
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
                const int localX = x - tile.rect.x();
                const int localY = y - tile.rect.y();
                tile.activeIndices[localY * tile.rect.width() + localX]
                    = activePixels.size();
                activePixels.push_back(pixel);
                if (activePixels.size() > maximumActivePixels) {
                    result.error = QStringLiteral(
                        "Patch Tool selection is too large for the current seamless solver.");
                    return result;
                }
                affectedMinX = std::min(affectedMinX, x);
                affectedMinY = std::min(affectedMinY, y);
                affectedMaxX = std::max(affectedMaxX, x);
                affectedMaxY = std::max(affectedMaxY, y);
            }
        }
    }

    if (activePixels.isEmpty()) {
        result.image = destination;
        return result;
    }

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
        (((pixel.x + pixel.y) & 1) == 0 ? redPixels : blackPixels).push_back(index);
    }

    const double activeCount = static_cast<double>(activePixels.size());
    const double omegaEstimate = 2.0
        - 1.0 / (0.1575 * std::sqrt(activeCount) + 0.8);
    const double omega = std::clamp(omegaEstimate, 1.0, 1.95);
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
                const double delta = omega * (target - pixel.correction[component]);
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
        after[3] = before[3];
        writeDestinationPixel(&destination,
                              pixel.x,
                              pixel.y,
                              destinationSixteenBit,
                              after);
    }

    destination.setColorSpace(request.destination.colorSpace());
    result.image = destination;
    result.affectedRect = QRect(affectedMinX,
                                affectedMinY,
                                affectedMaxX - affectedMinX + 1,
                                affectedMaxY - affectedMinY + 1);
    return result;
}

} // namespace vfx
