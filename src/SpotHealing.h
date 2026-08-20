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

struct SpotHealingRequest {
    QImage destination;
    QImage source;
    QVector<QLineF> targetSegments;

    // Same coordinate contract as HealingBrushRequest. Candidate source patches
    // are searched as deterministic document-space offsets and then passed to
    // the Poisson seamless-cloning solver.
    QTransform targetPixelToLayer;
    QTransform targetLayerToDocument;
    QTransform sourceDocumentToLayer;
    QTransform sourceLayerToPixel;

    double diameterPixels = 1.0;
    double opacity = 1.0;
    double hardness = 1.0;
    int maximumCandidates = 144;
    std::shared_ptr<std::atomic_bool> cancelRequested;
};

struct SpotHealingResult {
    QImage image;
    QRect affectedRect;
    QPointF sourceOffsetDocument;
    int candidatesEvaluated = 0;
    bool cancelled = false;
    QString error;
};

// Deterministic automatic-source healing. It searches nearby non-overlapping
// source patches, scores their boundary colour/gradient/alpha compatibility
// and distance, then feeds the winning immutable offset into Healing Brush's
// sparse Poisson seamless-cloning reference.
SpotHealingResult applySpotHealing(const SpotHealingRequest &request);

} // namespace vfx
