#include "SmartLayerTileCache.h"

#include <QCryptographicHash>
#include <QMutexLocker>
#include <QRgba64>
#include <QVector>

#include <algorithm>
#include <cstring>
#include <limits>

namespace vfx {
namespace {

template<typename T>
void appendValue(QByteArray *bytes, const T &value)
{
    bytes->append(reinterpret_cast<const char *>(&value), sizeof(T));
}

void appendTransform(QByteArray *bytes, const QTransform &transform)
{
    const double values[] = {
        transform.m11(), transform.m12(), transform.m13(),
        transform.m21(), transform.m22(), transform.m23(),
        transform.m31(), transform.m32(), transform.m33()
    };
    bytes->append(reinterpret_cast<const char *>(values), sizeof(values));
}

QByteArray colourSpaceFingerprint(const QColorSpace &space)
{
    if (!space.isValid()) return QByteArrayLiteral("invalid");
    const QByteArray profile = space.iccProfile();
    if (!profile.isEmpty()) {
        return QCryptographicHash::hash(profile, QCryptographicHash::Sha256);
    }
    const QByteArray fallback = space.description().toUtf8();
    return QCryptographicHash::hash(fallback, QCryptographicHash::Sha256);
}

QByteArray digest(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

constexpr quint64 FnvOffset = 1469598103934665603ULL;
constexpr quint64 FnvPrime = 1099511628211ULL;

void fingerprintBytes(quint64 *hash, const void *data, const qsizetype size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (qsizetype i = 0; i < size; ++i) {
        *hash ^= bytes[i];
        *hash *= FnvPrime;
    }
}

void combineFingerprint(quint64 *hash, const quint64 value)
{
    fingerprintBytes(hash, &value, sizeof(value));
}

} // namespace

SmartLayerTileCache &SmartLayerTileCache::instance()
{
    static SmartLayerTileCache cache;
    return cache;
}

void SmartLayerTileCache::setRamBudget(const qsizetype bytes)
{
    QMutexLocker lock(&m_mutex);
    m_ramBudget = std::max<qsizetype>(0, bytes);
    evictLocked();
}

qsizetype SmartLayerTileCache::ramBudget() const
{
    QMutexLocker lock(&m_mutex);
    return m_ramBudget;
}

SmartLayerTileCache::Stats SmartLayerTileCache::stats() const
{
    QMutexLocker lock(&m_mutex);
    Stats result = m_stats;
    result.ramBytes = m_ramBytes;
    result.sourceTiles = 0;
    result.transformedTiles = 0;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (it.value().kind == EntryKind::Source) ++result.sourceTiles;
        else ++result.transformedTiles;
    }
    return result;
}

void SmartLayerTileCache::clear()
{
    QMutexLocker lock(&m_mutex);
    m_entries.clear();
    m_regionFingerprints.clear();
    m_ramBytes = 0;
    m_stats = {};
}

void SmartLayerTileCache::invalidateSource(const QUuid &sourceId)
{
    if (sourceId.isNull()) return;
    invalidateSources(QSet<QUuid>{sourceId});
}

void SmartLayerTileCache::invalidateSources(const QSet<QUuid> &sourceIds)
{
    if (sourceIds.isEmpty()) return;
    QMutexLocker lock(&m_mutex);
    QVector<QByteArray> keys;
    keys.reserve(m_entries.size());
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (sourceIds.contains(it.value().sourceId)) keys.push_back(it.key());
    }
    std::sort(keys.begin(), keys.end());
    for (const QByteArray &key : keys) removeLocked(key);
    QVector<QByteArray> fingerprintKeys;
    fingerprintKeys.reserve(m_regionFingerprints.size());
    for (auto it = m_regionFingerprints.cbegin(); it != m_regionFingerprints.cend(); ++it) {
        if (sourceIds.contains(it.value().sourceId)) fingerprintKeys.push_back(it.key());
    }
    for (const QByteArray &key : fingerprintKeys) m_regionFingerprints.remove(key);
    m_stats.invalidations += static_cast<quint64>(keys.size() + fingerprintKeys.size());
}

