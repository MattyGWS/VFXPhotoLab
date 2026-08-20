#include "SelectionMask.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QPainter>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace vfx {
namespace {

constexpr int MaximumEncodedTileBytes = 128 * 1024;

void setResult(bool *ok, const bool value)
{
    if (ok) {
        *ok = value;
    }
}

void setWarning(QString *warning, const QString &value)
{
    if (warning) {
        *warning = value;
    }
}

bool uniformBytes(const QByteArray &bytes, const quint8 value)
{
    return std::all_of(bytes.cbegin(), bytes.cend(), [value](const char byte) {
        return static_cast<quint8>(byte) == value;
    });
}

quint32 compressedDeclaredSize(const QByteArray &bytes)
{
    if (bytes.size() < 4) {
        return 0;
    }
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData()));
}

} // namespace

SelectionMask::SelectionMask(const QSize &documentSize)
{
    reset(documentSize);
}

void SelectionMask::reset(const QSize &documentSize)
{
    m_size = documentSize.isValid() ? documentSize : QSize();
    m_active = false;
    m_implicitCoverage = 0;
    m_tiles.clear();
    m_nonZeroBounds = {};
    touch();
}

const QSize &SelectionMask::size() const
{
    return m_size;
}

bool SelectionMask::isActive() const
{
    return m_active;
}

bool SelectionMask::isEmpty() const
{
    return m_active && m_nonZeroBounds.isEmpty();
}

bool SelectionMask::isFull() const
{
    return m_active && !m_size.isEmpty() && m_implicitCoverage == 255
        && m_tiles.isEmpty();
}

quint8 SelectionMask::implicitCoverage() const
{
    return m_implicitCoverage;
}

quint64 SelectionMask::revision() const
{
    return m_revision;
}

int SelectionMask::explicitTileCount() const
{
    return m_tiles.size();
}

qint64 SelectionMask::estimatedResidentBytes() const
{
    qint64 total = sizeof(SelectionMask);
    for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it) {
        total += sizeof(quint64) + sizeof(QByteArray) + it.value().capacity();
    }
    return total;
}

QRect SelectionMask::nonZeroBounds() const
{
    return m_active ? m_nonZeroBounds : QRect();
}

void SelectionMask::deactivate()
{
    if (!m_active && m_implicitCoverage == 0 && m_tiles.isEmpty()) {
        return;
    }
    m_active = false;
    m_implicitCoverage = 0;
    m_tiles.clear();
    m_nonZeroBounds = {};
    touch();
}

void SelectionMask::selectAll()
{
    if (m_active && m_implicitCoverage == 255 && m_tiles.isEmpty()) {
        return;
    }
    m_active = true;
    m_implicitCoverage = 255;
    m_tiles.clear();
    m_nonZeroBounds = QRect(QPoint(0, 0), m_size);
    touch();
}

void SelectionMask::selectNone()
{
    if (m_active && m_implicitCoverage == 0 && m_tiles.isEmpty()) {
        return;
    }
    m_active = true;
    m_implicitCoverage = 0;
    m_tiles.clear();
    m_nonZeroBounds = {};
    touch();
}

bool SelectionMask::validDocumentPosition(const int x, const int y) const
{
    return x >= 0 && y >= 0 && x < m_size.width() && y < m_size.height();
}

bool SelectionMask::validTileIndex(const QPoint &tileIndex) const
{
    if (tileIndex.x() < 0 || tileIndex.y() < 0 || m_size.isEmpty()) {
        return false;
    }
    const int tileColumns = (m_size.width() + TileSize - 1) / TileSize;
    const int tileRows = (m_size.height() + TileSize - 1) / TileSize;
    return tileIndex.x() < tileColumns && tileIndex.y() < tileRows;
}

quint8 SelectionMask::coverageAt(const int x, const int y) const
{
    if (!m_active || !validDocumentPosition(x, y)) {
        return 0;
    }
    const QPoint tileIndex(x / TileSize, y / TileSize);
    const auto found = m_tiles.constFind(tileKey(tileIndex));
    if (found == m_tiles.cend()) {
        return m_implicitCoverage;
    }
    const QSize dimensions = tilePixelSize(tileIndex);
    const int localX = x - tileIndex.x() * TileSize;
    const int localY = y - tileIndex.y() * TileSize;
    const int offset = localY * dimensions.width() + localX;
    return offset >= 0 && offset < found->size()
        ? static_cast<quint8>(found->at(offset))
        : m_implicitCoverage;
}

