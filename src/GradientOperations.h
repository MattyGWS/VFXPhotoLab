#pragma once

#include "FillOperations.h"
#include "SelectionMask.h"

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTransform>

namespace vfx {

enum class RasterGradientType {
    Linear = 0,
    Radial = 1,
    Angle = 2,
    Reflected = 3,
    Diamond = 4
};

enum class RasterGradientColourMode {
    PrimaryToSecondary = 0,
    ForegroundToTransparent = 1
};

struct GradientCoverageRequest {
    QSize targetSize;
    QSize documentSize;
    QTransform targetToDocument;
    SelectionMask::Snapshot selectionSnapshot;
};

struct GradientApplyRequest {
    QImage sourceImage;
    QImage selectionCoverage;
    FillTarget target = FillTarget::RasterPixels;
    int componentIndex = -1;
    QPointF start;
    QPointF end;
    RasterGradientType type = RasterGradientType::Linear;
    QColor startColour = Qt::black;
    QColor endColour = Qt::white;
    bool reverse = false;
};

struct GradientApplyResult {
    QImage image;
    QRect affectedRect;
    int changedPixelCount = 0;
    QString error;

    bool succeeded() const { return error.isEmpty() && !image.isNull(); }
    bool changed() const { return changedPixelCount > 0 && !affectedRect.isEmpty(); }
};

QImage buildGradientSelectionCoverage(const GradientCoverageRequest &request,
                                      QString *error = nullptr);

double gradientAmountAt(const QPointF &pixelCentre,
                        const QPointF &start,
                        const QPointF &end,
                        RasterGradientType type,
                        bool reverse);

GradientApplyResult applyGradientCpu(const GradientApplyRequest &request);

QString rasterGradientTypeDisplayName(RasterGradientType type);

} // namespace vfx
