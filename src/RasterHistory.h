#pragma once

#include <QByteArray>
#include <QColorSpace>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QVector>
#include <QtGlobal>

namespace vfx {

// One symmetric XOR delta for a tile. Applying the same delta toggles between
// the before and after pixels, so Undo and Redo share one payload.
struct RasterTileDelta {
    QRect rect;
    QByteArray payload;
    int rawByteCount = 0;
    quint64 beforeHash = 0;
    quint64 afterHash = 0;
    bool compressed = false;
};

struct RasterTileDeltaSet {
    QSize imageSize;
    QColorSpace colourSpace;
    QVector<RasterTileDelta> tiles;
    QImage::Format storageFormat = QImage::Format_Invalid;
    QImage::Format beforeFormat = QImage::Format_Invalid;
    QImage::Format afterFormat = QImage::Format_Invalid;
    int bytesPerPixel = 4;
    bool beforeWasNull = false;
    bool afterWasNull = false;

    bool isEmpty() const { return tiles.isEmpty(); }
    qint64 storedBytes() const;
};

// Masks use the same compressed symmetric XOR architecture as raster history,
// but retain only one greyscale byte per pixel. Compact 1x1 masks are expanded
// logically while editing and restored exactly by Undo/Redo.
struct MaskTileDeltaSet {
    QSize imageSize;
    QVector<RasterTileDelta> tiles;
    bool beforeWasCompact = false;
    bool afterWasCompact = false;
    quint8 beforeCompactValue = 255;
    quint8 afterCompactValue = 255;

    bool isEmpty() const { return tiles.isEmpty(); }
    qint64 storedBytes() const;
};

// Sparse history for direct R/G/B/A editing. Only the selected component is
// stored: one byte per pixel for 8-bit documents and two bytes per pixel for
// 16-bit documents. Unselected components are never reconstructed or touched.
struct ChannelTileDeltaSet {
    QSize imageSize;
    QColorSpace colourSpace;
    QVector<RasterTileDelta> tiles;
    int channelIndex = -1;
    int bytesPerChannel = 1;
    bool beforeWasNull = false;
    bool afterWasNull = false;

    bool isEmpty() const { return tiles.isEmpty(); }
    qint64 storedBytes() const;
};

struct RasterHistoryStats {
    qint64 storedBytes = 0;
    qint64 tileCount = 0;
    int commandCount = 0;
};

RasterTileDeltaSet buildRasterTileDeltaSet(const QImage &before,
                                           const QImage &after,
                                           const QRect &affectedRect,
                                           int tileSize = 256);

// Sparse variant used by long path-based tools. Each rectangle is expanded to
// history tiles once, deduplicated, and scanned without touching empty tiles
// inside the stroke's overall bounding box.
RasterTileDeltaSet buildRasterTileDeltaSet(const QImage &before,
                                           const QImage &after,
                                           const QVector<QRect> &affectedRects,
                                           int tileSize = 256);

// Toggle the supplied raster between its before/after state. Raster deltas retain
// the source straight/premultiplied 8-bit format or exact RGBA64 precision, while
// targetAfter restores the original null/format state. Pixel mutation is symmetric.
QImage applyRasterTileDeltaSet(const QImage &current,
                               const RasterTileDeltaSet &deltaSet,
                               bool targetAfter,
                               bool *ok = nullptr);

ChannelTileDeltaSet buildChannelTileDeltaSet(const QImage &before,
                                             const QImage &after,
                                             const QRect &affectedRect,
                                             int channelIndex,
                                             int tileSize = 256);

ChannelTileDeltaSet buildChannelTileDeltaSet(const QImage &before,
                                             const QImage &after,
                                             const QVector<QRect> &affectedRects,
                                             int channelIndex,
                                             int tileSize = 256);

QImage applyChannelTileDeltaSet(const QImage &current,
                                const ChannelTileDeltaSet &deltaSet,
                                bool targetAfter,
                                bool *ok = nullptr);

MaskTileDeltaSet buildMaskTileDeltaSet(const QImage &before,
                                       const QImage &after,
                                       const QRect &affectedRect,
                                       const QSize &documentSize,
                                       int tileSize = 256);

MaskTileDeltaSet buildMaskTileDeltaSet(const QImage &before,
                                       const QImage &after,
                                       const QVector<QRect> &affectedRects,
                                       const QSize &documentSize,
                                       int tileSize = 256);

QImage applyMaskTileDeltaSet(const QImage &current,
                             const MaskTileDeltaSet &deltaSet,
                             bool targetAfter,
                             bool *ok = nullptr);

} // namespace vfx
