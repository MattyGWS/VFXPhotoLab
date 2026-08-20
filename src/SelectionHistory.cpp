#include "SelectionHistory.h"

#include <QSet>

#include <algorithm>
#include <utility>

namespace vfx {
namespace {

constexpr quint64 FnvOffset = 1469598103934665603ULL;
constexpr quint64 FnvPrime = 1099511628211ULL;

quint64 hashBytes(const QByteArray &bytes)
{
    quint64 hash = FnvOffset;
    for (const char byte : bytes) {
        hash ^= static_cast<quint8>(byte);
        hash *= FnvPrime;
    }
    return hash == 0 ? 1 : hash;
}

QByteArray actualTileBytes(const SelectionMask::Snapshot &snapshot,
                           const QPoint &tileIndex)
{
    if (snapshot.size.isEmpty() || tileIndex.x() < 0 || tileIndex.y() < 0) {
        return {};
    }
    const int tileColumns =
        (snapshot.size.width() + SelectionMask::TileSize - 1)
        / SelectionMask::TileSize;
    const int tileRows =
        (snapshot.size.height() + SelectionMask::TileSize - 1)
        / SelectionMask::TileSize;
    if (tileIndex.x() >= tileColumns || tileIndex.y() >= tileRows) {
        return {};
    }
    const QSize dimensions(
        std::min(SelectionMask::TileSize,
                 snapshot.size.width() - tileIndex.x() * SelectionMask::TileSize),
        std::min(SelectionMask::TileSize,
                 snapshot.size.height() - tileIndex.y() * SelectionMask::TileSize));
    const auto found = snapshot.tiles.constFind(SelectionMask::tileKey(tileIndex));
    if (found != snapshot.tiles.cend()) {
        return *found;
    }
    return QByteArray(dimensions.width() * dimensions.height(),
                      static_cast<char>(snapshot.active
                                            ? snapshot.implicitCoverage
                                            : 0));
}

bool uniformBytes(const QByteArray &bytes, const quint8 value)
{
    return std::all_of(bytes.cbegin(), bytes.cend(), [value](const char byte) {
        return static_cast<quint8>(byte) == value;
    });
}

QByteArray decodedPayload(const SelectionTileDelta &delta)
{
    const QByteArray raw = delta.compressed ? qUncompress(delta.payload)
                                            : delta.payload;
    return raw.size() == delta.rawByteCount ? raw : QByteArray();
}

} // namespace

bool SelectionTileDeltaSet::isEmpty() const
{
    return beforeActive == afterActive
        && beforeImplicitCoverage == afterImplicitCoverage
        && tiles.isEmpty();
}

qint64 SelectionTileDeltaSet::storedBytes() const
{
    qint64 bytes = sizeof(SelectionTileDeltaSet);
    for (const SelectionTileDelta &tile : tiles) {
        bytes += sizeof(SelectionTileDelta) + tile.payload.size();
    }
    return bytes;
}

SelectionTileDeltaSet buildSelectionTileDeltaSet(
    const SelectionMask::Snapshot &before,
    const SelectionMask::Snapshot &after,
    const QRect &affectedRect)
{
    SelectionTileDeltaSet result;
    result.selectionSize = after.size.isEmpty() ? before.size : after.size;
    result.beforeActive = before.active;
    result.afterActive = after.active;
    result.beforeImplicitCoverage = before.active ? before.implicitCoverage : 0;
    result.afterImplicitCoverage = after.active ? after.implicitCoverage : 0;

    if (result.selectionSize.isEmpty() || before.size != result.selectionSize
        || after.size != result.selectionSize) {
        return {};
    }

    QSet<quint64> keys;
    for (auto it = before.tiles.cbegin(); it != before.tiles.cend(); ++it) {
        keys.insert(it.key());
    }
    for (auto it = after.tiles.cbegin(); it != after.tiles.cend(); ++it) {
        keys.insert(it.key());
    }

    const QRect clippedAffected = affectedRect.isEmpty()
        ? QRect(QPoint(0, 0), result.selectionSize)
        : affectedRect.intersected(QRect(QPoint(0, 0), result.selectionSize));
    QVector<quint64> orderedKeys;
    orderedKeys.reserve(keys.size());
    for (const quint64 key : std::as_const(keys)) {
        orderedKeys.push_back(key);
    }
    std::sort(orderedKeys.begin(), orderedKeys.end());
    for (const quint64 key : orderedKeys) {
        const QPoint tileIndex = SelectionMask::tileIndexFromKey(key);
        const QSize dimensions(
            std::min(SelectionMask::TileSize,
                     result.selectionSize.width()
                         - tileIndex.x() * SelectionMask::TileSize),
            std::min(SelectionMask::TileSize,
                     result.selectionSize.height()
                         - tileIndex.y() * SelectionMask::TileSize));
        const QRect tileRect(tileIndex.x() * SelectionMask::TileSize,
                             tileIndex.y() * SelectionMask::TileSize,
                             dimensions.width(),
                             dimensions.height());
        if (dimensions.isEmpty() || (!affectedRect.isEmpty()
                                     && !tileRect.intersects(clippedAffected))) {
            continue;
        }

        const QByteArray beforeBytes = actualTileBytes(before, tileIndex);
        const QByteArray afterBytes = actualTileBytes(after, tileIndex);
        if (beforeBytes.size() != afterBytes.size() || beforeBytes.isEmpty()) {
            return {};
        }
        QByteArray xorBytes(beforeBytes.size(), '\0');
        bool changed = before.tiles.contains(key) != after.tiles.contains(key);
        for (qsizetype index = 0; index < beforeBytes.size(); ++index) {
            const char value = beforeBytes.at(index) ^ afterBytes.at(index);
            xorBytes[index] = value;
            changed = changed || value != 0;
        }
        if (!changed) {
            continue;
        }

        const QByteArray compressed = qCompress(xorBytes, 1);
        SelectionTileDelta delta;
        delta.tileIndex = tileIndex;
        delta.tileSize = dimensions;
        delta.rawByteCount = xorBytes.size();
        delta.beforeHash = hashBytes(beforeBytes);
        delta.afterHash = hashBytes(afterBytes);
        delta.compressed = !compressed.isEmpty() && compressed.size() < xorBytes.size();
        delta.payload = delta.compressed ? compressed : xorBytes;
        result.tiles.push_back(std::move(delta));
    }
    return result;
}

bool applySelectionTileDeltaSet(SelectionMask *selection,
                                const SelectionTileDeltaSet &deltaSet,
                                const bool targetAfter)
{
    if (!selection || selection->size() != deltaSet.selectionSize
        || deltaSet.selectionSize.isEmpty()) {
        return false;
    }
    const bool expectedActive = targetAfter ? deltaSet.beforeActive
                                            : deltaSet.afterActive;
    const quint8 expectedImplicit = targetAfter
        ? deltaSet.beforeImplicitCoverage : deltaSet.afterImplicitCoverage;
    if (selection->isActive() != expectedActive
        || (selection->isActive() ? selection->implicitCoverage() : 0)
            != expectedImplicit) {
        return false;
    }

    SelectionMask::Snapshot next = selection->snapshot();
    next.active = targetAfter ? deltaSet.afterActive : deltaSet.beforeActive;
    next.implicitCoverage = next.active
        ? (targetAfter ? deltaSet.afterImplicitCoverage
                       : deltaSet.beforeImplicitCoverage)
        : 0;

    for (const SelectionTileDelta &delta : deltaSet.tiles) {
        if (selection->tilePixelSize(delta.tileIndex) != delta.tileSize) {
            return false;
        }
        const QByteArray current = selection->actualTileBytes(delta.tileIndex);
        const quint64 expectedHash = targetAfter ? delta.beforeHash : delta.afterHash;
        const quint64 targetHash = targetAfter ? delta.afterHash : delta.beforeHash;
        if (current.size() != delta.rawByteCount || hashBytes(current) != expectedHash) {
            return false;
        }
        const QByteArray xorBytes = decodedPayload(delta);
        if (xorBytes.size() != current.size()) {
            return false;
        }
        QByteArray target = current;
        for (qsizetype index = 0; index < target.size(); ++index) {
            target[index] = target.at(index) ^ xorBytes.at(index);
        }
        if (hashBytes(target) != targetHash) {
            return false;
        }
        const quint64 key = SelectionMask::tileKey(delta.tileIndex);
        if (!next.active || uniformBytes(target, next.implicitCoverage)) {
            next.tiles.remove(key);
        } else {
            next.tiles.insert(key, target);
        }
    }

    if (!next.active) {
        next.tiles.clear();
    }
    return selection->restoreSnapshot(next, true);
}

} // namespace vfx
