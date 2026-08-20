#pragma once

#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTransform>
#include <QtGlobal>

namespace vfx {

// Shared transform preflight used before any allocation-bearing Apply path.
// The public project format permits off-canvas origins, but QImage-backed
// payloads still need finite, bounded coordinates and exact snapshot-safe
// dimensions. These checks deliberately run before mutating the live layer tree.
struct TransformStoragePlan {
    QRect bounds;
    quint64 bytes = 0;
};

bool transformMatrixIsFiniteAndBounded(
    const QTransform &transform,
    double maximumAbsoluteValue = 1.0e12);

bool transformHasSafeDomain(
    const QTransform &transform,
    const QRectF &sourceBounds,
    double maximumAbsoluteCoordinate = 1.0e9,
    QString *errorMessage = nullptr);

bool planTransformStorage(
    const QSize &referenceSize,
    const QPointF &referenceOrigin,
    const QTransform &transform,
    const QSize &fallbackSize,
    quint64 bytesPerPixel,
    quint64 maximumPreparedBytes,
    quint64 *runningPreparedBytes,
    TransformStoragePlan *plan,
    QString *errorMessage = nullptr);

} // namespace vfx
