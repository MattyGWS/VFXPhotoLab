#pragma once

#include "Adjustment.h"
#include "ColourManagement.h"
#include "CloneStamp.h"
#include "FillOperations.h"
#include "GradientOperations.h"
#include "SelectionMask.h"
#include "gpu/TileCache.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QLineF>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>
#include <QTransform>
#include <QUuid>
#include <QVector>

#include <atomic>

namespace vfx {

class WebGpuContext;
struct PreparedTileLayer;

class TiledCanvasEngine final {
public:
    static constexpr int TileSize = 256;

    struct BrushResult {
        QImage image;
        QRect affectedRect;
        bool usedGpu = false;
        bool selectionApplied = false;
        QString error;
    };

    struct FillResult {
        QImage image;
        QRect affectedRect;
        int changedPixelCount = 0;
        bool usedGpu = false;
        QString error;

        bool changed() const { return changedPixelCount > 0 && !affectedRect.isEmpty(); }
    };

    struct RenderInfo {
        QString path;
        QString fallbackReason;
        int visiblePassThroughGroups = 0;
        int visibleIsolatedGroups = 0;
        int maximumGroupDepth = 0;
        bool usedGpu = false;
        bool usedCpu = false;
        bool mixedBackend = false;
        bool cancelled = false;
    };

    explicit TiledCanvasEngine(WebGpuContext *webGpu = nullptr);

    QImage renderRegion(const QImage &source,
                        const QVector<LayerNode> &layers,
                        const QRect &previewRegion,
                        const QSize &documentSize,
                        bool allowGpu,
                        int level = 0,
                        const std::atomic_bool *cancelRequested = nullptr,
                        RenderInfo *renderInfo = nullptr,
                        const QUuid &documentSessionId = QUuid(),
                        quint64 colourStateRevision = 0,
                        ColourProcessingCompatibility processingCompatibility =
                            ColourProcessingCompatibility::LegacyV1);

    // Low-latency interactive path. The complete requested region is encoded in
    // one GPU command submission and read back once, avoiding a serial submit/map
    // cycle for every 256px tile. Ordinary callers retain the normal bounded tiled
    // fallback; transient live-paint callers may request exact bounded CPU fallback
    // so short-lived generations never enter the persistent tile cache.
    QImage renderInteractiveRegion(const QImage &source,
                                   const QVector<LayerNode> &layers,
                                   const QRect &previewRegion,
                                   const QSize &documentSize,
                                   bool allowGpu,
                                   int level = 0,
                                   const std::atomic_bool *cancelRequested = nullptr,
                                   RenderInfo *renderInfo = nullptr,
                                   const QUuid &documentSessionId = QUuid(),
                                   quint64 colourStateRevision = 0,
                                   ColourProcessingCompatibility processingCompatibility =
                                       ColourProcessingCompatibility::LegacyV1,
                                   bool skipPersistentCacheFallback = false);

    BrushResult stampRasterStroke(const QImage &sourceRaster,
                                  const QSize &documentSize,
                                  const QColorSpace &colourSpace,
                                  const QUuid &layerId,
                                  quint64 layerRevision,
                                  const QVector<QLineF> &documentSegments,
                                  const QTransform &documentToLayer,
                                  double diameter,
                                  double opacity,
                                  double hardness,
                                  const QColor &colour,
                                  bool erasing,
                                  bool allowGpu,
                                  const QUuid &documentSessionId = QUuid(),
                                  const SelectionMask::Snapshot *selectionSnapshot = nullptr,
                                  const QTransform &layerToDocument = QTransform());

    BrushResult stampChannelStroke(const QImage &sourceRaster,
                                   const QSize &documentSize,
                                   const QColorSpace &colourSpace,
                                   const QUuid &layerId,
                                   quint64 layerRevision,
                                   const QVector<QLineF> &documentSegments,
                                   const QTransform &documentToLayer,
                                   double diameter,
                                   double opacity,
                                   double hardness,
                                   int channelIndex,
                                   int channelValue,
                                   const QUuid &documentSessionId = QUuid());

    BrushResult stampMaskStroke(const QImage &sourceMask,
                                const QSize &documentSize,
                                const QUuid &layerId,
                                quint64 layerRevision,
                                const QVector<QLineF> &documentSegments,
                                const QTransform &documentToLayer,
                                double diameter,
                                double opacity,
                                double hardness,
                                int greyscaleValue,
                                bool restoringCoverage,
                                bool allowGpu,
                                const QUuid &documentSessionId = QUuid(),
                                const SelectionMask::Snapshot *selectionSnapshot = nullptr,
                                const QTransform &layerToDocument = QTransform());

    BrushResult stampCloneStroke(const CloneStampRequest &request,
                                 const QSize &documentSize,
                                 const QUuid &layerId,
                                 quint64 layerRevision,
                                 bool allowGpu,
                                 const QUuid &documentSessionId = QUuid(),
                                 const SelectionMask::Snapshot *selectionSnapshot = nullptr);

    FillResult applyFillCoverage(const QImage &sourceImage,
                                 const QImage &coverage,
                                 const QUuid &layerId,
                                 quint64 layerRevision,
                                 FillTarget target,
                                 int componentIndex,
                                 const QColor &colour,
                                 bool preserveTransparency,
                                 bool allowGpu,
                                 const QUuid &documentSessionId = QUuid());

    FillResult applyGradient(const GradientApplyRequest &request,
                             const QUuid &layerId,
                             quint64 layerRevision,
                             bool allowGpu,
                             const QUuid &documentSessionId = QUuid());

    void invalidateSurface(const QUuid &surfaceId);
    void invalidateSurface(const QUuid &documentSessionId, const QUuid &surfaceId);
    void invalidateSession(const QUuid &documentSessionId);
    void clear();
    TileCache::Stats cacheStats() const;
    TileCache::Stats cacheStatsForSession(const QUuid &documentSessionId) const;
    QString lastBackendText() const;
    quint64 cancelledCompositeTiles() const;

private:
    QVector<QRect> tileRectsForRegion(const QRect &region, const QSize &extent) const;
    quint64 compositeRevision(const QImage &source,
                              const QVector<LayerNode> &layers,
                              const QRect &tileRect,
                              const QSize &documentSize,
                              int level,
                              quint64 colourStateRevision,
                              quint64 spatialPlanFingerprint,
                              ColourProcessingCompatibility processingCompatibility) const;
    bool prepareGpuHierarchy(const QImage &source,
                             const QVector<LayerNode> &layers,
                             const QRect &tileRect,
                             const QSize &documentSize,
                             QVector<PreparedTileLayer> *prepared,
                             const std::atomic_bool *cancelRequested,
                             ColourProcessingCompatibility processingCompatibility,
                             QString *error = nullptr) const;
    void cacheResidentTile(const TileAddress &address,
                           quint64 revision,
                           const QImage &image);
    void trackSmartResidentTiles(const QUuid &documentSessionId,
                                 const QVector<PreparedTileLayer> &prepared);
    void invalidateResidentSurface(const QUuid &documentSessionId,
                                   const QUuid &surfaceId);
    void invalidateResidentSession(const QUuid &documentSessionId);
    static QString residentSurfaceKey(const QUuid &documentSessionId,
                                      const QUuid &surfaceId);

    WebGpuContext *m_webGpu = nullptr;
    TileCache m_cache;
    QHash<QString, QSet<quint64>> m_residentKeysBySurface;
    QString m_lastBackend = QStringLiteral("CPU tiled reference");
    quint64 m_cancelledCompositeTiles = 0;
};

} // namespace vfx
