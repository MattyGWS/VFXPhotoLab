#pragma once

#include <QRect>
#include <QSize>
#include <QVector>
#include <QtGlobal>

namespace vfx {

class ProgressivePreview final {
public:
    static constexpr int TileSize = 256;
    static constexpr int MaximumLevel = 3;

    static int chooseCoarseLevel(double canvasZoom,
                                 const QRect &visibleBaseRegion,
                                 const QSize &baseSize,
                                 int targetTileCount = 8);

    static int chooseSpatialInteractionLevel(const QRect &visibleBaseRegion,
                                             const QSize &baseSize,
                                             int navigationCoarseLevel,
                                             bool preserveHighFrequencyDetail,
                                             qint64 targetInteractivePixels =
                                                 512LL * 512LL);

    static QVector<QRect> levelTileRects(const QRect &visibleBaseRegion,
                                         const QSize &baseSize,
                                         int level,
                                         int prefetchTiles = 0);

    static QRect baseRectForLevelTile(const QRect &levelTile,
                                      const QSize &baseSize,
                                      int level);
};

} // namespace vfx
