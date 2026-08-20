#include "RasterHistory.h"

#include <QtGlobal>
#include <QtEndian>
#include <QSet>

#include <algorithm>
#include <utility>

namespace vfx {
namespace {

constexpr quint64 FnvOffset = 1469598103934665603ULL;
constexpr quint64 FnvPrime = 1099511628211ULL;

bool validImageByteLayout(const QImage &image,
                          const QSize &expectedSize,
                          const int bytesPerPixel)
{
    if (image.isNull() || image.size() != expectedSize
        || expectedSize.isEmpty() || bytesPerPixel <= 0) {
        return false;
    }
    const qsizetype minimumRowBytes =
        static_cast<qsizetype>(expectedSize.width()) * bytesPerPixel;
    return minimumRowBytes > 0 && image.bytesPerLine() >= minimumRowBytes;
}

bool validRectByteLayout(const QImage &image,
                         const QRect &rect,
                         const int bytesPerPixel)
{
    if (!validImageByteLayout(image, image.size(), bytesPerPixel)
        || rect.isEmpty() || !image.rect().contains(rect)) {
        return false;
    }
    const qsizetype offset = static_cast<qsizetype>(rect.x()) * bytesPerPixel;
    const qsizetype rowBytes =
        static_cast<qsizetype>(rect.width()) * bytesPerPixel;
    return offset >= 0 && rowBytes > 0
        && offset + rowBytes <= image.bytesPerLine();
}

quint64 hashTile(const QImage &image, const QRect &rect, const int bytesPerPixel)
{
    if (!validRectByteLayout(image, rect, bytesPerPixel)) {
        return 0;
    }
    quint64 hash = FnvOffset;
    const int rowBytes = rect.width() * bytesPerPixel;
    for (int row = 0; row < rect.height(); ++row) {
        const uchar *line = image.constScanLine(rect.y() + row)
            + rect.x() * bytesPerPixel;
        for (int byte = 0; byte < rowBytes; ++byte) {
            hash ^= line[byte];
            hash *= FnvPrime;
        }
    }
    return hash == 0 ? 1 : hash;
}

bool supportedRasterHistoryFormat(const QImage::Format format)
{
    switch (format) {
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGB32:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied:
    case QImage::Format_RGBX64:
        return true;
    default:
        return false;
    }
}

QImage::Format rasterHistoryStorageFormat(const QImage &before, const QImage &after)
{
    const QImage &preferred = !after.isNull() ? after : before;
    if (preferred.isNull()) {
        return QImage::Format_Invalid;
    }
    if ((!before.isNull() && before.depth() > 32)
        || (!after.isNull() && after.depth() > 32)) {
        if ((!before.isNull() && before.format() == after.format()
             && supportedRasterHistoryFormat(before.format()))) {
            return before.format();
        }
        return QImage::Format_RGBA64;
    }
    if ((!before.isNull() && !after.isNull() && before.format() == after.format()
         && supportedRasterHistoryFormat(before.format()))) {
        return before.format();
    }
    if (supportedRasterHistoryFormat(preferred.format()) && preferred.depth() == 32) {
        return preferred.format();
    }
    return QImage::Format_RGBA8888;
}

QImage normalisedRaster(const QImage &image,
                        const QSize &size,
                        const QColorSpace &colourSpace,
                        const QImage::Format format)
{
    if (size.isEmpty() || !supportedRasterHistoryFormat(format)) {
        return {};
    }
    if (image.isNull()) {
        QImage transparent(size, format);
        transparent.fill(Qt::transparent);
        transparent.setColorSpace(colourSpace);
        return transparent;
    }
    if (image.size() != size) {
        return {};
    }
    QImage converted = image.format() == format
        ? image : image.convertToFormat(format);
    const QColorSpace desiredColourSpace = image.colorSpace().isValid()
        ? image.colorSpace() : colourSpace;
    if (converted.colorSpace() != desiredColourSpace) {
        converted.setColorSpace(desiredColourSpace);
    }
    return converted;
}

QImage normalisedMask(const QImage &image, const QSize &size)
{
    if (image.isNull() || size.isEmpty()) {
        return {};
    }
    if (image.size() == QSize(1, 1)) {
        QImage expanded(size, QImage::Format_Grayscale8);
        expanded.fill(qGray(image.pixel(0, 0)));
        return expanded;
    }
    QImage converted = image;
    if (converted.size() != size) {
        converted = converted.scaled(size,
                                     Qt::IgnoreAspectRatio,
                                     Qt::SmoothTransformation);
    }
    return converted.convertToFormat(QImage::Format_Grayscale8);
}

QRect alignedTileBounds(const QRect &affected, const QSize &size, const int tileSize)
{
    if (affected.isEmpty() || size.isEmpty() || tileSize <= 0) {
        return {};
    }
    const QRect clipped = affected.intersected(QRect(QPoint(0, 0), size));
    if (clipped.isEmpty()) {
        return {};
    }
    const int left = (clipped.left() / tileSize) * tileSize;
    const int top = (clipped.top() / tileSize) * tileSize;
    const int right = std::min(size.width(), ((clipped.right() / tileSize) + 1) * tileSize);
    const int bottom = std::min(size.height(), ((clipped.bottom() / tileSize) + 1) * tileSize);
    return QRect(QPoint(left, top), QSize(right - left, bottom - top));
}

QVector<QRect> affectedTileRects(const QVector<QRect> &affectedRects,
                                 const QSize &imageSize,
                                 const int tileSize)
{
    QVector<QRect> result;
    if (imageSize.isEmpty() || tileSize <= 0 || affectedRects.isEmpty()) {
        return result;
    }
    const int tileColumns = (imageSize.width() + tileSize - 1) / tileSize;
    QSet<quint64> keys;
    for (const QRect &affectedRect : affectedRects) {
        const QRect bounds = alignedTileBounds(affectedRect, imageSize, tileSize);
        if (bounds.isEmpty()) {
            continue;
        }
        for (int y = bounds.top(); y < bounds.y() + bounds.height(); y += tileSize) {
            for (int x = bounds.left(); x < bounds.x() + bounds.width(); x += tileSize) {
                const quint64 key = static_cast<quint64>(y / tileSize)
                        * static_cast<quint64>(tileColumns)
                    + static_cast<quint64>(x / tileSize);
                keys.insert(key);
            }
        }
    }
    QVector<quint64> ordered;
    ordered.reserve(keys.size());
    for (const quint64 key : keys) {
        ordered.push_back(key);
    }
    std::sort(ordered.begin(), ordered.end());
    result.reserve(ordered.size());
    for (const quint64 key : std::as_const(ordered)) {
        const int tileY = static_cast<int>(key / static_cast<quint64>(tileColumns))
            * tileSize;
        const int tileX = static_cast<int>(key % static_cast<quint64>(tileColumns))
            * tileSize;
        result.push_back(QRect(tileX,
                               tileY,
                               std::min(tileSize, imageSize.width() - tileX),
                               std::min(tileSize, imageSize.height() - tileY)));
    }
    return result;
}

QByteArray decodedPayload(const RasterTileDelta &delta)
{
    const QByteArray raw = delta.compressed ? qUncompress(delta.payload) : delta.payload;
    return raw.size() == delta.rawByteCount ? raw : QByteArray();
}

QVector<RasterTileDelta> buildTileDeltasForRects(const QImage &before,
                                                 const QImage &after,
                                                 const QVector<QRect> &affectedRects,
                                                 const QSize &imageSize,
                                                 const int bytesPerPixel,
                                                 const int tileSize)
{
    QVector<RasterTileDelta> result;
    if (!validImageByteLayout(before, imageSize, bytesPerPixel)
        || !validImageByteLayout(after, imageSize, bytesPerPixel)) {
        return result;
    }
    const QVector<QRect> tileRects = affectedTileRects(affectedRects,
                                                        imageSize,
                                                        tileSize);
    result.reserve(tileRects.size());
    for (const QRect &tileRect : tileRects) {
        const int rowBytes = tileRect.width() * bytesPerPixel;
        QByteArray xorBytes(rowBytes * tileRect.height(), '\0');
        bool changed = false;
        for (int row = 0; row < tileRect.height(); ++row) {
            const uchar *beforeLine = before.constScanLine(tileRect.y() + row)
                + tileRect.x() * bytesPerPixel;
            const uchar *afterLine = after.constScanLine(tileRect.y() + row)
                + tileRect.x() * bytesPerPixel;
            uchar *deltaLine = reinterpret_cast<uchar *>(xorBytes.data()) + row * rowBytes;
            for (int byte = 0; byte < rowBytes; ++byte) {
                const uchar value = beforeLine[byte] ^ afterLine[byte];
                deltaLine[byte] = value;
                changed = changed || value != 0;
            }
        }
        if (!changed) {
            continue;
        }

        const QByteArray compressed = qCompress(xorBytes, 1);
        RasterTileDelta delta;
        delta.rect = tileRect;
        delta.rawByteCount = xorBytes.size();
        delta.beforeHash = hashTile(before, tileRect, bytesPerPixel);
        delta.afterHash = hashTile(after, tileRect, bytesPerPixel);
        delta.compressed = !compressed.isEmpty() && compressed.size() < xorBytes.size();
        delta.payload = delta.compressed ? compressed : xorBytes;
        result.push_back(std::move(delta));
    }
    return result;
}

QVector<RasterTileDelta> buildTileDeltas(const QImage &before,
                                         const QImage &after,
                                         const QRect &affectedRect,
                                         const QSize &imageSize,
                                         const int bytesPerPixel,
                                         const int tileSize)
{
    return buildTileDeltasForRects(before,
                                   after,
                                   QVector<QRect>{affectedRect},
                                   imageSize,
                                   bytesPerPixel,
                                   tileSize);
}

bool applyTileDeltas(QImage *output,
                     const QVector<RasterTileDelta> &tiles,
                     const QSize &imageSize,
                     const int bytesPerPixel,
                     const bool targetAfter)
{
    if (!output || !validImageByteLayout(*output, imageSize, bytesPerPixel)) {
        return false;
    }
    output->detach();
    const QRect imageRect(QPoint(0, 0), imageSize);
    for (const RasterTileDelta &delta : tiles) {
        if (delta.rect.isEmpty() || !imageRect.contains(delta.rect)
            || !validRectByteLayout(*output, delta.rect, bytesPerPixel)) {
            return false;
        }
        const quint64 expectedCurrentHash = targetAfter ? delta.beforeHash : delta.afterHash;
        const quint64 expectedTargetHash = targetAfter ? delta.afterHash : delta.beforeHash;
        if (hashTile(*output, delta.rect, bytesPerPixel) != expectedCurrentHash) {
            return false;
        }
        const QByteArray raw = decodedPayload(delta);
        const int rowBytes = delta.rect.width() * bytesPerPixel;
        if (raw.isEmpty() || raw.size() != rowBytes * delta.rect.height()) {
            return false;
        }
        for (int row = 0; row < delta.rect.height(); ++row) {
            uchar *outputLine = output->scanLine(delta.rect.y() + row)
                + delta.rect.x() * bytesPerPixel;
            const uchar *deltaLine = reinterpret_cast<const uchar *>(raw.constData())
                + row * rowBytes;
            for (int byte = 0; byte < rowBytes; ++byte) {
                outputLine[byte] ^= deltaLine[byte];
            }
        }
        if (hashTile(*output, delta.rect, bytesPerPixel) != expectedTargetHash) {
            return false;
        }
    }
    return true;
}

QImage normalisedChannelRaster(const QImage &image,
                              const QSize &size,
                              const QColorSpace &colourSpace,
                              const int bytesPerChannel)
{
    if (size.isEmpty() || (bytesPerChannel != 1 && bytesPerChannel != 2)) {
        return {};
    }
    const QImage::Format format = bytesPerChannel == 2
        ? QImage::Format_RGBA64
        : QImage::Format_RGBA8888;
    if (image.isNull()) {
        QImage transparent(size, format);
        transparent.fill(Qt::transparent);
        transparent.setColorSpace(colourSpace);
        return transparent;
    }
    if (image.size() != size) {
        return {};
    }
    QImage converted = image.format() == format
        ? image : image.convertToFormat(format);
    const QColorSpace desiredColourSpace = image.colorSpace().isValid()
        ? image.colorSpace() : colourSpace;
    if (converted.colorSpace() != desiredColourSpace) {
        converted.setColorSpace(desiredColourSpace);
    }
    return converted;
}

quint16 rgba64Channel(const QRgba64 pixel, const int channel)
{
    switch (channel) {
    case 0: return pixel.red();
    case 1: return pixel.green();
    case 2: return pixel.blue();
    case 3: return pixel.alpha();
    default: return 0;
    }
}

QRgba64 withRgba64Channel(const QRgba64 pixel, const int channel, const quint16 value)
{
    quint16 red = pixel.red();
    quint16 green = pixel.green();
    quint16 blue = pixel.blue();
    quint16 alpha = pixel.alpha();
    switch (channel) {
    case 0: red = value; break;
    case 1: green = value; break;
    case 2: blue = value; break;
    case 3: alpha = value; break;
    default: break;
    }
    return QRgba64::fromRgba64(red, green, blue, alpha);
}

quint64 hashChannelTile(const QImage &image, const QRect &rect, const int channel)
{
    const int bytesPerPixel = image.format() == QImage::Format_RGBA64 ? 8
        : image.format() == QImage::Format_RGBA8888 ? 4 : 0;
    if (channel < 0 || channel > 3
        || !validRectByteLayout(image, rect, bytesPerPixel)) {
        return 0;
    }
    quint64 hash = FnvOffset;
    if (image.format() == QImage::Format_RGBA64) {
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            const auto *row = reinterpret_cast<const QRgba64 *>(image.constScanLine(y));
            for (int x = rect.left(); x <= rect.right(); ++x) {
                const quint16 value = rgba64Channel(row[x], channel);
                hash ^= static_cast<quint8>(value & 0xffu);
                hash *= FnvPrime;
                hash ^= static_cast<quint8>((value >> 8) & 0xffu);
                hash *= FnvPrime;
            }
        }
    } else {
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            const uchar *row = image.constScanLine(y);
            for (int x = rect.left(); x <= rect.right(); ++x) {
                hash ^= row[x * 4 + channel];
                hash *= FnvPrime;
            }
        }
    }
    return hash == 0 ? 1 : hash;
}

