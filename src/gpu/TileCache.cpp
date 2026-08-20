#include "gpu/TileCache.h"

#include <QMutexLocker>

#include <algorithm>
#include <tuple>

namespace vfx {

size_t qHash(const TileAddress &address, const size_t seed) noexcept
{
    size_t value = qHash(address.surfaceId, seed);
    value = qHashMulti(value,
                       address.x,
                       address.y,
                       address.level,
                       static_cast<quint8>(address.domain),
                       address.documentSessionId);
    return value;
}

TileCache::TileCache()
    : TileCache(Budgets{})
{
}

TileCache::TileCache(Budgets budgets)
    : m_budgets(budgets)
{
    setBudgets(budgets);
}

void TileCache::setBudgets(Budgets budgets)
{
    budgets.ramBytes = std::max<qsizetype>(0, budgets.ramBytes);
    budgets.vramBytes = std::max<qsizetype>(0, budgets.vramBytes);
    QMutexLocker lock(&m_mutex);
    m_budgets = budgets;
    evictLocked();
}

TileCache::Budgets TileCache::budgets() const
{
    QMutexLocker lock(&m_mutex);
    return m_budgets;
}

void TileCache::beginUpdate(const TileAddress &address, const quint64 revision)
{
    QMutexLocker lock(&m_mutex);
    Record &record = m_records[address];
    if (!record.dirty
        && record.synchronizedRevision != 0
        && record.synchronizedRevision == record.publishedRevision
        && !record.image.isNull()) {
        record.rollbackImage = record.image;
        record.rollbackRevision = record.synchronizedRevision;
        record.rollbackGpuResident = record.gpuResident;
        record.rollbackVramBytes = record.vramBytes;
    }
    record.requestedRevision = revision;
    record.dirty = true;
    record.lastUseSerial = ++m_useSerial;
}

bool TileCache::publish(const TileAddress &address,
                        const quint64 revision,
                        const QImage &image,
                        const bool gpuResident,
                        qsizetype vramBytes)
{
    if (image.isNull()) {
        return false;
    }

    QMutexLocker lock(&m_mutex);
    auto iterator = m_records.find(address);
    if (iterator == m_records.end() || iterator->requestedRevision != revision) {
        ++m_stats.rejectedStalePublications;
        return false;
    }

    Record &record = iterator.value();
    m_stats.ramBytes -= record.ramBytes;
    m_stats.vramBytes -= record.vramBytes;

    record.image = image;
    record.publishedRevision = revision;
    record.synchronizedRevision = 0;
    record.ramBytes = imageBytes(image);
    record.gpuResident = gpuResident;
    if (vramBytes <= 0 && gpuResident) {
        vramBytes = record.ramBytes;
    }
    record.vramBytes = gpuResident ? std::max<qsizetype>(0, vramBytes) : 0;
    record.lastUseSerial = ++m_useSerial;
    record.dirty = true;

    m_stats.ramBytes += record.ramBytes;
    m_stats.vramBytes += record.vramBytes;
    evictLocked();
    return true;
}

bool TileCache::markSynchronized(const TileAddress &address, const quint64 revision)
{
    QMutexLocker lock(&m_mutex);
    auto iterator = m_records.find(address);
    if (iterator == m_records.end()) {
        return false;
    }
    Record &record = iterator.value();
    if (record.requestedRevision != revision || record.publishedRevision != revision) {
        return false;
    }
    record.synchronizedRevision = revision;
    record.dirty = false;
    record.rollbackImage = {};
    record.rollbackRevision = 0;
    record.rollbackGpuResident = false;
    record.rollbackVramBytes = 0;
    record.lastUseSerial = ++m_useSerial;
    evictLocked();
    return true;
}

bool TileCache::cancelUpdate(const TileAddress &address, const quint64 revision)
{
    QMutexLocker lock(&m_mutex);
    auto iterator = m_records.find(address);
    if (iterator == m_records.end() || iterator.value().requestedRevision != revision) {
        return false;
    }

    Record &record = iterator.value();
    if (record.rollbackRevision != 0 && !record.rollbackImage.isNull()) {
        m_stats.ramBytes -= record.ramBytes;
        m_stats.vramBytes -= record.vramBytes;
        record.image = record.rollbackImage;
        record.requestedRevision = record.rollbackRevision;
        record.publishedRevision = record.rollbackRevision;
        record.synchronizedRevision = record.rollbackRevision;
        record.ramBytes = imageBytes(record.image);
        record.gpuResident = record.rollbackGpuResident;
        record.vramBytes = record.rollbackVramBytes;
        record.rollbackImage = {};
        record.rollbackRevision = 0;
        record.rollbackGpuResident = false;
        record.rollbackVramBytes = 0;
        record.dirty = false;
        record.lastUseSerial = ++m_useSerial;
        m_stats.ramBytes += record.ramBytes;
        m_stats.vramBytes += record.vramBytes;
        evictLocked();
        return true;
    }

    if (record.synchronizedRevision == record.publishedRevision
        && record.publishedRevision != 0
        && !record.image.isNull()) {
        record.requestedRevision = record.publishedRevision;
        record.dirty = false;
        record.lastUseSerial = ++m_useSerial;
        evictLocked();
        return true;
    }

    removeLocked(address);
    return true;
}

std::optional<TileSnapshot> TileCache::lookup(const TileAddress &address,
                                              const quint64 revision)
{
    QMutexLocker lock(&m_mutex);
    auto iterator = m_records.find(address);
    if (iterator == m_records.end()
        || iterator->dirty
        || iterator->synchronizedRevision != revision
        || iterator->image.isNull()) {
        ++m_stats.misses;
        return std::nullopt;
    }

    Record &record = iterator.value();
    record.lastUseSerial = ++m_useSerial;
    ++m_stats.hits;
    return TileSnapshot{record.image, record.synchronizedRevision, record.gpuResident};
}

void TileCache::invalidateSurface(const QUuid &surfaceId)
{
    invalidateSurface(QUuid(), surfaceId);
}

void TileCache::invalidateSurface(const QUuid &documentSessionId,
                                  const QUuid &surfaceId)
{
    QMutexLocker lock(&m_mutex);
    QVector<TileAddress> matches;
    matches.reserve(m_records.size());
    for (auto iterator = m_records.cbegin(); iterator != m_records.cend(); ++iterator) {
        const TileAddress &address = iterator.key();
        if (address.documentSessionId == documentSessionId
            && address.surfaceId == surfaceId
            && !iterator.value().dirty) {
            matches.push_back(address);
        }
    }
    std::sort(matches.begin(), matches.end(), [](const TileAddress &left,
                                                  const TileAddress &right) {
        return stableAddressText(left) < stableAddressText(right);
    });
    for (const TileAddress &address : matches) {
        removeLocked(address);
    }
}

void TileCache::invalidateSession(const QUuid &documentSessionId)
{
    QMutexLocker lock(&m_mutex);
    QVector<TileAddress> matches;
    matches.reserve(m_records.size());
    for (auto iterator = m_records.cbegin(); iterator != m_records.cend(); ++iterator) {
        if (iterator.key().documentSessionId == documentSessionId) {
            matches.push_back(iterator.key());
        }
    }
    std::sort(matches.begin(), matches.end(), [](const TileAddress &left,
                                                  const TileAddress &right) {
        return stableAddressText(left) < stableAddressText(right);
    });
    for (const TileAddress &address : matches) {
        removeLocked(address);
    }
}

void TileCache::clear()
{
    QMutexLocker lock(&m_mutex);
    m_records.clear();
    m_stats.ramBytes = 0;
    m_stats.vramBytes = 0;
    m_stats.residentTiles = 0;
    m_stats.dirtyTiles = 0;
}

TileCache::Stats TileCache::stats() const
{
    QMutexLocker lock(&m_mutex);
    Stats result = m_stats;
    result.residentTiles = m_records.size();
    result.dirtyTiles = 0;
    for (auto iterator = m_records.cbegin(); iterator != m_records.cend(); ++iterator) {
        if (iterator.value().dirty) {
            ++result.dirtyTiles;
        }
    }
    return result;
}

TileCache::Stats TileCache::statsForSession(const QUuid &documentSessionId) const
{
    QMutexLocker lock(&m_mutex);
    Stats result;
    // Hit/miss and eviction counters are intentionally process-wide. This
    // method reports only the resident working set belonging to one session.
    result.hits = m_stats.hits;
    result.misses = m_stats.misses;
    result.evictions = m_stats.evictions;
    result.rejectedStalePublications = m_stats.rejectedStalePublications;
    for (auto iterator = m_records.cbegin(); iterator != m_records.cend(); ++iterator) {
        if (iterator.key().documentSessionId != documentSessionId) {
            continue;
        }
        ++result.residentTiles;
        result.ramBytes += iterator.value().ramBytes;
        result.vramBytes += iterator.value().vramBytes;
        if (iterator.value().dirty) {
            ++result.dirtyTiles;
        }
    }
    return result;
}

qsizetype TileCache::imageBytes(const QImage &image)
{
    return image.isNull() ? 0 : image.sizeInBytes();
}

QString TileCache::stableAddressText(const TileAddress &address)
{
    return QStringLiteral("%1/%2/%3/%4/%5/%6")
        .arg(address.documentSessionId.toString(QUuid::WithoutBraces))
        .arg(address.surfaceId.toString(QUuid::WithoutBraces))
        .arg(static_cast<int>(address.domain), 2, 10, QLatin1Char('0'))
        .arg(address.level, 8, 10, QLatin1Char('0'))
        .arg(address.y, 12, 10, QLatin1Char('0'))
        .arg(address.x, 12, 10, QLatin1Char('0'));
}

void TileCache::evictLocked()
{
    struct Candidate {
        TileAddress address;
        quint64 lastUse = 0;
        QString stable;
    };

    while (m_stats.ramBytes > m_budgets.ramBytes
           || m_stats.vramBytes > m_budgets.vramBytes) {
        QVector<Candidate> candidates;
        candidates.reserve(m_records.size());
        for (auto iterator = m_records.cbegin(); iterator != m_records.cend(); ++iterator) {
            if (iterator.value().dirty) {
                continue;
            }
            candidates.push_back({iterator.key(),
                                  iterator.value().lastUseSerial,
                                  stableAddressText(iterator.key())});
        }
        if (candidates.isEmpty()) {
            break;
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate &left,
                                                            const Candidate &right) {
            return std::tie(left.lastUse, left.stable)
                < std::tie(right.lastUse, right.stable);
        });
        removeLocked(candidates.constFirst().address);
        ++m_stats.evictions;
    }
}

void TileCache::removeLocked(const TileAddress &address)
{
    auto iterator = m_records.find(address);
    if (iterator == m_records.end()) {
        return;
    }
    m_stats.ramBytes -= iterator.value().ramBytes;
    m_stats.vramBytes -= iterator.value().vramBytes;
    m_records.erase(iterator);
}

} // namespace vfx
