#pragma once

#include <QPointF>

#include <algorithm>
#include <cmath>

namespace vfx {

// Document coordinates describe pixel boundaries at integers and pixel centres
// at half-integers. Guides may use both; vector anchors use boundaries only.
inline double snapGuideCoordinate(const double value, const double maximum)
{
    const double safeMaximum = std::max(0.0, maximum);
    return std::clamp(std::round(value * 2.0) * 0.5, 0.0, safeMaximum);
}

inline double snapVectorBoundaryCoordinate(const double value)
{
    return std::round(value);
}

inline QPointF snapVectorBoundaryPoint(const QPointF &point)
{
    return QPointF(snapVectorBoundaryCoordinate(point.x()),
                   snapVectorBoundaryCoordinate(point.y()));
}

inline QPointF constrainPixelBoundaryPointTo45(const QPointF &start,
                                                const QPointF &point)
{
    const QPointF snappedStart = snapVectorBoundaryPoint(start);
    const QPointF snappedPoint = snapVectorBoundaryPoint(point);
    const QPointF delta = snappedPoint - snappedStart;
    if (qFuzzyIsNull(delta.x()) && qFuzzyIsNull(delta.y())) return snappedPoint;

    constexpr double QuarterTurnStep = 0.78539816339744830962;
    int octant = static_cast<int>(std::llround(
        std::atan2(delta.y(), delta.x()) / QuarterTurnStep));
    octant = ((octant % 8) + 8) % 8;
    const double extent = std::max(std::abs(delta.x()), std::abs(delta.y()));
    switch (octant) {
    case 0: return QPointF(snappedStart.x() + extent, snappedStart.y());
    case 1: return QPointF(snappedStart.x() + extent, snappedStart.y() + extent);
    case 2: return QPointF(snappedStart.x(), snappedStart.y() + extent);
    case 3: return QPointF(snappedStart.x() - extent, snappedStart.y() + extent);
    case 4: return QPointF(snappedStart.x() - extent, snappedStart.y());
    case 5: return QPointF(snappedStart.x() - extent, snappedStart.y() - extent);
    case 6: return QPointF(snappedStart.x(), snappedStart.y() - extent);
    case 7: return QPointF(snappedStart.x() + extent, snappedStart.y() - extent);
    }
    return snappedPoint;
}

inline bool isPixelBoundaryCoordinate(const double value)
{
    return std::isfinite(value)
        && std::abs(value - std::round(value)) <= 1.0e-9;
}

} // namespace vfx