QVector<RasterTileDelta> buildChannelTileDeltasForRects(
    const QImage &before,
    const QImage &after,
    const QVector<QRect> &affectedRects,
    const QSize &imageSize,
    const int channel,
    const int bytesPerChannel,
    const int tileSize)
{
    QVector<RasterTileDelta> result;
    const int imageBytesPerPixel = bytesPerChannel * 4;
    if (!validImageByteLayout(before, imageSize, imageBytesPerPixel)
        || !validImageByteLayout(after, imageSize, imageBytesPerPixel)
        || channel < 0 || channel > 3) {
        return result;
    }
    const QVector<QRect> tileRects = affectedTileRects(affectedRects,
                                                        imageSize,
                                                        tileSize);
    result.reserve(tileRects.size());
    for (const QRect &tileRect : tileRects) {
        const int rowBytes = tileRect.width() * bytesPerChannel;
        QByteArray xorBytes(rowBytes * tileRect.height(), '\0');
        bool changed = false;
        for (int rowIndex = 0; rowIndex < tileRect.height(); ++rowIndex) {
            uchar *deltaLine = reinterpret_cast<uchar *>(xorBytes.data()) + rowIndex * rowBytes;
            const int y = tileRect.y() + rowIndex;
            if (bytesPerChannel == 2) {
                const auto *beforeLine = reinterpret_cast<const QRgba64 *>(before.constScanLine(y));
                const auto *afterLine = reinterpret_cast<const QRgba64 *>(after.constScanLine(y));
                for (int x = 0; x < tileRect.width(); ++x) {
                    const quint16 value = rgba64Channel(beforeLine[tileRect.x() + x], channel)
                        ^ rgba64Channel(afterLine[tileRect.x() + x], channel);
                    qToLittleEndian<quint16>(value, deltaLine + x * 2);
                    changed = changed || value != 0;
                }
            } else {
                const uchar *beforeLine = before.constScanLine(y);
                const uchar *afterLine = after.constScanLine(y);
                for (int x = 0; x < tileRect.width(); ++x) {
                    const int pixelX = tileRect.x() + x;
                    const uchar value = beforeLine[pixelX * 4 + channel]
                        ^ afterLine[pixelX * 4 + channel];
                    deltaLine[x] = value;
                    changed = changed || value != 0;
                }
            }
        }
        if (!changed) {
            continue;
        }
        const QByteArray compressed = qCompress(xorBytes, 1);
        RasterTileDelta delta;
        delta.rect = tileRect;
        delta.rawByteCount = xorBytes.size();
        delta.beforeHash = hashChannelTile(before, tileRect, channel);
        delta.afterHash = hashChannelTile(after, tileRect, channel);
        delta.compressed = !compressed.isEmpty() && compressed.size() < xorBytes.size();
        delta.payload = delta.compressed ? compressed : xorBytes;
        result.push_back(std::move(delta));
    }
    return result;
}

