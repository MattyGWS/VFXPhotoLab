#pragma once

#include "TransformInterpolation.h"

#include <QImage>
#include <QPointF>
#include <QRgba64>
#include <QSize>

namespace vfx {

// Sample straight, unassociated components. RGB is never multiplied by Alpha,
// so colour beneath zero Alpha survives transforms exactly as editable data.
QRgba64 sampleTransformRgba64(const QImage &rgba64,
                              const QSize &localExtent,
                              const QPointF &localPoint,
                              TransformInterpolation interpolation,
                              QRgba64 outside = QRgba64::fromRgba64(0, 0, 0, 0));

quint16 sampleTransformGrey16(const QImage &grey16,
                              const QSize &localExtent,
                              const QPointF &localPoint,
                              TransformInterpolation interpolation,
                              quint16 outside = 0,
                              bool compactConstant = false);

} // namespace vfx
