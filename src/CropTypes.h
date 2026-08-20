#pragma once

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QSizeF>

#include <algorithm>

namespace vfx {

enum class CropMode {
    Free,
    Ratio,
    FixedSize
};

enum class CropOverlay {
    None,
    RuleOfThirds,
    Grid,
    Diagonal,
    Triangle,
    GoldenRatio,
    GoldenSpiral
};

struct CropSessionState {
    bool initialised = false;
    QRectF frame;
    CropMode mode = CropMode::Free;
    double ratioWidth = 1.0;
    double ratioHeight = 1.0;
    bool originalRatio = false;
    QSize fixedSize {1920, 1080};
    CropOverlay overlay = CropOverlay::RuleOfThirds;
    int overlayOrientation = 0;
    double dimOpacity = 0.60;
    bool snappingEnabled = true;
    bool deleteCroppedPixels = false;
    bool straightenSampling = false;
    double straightenAngle = 0.0;
};

// Crop geometry and straighten values are pending tool interaction, not a
// second durable edit layered on top of an applied crop. Both sides of Crop's
// structural Undo command therefore use a settled full-canvas tool state. This
// prevents Undo from restoring the original document and then immediately
// re-applying the submitted straighten angle as a live preview.
inline CropSessionState settledCropStateForCanvas(CropSessionState state,
                                                  const QSize &canvasSize)
{
    state.initialised = true;
    state.frame = QRectF(QPointF(), QSizeF(canvasSize));
    state.fixedSize = canvasSize;
    state.ratioWidth = std::max(1, canvasSize.width());
    state.ratioHeight = std::max(1, canvasSize.height());
    state.originalRatio = true;
    state.straightenSampling = false;
    state.straightenAngle = 0.0;
    return state;
}

} // namespace vfx