QVector<RasterTileDelta> buildChannelTileDeltas(const QImage &before,
                                                const QImage &after,
                                                const QRect &affectedRect,
                                                const QSize &imageSize,
                                                const int channel,
                                                const int bytesPerChannel,
                                                const int tileSize)
{
    return buildChannelTileDeltasForRects(before,
                                          after,
                                          QVector<QRect>{affectedRect},
                                          imageSize,
                                          channel,
                                          bytesPerChannel,
                                          tileSize);
}

bool applyChannelTileDeltas(QImage *output,
                            const QVector<RasterTileDelta> &tiles,
                            const QSize &imageSize,
                            const int channel,
                            const int bytesPerChannel,
                            const bool targetAfter)
{
    const int imageBytesPerPixel = bytesPerChannel * 4;
    if (!output || channel < 0 || channel > 3
        || !validImageByteLayout(*output, imageSize, imageBytesPerPixel)) {
        return false;
    }
    output->detach();
    const QRect imageRect(QPoint(0, 0), imageSize);
    for (const RasterTileDelta &delta : tiles) {
        if (delta.rect.isEmpty() || !imageRect.contains(delta.rect)
            || !validRectByteLayout(*output, delta.rect, imageBytesPerPixel)) {
            return false;
        }
        const quint64 expectedCurrentHash = targetAfter ? delta.beforeHash : delta.afterHash;
        const quint64 expectedTargetHash = targetAfter ? delta.afterHash : delta.beforeHash;
        if (hashChannelTile(*output, delta.rect, channel) != expectedCurrentHash) {
            return false;
        }
        const QByteArray raw = decodedPayload(delta);
        const int rowBytes = delta.rect.width() * bytesPerChannel;
        if (raw.isEmpty() || raw.size() != rowBytes * delta.rect.height()) {
            return false;
        }
        for (int rowIndex = 0; rowIndex < delta.rect.height(); ++rowIndex) {
            const uchar *deltaLine = reinterpret_cast<const uchar *>(raw.constData())
                + rowIndex * rowBytes;
            const int y = delta.rect.y() + rowIndex;
            if (bytesPerChannel == 2) {
                auto *outputLine = reinterpret_cast<QRgba64 *>(output->scanLine(y));
                for (int x = 0; x < delta.rect.width(); ++x) {
                    const int pixelX = delta.rect.x() + x;
                    const quint16 xorValue = qFromLittleEndian<quint16>(deltaLine + x * 2);
                    const quint16 value = rgba64Channel(outputLine[pixelX], channel) ^ xorValue;
                    outputLine[pixelX] = withRgba64Channel(outputLine[pixelX], channel, value);
                }
            } else {
                uchar *outputLine = output->scanLine(y);
                for (int x = 0; x < delta.rect.width(); ++x) {
                    outputLine[(delta.rect.x() + x) * 4 + channel] ^= deltaLine[x];
                }
            }
        }
        if (hashChannelTile(*output, delta.rect, channel) != expectedTargetHash) {
            return false;
        }
    }
    return true;
}