quint8 SelectionMask::coverageAt(const QPoint &position) const
{
    return coverageAt(position.x(), position.y());
}

QImage SelectionMask::coverageImage(const QRect &requestedRect,
                                    const QSize &requestedOutputSize) const
{
    if (m_size.isEmpty()) {
        return {};
    }
    const QRect documentBounds(QPoint(0, 0), m_size);
    const QRect documentRect = (requestedRect.isEmpty() ? documentBounds : requestedRect)
        .intersected(documentBounds);
    if (documentRect.isEmpty()) {
        return {};
    }
    const QSize outputSize = requestedOutputSize.isEmpty()
        ? documentRect.size() : requestedOutputSize;
    if (outputSize.isEmpty()) {
        return {};
    }

    QImage output(outputSize, QImage::Format_Grayscale8);
    if (output.isNull()) {
        return {};
    }
    if (!m_active) {
        output.fill(0);
        return output;
    }
    if (m_tiles.isEmpty()) {
        output.fill(m_implicitCoverage);
        return output;
    }

    const double scaleX = static_cast<double>(documentRect.width()) / outputSize.width();
    const double scaleY = static_cast<double>(documentRect.height()) / outputSize.height();
    for (int outputY = 0; outputY < output.height(); ++outputY) {
        uchar *line = output.scanLine(outputY);
        const int documentY = std::clamp(
            documentRect.y() + static_cast<int>((outputY + 0.5) * scaleY),
            documentRect.top(),
            documentRect.bottom());
        for (int outputX = 0; outputX < output.width(); ++outputX) {
            const int documentX = std::clamp(
                documentRect.x() + static_cast<int>((outputX + 0.5) * scaleX),
                documentRect.left(),
                documentRect.right());
            line[outputX] = coverageAt(documentX, documentY);
        }
    }
    return output;
}

QSize SelectionMask::tilePixelSize(const QPoint &tileIndex) const
{
    if (!validTileIndex(tileIndex)) {
        return {};
    }
    return QSize(std::min(TileSize, m_size.width() - tileIndex.x() * TileSize),
                 std::min(TileSize, m_size.height() - tileIndex.y() * TileSize));
}

QByteArray SelectionMask::actualTileBytes(const QPoint &tileIndex) const
{
    const QSize dimensions = tilePixelSize(tileIndex);
    if (dimensions.isEmpty()) {
        return {};
    }
    const auto found = m_tiles.constFind(tileKey(tileIndex));
    if (found != m_tiles.cend()) {
        return *found;
    }
    return QByteArray(dimensions.width() * dimensions.height(),
                      static_cast<char>(m_implicitCoverage));
}

QByteArray SelectionMask::canonicalTileBytes(const QPoint &tileIndex,
                                             const QByteArray &actualBytes,
                                             const quint8 implicitCoverage) const
{
    const QSize dimensions = tilePixelSize(tileIndex);
    const int expectedSize = dimensions.width() * dimensions.height();
    if (dimensions.isEmpty() || actualBytes.size() != expectedSize
        || uniformBytes(actualBytes, implicitCoverage)) {
        return {};
    }
    return actualBytes;
}

void SelectionMask::setActualTileBytes(const QPoint &tileIndex,
                                       const QByteArray &actualBytes)
{
    const QByteArray canonical = canonicalTileBytes(tileIndex,
                                                    actualBytes,
                                                    m_implicitCoverage);
    const quint64 key = tileKey(tileIndex);
    if (canonical.isEmpty()) {
        m_tiles.remove(key);
    } else {
        m_tiles.insert(key, canonical);
    }
}

