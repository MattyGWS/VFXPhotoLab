#pragma once

#include "PhotoDocument.h"

#include <QRect>

#include <atomic>

namespace vfx {

struct CropRequest {
    QRect documentRect;
    double straightenAngle = 0.0;
    bool deleteCroppedPixels = false;
};

struct CropResult {
    QImage canvasImage;
    QVector<LayerNode> layers;
    SelectionMask::Snapshot selection;
    QVector<double> horizontalGuides;
    QVector<double> verticalGuides;
};

bool buildCropResult(const PhotoDocument &document,
                     const CropRequest &request,
                     CropResult *result,
                     const std::atomic_bool *cancelRequested = nullptr,
                     QString *errorMessage = nullptr);

} // namespace vfx