qint64 storedBytesForTiles(const QVector<RasterTileDelta> &tiles, const qint64 headerBytes)
{
    qint64 bytes = headerBytes;
    for (const RasterTileDelta &tile : tiles) {
        bytes += sizeof(RasterTileDelta) + tile.payload.size();
    }
    return bytes;
}

} // namespace

qint64 RasterTileDeltaSet::storedBytes() const
{
    return storedBytesForTiles(tiles, sizeof(RasterTileDeltaSet));
}

qint64 MaskTileDeltaSet::storedBytes() const
{
    return storedBytesForTiles(tiles, sizeof(MaskTileDeltaSet));
}

qint64 ChannelTileDeltaSet::storedBytes() const
{
    return storedBytesForTiles(tiles, sizeof(ChannelTileDeltaSet));
}

RasterTileDeltaSet buildRasterTileDeltaSet(const QImage &before,
                                           const QImage &after,
                                           const QRect &affectedRect,
                                           const int tileSize)
{
    RasterTileDeltaSet result;
    result.beforeWasNull = before.isNull();
    result.afterWasNull = after.isNull();
    result.beforeFormat = before.isNull() ? QImage::Format_Invalid : before.format();
    result.afterFormat = after.isNull() ? QImage::Format_Invalid : after.format();
    result.imageSize = !after.isNull() ? after.size() : before.size();
    result.colourSpace = after.colorSpace().isValid() ? after.colorSpace() : before.colorSpace();
    result.storageFormat = rasterHistoryStorageFormat(before, after);
    result.bytesPerPixel = result.storageFormat == QImage::Format_Invalid
        ? 0 : QImage::toPixelFormat(result.storageFormat).bitsPerPixel() / 8;

    if (result.imageSize.isEmpty() || tileSize <= 0
        || (result.bytesPerPixel != 4 && result.bytesPerPixel != 8)
        || (!before.isNull() && before.size() != result.imageSize)
        || (!after.isNull() && after.size() != result.imageSize)) {
        return result;
    }

    const QImage beforeImage = normalisedRaster(before,
                                                result.imageSize,
                                                result.colourSpace,
                                                result.storageFormat);
    const QImage afterImage = normalisedRaster(after,
                                               result.imageSize,
                                               result.colourSpace,
                                               result.storageFormat);
    if (beforeImage.isNull() || afterImage.isNull()) {
        return {};
    }
    result.tiles = buildTileDeltas(beforeImage,
                                   afterImage,
                                   affectedRect,
                                   result.imageSize,
                                   result.bytesPerPixel,
                                   tileSize);
    return result;
}

