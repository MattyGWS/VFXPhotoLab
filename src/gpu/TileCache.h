#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QString>
#include <QUuid>
#include <QVector>
#include <QtGlobal>

#include <optional>

namespace vfx {

enum class TileDomain : quint8 {
    Raster = 0,
    Composite = 1,
    Mask = 2,
    Source = 3
};

struct TileAddress {
    QUuid surfaceId;
    int x = 0;
    int y = 0;
    int level = 0;
    TileDomain domain = TileDomain::Raster;
    // A layer UUID is unique inside a document, not an application-wide cache
    // namespace. Keep the owning document session in every tile address so two
    // simultaneously open projects can never alias CPU or native GPU tiles.
    QUuid documentSessionId;

    bool operator==(const TileAddress &other) const = default;
};

size_t qHash(const TileAddress &address, size_t seed = 0) noexcept;

struct TileSnapshot {
    QImage image;
    quint64 revision = 0;
    bool gpuResident = false;
};

class TileCache final {
public:
    struct Budgets {
        qsizetype ramBytes = qsizetype(256) * 1024 * 1024;
        qsizetype vramBytes = qsizetype(512) * 1024 * 1024;
    };

    struct Stats {
        qsizetype ramBytes = 0;
        qsizetype vramBytes = 0;
        int residentTiles = 0;
        int dirtyTiles = 0;
        quint64 hits = 0;
        quint64 misses = 0;
        quint64 evictions = 0;
        quint64 rejectedStalePublications = 0;
    };

    TileCache();
    explicit TileCache(Budgets budgets);

    void setBudgets(Budgets budgets);
    Budgets budgets() const;

    void beginUpdate(const TileAddress &address, quint64 revision);
    bool publish(const TileAddress &address,
                 quint64 revision,
                 const QImage &image,
                 bool gpuResident,
                 qsizetype vramBytes = 0);
    bool markSynchronized(const TileAddress &address, quint64 revision);
    bool cancelUpdate(const TileAddress &address, quint64 revision);

    std::optional<TileSnapshot> lookup(const TileAddress &address, quint64 revision);

    void invalidateSurface(const QUuid &surfaceId);
    void invalidateSurface(const QUuid &documentSessionId, const QUuid &surfaceId);
    void invalidateSession(const QUuid &documentSessionId);
    void clear();
    Stats stats() const;
    Stats statsForSession(const QUuid &documentSessionId) const;

private:
    struct Record {
        QImage image;
        QImage rollbackImage;
        quint64 requestedRevision = 0;
        quint64 publishedRevision = 0;
        quint64 synchronizedRevision = 0;
        quint64 rollbackRevision = 0;
        quint64 lastUseSerial = 0;
        qsizetype ramBytes = 0;
        qsizetype vramBytes = 0;
        qsizetype rollbackVramBytes = 0;
        bool dirty = false;
        bool gpuResident = false;
        bool rollbackGpuResident = false;
    };

    static qsizetype imageBytes(const QImage &image);
    static QString stableAddressText(const TileAddress &address);
    void evictLocked();
    void removeLocked(const TileAddress &address);

    mutable QMutex m_mutex;
    QHash<TileAddress, Record> m_records;
    Budgets m_budgets;
    Stats m_stats;
    quint64 m_useSerial = 0;
};

} // namespace vfx