bool SelectionMask::setCoverageRect(const QRect &requestedRect,
                                    const quint8 coverage)
{
    const QRect rect = requestedRect.normalized().intersected(
        QRect(QPoint(0, 0), m_size));
    if (rect.isEmpty()) {
        return false;
    }
    const bool wasActive = m_active;
    m_active = true;
    bool changed = !wasActive;
    const int firstTileX = rect.left() / TileSize;
    const int lastTileX = rect.right() / TileSize;
    const int firstTileY = rect.top() / TileSize;
    const int lastTileY = rect.bottom() / TileSize;
    for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
        for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
            const QPoint tileIndex(tileX, tileY);
            const QSize dimensions = tilePixelSize(tileIndex);
            QByteArray bytes = actualTileBytes(tileIndex);
            const QRect tileDocumentRect(tileX * TileSize,
                                         tileY * TileSize,
                                         dimensions.width(),
                                         dimensions.height());
            const QRect local = rect.intersected(tileDocumentRect)
                .translated(-tileDocumentRect.topLeft());
            for (int y = local.top(); y <= local.bottom(); ++y) {
                char *line = bytes.data() + y * dimensions.width();
                for (int x = local.left(); x <= local.right(); ++x) {
                    if (static_cast<quint8>(line[x]) != coverage) {
                        line[x] = static_cast<char>(coverage);
                        changed = true;
                    }
                }
            }
            setActualTileBytes(tileIndex, bytes);
        }
    }
    if (changed) {
        recalculateBounds();
        touch();
    }
    return changed;
}

bool SelectionMask::setCoverageImage(const QRect &requestedRect,
                                     const QImage &coverageImage)
{
    const QRect sourceDocumentRect = requestedRect.normalized();
    const QRect rect = sourceDocumentRect.intersected(QRect(QPoint(0, 0), m_size));
    if (sourceDocumentRect.isEmpty() || rect.isEmpty() || coverageImage.isNull()) {
        return false;
    }
    QImage prepared = coverageImage.convertToFormat(QImage::Format_Grayscale8);
    if (prepared.size() != sourceDocumentRect.size()) {
        prepared = prepared.scaled(sourceDocumentRect.size(),
                                   Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
    }
    if (prepared.isNull()) {
        return false;
    }

    const bool wasActive = m_active;
    m_active = true;
    bool changed = !wasActive;
    const int firstTileX = rect.left() / TileSize;
    const int lastTileX = rect.right() / TileSize;
    const int firstTileY = rect.top() / TileSize;
    const int lastTileY = rect.bottom() / TileSize;
    for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
        for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
            const QPoint tileIndex(tileX, tileY);
            const QSize dimensions = tilePixelSize(tileIndex);
            QByteArray bytes = actualTileBytes(tileIndex);
            const QRect tileDocumentRect(tileX * TileSize,
                                         tileY * TileSize,
                                         dimensions.width(),
                                         dimensions.height());
            const QRect intersection = rect.intersected(tileDocumentRect);
            for (int documentY = intersection.top(); documentY <= intersection.bottom(); ++documentY) {
                const uchar *source = prepared.constScanLine(
                    documentY - sourceDocumentRect.y());
                char *destination = bytes.data()
                    + (documentY - tileDocumentRect.y()) * dimensions.width();
                for (int documentX = intersection.left(); documentX <= intersection.right(); ++documentX) {
                    const quint8 value = source[documentX - sourceDocumentRect.x()];
                    const int localX = documentX - tileDocumentRect.x();
                    if (static_cast<quint8>(destination[localX]) != value) {
                        destination[localX] = static_cast<char>(value);
                        changed = true;
                    }
                }
            }
            setActualTileBytes(tileIndex, bytes);
        }
    }
    if (changed) {
        recalculateBounds();
        touch();
    }
    return changed;
}

bool SelectionMask::combineShape(const QRectF &requestedBounds,
                                 const SelectionShape shape,
                                 const SelectionCombineMode mode,
                                 const bool antialias)
{
    QPainterPath path;
    if (shape == SelectionShape::Ellipse) {
        path.addEllipse(requestedBounds.normalized());
    } else {
        path.addRect(requestedBounds.normalized());
    }
    return combinePath(path, mode, antialias);
}