RasterTileDeltaSet buildRasterTileDeltaSet(const QImage &before,
                                           const QImage &after,
                                           const QVector<QRect> &affectedRects,
                                           const int tileSize)
{
    RasterTileDeltaSet result;
    result.beforeWasNull = before.isNull();
    result.afterWasNull = after.isNull();
    result.beforeFormat = before.isNull() ? QImage::Format_Invalid : before.format();
    result.afterFormat = after.isNull() ? QImage::Format_Invalid : after.format();
    result.imageSize = !after.isNull() ? after.size() : before.size();
    result.colourSpace = after.colorSpace().isValid() ? after.colorSpace() : before.colorSpace();
    result.storageFormat = rasterHistoryStorageFormat(before, after);
    result.bytesPerPixel = result.storageFormat == QImage::Format_Invalid
        ? 0 : QImage::toPixelFormat(result.storageFormat).bitsPerPixel() / 8;

    if (result.imageSize.isEmpty() || tileSize <= 0 || affectedRects.isEmpty()
        || (result.bytesPerPixel != 4 && result.bytesPerPixel != 8)
        || (!before.isNull() && before.size() != result.imageSize)
        || (!after.isNull() && after.size() != result.imageSize)) {
        return result;
    }

    const QImage beforeImage = normalisedRaster(before,
                                                result.imageSize,
                                                result.colourSpace,
                                                result.storageFormat);
    const QImage afterImage = normalisedRaster(after,
                                               result.imageSize,
                                               result.colourSpace,
                                               result.storageFormat);
    if (beforeImage.isNull() || afterImage.isNull()) {
        return {};
    }
    result.tiles = buildTileDeltasForRects(beforeImage,
                                           afterImage,
                                           affectedRects,
                                           result.imageSize,
                                           result.bytesPerPixel,
                                           tileSize);
    return result;
}

