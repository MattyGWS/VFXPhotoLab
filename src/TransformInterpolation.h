#pragma once

#include <QtGlobal>

namespace vfx {

enum class TransformInterpolation : quint8 {
    NearestNeighbour = 0,
    Bilinear = 1,
    Bicubic = 2,
    Lanczos3 = 3
};

} // namespace vfx
