#pragma once

#include "CanvasSizeOperations.h"
#include "PhotoDocument.h"

#include <QPoint>
#include <QRect>

#include <atomic>

namespace vfx {

enum class AutomaticTrimMode {
    TransparentPixels,
    CornerColour
};

enum class TrimSampleCorner {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
};

struct AutomaticTrimRequest {
    AutomaticTrimMode mode = AutomaticTrimMode::TransparentPixels;
    TrimSampleCorner sampleCorner = TrimSampleCorner::TopLeft;
    int tolerance = 0;
    bool trimTop = true;
    bool trimBottom = true;
    bool trimLeft = true;
    bool trimRight = true;
    bool deleteOutsideCanvas = false;
};

struct AutomaticTrimResult {
    CanvasSizeResult canvas;
    QRect documentRect;
    QColor sampledColour;
    bool sampledColourValid = false;
    bool noChange = false;
    QString noChangeMessage;
};

// Analyse the visible merged composite in cancellable strips and prepare one
// atomic document-bounds transaction. Transparent trimming treats every
// non-zero alpha sample as content. Corner-colour trimming compares straight
// RGBA with an inclusive per-channel tolerance; fully transparent samples match
// regardless of hidden RGB.
bool buildAutomaticTrimResult(
    const PhotoDocument &document,
    const AutomaticTrimRequest &request,
    AutomaticTrimResult *result,
    const std::atomic_bool *cancelRequested = nullptr,
    QString *errorMessage = nullptr);

} // namespace vfx