QImage applyRasterTileDeltaSet(const QImage &current,
                               const RasterTileDeltaSet &deltaSet,
                               const bool targetAfter,
                               bool *ok)
{
    if (ok) {
        *ok = false;
    }
    if (deltaSet.imageSize.isEmpty() || deltaSet.tiles.isEmpty()
        || (deltaSet.bytesPerPixel != 4 && deltaSet.bytesPerPixel != 8)
        || !supportedRasterHistoryFormat(deltaSet.storageFormat)) {
        return current;
    }
    if (!current.isNull() && current.size() != deltaSet.imageSize) {
        return {};
    }

    QImage output = normalisedRaster(current,
                                     deltaSet.imageSize,
                                     deltaSet.colourSpace,
                                     deltaSet.storageFormat);
    if (!applyTileDeltas(&output,
                         deltaSet.tiles,
                         deltaSet.imageSize,
                         deltaSet.bytesPerPixel,
                         targetAfter)) {
        return {};
    }

    const bool targetNull = (targetAfter && deltaSet.afterWasNull)
        || (!targetAfter && deltaSet.beforeWasNull);
    if (targetNull) {
        output = {};
    } else {
        const QImage::Format targetFormat = targetAfter
            ? deltaSet.afterFormat : deltaSet.beforeFormat;
        if (targetFormat != QImage::Format_Invalid && output.format() != targetFormat) {
            output = output.convertToFormat(targetFormat);
            output.setColorSpace(deltaSet.colourSpace);
        }
    }
    if (ok) {
        *ok = true;
    }
    return output;
}

