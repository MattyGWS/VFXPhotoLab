#pragma once

#include "PhotoDocument.h"

#include <QString>

namespace vfx {

enum class OrthogonalDocumentTransform {
    FlipHorizontal,
    FlipVertical,
    Rotate90Clockwise,
    Rotate90CounterClockwise,
    Rotate180
};

struct OrthogonalDocumentTransformResult {
    QImage canvasImage;
    QVector<LayerNode> layers;
    SelectionMask::Snapshot selection;
    QVector<double> horizontalGuides;
    QVector<double> verticalGuides;
    double resolutionX = 72.0;
    double resolutionY = 72.0;
};

QTransform documentTransformMatrix(OrthogonalDocumentTransform operation,
                                   const QSize &sourceSize);

QImage transformImageOrthogonally(const QImage &source,
                                  OrthogonalDocumentTransform operation);

bool buildOrthogonalDocumentTransform(
    const PhotoDocument &document,
    OrthogonalDocumentTransform operation,
    OrthogonalDocumentTransformResult *result,
    QString *errorMessage = nullptr);

} // namespace vfx