QByteArray SmartLayerTileCache::sourceTileKey(const QImage &source,
                                               const QUuid &sourceId,
                                               const quint64 sourceRevision,
                                               const int tileX,
                                               const int tileY) const
{
    QByteArray bytes = QByteArrayLiteral("smart-source-tile-v1");
    bytes += sourceId.toRfc4122();
    appendValue(&bytes, sourceRevision);
    const qint64 cacheKey = source.cacheKey();
    appendValue(&bytes, cacheKey);
    appendValue(&bytes, source.width());
    appendValue(&bytes, source.height());
    appendValue(&bytes, static_cast<int>(source.format()));
    appendValue(&bytes, tileX);
    appendValue(&bytes, tileY);
    bytes += colourSpaceFingerprint(source.colorSpace());
    return digest(bytes);
}

std::optional<QImage> SmartLayerTileCache::lookupLocked(const QByteArray &key,
                                                         const EntryKind kind)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end() || it.value().kind != kind || it.value().image.isNull()) {
        if (kind == EntryKind::Source) ++m_stats.sourceMisses;
        else ++m_stats.transformedMisses;
        return std::nullopt;
    }
    it.value().lastUseSerial = ++m_useSerial;
    if (kind == EntryKind::Source) ++m_stats.sourceHits;
    else ++m_stats.transformedHits;
    return it.value().image;
}

void SmartLayerTileCache::storeLocked(const QByteArray &key,
                                      const EntryKind kind,
                                      const QUuid &sourceId,
                                      const quint64 sourceRevision,
                                      const QImage &image)
{
    if (key.isEmpty() || image.isNull() || m_ramBudget <= 0) return;
    const qsizetype bytes = image.sizeInBytes();
    if (bytes <= 0 || bytes > m_ramBudget) return;
    removeLocked(key);
    Entry entry;
    entry.image = image;
    entry.sourceId = sourceId;
    entry.sourceRevision = sourceRevision;
    entry.lastUseSerial = ++m_useSerial;
    entry.bytes = bytes;
    entry.kind = kind;
    m_entries.insert(key, entry);
    m_ramBytes += bytes;
    evictLocked();
}

void SmartLayerTileCache::removeLocked(const QByteArray &key)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end()) return;
    m_ramBytes -= it.value().bytes;
    if (m_ramBytes < 0) m_ramBytes = 0;
    m_entries.erase(it);
}

void SmartLayerTileCache::evictLocked()
{
    while (m_ramBytes > m_ramBudget && !m_entries.isEmpty()) {
        auto victim = m_entries.cbegin();
        for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
            if (it.value().lastUseSerial < victim.value().lastUseSerial
                || (it.value().lastUseSerial == victim.value().lastUseSerial
                    && it.key() < victim.key())) {
                victim = it;
            }
        }
        const QByteArray key = victim.key();
        removeLocked(key);
        ++m_stats.evictions;
    }
}

QImage SmartLayerTileCache::sourcePatch(const QImage &source,
                                        const QUuid &sourceId,
                                        const quint64 sourceRevision,
                                        const QRect &sourceRect,
                                        const std::atomic_bool *cancelRequested)
{
    const QRect requested = sourceRect.intersected(source.rect());
    if (source.isNull() || sourceId.isNull() || sourceRevision < 1 || requested.isEmpty()) {
        return {};
    }
    QImage patch(requested.size(), QImage::Format_RGBA64);
    if (patch.isNull()) return {};
    patch.fill(Qt::transparent);
    patch.setColorSpace(source.colorSpace());

    const int firstTileX = requested.left() / SourceTileSize;
    const int lastTileX = requested.right() / SourceTileSize;
    const int firstTileY = requested.top() / SourceTileSize;
    const int lastTileY = requested.bottom() / SourceTileSize;
    for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
        for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
            const QRect tileRect(tileX * SourceTileSize,
                                 tileY * SourceTileSize,
                                 SourceTileSize,
                                 SourceTileSize);
            const QRect clippedTile = tileRect.intersected(source.rect());
            if (clippedTile.isEmpty()) continue;
            const QByteArray key = sourceTileKey(source, sourceId, sourceRevision,
                                                 tileX, tileY);
            QImage tile;
            {
                QMutexLocker lock(&m_mutex);
                if (const auto cached = lookupLocked(key, EntryKind::Source)) {
                    tile = *cached;
                }
            }
            if (tile.isNull()) {
                tile = source.copy(clippedTile).convertToFormat(QImage::Format_RGBA64);
                if (tile.isNull()) return {};
                tile.setColorSpace(source.colorSpace());
                QMutexLocker lock(&m_mutex);
                storeLocked(key, EntryKind::Source, sourceId, sourceRevision, tile);
            }

            const QRect overlap = requested.intersected(clippedTile);
            if (overlap.isEmpty()) continue;
            const int srcX = overlap.left() - clippedTile.left();
            const int dstX = overlap.left() - requested.left();
            const int width = overlap.width();
            for (int y = overlap.top(); y <= overlap.bottom(); ++y) {
                const auto *src = reinterpret_cast<const QRgba64 *>(
                    tile.constScanLine(y - clippedTile.top())) + srcX;
                auto *dst = reinterpret_cast<QRgba64 *>(
                    patch.scanLine(y - requested.top())) + dstX;
                std::memcpy(dst, src, static_cast<size_t>(width) * sizeof(QRgba64));
            }
        }
    }
    return patch;
}