ChannelTileDeltaSet buildChannelTileDeltaSet(const QImage &before,
                                             const QImage &after,
                                             const QRect &affectedRect,
                                             const int channelIndex,
                                             const int tileSize)
{
    ChannelTileDeltaSet result;
    result.beforeWasNull = before.isNull();
    result.afterWasNull = after.isNull();
    result.imageSize = !after.isNull() ? after.size() : before.size();
    result.colourSpace = after.colorSpace().isValid() ? after.colorSpace() : before.colorSpace();
    result.channelIndex = channelIndex;
    result.bytesPerChannel = ((!after.isNull() && after.depth() > 32)
                              || (!before.isNull() && before.depth() > 32)) ? 2 : 1;
    if (result.imageSize.isEmpty() || channelIndex < 0 || channelIndex > 3 || tileSize <= 0
        || (!before.isNull() && before.size() != result.imageSize)
        || (!after.isNull() && after.size() != result.imageSize)) {
        return result;
    }
    const QImage beforeImage = normalisedChannelRaster(before,
                                                       result.imageSize,
                                                       result.colourSpace,
                                                       result.bytesPerChannel);
    const QImage afterImage = normalisedChannelRaster(after,
                                                      result.imageSize,
                                                      result.colourSpace,
                                                      result.bytesPerChannel);
    if (beforeImage.isNull() || afterImage.isNull()) {
        return {};
    }
    result.tiles = buildChannelTileDeltas(beforeImage,
                                          afterImage,
                                          affectedRect,
                                          result.imageSize,
                                          channelIndex,
                                          result.bytesPerChannel,
                                          tileSize);
    return result;
}

ChannelTileDeltaSet buildChannelTileDeltaSet(
    const QImage &before,
    const QImage &after,
    const QVector<QRect> &affectedRects,
    const int channelIndex,
    const int tileSize)
{
    ChannelTileDeltaSet result;
    result.beforeWasNull = before.isNull();
    result.afterWasNull = after.isNull();
    result.imageSize = !after.isNull() ? after.size() : before.size();
    result.colourSpace = after.colorSpace().isValid() ? after.colorSpace() : before.colorSpace();
    result.channelIndex = channelIndex;
    result.bytesPerChannel = ((!after.isNull() && after.depth() > 32)
                              || (!before.isNull() && before.depth() > 32)) ? 2 : 1;
    if (result.imageSize.isEmpty() || affectedRects.isEmpty()
        || channelIndex < 0 || channelIndex > 3 || tileSize <= 0
        || (!before.isNull() && before.size() != result.imageSize)
        || (!after.isNull() && after.size() != result.imageSize)) {
        return result;
    }
    const QImage beforeImage = normalisedChannelRaster(before,
                                                       result.imageSize,
                                                       result.colourSpace,
                                                       result.bytesPerChannel);
    const QImage afterImage = normalisedChannelRaster(after,
                                                      result.imageSize,
                                                      result.colourSpace,
                                                      result.bytesPerChannel);
    if (beforeImage.isNull() || afterImage.isNull()) {
        return {};
    }
    result.tiles = buildChannelTileDeltasForRects(beforeImage,
                                                  afterImage,
                                                  affectedRects,
                                                  result.imageSize,
                                                  channelIndex,
                                                  result.bytesPerChannel,
                                                  tileSize);
    return result;
}