bool SelectionMask::combinePath(const QPainterPath &documentPath,
                                const SelectionCombineMode mode,
                                const bool antialias)
{
    if (m_size.isEmpty()) {
        return false;
    }

    const Snapshot before = snapshot();
    const QRectF documentBounds(QPointF(0.0, 0.0), QSizeF(m_size));
    const QRectF clippedPathBounds = documentPath.boundingRect()
        .intersected(documentBounds);

    const bool preserveOutside = mode == SelectionCombineMode::Add
        || mode == SelectionCombineMode::Subtract;
    m_active = true;
    if (preserveOutside && before.active) {
        m_implicitCoverage = before.implicitCoverage;
        m_tiles = before.tiles;
    } else {
        m_implicitCoverage = 0;
        m_tiles.clear();
    }

    auto bytesFromSnapshot = [](const Snapshot &snapshot,
                                const QPoint &tileIndex,
                                const QSize &dimensions) {
        if (!snapshot.active) {
            return QByteArray(dimensions.width() * dimensions.height(), '\0');
        }
        const auto found = snapshot.tiles.constFind(tileKey(tileIndex));
        if (found != snapshot.tiles.cend()) {
            return *found;
        }
        return QByteArray(dimensions.width() * dimensions.height(),
                          static_cast<char>(snapshot.implicitCoverage));
    };

    QRect pixelBounds;
    if (!documentPath.isEmpty() && !clippedPathBounds.isEmpty()
        && clippedPathBounds.width() > 0.0 && clippedPathBounds.height() > 0.0) {
        pixelBounds = clippedPathBounds.toAlignedRect().adjusted(-1, -1, 1, 1)
            .intersected(QRect(QPoint(0, 0), m_size));
    }

    if (!pixelBounds.isEmpty()) {
        const int firstTileX = pixelBounds.left() / TileSize;
        const int lastTileX = pixelBounds.right() / TileSize;
        const int firstTileY = pixelBounds.top() / TileSize;
        const int lastTileY = pixelBounds.bottom() / TileSize;
        for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
            for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
                const QPoint tileIndex(tileX, tileY);
                const QSize dimensions = tilePixelSize(tileIndex);
                if (dimensions.isEmpty()) {
                    continue;
                }

                QImage shapeCoverage(dimensions, QImage::Format_ARGB32_Premultiplied);
                shapeCoverage.fill(Qt::transparent);
                {
                    QPainter painter(&shapeCoverage);
                    painter.setRenderHint(QPainter::Antialiasing, antialias);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(Qt::white);
                    painter.translate(-tileX * TileSize, -tileY * TileSize);
                    painter.drawPath(documentPath);
                }

                const QByteArray oldBytes = bytesFromSnapshot(before, tileIndex, dimensions);
                QByteArray combined(dimensions.width() * dimensions.height(), '\0');
                for (int y = 0; y < dimensions.height(); ++y) {
                    const QRgb *shapeLine = reinterpret_cast<const QRgb *>(
                        shapeCoverage.constScanLine(y));
                    char *destination = combined.data() + y * dimensions.width();
                    const char *oldLine = oldBytes.constData() + y * dimensions.width();
                    for (int x = 0; x < dimensions.width(); ++x) {
                        const quint8 oldCoverage = static_cast<quint8>(oldLine[x]);
                        const quint8 shapeValue = static_cast<quint8>(qAlpha(shapeLine[x]));
                        quint8 result = 0;
                        switch (mode) {
                        case SelectionCombineMode::Replace:
                            result = shapeValue;
                            break;
                        case SelectionCombineMode::Add:
                            result = std::max(oldCoverage, shapeValue);
                            break;
                        case SelectionCombineMode::Subtract:
                            result = std::min<quint8>(oldCoverage,
                                                      static_cast<quint8>(255 - shapeValue));
                            break;
                        case SelectionCombineMode::Intersect:
                            result = std::min(oldCoverage, shapeValue);
                            break;
                        }
                        destination[x] = static_cast<char>(result);
                    }
                }
                setActualTileBytes(tileIndex, combined);
            }
        }
    }

    normaliseStorage();
    const bool changed = before.active != m_active
        || before.implicitCoverage != m_implicitCoverage
        || before.tiles != m_tiles;
    if (!changed) {
        restoreSnapshot(before, false);
        return false;
    }
    touch();
    return true;
}

