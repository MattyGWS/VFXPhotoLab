#pragma once

#include "Adjustment.h"

#include <QImage>
#include <QVector>

#include <array>

namespace vfx {

// Precombined master + component lookup used by Levels, Curves and future
// pointwise tonal tools. 8-bit rendering uses 256 entries; the exact 16-bit
// CPU reference uses all 65,536 source codes.
struct TonalLookupTable {
    int maximumValue = 0;
    std::array<QVector<quint16>, 3> channels;

    bool isValid() const;
    quint16 map(int component, int value) const;
    QImage toRgba8Image() const;
    qsizetype retainedBytes() const;

private:
    mutable QImage m_rgba8Image;
};

double evaluateCurveChannel(const CurveChannelParameters &channel,
                            CurveInterpolation interpolation,
                            double input);

bool adjustmentUsesTonalLookup(AdjustmentType type);
bool adjustmentUsesLuminanceLookup(AdjustmentType type);
TonalLookupTable buildTonalLookup(const AdjustmentData &adjustment,
                                  int bitDepth);

} // namespace vfx
