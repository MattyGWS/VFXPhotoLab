#pragma once

#include <QImage>
#include <QLineF>
#include <QPointF>
#include <QRect>
#include <QTransform>
#include <QVector>
#include <QString>

namespace vfx {

enum class CloneStampTarget {
    RasterPixels,
    GreyChannel,
    ComponentChannel,
    Mask
};

enum class CloneStampSample {
    Rgba,
    Luminance,
    Alpha,
    Component
};

struct CloneStampRequest {
    QImage destination;
    QImage source;
    QVector<QLineF> targetSegments;

    // Target pixels may be presentation-scaled. They are first mapped into
    // the target layer's full-resolution local coordinates, then into document
    // coordinates. Source document coordinates are offset and mapped through
    // the source layer before reaching source-image pixels.
    QTransform targetPixelToLayer;
    QTransform targetLayerToDocument;
    QTransform sourceDocumentToLayer;
    QTransform sourceLayerToPixel;
    QPointF sourceOffsetDocument;

    double diameterPixels = 1.0;
    double opacity = 1.0;
    double hardness = 1.0;
    CloneStampTarget target = CloneStampTarget::RasterPixels;
    CloneStampSample sample = CloneStampSample::Rgba;
    int componentIndex = -1;
};

struct CloneStampResult {
    QImage image;
    QRect affectedRect;
    QString error;
};

// Authoritative CPU reference for Clone Stamp. Raster pixels are blended in
// straight RGBA so hidden RGB beneath zero alpha is copied and preserved.
// Overlapping dabs accumulate floating-point coverage and each output pixel is
// quantised only once, preventing low-opacity channel contouring.
CloneStampResult applyCloneStamp(const CloneStampRequest &request);

} // namespace vfx