bool SelectionMask::combineCoverageImage(const QRect &requestedRect,
                                         const QImage &coverageImage,
                                         const SelectionCombineMode mode)
{
    if (m_size.isEmpty() || requestedRect.isEmpty() || coverageImage.isNull()) {
        return false;
    }

    const Snapshot before = snapshot();
    const QRect sourceRect = requestedRect.normalized();
    const QRect documentRect = sourceRect.intersected(QRect(QPoint(0, 0), m_size));
    QImage prepared = coverageImage.convertToFormat(QImage::Format_Grayscale8);
    if (prepared.size() != sourceRect.size()) {
        prepared = prepared.scaled(sourceRect.size(),
                                   Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
    }
    if (prepared.isNull()) {
        return false;
    }

    const bool preserveOutside = mode == SelectionCombineMode::Add
        || mode == SelectionCombineMode::Subtract;
    m_active = true;
    if (preserveOutside && before.active) {
        m_implicitCoverage = before.implicitCoverage;
        m_tiles = before.tiles;
    } else {
        m_implicitCoverage = 0;
        m_tiles.clear();
    }

    if (!documentRect.isEmpty()) {
        const int firstTileX = documentRect.left() / TileSize;
        const int lastTileX = documentRect.right() / TileSize;
        const int firstTileY = documentRect.top() / TileSize;
        const int lastTileY = documentRect.bottom() / TileSize;
        for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
            for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
                const QPoint tileIndex(tileX, tileY);
                const QSize dimensions = tilePixelSize(tileIndex);
                if (dimensions.isEmpty()) {
                    continue;
                }
                const QRect tileRect(tileX * TileSize,
                                     tileY * TileSize,
                                     dimensions.width(),
                                     dimensions.height());
                const QRect intersection = documentRect.intersected(tileRect);
                QByteArray oldBytes;
                if (before.active) {
                    const auto found = before.tiles.constFind(tileKey(tileIndex));
                    oldBytes = found != before.tiles.cend()
                        ? *found
                        : QByteArray(dimensions.width() * dimensions.height(),
                                     static_cast<char>(before.implicitCoverage));
                } else {
                    oldBytes = QByteArray(dimensions.width() * dimensions.height(), '\0');
                }
                QByteArray combined = preserveOutside
                    ? actualTileBytes(tileIndex)
                    : QByteArray(dimensions.width() * dimensions.height(), '\0');

                for (int y = intersection.top(); y <= intersection.bottom(); ++y) {
                    const uchar *source = prepared.constScanLine(y - sourceRect.y());
                    const char *oldLine = oldBytes.constData()
                        + (y - tileRect.y()) * dimensions.width();
                    char *destination = combined.data()
                        + (y - tileRect.y()) * dimensions.width();
                    for (int x = intersection.left(); x <= intersection.right(); ++x) {
                        const quint8 oldCoverage = static_cast<quint8>(
                            oldLine[x - tileRect.x()]);
                        const quint8 sourceCoverage = source[x - sourceRect.x()];
                        quint8 result = 0;
                        switch (mode) {
                        case SelectionCombineMode::Replace:
                            result = sourceCoverage;
                            break;
                        case SelectionCombineMode::Add:
                            result = std::max(oldCoverage, sourceCoverage);
                            break;
                        case SelectionCombineMode::Subtract:
                            result = std::min<quint8>(
                                oldCoverage,
                                static_cast<quint8>(255 - sourceCoverage));
                            break;
                        case SelectionCombineMode::Intersect:
                            result = std::min(oldCoverage, sourceCoverage);
                            break;
                        }
                        destination[x - tileRect.x()] = static_cast<char>(result);
                    }
                }
                setActualTileBytes(tileIndex, combined);
            }
        }
    }

    normaliseStorage();
    const bool changed = before.active != m_active
        || before.implicitCoverage != m_implicitCoverage
        || before.tiles != m_tiles;
    if (!changed) {
        restoreSnapshot(before, false);
        return false;
    }
    touch();
    return true;
}

SelectionMask::Snapshot SelectionMask::snapshot() const
{
    Snapshot result;
    result.size = m_size;
    result.active = m_active;
    result.implicitCoverage = m_implicitCoverage;
    result.tiles = m_tiles;
    result.nonZeroBounds = m_nonZeroBounds;
    result.revision = m_revision;
    return result;
}

