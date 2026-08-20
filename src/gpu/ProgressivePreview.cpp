#include "gpu/ProgressivePreview.h"

#include <QPointF>
#include <QRectF>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace vfx {
namespace {

int levelScale(const int level)
{
    return 1 << std::clamp(level, 0, ProgressivePreview::MaximumLevel);
}

QSize levelSize(const QSize &baseSize, const int level)
{
    const int scale = levelScale(level);
    return QSize((baseSize.width() + scale - 1) / scale,
                 (baseSize.height() + scale - 1) / scale);
}

QRect alignedLevelRegion(const QRect &baseRegion,
                         const QSize &baseSize,
                         const int level,
                         const int prefetchTiles)
{
    if (baseSize.isEmpty()) {
        return {};
    }
    const QRect clipped = baseRegion.intersected(QRect(QPoint(0, 0), baseSize));
    if (clipped.isEmpty()) {
        return {};
    }

    const int scale = levelScale(level);
    const QSize extent = levelSize(baseSize, level);
    QRect levelRegion(clipped.left() / scale,
                      clipped.top() / scale,
                      (clipped.right() / scale) - (clipped.left() / scale) + 1,
                      (clipped.bottom() / scale) - (clipped.top() / scale) + 1);
    if (prefetchTiles > 0) {
        const int margin = prefetchTiles * ProgressivePreview::TileSize;
        levelRegion.adjust(-margin, -margin, margin, margin);
    }
    return levelRegion.intersected(QRect(QPoint(0, 0), extent));
}

int tileCountAtLevel(const QRect &visibleBaseRegion,
                     const QSize &baseSize,
                     const int level)
{
    const QRect region = alignedLevelRegion(visibleBaseRegion, baseSize, level, 0);
    if (region.isEmpty()) {
        return 0;
    }
    const int columns = region.right() / ProgressivePreview::TileSize
        - region.left() / ProgressivePreview::TileSize + 1;
    const int rows = region.bottom() / ProgressivePreview::TileSize
        - region.top() / ProgressivePreview::TileSize + 1;
    return columns * rows;
}

} // namespace

int ProgressivePreview::chooseCoarseLevel(const double canvasZoom,
                                          const QRect &visibleBaseRegion,
                                          const QSize &baseSize,
                                          const int targetTileCount)
{
    if (baseSize.isEmpty() || visibleBaseRegion.isEmpty()) {
        return 0;
    }

    // Keep a coarse texel at or below roughly one screen pixel. This avoids
    // rendering detail that cannot yet be seen while the viewport is moving.
    const double safeZoom = std::max(0.01, canvasZoom);
    int level = std::clamp(static_cast<int>(std::floor(std::log2(1.0 / safeZoom))),
                           0,
                           MaximumLevel);
    while (level < MaximumLevel
           && tileCountAtLevel(visibleBaseRegion, baseSize, level)
                  > std::max(1, targetTileCount)) {
        ++level;
    }
    return level;
}

int ProgressivePreview::chooseSpatialInteractionLevel(
    const QRect &visibleBaseRegion,
    const QSize &baseSize,
    const int navigationCoarseLevel,
    const bool preserveHighFrequencyDetail,
    const qint64 targetInteractivePixels)
{
    if (preserveHighFrequencyDetail) {
        return 0;
    }

    const QRect visible = visibleBaseRegion.intersected(
        QRect(QPoint(0, 0), baseSize));
    if (visible.isEmpty()) {
        return 0;
    }

    const qint64 boundedTarget = std::max<qint64>(1, targetInteractivePixels);
    const qint64 pixels = static_cast<qint64>(visible.width()) * visible.height();
    int level = std::clamp(navigationCoarseLevel, 0, MaximumLevel);
    while (level < MaximumLevel
           && pixels / (qint64(1) << (level * 2)) > boundedTarget) {
        ++level;
    }
    return level;
}

QVector<QRect> ProgressivePreview::levelTileRects(const QRect &visibleBaseRegion,
                                                   const QSize &baseSize,
                                                   const int level,
                                                   const int prefetchTiles)
{
    QVector<QRect> tiles;
    const int clampedLevel = std::clamp(level, 0, MaximumLevel);
    const QRect region = alignedLevelRegion(visibleBaseRegion,
                                            baseSize,
                                            clampedLevel,
                                            std::max(0, prefetchTiles));
    if (region.isEmpty()) {
        return tiles;
    }

    const QSize extent = levelSize(baseSize, clampedLevel);
    const int firstX = region.left() / TileSize;
    const int lastX = region.right() / TileSize;
    const int firstY = region.top() / TileSize;
    const int lastY = region.bottom() / TileSize;
    tiles.reserve((lastX - firstX + 1) * (lastY - firstY + 1));
    for (int y = firstY; y <= lastY; ++y) {
        for (int x = firstX; x <= lastX; ++x) {
            tiles.push_back(QRect(x * TileSize,
                                  y * TileSize,
                                  TileSize,
                                  TileSize)
                                .intersected(QRect(QPoint(0, 0), extent)));
        }
    }

    const QPointF centre = QRectF(region).center();
    std::sort(tiles.begin(), tiles.end(), [centre](const QRect &left, const QRect &right) {
        const QPointF leftDelta = QRectF(left).center() - centre;
        const QPointF rightDelta = QRectF(right).center() - centre;
        const double leftDistance = leftDelta.x() * leftDelta.x()
            + leftDelta.y() * leftDelta.y();
        const double rightDistance = rightDelta.x() * rightDelta.x()
            + rightDelta.y() * rightDelta.y();
        if (!qFuzzyCompare(leftDistance + 1.0, rightDistance + 1.0)) {
            return leftDistance < rightDistance;
        }
        if (left.y() != right.y()) {
            return left.y() < right.y();
        }
        return left.x() < right.x();
    });
    return tiles;
}

QRect ProgressivePreview::baseRectForLevelTile(const QRect &levelTile,
                                                const QSize &baseSize,
                                                const int level)
{
    if (levelTile.isEmpty() || baseSize.isEmpty()) {
        return {};
    }
    const int scale = levelScale(level);
    return QRect(levelTile.x() * scale,
                 levelTile.y() * scale,
                 levelTile.width() * scale,
                 levelTile.height() * scale)
        .intersected(QRect(QPoint(0, 0), baseSize));
}

} // namespace vfx
