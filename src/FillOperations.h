#pragma once

#include "SelectionMask.h"

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTransform>

namespace vfx {

enum class FillTarget {
    RasterPixels = 0,
    GreyChannel = 1,
    ComponentChannel = 2,
    Mask = 3
};

struct FillCoverageRequest {
    QImage sourceImage;
    QSize documentSize;
    QTransform targetToDocument;
    QPointF documentPosition;
    int tolerance = 32;
    bool contiguous = true;
    FillTarget target = FillTarget::RasterPixels;
    int componentIndex = -1;
    SelectionMask::Snapshot selectionSnapshot;
};

struct FillCoverageResult {
    QImage coverage;
    QRect affectedRect;
    QPoint seedPixel {-1, -1};
    int matchedPixelCount = 0;
    QString error;

    bool succeeded() const { return error.isEmpty() && !coverage.isNull(); }
};

struct FillApplyResult {
    QImage image;
    QRect affectedRect;
    int changedPixelCount = 0;
    QString error;

    bool succeeded() const { return error.isEmpty() && !image.isNull(); }
    bool changed() const { return changedPixelCount > 0 && !affectedRect.isEmpty(); }
};

// Build one deterministic target-local coverage mask. Matching is performed on
// straight RGBA/scalar values; fully transparent raster pixels compare by
// alpha only so hidden RGB never fragments an apparently uniform transparent
// area. An active selection constrains both region traversal and final feather
// coverage.
FillCoverageResult buildFillCoverage(const FillCoverageRequest &request);

// Exact CPU reference used directly for 16-bit documents and as the fallback
// for native tiled GPU application. Stored pixels remain straight RGBA; partial
// raster coverage blends in premultiplied space to avoid transparent-edge
// contamination, while meaningful source RGB is retained when output alpha is zero.
FillApplyResult applyFillCoverageCpu(const QImage &sourceImage,
                                     const QImage &coverage,
                                     FillTarget target,
                                     int componentIndex,
                                     const QColor &colour,
                                     bool preserveTransparency);

// Preserve compact all-white/all-black layer-mask storage after a full fill.
QImage compactUniformFillMask(const QImage &mask);

} // namespace vfx