bool SelectionMask::restoreSnapshot(const Snapshot &snapshot,
                                    const bool advanceRevision)
{
    if (snapshot.size != m_size && !m_size.isEmpty()) {
        return false;
    }
    if (snapshot.size.isEmpty()) {
        return false;
    }
    const int tileColumns = (snapshot.size.width() + TileSize - 1) / TileSize;
    const int tileRows = (snapshot.size.height() + TileSize - 1) / TileSize;
    for (auto it = snapshot.tiles.cbegin(); it != snapshot.tiles.cend(); ++it) {
        const QPoint tileIndex = tileIndexFromKey(it.key());
        if (tileIndex.x() < 0 || tileIndex.y() < 0
            || tileIndex.x() >= tileColumns || tileIndex.y() >= tileRows) {
            return false;
        }
        const QSize dimensions(std::min(TileSize,
                                        snapshot.size.width() - tileIndex.x() * TileSize),
                               std::min(TileSize,
                                        snapshot.size.height() - tileIndex.y() * TileSize));
        if (dimensions.isEmpty()
            || it.value().size() != dimensions.width() * dimensions.height()) {
            return false;
        }
    }
    const quint64 oldRevision = m_revision;
    m_size = snapshot.size;
    m_active = snapshot.active;
    m_implicitCoverage = snapshot.active ? snapshot.implicitCoverage : 0;
    m_tiles = snapshot.active ? snapshot.tiles : QHash<quint64, QByteArray>();
    recalculateBounds();
    m_revision = advanceRevision ? std::max<quint64>(oldRevision + 1, 1)
                                 : std::max<quint64>(snapshot.revision, 1);
    return true;
}

QVector<QPoint> SelectionMask::explicitTileIndices() const
{
    QVector<QPoint> result;
    result.reserve(m_tiles.size());
    for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it) {
        result.push_back(tileIndexFromKey(it.key()));
    }
    std::sort(result.begin(), result.end(), [](const QPoint &left, const QPoint &right) {
        return left.y() == right.y() ? left.x() < right.x() : left.y() < right.y();
    });
    return result;
}

void SelectionMask::recalculateBounds()
{
    if (!m_active || m_size.isEmpty()) {
        m_nonZeroBounds = {};
        return;
    }
    QRect bounds;
    const int tileColumns = (m_size.width() + TileSize - 1) / TileSize;
    const int tileRows = (m_size.height() + TileSize - 1) / TileSize;
    for (int tileY = 0; tileY < tileRows; ++tileY) {
        for (int tileX = 0; tileX < tileColumns; ++tileX) {
            const QPoint tileIndex(tileX, tileY);
            const QSize dimensions = tilePixelSize(tileIndex);
            const auto found = m_tiles.constFind(tileKey(tileIndex));
            if (found == m_tiles.cend()) {
                if (m_implicitCoverage > 0) {
                    const QRect tileBounds(tileX * TileSize,
                                           tileY * TileSize,
                                           dimensions.width(),
                                           dimensions.height());
                    bounds = bounds.isNull() ? tileBounds : bounds.united(tileBounds);
                }
                continue;
            }

            const QByteArray &bytes = *found;
            int minX = dimensions.width();
            int minY = dimensions.height();
            int maxX = -1;
            int maxY = -1;
            for (int y = 0; y < dimensions.height(); ++y) {
                const uchar *line = reinterpret_cast<const uchar *>(bytes.constData())
                    + y * dimensions.width();
                for (int x = 0; x < dimensions.width(); ++x) {
                    if (line[x] > 0) {
                        minX = std::min(minX, x);
                        minY = std::min(minY, y);
                        maxX = std::max(maxX, x);
                        maxY = std::max(maxY, y);
                    }
                }
            }
            if (maxX >= minX && maxY >= minY) {
                const QRect tileBounds(tileX * TileSize + minX,
                                       tileY * TileSize + minY,
                                       maxX - minX + 1,
                                       maxY - minY + 1);
                bounds = bounds.isNull() ? tileBounds : bounds.united(tileBounds);
            }
        }
    }
    m_nonZeroBounds = bounds;
}