QImage applyChannelTileDeltaSet(const QImage &current,
                                const ChannelTileDeltaSet &deltaSet,
                                const bool targetAfter,
                                bool *ok)
{
    if (ok) {
        *ok = false;
    }
    if (deltaSet.imageSize.isEmpty() || deltaSet.tiles.isEmpty()
        || deltaSet.channelIndex < 0 || deltaSet.channelIndex > 3
        || (deltaSet.bytesPerChannel != 1 && deltaSet.bytesPerChannel != 2)) {
        return current;
    }
    if (!current.isNull() && current.size() != deltaSet.imageSize) {
        return {};
    }
    QImage output = normalisedChannelRaster(current,
                                            deltaSet.imageSize,
                                            deltaSet.colourSpace,
                                            deltaSet.bytesPerChannel);
    if (!applyChannelTileDeltas(&output,
                                deltaSet.tiles,
                                deltaSet.imageSize,
                                deltaSet.channelIndex,
                                deltaSet.bytesPerChannel,
                                targetAfter)) {
        return {};
    }
    if ((targetAfter && deltaSet.afterWasNull)
        || (!targetAfter && deltaSet.beforeWasNull)) {
        output = {};
    }
    if (ok) {
        *ok = true;
    }
    return output;
}

MaskTileDeltaSet buildMaskTileDeltaSet(const QImage &before,
                                       const QImage &after,
                                       const QRect &affectedRect,
                                       const QSize &documentSize,
                                       const int tileSize)
{
    MaskTileDeltaSet result;
    result.imageSize = documentSize;
    result.beforeWasCompact = before.size() == QSize(1, 1);
    result.afterWasCompact = after.size() == QSize(1, 1);
    if (result.beforeWasCompact) {
        result.beforeCompactValue = static_cast<quint8>(qGray(before.pixel(0, 0)));
    }
    if (result.afterWasCompact) {
        result.afterCompactValue = static_cast<quint8>(qGray(after.pixel(0, 0)));
    }
    if (documentSize.isEmpty() || before.isNull() || after.isNull() || tileSize <= 0) {
        return result;
    }

    const QImage beforeImage = normalisedMask(before, documentSize);
    const QImage afterImage = normalisedMask(after, documentSize);
    if (beforeImage.isNull() || afterImage.isNull()) {
        return {};
    }
    result.tiles = buildTileDeltas(beforeImage,
                                   afterImage,
                                   affectedRect,
                                   documentSize,
                                   1,
                                   tileSize);
    return result;
}

MaskTileDeltaSet buildMaskTileDeltaSet(const QImage &before,
                                       const QImage &after,
                                       const QVector<QRect> &affectedRects,
                                       const QSize &documentSize,
                                       const int tileSize)
{
    MaskTileDeltaSet result;
    result.imageSize = documentSize;
    result.beforeWasCompact = before.size() == QSize(1, 1);
    result.afterWasCompact = after.size() == QSize(1, 1);
    if (result.beforeWasCompact) {
        result.beforeCompactValue = static_cast<quint8>(qGray(before.pixel(0, 0)));
    }
    if (result.afterWasCompact) {
        result.afterCompactValue = static_cast<quint8>(qGray(after.pixel(0, 0)));
    }
    if (documentSize.isEmpty() || affectedRects.isEmpty()
        || before.isNull() || after.isNull() || tileSize <= 0) {
        return result;
    }

    const QImage beforeImage = normalisedMask(before, documentSize);
    const QImage afterImage = normalisedMask(after, documentSize);
    if (beforeImage.isNull() || afterImage.isNull()) {
        return {};
    }
    result.tiles = buildTileDeltasForRects(beforeImage,
                                           afterImage,
                                           affectedRects,
                                           documentSize,
                                           1,
                                           tileSize);
    return result;
}

QImage applyMaskTileDeltaSet(const QImage &current,
                             const MaskTileDeltaSet &deltaSet,
                             const bool targetAfter,
                             bool *ok)
{
    if (ok) {
        *ok = false;
    }
    if (deltaSet.imageSize.isEmpty() || deltaSet.tiles.isEmpty() || current.isNull()) {
        return current;
    }

    QImage output = normalisedMask(current, deltaSet.imageSize);
    if (!applyTileDeltas(&output, deltaSet.tiles, deltaSet.imageSize, 1, targetAfter)) {
        return {};
    }

    const bool compact = targetAfter ? deltaSet.afterWasCompact : deltaSet.beforeWasCompact;
    if (compact) {
        const quint8 value = targetAfter ? deltaSet.afterCompactValue
                                         : deltaSet.beforeCompactValue;
        QImage compactMask(1, 1, QImage::Format_Grayscale8);
        compactMask.fill(value);
        output = compactMask;
    }
    if (ok) {
        *ok = true;
    }
    return output;
}

} // namespace vfx