quint64 SmartLayerTileCache::sourceRegionFingerprint(const QImage &source,
                                                      const QUuid &sourceId,
                                                      const quint64 sourceRevision,
                                                      const QRect &sourceRect)
{
    const QRect requested = sourceRect.intersected(source.rect());
    if (source.isNull() || sourceId.isNull() || sourceRevision < 1 || requested.isEmpty()) {
        return 1;
    }
    quint64 combined = FnvOffset;
    const int firstTileX = requested.left() / SourceTileSize;
    const int lastTileX = requested.right() / SourceTileSize;
    const int firstTileY = requested.top() / SourceTileSize;
    const int lastTileY = requested.bottom() / SourceTileSize;
    for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
        for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
            const QRect clippedTile = QRect(tileX * SourceTileSize,
                                            tileY * SourceTileSize,
                                            SourceTileSize,
                                            SourceTileSize)
                                          .intersected(source.rect());
            const QRect overlap = requested.intersected(clippedTile);
            if (overlap.isEmpty()) continue;
            const QByteArray tileKey = sourceTileKey(source, sourceId, sourceRevision,
                                                     tileX, tileY);
            QByteArray regionKeyBytes = tileKey;
            const int localX = overlap.left() - clippedTile.left();
            const int localY = overlap.top() - clippedTile.top();
            const int localW = overlap.width();
            const int localH = overlap.height();
            appendValue(&regionKeyBytes, localX);
            appendValue(&regionKeyBytes, localY);
            appendValue(&regionKeyBytes, localW);
            appendValue(&regionKeyBytes, localH);
            const QByteArray regionKey = digest(regionKeyBytes);

            quint64 regionFingerprint = 0;
            {
                QMutexLocker lock(&m_mutex);
                auto fp = m_regionFingerprints.find(regionKey);
                if (fp != m_regionFingerprints.end()) {
                    fp.value().lastUseSerial = ++m_useSerial;
                    regionFingerprint = fp.value().value;
                }
            }
            if (regionFingerprint == 0) {
                QImage tile;
                {
                    QMutexLocker lock(&m_mutex);
                    auto it = m_entries.find(tileKey);
                    if (it != m_entries.end()
                        && it.value().kind == EntryKind::Source
                        && !it.value().image.isNull()) {
                        it.value().lastUseSerial = ++m_useSerial;
                        ++m_stats.sourceHits;
                        tile = it.value().image;
                    } else {
                        ++m_stats.sourceMisses;
                    }
                }
                if (tile.isNull()) {
                    tile = source.copy(clippedTile).convertToFormat(QImage::Format_RGBA64);
                    if (tile.isNull()) return 1;
                    tile.setColorSpace(source.colorSpace());
                    QMutexLocker lock(&m_mutex);
                    storeLocked(tileKey, EntryKind::Source, sourceId, sourceRevision, tile);
                }

                regionFingerprint = FnvOffset;
                for (int row = 0; row < localH; ++row) {
                    const auto *pixels = reinterpret_cast<const QRgba64 *>(
                        tile.constScanLine(localY + row)) + localX;
                    fingerprintBytes(&regionFingerprint, pixels,
                                     static_cast<qsizetype>(localW) * sizeof(QRgba64));
                }
                if (regionFingerprint == 0) regionFingerprint = 1;
                QMutexLocker lock(&m_mutex);
                FingerprintEntry entry;
                entry.sourceId = sourceId;
                entry.value = regionFingerprint;
                entry.lastUseSerial = ++m_useSerial;
                m_regionFingerprints.insert(regionKey, entry);
                constexpr int MaximumRegionFingerprints = 65536;
                while (m_regionFingerprints.size() > MaximumRegionFingerprints) {
                    auto victim = m_regionFingerprints.cbegin();
                    for (auto it = m_regionFingerprints.cbegin();
                         it != m_regionFingerprints.cend(); ++it) {
                        if (it.value().lastUseSerial < victim.value().lastUseSerial
                            || (it.value().lastUseSerial == victim.value().lastUseSerial
                                && it.key() < victim.key())) {
                            victim = it;
                        }
                    }
                    m_regionFingerprints.remove(victim.key());
                }
            }
            combineFingerprint(&combined, static_cast<quint64>(overlap.x()));
            combineFingerprint(&combined, static_cast<quint64>(overlap.y()));
            combineFingerprint(&combined, static_cast<quint64>(overlap.width()));
            combineFingerprint(&combined, static_cast<quint64>(overlap.height()));
            combineFingerprint(&combined, regionFingerprint);
        }
    }
    return combined == 0 ? 1 : combined;
}