void SelectionMask::normaliseStorage()
{
    recalculateBounds();
    if (!m_active || m_nonZeroBounds.isEmpty()) {
        // Preserve the semantic distinction between an active empty selection
        // and no selection, while keeping the empty state allocation-free.
        m_implicitCoverage = 0;
        m_tiles.clear();
        m_nonZeroBounds = {};
        return;
    }

    bool completelySelected = true;
    const int tileColumns = (m_size.width() + TileSize - 1) / TileSize;
    const int tileRows = (m_size.height() + TileSize - 1) / TileSize;
    for (int tileY = 0; tileY < tileRows && completelySelected; ++tileY) {
        for (int tileX = 0; tileX < tileColumns; ++tileX) {
            const QPoint tileIndex(tileX, tileY);
            const auto found = m_tiles.constFind(tileKey(tileIndex));
            if (found == m_tiles.cend()) {
                if (m_implicitCoverage != 255) {
                    completelySelected = false;
                    break;
                }
            } else if (!uniformBytes(*found, 255)) {
                completelySelected = false;
                break;
            }
        }
    }
    if (completelySelected) {
        m_implicitCoverage = 255;
        m_tiles.clear();
        m_nonZeroBounds = QRect(QPoint(0, 0), m_size);
    }
}

void SelectionMask::touch()
{
    ++m_revision;
    if (m_revision == 0) {
        m_revision = 1;
    }
}

quint64 SelectionMask::tileKey(const QPoint &tileIndex)
{
    return (static_cast<quint64>(static_cast<quint32>(tileIndex.y())) << 32)
        | static_cast<quint32>(tileIndex.x());
}

QPoint SelectionMask::tileIndexFromKey(const quint64 key)
{
    return QPoint(static_cast<qint32>(key & 0xffffffffULL),
                  static_cast<qint32>((key >> 32) & 0xffffffffULL));
}

QJsonObject SelectionMask::toJson(bool *ok) const
{
    setResult(ok, false);
    QJsonObject object;
    object.insert(QStringLiteral("active"), m_active);
    object.insert(QStringLiteral("implicitCoverage"), static_cast<int>(m_implicitCoverage));
    object.insert(QStringLiteral("tileSize"), TileSize);
    object.insert(QStringLiteral("revision"), QString::number(m_revision));

    QJsonArray tileArray;
    const QVector<QPoint> indices = explicitTileIndices();
    for (const QPoint &tileIndex : indices) {
        const QByteArray raw = m_tiles.value(tileKey(tileIndex));
        const QSize dimensions = tilePixelSize(tileIndex);
        if (raw.size() != dimensions.width() * dimensions.height()) {
            return {};
        }
        const QByteArray compressed = qCompress(raw, 1);
        const bool useCompression = !compressed.isEmpty() && compressed.size() < raw.size();
        const QByteArray payload = useCompression ? compressed : raw;
        QJsonObject tileObject;
        tileObject.insert(QStringLiteral("x"), tileIndex.x());
        tileObject.insert(QStringLiteral("y"), tileIndex.y());
        tileObject.insert(QStringLiteral("width"), dimensions.width());
        tileObject.insert(QStringLiteral("height"), dimensions.height());
        tileObject.insert(QStringLiteral("compressed"), useCompression);
        tileObject.insert(QStringLiteral("sha256"), QString::fromLatin1(
            QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex()));
        tileObject.insert(QStringLiteral("data"), QString::fromLatin1(payload.toBase64()));
        tileArray.append(tileObject);
    }
    object.insert(QStringLiteral("tiles"), tileArray);
    setResult(ok, true);
    return object;
}

