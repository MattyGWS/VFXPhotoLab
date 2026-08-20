#pragma once

#include <QColor>
#include <QImage>
#include <QLineF>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QtGlobal>
#include <QVector>

namespace vfx {

enum class SmudgeBrushTarget {
    RasterRgba,
    GreyChannel,
    ComponentChannel,
    Mask
};

struct SmudgeBrushRequest {
    QImage destination;
    QVector<QLineF> targetSegments;
    double diameterPixels = 1.0;
    double strength = 0.5;
    double hardness = 0.5;
    SmudgeBrushTarget target = SmudgeBrushTarget::RasterRgba;
    int componentIndex = -1;
    bool fingerPainting = false;
    QColor fingerColour = QColor(Qt::black);

    // Incremental callers leave this false while pointer segments are still
    // arriving. Set it on the final append to transport the remaining
    // sub-spacing distance to the released pointer position. The batch API
    // always treats its request as a complete stroke.
    bool finishStroke = false;

    // Optional layer-local selection coverage. A null image means unrestricted,
    // a 1x1 image is interpreted as uniform coverage, and otherwise the size
    // must match destination. Selection coverage participates in every ordered
    // dab so rejected writes outside the selection cannot become later stroke feedback.
    QImage selectionCoverage;
};

struct SmudgeBrushResult {
    QImage image;
    QRect affectedRect;
    quint64 appliedDabCount = 0;
    QString error;
};

struct SmudgeBrushStrokeState {
    QSize imageSize;
    bool initialised = false;
    QPointF lastCentre;
    QPointF lastInputPoint;
    double distanceUntilNextDab = 0.0;
    quint64 appliedDabCount = 0;
    QRect affectedRect;

    // Reused immutable pre-dab footprint. Keeping this allocation in the
    // stroke state prevents every transported dab from allocating and freeing
    // another QImage, which previously made long strokes progressively stall.
    QImage scratchImage;
    QSize scratchDataSize;

    void reset();
};

// Ordered CPU reference implementation. Each dab reads from the result of the
// previous dab, transporting colour/alpha (or scalar channel values) along the
// gesture without scan-order dependence.
SmudgeBrushResult applySmudgeBrush(const SmudgeBrushRequest &request);

// Incremental live-preview path. workingImage retains the ordered result and
// request.targetSegments contains only newly appended pointer segments.
SmudgeBrushResult applySmudgeBrushIncremental(const SmudgeBrushRequest &request,
                                              SmudgeBrushStrokeState *state,
                                              QImage *workingImage);

} // namespace vfx
