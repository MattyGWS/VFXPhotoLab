#pragma once

#include <QImage>
#include <QLineF>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QTransform>
#include <QVector>

#include <atomic>
#include <memory>

namespace vfx {

struct HealingBrushRequest {
    QImage destination;
    QImage source;
    QVector<QLineF> targetSegments;

    // The coordinate contract matches Clone Stamp: destination pixels map to
    // target-layer coordinates and then document coordinates. The sampled
    // document point is offset and mapped into the immutable source image.
    QTransform targetPixelToLayer;
    QTransform targetLayerToDocument;
    QTransform sourceDocumentToLayer;
    QTransform sourceLayerToPixel;
    QPointF sourceOffsetDocument;

    double diameterPixels = 1.0;
    double opacity = 1.0;
    double hardness = 1.0;

    // Optional cooperative cancellation used by document/session switches and
    // application shutdown.  A cancelled operation never publishes a partial
    // Poisson solution.
    std::shared_ptr<std::atomic_bool> cancelRequested;
};

struct HealingBrushResult {
    QImage image;
    QRect affectedRect;
    bool cancelled = false;
    QString error;
};

// Authoritative CPU Healing Brush reference. It performs sparse Poisson
// seamless cloning: sampled source structure supplies the interior gradients,
// while a harmonic destination-minus-source correction matches colour and
// illumination at the stroke boundary. Destination Alpha is never changed;
// straight hidden RGB is still updated beneath zero Alpha.
HealingBrushResult applyHealingBrush(const HealingBrushRequest &request);

} // namespace vfx