SelectionMask SelectionMask::fromJson(const QJsonObject &object,
                                      const QSize &documentSize,
                                      bool *ok,
                                      QString *warning)
{
    setResult(ok, false);
    setWarning(warning, {});
    SelectionMask result(documentSize);
    if (object.isEmpty()) {
        setResult(ok, true);
        return result;
    }
    const QJsonValue activeValue = object.value(QStringLiteral("active"));
    const QJsonValue implicitCoverageValue =
        object.value(QStringLiteral("implicitCoverage"));
    const QJsonValue tileSizeValue = object.value(QStringLiteral("tileSize"));
    const QJsonValue tilesValue = object.value(QStringLiteral("tiles"));
    if (documentSize.isEmpty() || !activeValue.isBool()
        || !implicitCoverageValue.isDouble() || !tileSizeValue.isDouble()
        || tileSizeValue.toInt(-1) != TileSize || !tilesValue.isArray()) {
        setWarning(warning, QStringLiteral("The saved selection used invalid dimensions or tile settings and was discarded."));
        return result;
    }

    const bool active = activeValue.toBool(false);
    const int implicitValue = implicitCoverageValue.toInt(-1);
    if (implicitValue < 0 || implicitValue > 255) {
        setWarning(warning, QStringLiteral("The saved selection coverage was invalid and was discarded."));
        return result;
    }

    Snapshot snapshot;
    snapshot.size = documentSize;
    snapshot.active = active;
    snapshot.implicitCoverage = active ? static_cast<quint8>(implicitValue) : 0;
    bool revisionOk = false;
    snapshot.revision = object.value(QStringLiteral("revision")).toString().toULongLong(&revisionOk);
    if (!revisionOk || snapshot.revision == 0) {
        snapshot.revision = 1;
    }

    const qint64 maximumTilesX =
        (static_cast<qint64>(documentSize.width()) + TileSize - 1) / TileSize;
    const qint64 maximumTilesY =
        (static_cast<qint64>(documentSize.height()) + TileSize - 1) / TileSize;
    const qint64 maximumTileCount = maximumTilesX * maximumTilesY;
    const QJsonArray tileArray = tilesValue.toArray();
    if ((!active && (implicitValue != 0 || !tileArray.isEmpty()))
        || static_cast<qint64>(tileArray.size()) > maximumTileCount) {
        setWarning(warning, QStringLiteral("The saved selection contained inconsistent or excessive tile data and was discarded."));
        return result;
    }

    for (const QJsonValue &value : tileArray) {
        if (!value.isObject()) {
            setWarning(warning, QStringLiteral("The saved selection contained a damaged tile and was discarded."));
            return result;
        }
        const QJsonObject tileObject = value.toObject();
        const QPoint tileIndex(tileObject.value(QStringLiteral("x")).toInt(-1),
                               tileObject.value(QStringLiteral("y")).toInt(-1));
        if (tileIndex.x() < 0 || tileIndex.y() < 0
            || tileIndex.x() >= maximumTilesX || tileIndex.y() >= maximumTilesY) {
            setWarning(warning, QStringLiteral("The saved selection contained an out-of-range tile and was discarded."));
            return result;
        }
        const QSize expectedDimensions(
            std::min(TileSize, documentSize.width() - tileIndex.x() * TileSize),
            std::min(TileSize, documentSize.height() - tileIndex.y() * TileSize));
        if (tileObject.value(QStringLiteral("width")).toInt(-1) != expectedDimensions.width()
            || tileObject.value(QStringLiteral("height")).toInt(-1) != expectedDimensions.height()) {
            setWarning(warning, QStringLiteral("The saved selection contained an incorrectly sized tile and was discarded."));
            return result;
        }
        const QByteArray encoded = tileObject.value(QStringLiteral("data")).toString().toLatin1();
        if (encoded.size() > MaximumEncodedTileBytes) {
            setWarning(warning, QStringLiteral("The saved selection tile payload was too large and was discarded."));
            return result;
        }
        const QByteArray payload = QByteArray::fromBase64(encoded);
        const int expectedRawSize = expectedDimensions.width() * expectedDimensions.height();
        const bool compressed = tileObject.value(QStringLiteral("compressed")).toBool(false);
        if (compressed && compressedDeclaredSize(payload) != static_cast<quint32>(expectedRawSize)) {
            setWarning(warning, QStringLiteral("The saved selection tile header was damaged and was discarded."));
            return result;
        }
        const QByteArray raw = compressed ? qUncompress(payload) : payload;
        const QByteArray expectedDigest = QByteArray::fromHex(
            tileObject.value(QStringLiteral("sha256")).toString().toLatin1());
        if (raw.size() != expectedRawSize || expectedDigest.size() != 32
            || QCryptographicHash::hash(raw, QCryptographicHash::Sha256) != expectedDigest) {
            setWarning(warning, QStringLiteral("The saved selection tile failed its integrity check and was discarded."));
            return result;
        }
        const quint64 key = tileKey(tileIndex);
        if (snapshot.tiles.contains(key)) {
            setWarning(warning, QStringLiteral("The saved selection contained duplicate tiles and was discarded."));
            return result;
        }
        if (!uniformBytes(raw, snapshot.implicitCoverage)) {
            snapshot.tiles.insert(key, raw);
        }
    }

    if (!result.restoreSnapshot(snapshot, false)) {
        setWarning(warning, QStringLiteral("The saved selection was inconsistent and was discarded."));
        return SelectionMask(documentSize);
    }
    setResult(ok, true);
    return result;
}

} // namespace vfx
