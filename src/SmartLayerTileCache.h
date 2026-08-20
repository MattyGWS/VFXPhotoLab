#pragma once

#include "SmartLayerFoundation.h"

#include <QByteArray>
#include <QColorSpace>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QPointF>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QTransform>
#include <QUuid>
#include <QtGlobal>

#include <atomic>
#include <optional>

namespace vfx {

// 0.14.0e bounded intermediate cache for Smart Layer rendering.
//
// There are two independent domains:
//   1. source tiles: immutable 256x256 straight-RGBA64 tiles cut from the
//      current source presentation revision;
//   2. transformed tiles: exact requested parent-space regions after the
//      instance transform/sampling step, before mask/opacity/blend.
//
// Keeping transformed tiles independent of the final composite cache is what
// allows painting underneath an unchanged Smart Layer to reuse its expensive
// transform result. Source-tile/fingerprint lookup remains revision + QImage
// identity scoped; transformed tiles may cross a revision boundary only when
// the exact sampled source-region fingerprint is identical. Undo/branch edits
// therefore cannot alias stale pixels while distant source edits can be reused.
class SmartLayerTileCache final {
public:
    static constexpr int SourceTileSize = 256;

    struct Stats {
        qsizetype ramBytes = 0;
        int sourceTiles = 0;
        int transformedTiles = 0;
        quint64 sourceHits = 0;
        quint64 sourceMisses = 0;
        quint64 transformedHits = 0;
        quint64 transformedMisses = 0;
        quint64 evictions = 0;
        quint64 invalidations = 0;
    };

    static SmartLayerTileCache &instance();

    void setRamBudget(qsizetype bytes);
    qsizetype ramBudget() const;
    Stats stats() const;
    void clear();
    void invalidateSource(const QUuid &sourceId);
    void invalidateSources(const QSet<QUuid> &sourceIds);

    // Assemble only the inverse-mapped source footprint required by one
    // transformed output request. Cached source tiles are converted once to
    // straight RGBA64 so Bicubic/Lanczos requests do not repeatedly convert
    // overlapping source pixels.
    QImage sourcePatch(const QImage &source,
                       const QUuid &sourceId,
                       quint64 sourceRevision,
                       const QRect &sourceRect,
                       const std::atomic_bool *cancelRequested = nullptr);

    // Exact bounded-region fingerprint for dirty propagation. Source pixels are
    // fetched through the 256x256 source-tile cache, but only the overlap that
    // the transformed output request can actually sample contributes. A source
    // edit outside that footprint therefore leaves the parent composite tile
    // key unchanged even when the Smart Source's global revision advances.
    quint64 sourceRegionFingerprint(const QImage &source,
                                    const QUuid &sourceId,
                                    quint64 sourceRevision,
                                    const QRect &sourceRect);

    std::optional<QImage> lookupTransformed(const QByteArray &key);
    void storeTransformed(const QByteArray &key,
                          const QUuid &sourceId,
                          quint64 sourceRevision,
                          const QImage &image);

    static QByteArray transformedKey(const QImage &source,
                                     const QUuid &sourceId,
                                     quint64 sourceRevision,
                                     const QSize &referenceSize,
                                     const QPointF &referenceOrigin,
                                     const QSize &previewSize,
                                     const QTransform &worldTransform,
                                     const QSize &documentSize,
                                     const QRect &region,
                                     QImage::Format format,
                                     const QColorSpace &space,
                                     const SmartTransformState &transform,
                                     bool forceOpaquePixelAlpha,
                                     quint64 sourceDependencyFingerprint = 0);

    // Stable 64-bit key used by the native renderer's VRAM residency cache.
    // It is derived from the same semantic transformed-tile key as the CPU
    // intermediate cache, so CPU/GPU residency cannot accidentally disagree.
    static quint64 gpuResidencyKey(const QByteArray &transformedKey);

private:
    enum class EntryKind : quint8 { Source, Transformed };
    struct Entry {
        QImage image;
        QUuid sourceId;
        quint64 sourceRevision = 0;
        quint64 lastUseSerial = 0;
        qsizetype bytes = 0;
        EntryKind kind = EntryKind::Source;
    };
    struct FingerprintEntry {
        QUuid sourceId;
        quint64 value = 1;
        quint64 lastUseSerial = 0;
    };

    SmartLayerTileCache() = default;

    QByteArray sourceTileKey(const QImage &source,
                             const QUuid &sourceId,
                             quint64 sourceRevision,
                             int tileX,
                             int tileY) const;
    std::optional<QImage> lookupLocked(const QByteArray &key, EntryKind kind);
    void storeLocked(const QByteArray &key,
                     EntryKind kind,
                     const QUuid &sourceId,
                     quint64 sourceRevision,
                     const QImage &image);
    void evictLocked();
    void removeLocked(const QByteArray &key);

    mutable QMutex m_mutex;
    QHash<QByteArray, Entry> m_entries;
    QHash<QByteArray, FingerprintEntry> m_regionFingerprints;
    qsizetype m_ramBudget = qsizetype(192) * 1024 * 1024;
    qsizetype m_ramBytes = 0;
    quint64 m_useSerial = 0;
    Stats m_stats;
};

} // namespace vfx