std::optional<QImage> SmartLayerTileCache::lookupTransformed(const QByteArray &key)
{
    QMutexLocker lock(&m_mutex);
    return lookupLocked(key, EntryKind::Transformed);
}

void SmartLayerTileCache::storeTransformed(const QByteArray &key,
                                           const QUuid &sourceId,
                                           const quint64 sourceRevision,
                                           const QImage &image)
{
    QMutexLocker lock(&m_mutex);
    storeLocked(key, EntryKind::Transformed, sourceId, sourceRevision, image);
}

QByteArray SmartLayerTileCache::transformedKey(const QImage &source,
                                                const QUuid &sourceId,
                                                const quint64 sourceRevision,
                                                const QSize &referenceSize,
                                                const QPointF &referenceOrigin,
                                                const QSize &previewSize,
                                                const QTransform &worldTransform,
                                                const QSize &documentSize,
                                                const QRect &region,
                                                const QImage::Format format,
                                                const QColorSpace &space,
                                                const SmartTransformState &transform,
                                                const bool forceOpaquePixelAlpha,
                                                const quint64 sourceDependencyFingerprint)
{
    QByteArray bytes = QByteArrayLiteral("smart-transformed-tile-v2");
    bytes += sourceId.toRfc4122();
    const bool regionAddressed = sourceDependencyFingerprint != 0;
    appendValue(&bytes, regionAddressed);
    if (regionAddressed) {
        appendValue(&bytes, sourceDependencyFingerprint);
    } else {
        appendValue(&bytes, sourceRevision);
        const qint64 cacheKey = source.cacheKey();
        appendValue(&bytes, cacheKey);
    }
    appendValue(&bytes, source.width());
    appendValue(&bytes, source.height());
    appendValue(&bytes, static_cast<int>(source.format()));
    appendValue(&bytes, referenceSize.width());
    appendValue(&bytes, referenceSize.height());
    appendValue(&bytes, referenceOrigin.x());
    appendValue(&bytes, referenceOrigin.y());
    appendValue(&bytes, previewSize.width());
    appendValue(&bytes, previewSize.height());
    appendTransform(&bytes, worldTransform);
    appendValue(&bytes, documentSize.width());
    appendValue(&bytes, documentSize.height());
    appendValue(&bytes, region.x());
    appendValue(&bytes, region.y());
    appendValue(&bytes, region.width());
    appendValue(&bytes, region.height());
    appendValue(&bytes, static_cast<int>(format));
    appendValue(&bytes, static_cast<int>(transform.interpolation));
    appendValue(&bytes, forceOpaquePixelAlpha);
    bytes += colourSpaceFingerprint(source.colorSpace());
    bytes += colourSpaceFingerprint(space);
    return digest(bytes);
}

quint64 SmartLayerTileCache::gpuResidencyKey(const QByteArray &transformedKey)
{
    if (transformedKey.isEmpty()) return 0;
    const QByteArray hash = transformedKey.size() >= 8
        ? transformedKey : digest(transformedKey);
    quint64 key = 0;
    std::memcpy(&key, hash.constData(), std::min<int>(8, hash.size()));
    return key == 0 ? 1 : key;
}

} // namespace vfx
