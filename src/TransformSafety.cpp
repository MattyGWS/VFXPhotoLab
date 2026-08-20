#include "TransformSafety.h"

#include <QPolygonF>
#include <QSizeF>

#include <algorithm>
#include <cmath>
#include <limits>

namespace vfx {
namespace {

constexpr int MaximumPersistentExtent = 32768;
constexpr quint64 MaximumPersistentImageBytes = 0xfffffffeULL;
constexpr double MinimumProjectiveDenominator = 1.0e-10;

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

bool finitePoint(const QPointF &point, const double maximumAbsoluteCoordinate)
{
    return std::isfinite(point.x()) && std::isfinite(point.y())
        && std::abs(point.x()) <= maximumAbsoluteCoordinate
        && std::abs(point.y()) <= maximumAbsoluteCoordinate;
}

double projectiveDenominator(const QTransform &transform, const QPointF &point)
{
    return transform.m13() * point.x()
        + transform.m23() * point.y()
        + transform.m33();
}

bool checkedAlignedImageBytes(const QSize &size,
                              const quint64 bytesPerPixel,
                              quint64 *bytes)
{
    if (!bytes || size.isEmpty() || bytesPerPixel == 0) {
        return false;
    }
    const quint64 width = static_cast<quint64>(size.width());
    const quint64 height = static_cast<quint64>(size.height());
    if (width > std::numeric_limits<quint64>::max() / bytesPerPixel) {
        return false;
    }
    const quint64 activeRowBytes = width * bytesPerPixel;
    if (activeRowBytes > std::numeric_limits<quint64>::max() - 3u) {
        return false;
    }
    const quint64 rowBytes = (activeRowBytes + 3u) & ~quint64(3u);
    if (height > 0 && rowBytes > std::numeric_limits<quint64>::max() / height) {
        return false;
    }
    *bytes = rowBytes * height;
    return *bytes <= MaximumPersistentImageBytes;
}

} // namespace

bool transformMatrixIsFiniteAndBounded(const QTransform &transform,
                                       const double maximumAbsoluteValue)
{
    if (!std::isfinite(maximumAbsoluteValue) || maximumAbsoluteValue <= 0.0) {
        return false;
    }
    const double values[] = {
        transform.m11(), transform.m12(), transform.m13(),
        transform.m21(), transform.m22(), transform.m23(),
        transform.m31(), transform.m32(), transform.m33(),
    };
    for (const double value : values) {
        if (!std::isfinite(value) || std::abs(value) > maximumAbsoluteValue) {
            return false;
        }
    }
    return true;
}

bool transformHasSafeDomain(const QTransform &transform,
                            const QRectF &sourceBounds,
                            const double maximumAbsoluteCoordinate,
                            QString *errorMessage)
{
    if (!transformMatrixIsFiniteAndBounded(transform)
        || !sourceBounds.isValid() || sourceBounds.isEmpty()
        || !std::isfinite(maximumAbsoluteCoordinate)
        || maximumAbsoluteCoordinate <= 0.0) {
        setError(errorMessage,
                 QStringLiteral("The transform contains invalid matrix or source-bound values."));
        return false;
    }

    const QRectF bounds = sourceBounds.normalized();
    const QPointF sourcePoints[] = {
        bounds.topLeft(), bounds.topRight(), bounds.bottomRight(), bounds.bottomLeft(),
        QPointF(bounds.center().x(), bounds.top()),
        QPointF(bounds.right(), bounds.center().y()),
        QPointF(bounds.center().x(), bounds.bottom()),
        QPointF(bounds.left(), bounds.center().y()),
        bounds.center(),
    };

    int denominatorSign = 0;
    for (const QPointF &sourcePoint : sourcePoints) {
        if (!finitePoint(sourcePoint, maximumAbsoluteCoordinate)) {
            setError(errorMessage,
                     QStringLiteral("The transform source lies outside the supported coordinate range."));
            return false;
        }
        const double denominator = projectiveDenominator(transform, sourcePoint);
        if (!std::isfinite(denominator)
            || std::abs(denominator) <= MinimumProjectiveDenominator) {
            setError(errorMessage,
                     QStringLiteral("The transform crosses or approaches a projective horizon."));
            return false;
        }
        const int sign = denominator > 0.0 ? 1 : -1;
        if (denominatorSign == 0) {
            denominatorSign = sign;
        } else if (denominatorSign != sign) {
            setError(errorMessage,
                     QStringLiteral("The transform crosses a projective horizon inside the source region."));
            return false;
        }
        const QPointF mapped = transform.map(sourcePoint);
        if (!finitePoint(mapped, maximumAbsoluteCoordinate)) {
            setError(errorMessage,
                     QStringLiteral("The transformed content exceeds the supported coordinate range."));
            return false;
        }
    }

    if (!transform.isInvertible()) {
        setError(errorMessage,
                 QStringLiteral("The transform is singular and cannot be sampled safely."));
        return false;
    }
    return true;
}

bool planTransformStorage(const QSize &referenceSize,
                          const QPointF &referenceOrigin,
                          const QTransform &transform,
                          const QSize &fallbackSize,
                          const quint64 bytesPerPixel,
                          const quint64 maximumPreparedBytes,
                          quint64 *runningPreparedBytes,
                          TransformStoragePlan *plan,
                          QString *errorMessage)
{
    if (!plan || !runningPreparedBytes || bytesPerPixel == 0) {
        setError(errorMessage,
                 QStringLiteral("The transform storage preflight was not configured correctly."));
        return false;
    }
    *plan = {};
    const QSize extent = referenceSize.isValid() && !referenceSize.isEmpty()
        ? referenceSize : fallbackSize;
    if (extent.isEmpty() || extent.width() > MaximumPersistentExtent
        || extent.height() > MaximumPersistentExtent
        || !std::isfinite(referenceOrigin.x())
        || !std::isfinite(referenceOrigin.y())) {
        setError(errorMessage,
                 QStringLiteral("An editable transform payload has invalid reference bounds."));
        return false;
    }

    const QRectF sourceBounds(referenceOrigin, QSizeF(extent));
    if (!transformHasSafeDomain(transform, sourceBounds, 1.0e9, errorMessage)) {
        return false;
    }

    const QRectF mapped = transform.mapRect(sourceBounds).normalized();
    const double leftValue = std::floor(mapped.left());
    const double topValue = std::floor(mapped.top());
    const double rightValue = std::ceil(mapped.right());
    const double bottomValue = std::ceil(mapped.bottom());
    const double widthValue = rightValue - leftValue;
    const double heightValue = bottomValue - topValue;
    const double minimum = static_cast<double>(std::numeric_limits<int>::min());
    const double maximum = static_cast<double>(std::numeric_limits<int>::max());
    if (!mapped.isValid() || mapped.isEmpty()
        || !std::isfinite(leftValue) || !std::isfinite(topValue)
        || !std::isfinite(rightValue) || !std::isfinite(bottomValue)
        || leftValue < minimum || topValue < minimum
        || rightValue > maximum || bottomValue > maximum
        || widthValue < 1.0 || heightValue < 1.0
        || widthValue > MaximumPersistentExtent
        || heightValue > MaximumPersistentExtent) {
        setError(errorMessage,
                 QStringLiteral("The transformed payload would exceed the supported 32768-pixel storage extent."));
        return false;
    }

    const int left = static_cast<int>(leftValue);
    const int top = static_cast<int>(topValue);
    const int width = static_cast<int>(widthValue);
    const int height = static_cast<int>(heightValue);
    const QSize outputSize(width, height);
    quint64 bytes = 0;
    if (!checkedAlignedImageBytes(outputSize, bytesPerPixel, &bytes)) {
        setError(errorMessage,
                 QStringLiteral("The transformed payload exceeds the exact persistent snapshot limit."));
        return false;
    }
    if (*runningPreparedBytes > std::numeric_limits<quint64>::max() - bytes) {
        setError(errorMessage,
                 QStringLiteral("The transform preparation byte estimate overflowed."));
        return false;
    }
    const quint64 combined = *runningPreparedBytes + bytes;
    if (maximumPreparedBytes > 0 && combined > maximumPreparedBytes) {
        constexpr quint64 MiB = 1024uLL * 1024uLL;
        const quint64 requiredMiB = combined / MiB + ((combined % MiB) != 0u);
        const quint64 budgetMiB = maximumPreparedBytes / MiB
            + ((maximumPreparedBytes % MiB) != 0u);
        setError(errorMessage,
                 QStringLiteral("The transformed editable payloads would require approximately %1 MiB, exceeding the current safe preparation budget of %2 MiB.")
                     .arg(requiredMiB)
                     .arg(budgetMiB));
        return false;
    }

    *runningPreparedBytes = combined;
    plan->bounds = QRect(QPoint(left, top), outputSize);
    plan->bytes = bytes;
    return true;
}

} // namespace vfx
