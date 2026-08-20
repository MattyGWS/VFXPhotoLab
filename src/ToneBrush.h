#pragma once

#include <QHash>
#include <QImage>
#include <QLineF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

namespace vfx {

enum class ToneBrushOperation {
    None,
    Dodge,
    Burn,
    SpongeSaturate,
    SpongeDesaturate,
    Blur,
    Sharpen,
    Smudge
};

enum class ToneBrushRange {
    Shadows,
    Midtones,
    Highlights
};

enum class ToneBrushTarget {
    RasterRgb,
    GreyChannel,
    ComponentChannel,
    Mask
};

struct ToneBrushRequest {
    QImage destination;
    QVector<QLineF> targetSegments;
    double diameterPixels = 1.0;
    double strength = 0.2;
    double hardness = 0.5;
    ToneBrushOperation operation = ToneBrushOperation::None;
    ToneBrushRange range = ToneBrushRange::Midtones;
    ToneBrushTarget target = ToneBrushTarget::RasterRgb;
    int componentIndex = -1;
    bool protectTones = true;
    bool vibranceProtection = true;
    double radiusPixels = 4.0;
    bool protectHighlights = true;
};

struct ToneBrushResult {
    QImage image;
    QRect affectedRect;
    QString error;
};

// Sparse per-stroke coverage retained by live tone/detail-brush previews.
// Coverage is the maximum dab contribution at each touched pixel, matching the
// authoritative whole-gesture kernel without replaying every earlier segment
// on each pointer event.
struct ToneBrushStrokeAccumulator {
    QSize imageSize;
    QHash<quint64, QVector<float>> coverageTiles;

    void reset();
};

ToneBrushResult applyToneBrush(const ToneBrushRequest &request);

// Incrementally appends request.targetSegments to an active tone-brush stroke.
// request.destination remains the immutable stroke-start image. workingImage is
// updated only where the new segments increase the retained stroke coverage.
// This makes live-preview cost proportional to the newly appended segment while
// remaining pixel-identical to applyToneBrush for the complete gesture.
ToneBrushResult applyToneBrushIncremental(const ToneBrushRequest &request,
                                          ToneBrushStrokeAccumulator *accumulator,
                                          QImage *workingImage);

} // namespace vfx
