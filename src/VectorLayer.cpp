#include "VectorLayer.h"

#include "TransformSafety.h"

#include <QJsonArray>
#include <QList>
#include <QLineF>
#include <QPainterPathStroker>
#include <QPolygonF>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <utility>

namespace vfx {
namespace {

constexpr double MaximumCoordinate = 1.0e9;
constexpr double MaximumExtent = 1.0e9;
constexpr double MinimumStrokeWidth = 0.01;
constexpr double MaximumStrokeWidth = 1.0e6;
constexpr double MinimumDashLength = 0.01;
constexpr double MaximumDashLength = 1.0e6;
constexpr double MaximumDashOffset = 1.0e9;
constexpr double MinimumArrowScale = 0.1;
constexpr double MaximumArrowScale = 10.0;
constexpr int MaximumExpandedStrokeElements = 400000;
constexpr int MaximumExpandedDashCount = 12000;
constexpr quint64 FnvOffset = 1469598103934665603ULL;
constexpr quint64 FnvPrime = 1099511628211ULL;
constexpr double CircleKappa = 0.5522847498307936;

bool finiteBounded(const double value, const double maximum = MaximumCoordinate)
{
    return std::isfinite(value) && std::abs(value) <= maximum;
}

bool safePoint(const QPointF &point)
{
    return finiteBounded(point.x()) && finiteBounded(point.y());
}

bool validShapeType(const VectorShapeType type)
{
    switch (type) {
    case VectorShapeType::Rectangle:
    case VectorShapeType::RoundedRectangle:
    case VectorShapeType::Ellipse:
    case VectorShapeType::Line:
    case VectorShapeType::Polygon:
    case VectorShapeType::Star:
    case VectorShapeType::Arrow:
    case VectorShapeType::Path:
        return true;
    }
    return false;
}

bool validStrokeAlignment(const VectorStrokeAlignment alignment)
{
    return alignment == VectorStrokeAlignment::Inside
        || alignment == VectorStrokeAlignment::Centre
        || alignment == VectorStrokeAlignment::Outside;
}

bool validStrokeCap(const VectorStrokeCap cap)
{
    return cap == VectorStrokeCap::Butt
        || cap == VectorStrokeCap::Round
        || cap == VectorStrokeCap::Square;
}

bool validStrokeJoin(const VectorStrokeJoin join)
{
    return join == VectorStrokeJoin::Miter
        || join == VectorStrokeJoin::Round
        || join == VectorStrokeJoin::Bevel;
}

bool validStrokePattern(const VectorStrokePattern pattern)
{
    return pattern == VectorStrokePattern::Solid
        || pattern == VectorStrokePattern::Dashed;
}

bool validArrowheadType(const VectorArrowheadType type)
{
    switch (type) {
    case VectorArrowheadType::None:
    case VectorArrowheadType::Open:
    case VectorArrowheadType::Triangle:
    case VectorArrowheadType::Stealth:
    case VectorArrowheadType::Diamond:
    case VectorArrowheadType::Circle:
        return true;
    }
    return false;
}

bool validPathFillRule(const VectorPathFillRule fillRule)
{
    return fillRule == VectorPathFillRule::EvenOdd
        || fillRule == VectorPathFillRule::NonZero;
}

QString pathFillRuleToString(const VectorPathFillRule fillRule)
{
    return fillRule == VectorPathFillRule::NonZero
        ? QStringLiteral("nonzero") : QStringLiteral("evenodd");
}

VectorPathFillRule pathFillRuleFromString(const QString &value, bool *ok = nullptr)
{
    const QString token = value.trimmed().toLower();
    if (token == QStringLiteral("evenodd")) {
        if (ok) *ok = true;
        return VectorPathFillRule::EvenOdd;
    }
    if (token == QStringLiteral("nonzero")
        || token == QStringLiteral("winding")) {
        if (ok) *ok = true;
        return VectorPathFillRule::NonZero;
    }
    if (ok) *ok = false;
    return VectorPathFillRule::EvenOdd;
}

bool validCornerStyle(const VectorCornerStyle style)
{
    return style == VectorCornerStyle::Rounded
        || style == VectorCornerStyle::Chamfer
        || style == VectorCornerStyle::Concave
        || style == VectorCornerStyle::Cutout;
}

void hashBytes(quint64 &hash, const void *data, const qsizetype size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (qsizetype index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= FnvPrime;
    }
}

template<typename T>
void hashValue(quint64 &hash, const T &value)
{
    hashBytes(hash, &value, sizeof(T));
}

QJsonObject transformToJson(const QTransform &transform)
{
    QJsonObject object;
    object.insert(QStringLiteral("m11"), transform.m11());
    object.insert(QStringLiteral("m12"), transform.m12());
    object.insert(QStringLiteral("m13"), transform.m13());
    object.insert(QStringLiteral("m21"), transform.m21());
    object.insert(QStringLiteral("m22"), transform.m22());
    object.insert(QStringLiteral("m23"), transform.m23());
    object.insert(QStringLiteral("dx"), transform.dx());
    object.insert(QStringLiteral("dy"), transform.dy());
    object.insert(QStringLiteral("m33"), transform.m33());
    return object;
}

QTransform transformFromJson(const QJsonObject &object, bool *ok)
{
    const QTransform transform(
        object.value(QStringLiteral("m11")).toDouble(1.0),
        object.value(QStringLiteral("m12")).toDouble(0.0),
        object.value(QStringLiteral("m13")).toDouble(0.0),
        object.value(QStringLiteral("m21")).toDouble(0.0),
        object.value(QStringLiteral("m22")).toDouble(1.0),
        object.value(QStringLiteral("m23")).toDouble(0.0),
        object.value(QStringLiteral("dx")).toDouble(0.0),
        object.value(QStringLiteral("dy")).toDouble(0.0),
        object.value(QStringLiteral("m33")).toDouble(1.0));
    const bool valid = transform.isInvertible()
        && transformMatrixIsFiniteAndBounded(transform);
    if (ok) *ok = valid;
    return valid ? transform : QTransform();
}

QJsonObject rectToJson(const QRectF &rect)
{
    QJsonObject object;
    object.insert(QStringLiteral("x"), rect.x());
    object.insert(QStringLiteral("y"), rect.y());
    object.insert(QStringLiteral("width"), rect.width());
    object.insert(QStringLiteral("height"), rect.height());
    return object;
}

QRectF rectFromJson(const QJsonObject &object,
                    const bool allowDegenerate,
                    bool *ok)
{
    const QRectF rect(object.value(QStringLiteral("x")).toDouble(),
                      object.value(QStringLiteral("y")).toDouble(),
                      object.value(QStringLiteral("width")).toDouble(),
                      object.value(QStringLiteral("height")).toDouble());
    const QRectF normalised = rect.normalized();
    const bool extentsValid = allowDegenerate
        ? normalised.width() >= 0.0 && normalised.height() >= 0.0
        : normalised.width() > 0.0 && normalised.height() > 0.0;
    const bool valid = finiteBounded(normalised.x())
        && finiteBounded(normalised.y())
        && std::isfinite(normalised.width())
        && std::isfinite(normalised.height())
        && extentsValid
        && normalised.width() <= MaximumExtent
        && normalised.height() <= MaximumExtent;
    if (ok) *ok = valid;
    return valid ? normalised : QRectF();
}

QJsonObject pointToJson(const QPointF &point)
{
    return {{QStringLiteral("x"), point.x()},
            {QStringLiteral("y"), point.y()}};
}

QPointF pointFromJson(const QJsonObject &object, bool *ok)
{
    const QJsonValue x = object.value(QStringLiteral("x"));
    const QJsonValue y = object.value(QStringLiteral("y"));
    const bool valid = x.isDouble() && y.isDouble()
        && finiteBounded(x.toDouble()) && finiteBounded(y.toDouble());
    if (ok) *ok = valid;
    return valid ? QPointF(x.toDouble(), y.toDouble()) : QPointF();
}

QJsonObject colourToJson(const QColor &colour)
{
    const QRgba64 rgba = colour.rgba64();
    QJsonObject object;
    object.insert(QStringLiteral("red"), static_cast<int>(rgba.red()));
    object.insert(QStringLiteral("green"), static_cast<int>(rgba.green()));
    object.insert(QStringLiteral("blue"), static_cast<int>(rgba.blue()));
    object.insert(QStringLiteral("alpha"), static_cast<int>(rgba.alpha()));
    return object;
}

QColor colourFromJson(const QJsonObject &object, bool *ok)
{
    const int red = object.value(QStringLiteral("red")).toInt(-1);
    const int green = object.value(QStringLiteral("green")).toInt(-1);
    const int blue = object.value(QStringLiteral("blue")).toInt(-1);
    const int alpha = object.value(QStringLiteral("alpha")).toInt(-1);
    const bool valid = red >= 0 && red <= 65535
        && green >= 0 && green <= 65535
        && blue >= 0 && blue <= 65535
        && alpha >= 0 && alpha <= 65535;
    if (ok) *ok = valid;
    return valid
        ? QColor::fromRgba64(QRgba64::fromRgba64(
              static_cast<quint16>(red), static_cast<quint16>(green),
              static_cast<quint16>(blue), static_cast<quint16>(alpha)))
        : QColor(Qt::black);
}

QString strokeAlignmentToString(const VectorStrokeAlignment alignment)
{
    switch (alignment) {
    case VectorStrokeAlignment::Inside: return QStringLiteral("inside");
    case VectorStrokeAlignment::Centre: return QStringLiteral("centre");
    case VectorStrokeAlignment::Outside: return QStringLiteral("outside");
    }
    return QStringLiteral("centre");
}

VectorStrokeAlignment strokeAlignmentFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString token = value.trimmed().toLower();
    if (token == QStringLiteral("inside")) return VectorStrokeAlignment::Inside;
    if (token == QStringLiteral("centre") || token == QStringLiteral("center")) {
        return VectorStrokeAlignment::Centre;
    }
    if (token == QStringLiteral("outside")) return VectorStrokeAlignment::Outside;
    if (ok) *ok = false;
    return VectorStrokeAlignment::Centre;
}

QString strokeCapToString(const VectorStrokeCap cap)
{
    switch (cap) {
    case VectorStrokeCap::Butt: return QStringLiteral("butt");
    case VectorStrokeCap::Round: return QStringLiteral("round");
    case VectorStrokeCap::Square: return QStringLiteral("square");
    }
    return QStringLiteral("butt");
}

VectorStrokeCap strokeCapFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString token = value.trimmed().toLower();
    if (token == QStringLiteral("butt")) return VectorStrokeCap::Butt;
    if (token == QStringLiteral("round")) return VectorStrokeCap::Round;
    if (token == QStringLiteral("square")) return VectorStrokeCap::Square;
    if (ok) *ok = false;
    return VectorStrokeCap::Butt;
}

QString strokePatternToString(const VectorStrokePattern pattern)
{
    switch (pattern) {
    case VectorStrokePattern::Solid: return QStringLiteral("solid");
    case VectorStrokePattern::Dashed: return QStringLiteral("dashed");
    }
    return QStringLiteral("solid");
}

VectorStrokePattern strokePatternFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString token = value.trimmed().toLower();
    if (token == QStringLiteral("solid")) return VectorStrokePattern::Solid;
    if (token == QStringLiteral("dashed") || token == QStringLiteral("dash")) {
        return VectorStrokePattern::Dashed;
    }
    if (ok) *ok = false;
    return VectorStrokePattern::Solid;
}

QString strokeJoinToString(const VectorStrokeJoin join)
{
    switch (join) {
    case VectorStrokeJoin::Miter: return QStringLiteral("miter");
    case VectorStrokeJoin::Round: return QStringLiteral("round");
    case VectorStrokeJoin::Bevel: return QStringLiteral("bevel");
    }
    return QStringLiteral("miter");
}

VectorStrokeJoin strokeJoinFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString token = value.trimmed().toLower();
    if (token == QStringLiteral("miter") || token == QStringLiteral("mitre")) {
        return VectorStrokeJoin::Miter;
    }
    if (token == QStringLiteral("round")) return VectorStrokeJoin::Round;
    if (token == QStringLiteral("bevel")) return VectorStrokeJoin::Bevel;
    if (ok) *ok = false;
    return VectorStrokeJoin::Miter;
}

Qt::PenCapStyle qtCapStyle(const VectorStrokeCap cap)
{
    switch (cap) {
    case VectorStrokeCap::Butt: return Qt::FlatCap;
    case VectorStrokeCap::Round: return Qt::RoundCap;
    case VectorStrokeCap::Square: return Qt::SquareCap;
    }
    return Qt::FlatCap;
}

Qt::PenJoinStyle qtJoinStyle(const VectorStrokeJoin join)
{
    switch (join) {
    case VectorStrokeJoin::Miter: return Qt::MiterJoin;
    case VectorStrokeJoin::Round: return Qt::RoundJoin;
    case VectorStrokeJoin::Bevel: return Qt::BevelJoin;
    }
    return Qt::MiterJoin;
}

QPainterPath roundedParallelogramPath(const QPointF &topLeft,
                                      const QPointF &horizontal,
                                      const QPointF &vertical,
                                      VectorCornerRadii radii)
{
    const double width = std::hypot(horizontal.x(), horizontal.y());
    const double height = std::hypot(vertical.x(), vertical.y());
    if (width <= 1.0e-12 || height <= 1.0e-12) return {};
    radii.normalise(QSizeF(width, height));

    const QPointF ux(horizontal.x() / width, horizontal.y() / width);
    const QPointF uy(vertical.x() / height, vertical.y() / height);
    const QPointF topRight = topLeft + horizontal;
    const QPointF bottomRight = topRight + vertical;
    const QPointF bottomLeft = topLeft + vertical;
    const auto along = [](const QPointF &axis, const double distance) {
        return QPointF(axis.x() * distance, axis.y() * distance);
    };

    QPainterPath path;
    path.moveTo(topLeft + along(ux, radii.topLeft));
    path.lineTo(topRight - along(ux, radii.topRight));
    if (radii.topRight > 0.0) {
        const QPointF start = topRight - along(ux, radii.topRight);
        const QPointF end = topRight + along(uy, radii.topRight);
        path.cubicTo(start + along(ux, CircleKappa * radii.topRight),
                     end - along(uy, CircleKappa * radii.topRight), end);
    } else path.lineTo(topRight);

    path.lineTo(bottomRight - along(uy, radii.bottomRight));
    if (radii.bottomRight > 0.0) {
        const QPointF start = bottomRight - along(uy, radii.bottomRight);
        const QPointF end = bottomRight - along(ux, radii.bottomRight);
        path.cubicTo(start + along(uy, CircleKappa * radii.bottomRight),
                     end + along(ux, CircleKappa * radii.bottomRight), end);
    } else path.lineTo(bottomRight);

    path.lineTo(bottomLeft + along(ux, radii.bottomLeft));
    if (radii.bottomLeft > 0.0) {
        const QPointF start = bottomLeft + along(ux, radii.bottomLeft);
        const QPointF end = bottomLeft - along(uy, radii.bottomLeft);
        path.cubicTo(start - along(ux, CircleKappa * radii.bottomLeft),
                     end + along(uy, CircleKappa * radii.bottomLeft), end);
    } else path.lineTo(bottomLeft);

    path.lineTo(topLeft + along(uy, radii.topLeft));
    if (radii.topLeft > 0.0) {
        const QPointF start = topLeft + along(uy, radii.topLeft);
        const QPointF end = topLeft + along(ux, radii.topLeft);
        path.cubicTo(start - along(uy, CircleKappa * radii.topLeft),
                     end - along(ux, CircleKappa * radii.topLeft), end);
    } else path.lineTo(topLeft);
    path.closeSubpath();
    return path;
}

QPainterPath roundedRectPath(const QRectF &rect, const VectorCornerRadii &radii)
{
    return roundedParallelogramPath(rect.topLeft(),
                                    QPointF(rect.width(), 0.0),
                                    QPointF(0.0, rect.height()), radii);
}

QVector<QPointF> regularVertices(const QRectF &rect,
                                 const int sides,
                                 const double rotationDegrees,
                                 const bool star,
                                 const double innerRatio)
{
    QVector<QPointF> vertices;
    const int count = star ? sides * 2 : sides;
    vertices.reserve(count);
    const QPointF centre = rect.center();
    const double rx = rect.width() * 0.5;
    const double ry = rect.height() * 0.5;
    const double rotation = rotationDegrees * std::numbers::pi / 180.0;
    for (int index = 0; index < count; ++index) {
        const double angle = rotation + (2.0 * std::numbers::pi * index / count);
        const double ratio = star && (index % 2 == 1) ? innerRatio : 1.0;
        vertices.push_back(QPointF(centre.x() + std::cos(angle) * rx * ratio,
                                   centre.y() + std::sin(angle) * ry * ratio));
    }
    return vertices;
}

QPainterPath polygonPath(const QVector<QPointF> &vertices)
{
    QPainterPath path;
    if (vertices.size() < 3) return path;
    path.moveTo(vertices.constFirst());
    for (int index = 1; index < vertices.size(); ++index) path.lineTo(vertices.at(index));
    path.closeSubpath();
    path.setFillRule(Qt::WindingFill);
    return path;
}

QPainterPath blockArrowPath(const QRectF &rect,
                            const double headLengthRatio,
                            const double shaftWidthRatio)
{
    const QRectF bounds = rect.normalized();
    if (bounds.width() <= 0.0 || bounds.height() <= 0.0) return {};
    const double headRatio = std::clamp(headLengthRatio, 0.1, 0.9);
    const double shaftRatio = std::clamp(shaftWidthRatio, 0.05, 0.95);
    const double headStart = bounds.right() - bounds.width() * headRatio;
    const double halfShaft = bounds.height() * shaftRatio * 0.5;
    const double centreY = bounds.center().y();
    QPainterPath path;
    path.moveTo(bounds.left(), centreY - halfShaft);
    path.lineTo(headStart, centreY - halfShaft);
    path.lineTo(headStart, bounds.top());
    path.lineTo(bounds.right(), centreY);
    path.lineTo(headStart, bounds.bottom());
    path.lineTo(headStart, centreY + halfShaft);
    path.lineTo(bounds.left(), centreY + halfShaft);
    path.closeSubpath();
    path.setFillRule(Qt::WindingFill);
    return path;
}

bool endpointFrame(const QPainterPath &path, const bool start,
                   QPointF *tip, QPointF *outward)
{
    if (!tip || !outward || path.isEmpty()) return false;
    const double length = path.length();
    if (!std::isfinite(length) || length <= 1.0e-8) return false;
    // Sample very close to the endpoint so a tiny first/last segment keeps
    // its own tangent instead of inheriting the direction of a later segment.
    const double sampleDistance = std::clamp(
        length * 1.0e-4, 1.0e-5, std::max(1.0e-5, std::min(1.0, length * 0.01)));
    const double interiorLength = start ? sampleDistance : length - sampleDistance;
    const double percent = path.percentAtLength(std::clamp(interiorLength, 0.0, length));
    const QPointF endpoint = path.pointAtPercent(start ? 0.0 : 1.0);
    QPointF interior = path.pointAtPercent(percent);
    QPointF direction = endpoint - interior;
    double magnitude = std::hypot(direction.x(), direction.y());
    if (magnitude <= 1.0e-8) {
        const double fallbackPercent = start ? 0.001 : 0.999;
        interior = path.pointAtPercent(fallbackPercent);
        direction = endpoint - interior;
        magnitude = std::hypot(direction.x(), direction.y());
    }
    if (!safePoint(endpoint) || magnitude <= 1.0e-8 || !std::isfinite(magnitude)) return false;
    *tip = endpoint;
    *outward = direction / magnitude;
    return safePoint(*outward);
}

QPainterPath arrowheadPath(const QPainterPath &centreLine,
                           const VectorArrowheadType type,
                           const double scale,
                           const double strokeWidth,
                           const bool start)
{
    if (type == VectorArrowheadType::None || centreLine.isEmpty()
        || !std::isfinite(scale) || !std::isfinite(strokeWidth)
        || scale <= 0.0 || strokeWidth <= 0.0) return {};
    QPointF endpoint;
    QPointF direction;
    if (!endpointFrame(centreLine, start, &endpoint, &direction)) return {};
    const QPointF normal(-direction.y(), direction.x());
    const double effectiveScale = std::clamp(scale, MinimumArrowScale, MaximumArrowScale);
    const double width = strokeWidth * effectiveScale;
    QPainterPath result;
    switch (type) {
    case VectorArrowheadType::None:
        return {};
    case VectorArrowheadType::Open: {
        const double length = width * 4.0;
        const double halfWidth = width * 1.8;
        // The path endpoint is the marker's longitudinal centre rather than
        // its sharp tip. This leaves half of the marker in front of the
        // centreline endpoint, so a round/square shaft cap cannot protrude
        // through and blunt the visible point.
        const QPointF tip = endpoint + direction * (length * 0.5);
        const QPointF base = endpoint - direction * (length * 0.5);
        QPainterPath arms;
        arms.moveTo(base + normal * halfWidth);
        arms.lineTo(tip);
        arms.lineTo(base - normal * halfWidth);
        QPainterPathStroker stroker;
        stroker.setWidth(std::max(0.01, strokeWidth * effectiveScale * 0.7));
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        result = stroker.createStroke(arms);
        break;
    }
    case VectorArrowheadType::Triangle: {
        const double length = width * 4.0;
        const double halfWidth = width * 2.0;
        const QPointF tip = endpoint + direction * (length * 0.5);
        const QPointF base = endpoint - direction * (length * 0.5);
        result.moveTo(tip);
        result.lineTo(base + normal * halfWidth);
        result.lineTo(base - normal * halfWidth);
        result.closeSubpath();
        break;
    }
    case VectorArrowheadType::Stealth: {
        const double length = width * 4.6;
        const double halfWidth = width * 2.0;
        const QPointF tip = endpoint + direction * (length * 0.5);
        const QPointF base = endpoint - direction * (length * 0.5);
        const QPointF notch = tip - direction * (length * 0.58);
        result.moveTo(tip);
        result.lineTo(base + normal * halfWidth);
        result.lineTo(notch);
        result.lineTo(base - normal * halfWidth);
        result.closeSubpath();
        break;
    }
    case VectorArrowheadType::Diamond: {
        const double length = width * 3.8;
        const double halfWidth = width * 1.55;
        const QPointF tip = endpoint + direction * (length * 0.5);
        const QPointF rear = endpoint - direction * (length * 0.5);
        const QPointF middle = endpoint;
        result.moveTo(tip);
        result.lineTo(middle + normal * halfWidth);
        result.lineTo(rear);
        result.lineTo(middle - normal * halfWidth);
        result.closeSubpath();
        break;
    }
    case VectorArrowheadType::Circle: {
        const double radius = width * 1.5;
        result.addEllipse(endpoint, radius, radius);
        break;
    }
    }
    result.setFillRule(Qt::WindingFill);
    return result;
}

QPainterPath clipStrokeCapAtEndpoint(const QPainterPath &outline,
                                     const QPainterPath &centreLine,
                                     const bool start,
                                     const double strokeWidth,
                                     const VectorStrokeCap cap)
{
    if (outline.isEmpty() || centreLine.isEmpty()
        || !std::isfinite(strokeWidth) || strokeWidth <= 0.0
        || cap == VectorStrokeCap::Butt) {
        return outline;
    }
    QPointF endpoint;
    QPointF outward;
    if (!endpointFrame(centreLine, start, &endpoint, &outward)) return outline;
    const QPointF normal(-outward.y(), outward.x());

    // Work in an endpoint-local orthonormal frame. Positive local X points
    // out of the path. Only remove the small round/square cap footprint next
    // to this endpoint; clipping against an unbounded half-plane would also
    // erase unrelated portions of a winding open path that happen to lie in
    // front of the endpoint plane.
    const QTransform toLocal(
        outward.x(), normal.x(), outward.y(), normal.y(),
        -(outward.x() * endpoint.x() + outward.y() * endpoint.y()),
        -(normal.x() * endpoint.x() + normal.y() * endpoint.y()));
    bool inverseOk = false;
    const QTransform fromLocal = toLocal.inverted(&inverseOk);
    if (!inverseOk) return outline;

    QPainterPath local = toLocal.map(outline);
    const double halfWidth = strokeWidth * 0.5;
    const double epsilon = std::max(1.0e-6, strokeWidth * 1.0e-6);
    QPainterPath capFootprint;
    capFootprint.addRect(QRectF(-epsilon, -halfWidth - epsilon,
                                halfWidth + 2.0 * epsilon,
                                strokeWidth + 2.0 * epsilon));
    local = local.subtracted(capFootprint);
    local.setFillRule(Qt::WindingFill);
    return fromLocal.map(local);
}

QPainterPath strokeOutline(const QPainterPath &path, const VectorStroke &stroke)
{
    if (!stroke.enabled || path.isEmpty() || stroke.width <= 0.0) return {};
    QPainterPathStroker stroker;
    stroker.setWidth(stroke.width);
    stroker.setCapStyle(qtCapStyle(stroke.cap));
    stroker.setJoinStyle(qtJoinStyle(stroke.join));
    stroker.setMiterLimit(stroke.miterLimit);
    if (stroke.pattern == VectorStrokePattern::Dashed) {
        // QPainterPathStroker follows QPen semantics: dash values and the
        // offset are expressed in multiples of the stroke width. The public
        // vector model stores all three in document pixels, so normalise them
        // here at the final geometry boundary.
        const double inverseWidth = 1.0 / stroke.width;
        stroker.setDashPattern(QList<qreal>{stroke.dashLength * inverseWidth,
                                             stroke.gapLength * inverseWidth});
        stroker.setDashOffset(stroke.dashOffset * inverseWidth);
    }
    QPainterPath outline = stroker.createStroke(path);
    outline.setFillRule(Qt::WindingFill);
    return outline;
}

double polygonAreaMagnitude(const QPolygonF &polygon)
{
    if (polygon.size() < 3) return 0.0;
    // Translate before applying the shoelace formula so contours positioned
    // far from the origin do not lose their small local area to cancellation.
    const QPointF origin = polygon.constFirst();
    long double twiceArea = 0.0L;
    for (qsizetype index = 0; index < polygon.size(); ++index) {
        const QPointF a = polygon.at(index) - origin;
        const QPointF b = polygon.at((index + 1) % polygon.size()) - origin;
        twiceArea += static_cast<long double>(a.x()) * b.y()
            - static_cast<long double>(a.y()) * b.x();
    }
    return static_cast<double>(std::abs(twiceArea) * 0.5L);
}

bool hasMeaningfulFillArea(const QPainterPath &path)
{
    if (path.isEmpty() || path.elementCount() < 2) return false;
    const QRectF bounds = path.boundingRect().normalized();
    if (!bounds.isValid() || !std::isfinite(bounds.width())
        || !std::isfinite(bounds.height())) {
        return false;
    }
    const double boundsArea = bounds.width() * bounds.height();
    if (!std::isfinite(boundsArea) || boundsArea <= 1.0e-16) return false;

    double fillArea = 0.0;
    const QList<QPolygonF> polygons = path.toFillPolygons();
    for (const QPolygonF &polygon : polygons) {
        fillArea += polygonAreaMagnitude(polygon);
        if (!std::isfinite(fillArea)) return false;
    }
    // Stroke widths and dash lengths can validly be as small as 0.01 px, so
    // use a relative threshold only to reject true seam/closure debris.
    return fillArea > std::max(1.0e-16, boundsArea * 1.0e-12);
}

QVector<QPainterPath> splitPainterSubpaths(const QPainterPath &source,
                                           bool *ok)
{
    QVector<QPainterPath> result;
    QPainterPath current;
    bool active = false;
    bool valid = !source.isEmpty();

    const auto finish = [&] {
        if (!active) return;
        current.setFillRule(Qt::WindingFill);
        // QPainterPathStroker can emit duplicate or zero-area fragments at the
        // seam of a closed dashed path. They have no rendered coverage and must
        // not veto all otherwise valid dash contours.
        if (hasMeaningfulFillArea(current)) result.push_back(current);
        current = QPainterPath();
        active = false;
    };

    for (int index = 0; valid && index < source.elementCount(); ++index) {
        const QPainterPath::Element element = source.elementAt(index);
        const QPointF point(element.x, element.y);
        if (!safePoint(point)) {
            valid = false;
            break;
        }
        if (element.isMoveTo()) {
            finish();
            current.moveTo(point);
            active = true;
        } else if (element.isLineTo()) {
            if (!active) {
                valid = false;
                break;
            }
            current.lineTo(point);
        } else if (element.type == QPainterPath::CurveToElement) {
            if (!active || index + 2 >= source.elementCount()) {
                valid = false;
                break;
            }
            const QPainterPath::Element control2 = source.elementAt(index + 1);
            const QPainterPath::Element endpoint = source.elementAt(index + 2);
            if (control2.type != QPainterPath::CurveToDataElement
                || endpoint.type != QPainterPath::CurveToDataElement
                || !safePoint(QPointF(control2.x, control2.y))
                || !safePoint(QPointF(endpoint.x, endpoint.y))) {
                valid = false;
                break;
            }
            current.cubicTo(point,
                            QPointF(control2.x, control2.y),
                            QPointF(endpoint.x, endpoint.y));
            index += 2;
        } else {
            valid = false;
        }
    }
    if (valid) finish();
    valid = valid && !result.isEmpty();
    if (ok) *ok = valid;
    return valid ? result : QVector<QPainterPath>();
}

bool equivalentFillCoverage(const QPainterPath &source,
                            const QPainterPath &candidate)
{
    if (source.isEmpty() || candidate.isEmpty()) return false;
    const QRectF bounds = source.boundingRect().united(candidate.boundingRect());
    if (!bounds.isValid() || bounds.isEmpty()) return false;
    constexpr int Samples = 81;
    for (int y = 0; y < Samples; ++y) {
        const double fy = (y + 0.5) / Samples;
        for (int x = 0; x < Samples; ++x) {
            const double fx = (x + 0.5) / Samples;
            const QPointF point(bounds.left() + bounds.width() * fx,
                                bounds.top() + bounds.height() * fy);
            if (source.contains(point) != candidate.contains(point)) return false;
        }
    }
    return true;
}

QPainterPath combinedContourPath(const QVector<QPainterPath> &subpaths,
                                  const Qt::FillRule fillRule)
{
    QPainterPath result;
    result.setFillRule(fillRule);
    for (const QPainterPath &subpath : subpaths) {
        if (!subpath.isEmpty()) result.addPath(subpath);
    }
    result.setFillRule(fillRule);
    return result;
}

QVector<QPainterPath> fillEquivalentContours(const QPainterPath &source,
                                             bool *ok)
{
    if (ok) *ok = false;
    bool splitOk = false;
    QVector<QPainterPath> contours = splitPainterSubpaths(source, &splitOk);
    if (!splitOk || contours.isEmpty()) return {};

    // Most stroked paths already consist of exact closed contours. Preserve
    // those curves directly and retain the source path's fill semantics. The
    // stroker normally uses nonzero winding; that matters for neighbouring
    // dash islands that overlap at a corner or closed-path seam, where
    // odd-even would incorrectly cancel the overlap and reject the result.
    QPainterPath candidate = combinedContourPath(contours, source.fillRule());
    if (equivalentFillCoverage(source, candidate)) {
        if (ok) *ok = true;
        return contours;
    }

    // Complex self-intersections may depend on winding accumulation. Qt's
    // simplified path resolves those overlaps into explicit fill-equivalent
    // contours. It can flatten some curves, but still avoids synthetic spokes
    // and anti-aliased seams between independent regions.
    const QPainterPath simplified = source.simplified();
    contours = splitPainterSubpaths(simplified, &splitOk);
    if (splitOk && !contours.isEmpty()) {
        candidate = combinedContourPath(contours, source.fillRule());
        if (equivalentFillCoverage(source, candidate)) {
            if (ok) *ok = true;
            return contours;
        }
    }

    // Refuse pathological geometry rather than creating a visually incorrect
    // bridged contour. The caller leaves the original stroke untouched.
    return {};

}

} // namespace

QString vectorShapeTypeToString(const VectorShapeType type)
{
    switch (type) {
    case VectorShapeType::Rectangle: return QStringLiteral("rectangle");
    case VectorShapeType::RoundedRectangle: return QStringLiteral("rounded-rectangle");
    case VectorShapeType::Ellipse: return QStringLiteral("ellipse");
    case VectorShapeType::Line: return QStringLiteral("line");
    case VectorShapeType::Polygon: return QStringLiteral("polygon");
    case VectorShapeType::Star: return QStringLiteral("star");
    case VectorShapeType::Arrow: return QStringLiteral("arrow");
    case VectorShapeType::Path: return QStringLiteral("path");
    }
    return QStringLiteral("rectangle");
}

QString vectorShapeTypeDisplayName(const VectorShapeType type)
{
    switch (type) {
    case VectorShapeType::Rectangle: return QStringLiteral("Rectangle");
    case VectorShapeType::RoundedRectangle: return QStringLiteral("Rounded Rectangle");
    case VectorShapeType::Ellipse: return QStringLiteral("Ellipse");
    case VectorShapeType::Line: return QStringLiteral("Line");
    case VectorShapeType::Polygon: return QStringLiteral("Polygon");
    case VectorShapeType::Star: return QStringLiteral("Star");
    case VectorShapeType::Arrow: return QStringLiteral("Arrow");
    case VectorShapeType::Path: return QStringLiteral("Path");
    }
    return QStringLiteral("Shape");
}

VectorShapeType vectorShapeTypeFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("rectangle")) return VectorShapeType::Rectangle;
    if (normalised == QStringLiteral("rounded-rectangle")
        || normalised == QStringLiteral("roundedrectangle")) {
        return VectorShapeType::RoundedRectangle;
    }
    if (normalised == QStringLiteral("ellipse") || normalised == QStringLiteral("circle")) {
        return VectorShapeType::Ellipse;
    }
    if (normalised == QStringLiteral("line")) return VectorShapeType::Line;
    if (normalised == QStringLiteral("polygon")) return VectorShapeType::Polygon;
    if (normalised == QStringLiteral("star")) return VectorShapeType::Star;
    if (normalised == QStringLiteral("arrow") || normalised == QStringLiteral("block-arrow")) {
        return VectorShapeType::Arrow;
    }
    if (normalised == QStringLiteral("path") || normalised == QStringLiteral("bezier-path")) {
        return VectorShapeType::Path;
    }
    if (ok) *ok = false;
    return VectorShapeType::Rectangle;
}

QString vectorStrokeAlignmentDisplayName(const VectorStrokeAlignment alignment)
{
    switch (alignment) {
    case VectorStrokeAlignment::Inside: return QStringLiteral("Inside");
    case VectorStrokeAlignment::Centre: return QStringLiteral("Centre");
    case VectorStrokeAlignment::Outside: return QStringLiteral("Outside");
    }
    return QStringLiteral("Centre");
}

QString vectorStrokeCapDisplayName(const VectorStrokeCap cap)
{
    switch (cap) {
    case VectorStrokeCap::Butt: return QStringLiteral("Butt");
    case VectorStrokeCap::Round: return QStringLiteral("Round");
    case VectorStrokeCap::Square: return QStringLiteral("Square");
    }
    return QStringLiteral("Butt");
}

QString vectorStrokeJoinDisplayName(const VectorStrokeJoin join)
{
    switch (join) {
    case VectorStrokeJoin::Miter: return QStringLiteral("Miter");
    case VectorStrokeJoin::Round: return QStringLiteral("Round");
    case VectorStrokeJoin::Bevel: return QStringLiteral("Bevel");
    }
    return QStringLiteral("Miter");
}

QString vectorStrokePatternDisplayName(const VectorStrokePattern pattern)
{
    switch (pattern) {
    case VectorStrokePattern::Solid: return QStringLiteral("Solid");
    case VectorStrokePattern::Dashed: return QStringLiteral("Dashed");
    }
    return QStringLiteral("Solid");
}

QString vectorArrowheadTypeToString(const VectorArrowheadType type)
{
    switch (type) {
    case VectorArrowheadType::None: return QStringLiteral("none");
    case VectorArrowheadType::Open: return QStringLiteral("open");
    case VectorArrowheadType::Triangle: return QStringLiteral("triangle");
    case VectorArrowheadType::Stealth: return QStringLiteral("stealth");
    case VectorArrowheadType::Diamond: return QStringLiteral("diamond");
    case VectorArrowheadType::Circle: return QStringLiteral("circle");
    }
    return QStringLiteral("none");
}

QString vectorArrowheadTypeDisplayName(const VectorArrowheadType type)
{
    switch (type) {
    case VectorArrowheadType::None: return QStringLiteral("None");
    case VectorArrowheadType::Open: return QStringLiteral("Open");
    case VectorArrowheadType::Triangle: return QStringLiteral("Triangle");
    case VectorArrowheadType::Stealth: return QStringLiteral("Stealth");
    case VectorArrowheadType::Diamond: return QStringLiteral("Diamond");
    case VectorArrowheadType::Circle: return QStringLiteral("Circle");
    }
    return QStringLiteral("None");
}

VectorArrowheadType vectorArrowheadTypeFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString token = value.trimmed().toLower();
    if (token.isEmpty() || token == QStringLiteral("none")) return VectorArrowheadType::None;
    if (token == QStringLiteral("open") || token == QStringLiteral("chevron")) return VectorArrowheadType::Open;
    if (token == QStringLiteral("triangle") || token == QStringLiteral("closed")) return VectorArrowheadType::Triangle;
    if (token == QStringLiteral("stealth")) return VectorArrowheadType::Stealth;
    if (token == QStringLiteral("diamond")) return VectorArrowheadType::Diamond;
    if (token == QStringLiteral("circle") || token == QStringLiteral("dot")) return VectorArrowheadType::Circle;
    if (ok) *ok = false;
    return VectorArrowheadType::None;
}

QString vectorNodeModeToString(const VectorNodeMode mode)
{
    switch (mode) {
    case VectorNodeMode::Corner: return QStringLiteral("corner");
    case VectorNodeMode::Smooth: return QStringLiteral("smooth");
    case VectorNodeMode::Symmetric: return QStringLiteral("symmetric");
    }
    return QStringLiteral("corner");
}

QString vectorNodeModeDisplayName(const VectorNodeMode mode)
{
    switch (mode) {
    case VectorNodeMode::Corner: return QStringLiteral("Corner");
    case VectorNodeMode::Smooth: return QStringLiteral("Smooth");
    case VectorNodeMode::Symmetric: return QStringLiteral("Symmetric");
    }
    return QStringLiteral("Corner");
}

VectorNodeMode vectorNodeModeFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString token = value.trimmed().toLower();
    if (token == QStringLiteral("corner")) return VectorNodeMode::Corner;
    if (token == QStringLiteral("smooth")) return VectorNodeMode::Smooth;
    if (token == QStringLiteral("symmetric") || token == QStringLiteral("symmetrical")) {
        return VectorNodeMode::Symmetric;
    }
    if (ok) *ok = false;
    return VectorNodeMode::Corner;
}

QString vectorCornerStyleToString(const VectorCornerStyle style)
{
    switch (style) {
    case VectorCornerStyle::Rounded: return QStringLiteral("rounded");
    case VectorCornerStyle::Chamfer: return QStringLiteral("chamfer");
    case VectorCornerStyle::Concave: return QStringLiteral("concave");
    case VectorCornerStyle::Cutout: return QStringLiteral("cutout");
    }
    return QStringLiteral("rounded");
}

QString vectorCornerStyleDisplayName(const VectorCornerStyle style)
{
    switch (style) {
    case VectorCornerStyle::Rounded: return QStringLiteral("Rounded");
    case VectorCornerStyle::Chamfer: return QStringLiteral("Chamfer");
    case VectorCornerStyle::Concave: return QStringLiteral("Concave");
    case VectorCornerStyle::Cutout: return QStringLiteral("Cutout");
    }
    return QStringLiteral("Rounded");
}

VectorCornerStyle vectorCornerStyleFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString token = value.trimmed().toLower();
    if (token == QStringLiteral("rounded") || token == QStringLiteral("round")) {
        return VectorCornerStyle::Rounded;
    }
    if (token == QStringLiteral("chamfer") || token == QStringLiteral("bevel")) {
        return VectorCornerStyle::Chamfer;
    }
    if (token == QStringLiteral("concave") || token == QStringLiteral("scallop")) {
        return VectorCornerStyle::Concave;
    }
    if (token == QStringLiteral("cutout") || token == QStringLiteral("cut-out")) {
        return VectorCornerStyle::Cutout;
    }
    if (ok) *ok = false;
    return VectorCornerStyle::Rounded;
}

void VectorPathNode::normalise()
{
    if (id.isNull()) id = QUuid::createUuid();
    if (!safePoint(anchor)) anchor = QPointF();
    if (!safePoint(inHandle)) inHandle = anchor;
    if (!safePoint(outHandle)) outHandle = anchor;
    if (inHandleActive && QLineF(anchor, inHandle).length() <= 1.0e-6) {
        inHandleActive = false;
    }
    if (outHandleActive && QLineF(anchor, outHandle).length() <= 1.0e-6) {
        outHandleActive = false;
    }
    if (!inHandleActive) inHandle = anchor;
    if (!outHandleActive) outHandle = anchor;
    if (mode != VectorNodeMode::Corner && mode != VectorNodeMode::Smooth
        && mode != VectorNodeMode::Symmetric) {
        mode = VectorNodeMode::Corner;
    }
    if (!std::isfinite(cornerRadius) || cornerRadius < 0.0) cornerRadius = 0.0;
    cornerRadius = std::min(cornerRadius, MaximumExtent);
    if (!validCornerStyle(cornerStyle)) cornerStyle = VectorCornerStyle::Rounded;
    // Do not enforce Smooth/Symmetric geometry here. Normalisation is called
    // while loading, inserting nodes and publishing interactive edits. Moving
    // an existing handle merely because an unrelated node was normalised would
    // alter adjacent curve segments and break exact save/reopen round-tripping.
    // Mode constraints are applied only by explicit handle edits/conversions.
}

bool VectorPathNode::isSafe() const
{
    const bool modeSafe = mode == VectorNodeMode::Corner
        || mode == VectorNodeMode::Smooth || mode == VectorNodeMode::Symmetric;
    return !id.isNull() && modeSafe && validCornerStyle(cornerStyle)
        && std::isfinite(cornerRadius) && cornerRadius >= 0.0
        && cornerRadius <= MaximumExtent && safePoint(anchor)
        && (!inHandleActive || safePoint(inHandle))
        && (!outHandleActive || safePoint(outHandle));
}

void VectorPathNode::moveBy(const QPointF &delta)
{
    if (!safePoint(anchor + delta)
        || (inHandleActive && !safePoint(inHandle + delta))
        || (outHandleActive && !safePoint(outHandle + delta))) {
        return;
    }
    anchor += delta;
    if (inHandleActive) inHandle += delta;
    if (outHandleActive) outHandle += delta;
}

void VectorPathNode::setInHandle(const QPointF &position, const bool preserveOpposite)
{
    if (!safePoint(position)) return;
    inHandle = position;
    inHandleActive = QLineF(anchor, position).length() > 1.0e-6;
    if (inHandleActive) {
        cornerRadius = 0.0;
        cornerStyle = VectorCornerStyle::Rounded;
    }
    if (preserveOpposite && inHandleActive && outHandleActive
        && mode != VectorNodeMode::Corner) {
        const QPointF delta = position - anchor;
        const double length = std::hypot(delta.x(), delta.y());
        if (length > 1.0e-9) {
            const double oppositeLength = mode == VectorNodeMode::Symmetric
                ? length : QLineF(anchor, outHandle).length();
            outHandle = anchor - QPointF(delta.x() / length * oppositeLength,
                                         delta.y() / length * oppositeLength);
        }
    }
}

void VectorPathNode::setOutHandle(const QPointF &position, const bool preserveOpposite)
{
    if (!safePoint(position)) return;
    outHandle = position;
    outHandleActive = QLineF(anchor, position).length() > 1.0e-6;
    if (outHandleActive) {
        cornerRadius = 0.0;
        cornerStyle = VectorCornerStyle::Rounded;
    }
    if (preserveOpposite && outHandleActive && inHandleActive
        && mode != VectorNodeMode::Corner) {
        const QPointF delta = position - anchor;
        const double length = std::hypot(delta.x(), delta.y());
        if (length > 1.0e-9) {
            const double oppositeLength = mode == VectorNodeMode::Symmetric
                ? length : QLineF(anchor, inHandle).length();
            inHandle = anchor - QPointF(delta.x() / length * oppositeLength,
                                        delta.y() / length * oppositeLength);
        }
    }
}

void VectorPathNode::clearHandles()
{
    inHandleActive = false;
    outHandleActive = false;
    inHandle = anchor;
    outHandle = anchor;
    mode = VectorNodeMode::Corner;
}

void VectorPathNode::makeSharp()
{
    clearHandles();
    cornerRadius = 0.0;
    cornerStyle = VectorCornerStyle::Rounded;
}

QJsonObject VectorPathNode::toJson(bool *ok) const
{
    if (!isSafe()) {
        if (ok) *ok = false;
        return {};
    }
    QJsonObject object;
    object.insert(QStringLiteral("id"), id.toString(QUuid::WithoutBraces));
    object.insert(QStringLiteral("anchor"), pointToJson(anchor));
    object.insert(QStringLiteral("inHandle"), pointToJson(inHandle));
    object.insert(QStringLiteral("outHandle"), pointToJson(outHandle));
    object.insert(QStringLiteral("inHandleActive"), inHandleActive);
    object.insert(QStringLiteral("outHandleActive"), outHandleActive);
    object.insert(QStringLiteral("mode"), vectorNodeModeToString(mode));
    object.insert(QStringLiteral("cornerRadius"), cornerRadius);
    object.insert(QStringLiteral("cornerStyle"), vectorCornerStyleToString(cornerStyle));
    if (ok) *ok = true;
    return object;
}

VectorPathNode VectorPathNode::fromJson(const QJsonObject &object, bool *ok)
{
    VectorPathNode result;
    result.id = QUuid(object.value(QStringLiteral("id")).toString());
    bool anchorOk = false;
    bool inOk = false;
    bool outOk = false;
    bool modeOk = false;
    bool cornerStyleOk = true;
    result.anchor = pointFromJson(object.value(QStringLiteral("anchor")).toObject(), &anchorOk);
    result.inHandle = pointFromJson(object.value(QStringLiteral("inHandle")).toObject(), &inOk);
    result.outHandle = pointFromJson(object.value(QStringLiteral("outHandle")).toObject(), &outOk);
    result.inHandleActive = object.value(QStringLiteral("inHandleActive")).toBool(false);
    result.outHandleActive = object.value(QStringLiteral("outHandleActive")).toBool(false);
    result.mode = vectorNodeModeFromString(object.value(QStringLiteral("mode")).toString(), &modeOk);
    const QJsonValue radiusValue = object.value(QStringLiteral("cornerRadius"));
    result.cornerRadius = radiusValue.isUndefined() ? 0.0 : radiusValue.toDouble(-1.0);
    const QJsonValue styleValue = object.value(QStringLiteral("cornerStyle"));
    result.cornerStyle = styleValue.isUndefined()
        ? VectorCornerStyle::Rounded
        : vectorCornerStyleFromString(styleValue.toString(), &cornerStyleOk);
    const bool valid = !result.id.isNull() && anchorOk && inOk && outOk && modeOk
        && cornerStyleOk && std::isfinite(result.cornerRadius)
        && result.cornerRadius >= 0.0 && result.cornerRadius <= MaximumExtent;
    result.normalise();
    if (ok) *ok = valid && result.isSafe();
    return result;
}

void VectorBezierPath::normalise()
{
    QSet<QUuid> ids;
    for (VectorPathNode &node : nodes) {
        node.normalise();
        if (ids.contains(node.id)) node.id = QUuid::createUuid();
        ids.insert(node.id);
    }
    if (nodes.size() < 2) closed = false;
}

bool VectorBezierPath::isSafe() const
{
    if (nodes.isEmpty() || nodes.size() > MaximumNodeCount
        || (closed && nodes.size() < 2)) return false;
    QSet<QUuid> ids;
    for (const VectorPathNode &node : nodes) {
        if (!node.isSafe() || ids.contains(node.id)) return false;
        ids.insert(node.id);
    }
    const QRectF bounds = contentBounds().normalized();
    return finiteBounded(bounds.left()) && finiteBounded(bounds.top())
        && finiteBounded(bounds.right()) && finiteBounded(bounds.bottom())
        && std::isfinite(bounds.width()) && std::isfinite(bounds.height())
        && bounds.width() <= MaximumExtent && bounds.height() <= MaximumExtent;
}

int VectorBezierPath::segmentCount() const
{
    if (nodes.size() < 2) return 0;
    return closed ? nodes.size() : nodes.size() - 1;
}

bool VectorBezierPath::transformNodes(const QSet<int> &nodeIndices,
                                      const QTransform &pointTransform)
{
    if (nodeIndices.isEmpty()
        || !transformMatrixIsFiniteAndBounded(pointTransform)) {
        return false;
    }

    QSet<int> validIndices;
    for (const int index : nodeIndices) {
        if (index < 0 || index >= nodes.size()) continue;
        const VectorPathNode &node = nodes.at(index);
        const QPointF anchor = pointTransform.map(node.anchor);
        const QPointF inHandle = pointTransform.map(node.inHandle);
        const QPointF outHandle = pointTransform.map(node.outHandle);
        if (!safePoint(anchor)
            || (node.inHandleActive && !safePoint(inHandle))
            || (node.outHandleActive && !safePoint(outHandle))) {
            return false;
        }
        validIndices.insert(index);
    }
    if (validIndices.isEmpty()) return false;

    VectorBezierPath moved = *this;
    for (const int index : validIndices) {
        VectorPathNode &node = moved.nodes[index];
        node.anchor = pointTransform.map(node.anchor);
        if (node.inHandleActive) node.inHandle = pointTransform.map(node.inHandle);
        if (node.outHandleActive) node.outHandle = pointTransform.map(node.outHandle);
    }
    moved.normalise();
    if (!moved.isSafe() || moved == *this) return false;
    *this = std::move(moved);
    return true;
}

bool VectorBezierPath::moveNodesBy(const QSet<int> &nodeIndices,
                                   const QPointF &delta)
{
    if (!safePoint(delta)
        || (qFuzzyIsNull(delta.x()) && qFuzzyIsNull(delta.y()))) {
        return false;
    }
    return transformNodes(nodeIndices,
                          QTransform::fromTranslate(delta.x(), delta.y()));
}

void VectorBezierPath::reverseDirection()
{
    std::reverse(nodes.begin(), nodes.end());
    for (VectorPathNode &node : nodes) {
        std::swap(node.inHandle, node.outHandle);
        std::swap(node.inHandleActive, node.outHandleActive);
    }
    normalise();
}

bool VectorBezierPath::joinFollowingPath(const VectorBezierPath &following,
                                         int *junctionNodeIndex,
                                         const double coincidentTolerance)
{
    if (junctionNodeIndex) *junctionNodeIndex = -1;
    if (closed || following.closed || nodes.isEmpty() || following.nodes.isEmpty()
        || !isSafe() || !following.isSafe()) {
        return false;
    }

    VectorBezierPath joined = *this;
    const VectorPathNode &followingFirst = following.nodes.constFirst();
    const double tolerance = std::clamp(
        std::isfinite(coincidentTolerance) ? coincidentTolerance : 0.0,
        0.0, 1.0e6);
    const bool coincident = QLineF(joined.nodes.constLast().anchor,
                                   followingFirst.anchor).length() <= tolerance;
    const qsizetype additionalNodes = following.nodes.size()
        - (coincident ? 1 : 0);
    if (additionalNodes < 0
        || joined.nodes.size() > MaximumNodeCount - additionalNodes) {
        return false;
    }
    int junction = -1;
    if (coincident) {
        VectorPathNode &junctionNode = joined.nodes.last();
        // The existing endpoint supplies the incoming side and the following
        // path supplies the outgoing side. Preserve both independently so a
        // joined smooth-looking pair never has its handles unexpectedly
        // mirrored or resized by node-mode coupling.
        junctionNode.outHandle = followingFirst.outHandle;
        junctionNode.outHandleActive = followingFirst.outHandleActive;
        junctionNode.mode = VectorNodeMode::Corner;
        junctionNode.cornerRadius = 0.0;
        junctionNode.cornerStyle = VectorCornerStyle::Rounded;
        junction = joined.nodes.size() - 1;
        for (int index = 1; index < following.nodes.size(); ++index) {
            joined.nodes.push_back(following.nodes.at(index));
        }
    } else {
        junction = joined.nodes.size();
        joined.nodes += following.nodes;
    }
    joined.closed = false;
    joined.normalise();
    if (!joined.isSafe()) return false;
    *this = std::move(joined);
    if (junctionNodeIndex) *junctionNodeIndex = junction;
    return true;
}

QPainterPath VectorBezierPath::basePainterPath() const
{
    QPainterPath path;
    if (nodes.isEmpty()) return path;
    path.moveTo(nodes.constFirst().anchor);
    const int segments = segmentCount();
    for (int segment = 0; segment < segments; ++segment) {
        const int nextIndex = (segment + 1) % static_cast<int>(nodes.size());
        const VectorPathNode &left = nodes.at(segment);
        const VectorPathNode &right = nodes.at(nextIndex);
        const QPointF c1 = left.outHandleActive ? left.outHandle : left.anchor;
        const QPointF c2 = right.inHandleActive ? right.inHandle : right.anchor;
        if (c1 == left.anchor && c2 == right.anchor) path.lineTo(right.anchor);
        else path.cubicTo(c1, c2, right.anchor);
    }
    if (closed) path.closeSubpath();
    return path;
}

bool VectorBezierPath::cornerableNode(const int nodeIndex) const
{
    if (!closed || nodes.size() < 3 || nodeIndex < 0 || nodeIndex >= nodes.size()) {
        return false;
    }
    const int previous = (nodeIndex - 1 + nodes.size()) % nodes.size();
    const int next = (nodeIndex + 1) % nodes.size();
    const VectorPathNode &before = nodes.at(previous);
    const VectorPathNode &node = nodes.at(nodeIndex);
    const VectorPathNode &after = nodes.at(next);
    if (node.mode != VectorNodeMode::Corner
        || before.outHandleActive || node.inHandleActive
        || node.outHandleActive || after.inHandleActive) {
        return false;
    }
    const QPointF incoming = before.anchor - node.anchor;
    const QPointF outgoing = after.anchor - node.anchor;
    const double incomingLength = std::hypot(incoming.x(), incoming.y());
    const double outgoingLength = std::hypot(outgoing.x(), outgoing.y());
    if (incomingLength <= 1.0e-6 || outgoingLength <= 1.0e-6) return false;
    const double cosine = std::clamp(
        (incoming.x() * outgoing.x() + incoming.y() * outgoing.y())
            / (incomingLength * outgoingLength),
        -1.0, 1.0);
    const double angle = std::acos(cosine);
    return angle > 1.0e-3 && angle < std::numbers::pi - 1.0e-3;
}

double VectorBezierPath::maximumCornerRadius(const int nodeIndex) const
{
    if (!cornerableNode(nodeIndex)) return 0.0;
    const int previous = (nodeIndex - 1 + nodes.size()) % nodes.size();
    const int next = (nodeIndex + 1) % nodes.size();
    const double incoming = QLineF(nodes.at(nodeIndex).anchor,
                                   nodes.at(previous).anchor).length();
    const double outgoing = QLineF(nodes.at(nodeIndex).anchor,
                                   nodes.at(next).anchor).length();
    return std::max(0.0, std::min(incoming, outgoing) * 0.5);
}

QPointF VectorBezierPath::cornerHandlePoint(const int nodeIndex) const
{
    if (!cornerableNode(nodeIndex)) {
        return nodeIndex >= 0 && nodeIndex < nodes.size()
            ? nodes.at(nodeIndex).anchor : QPointF();
    }
    const VectorPathNode &node = nodes.at(nodeIndex);
    const int previous = (nodeIndex - 1 + nodes.size()) % nodes.size();
    const int next = (nodeIndex + 1) % nodes.size();
    QPointF incoming = nodes.at(previous).anchor - node.anchor;
    QPointF outgoing = nodes.at(next).anchor - node.anchor;
    const double incomingLength = std::hypot(incoming.x(), incoming.y());
    const double outgoingLength = std::hypot(outgoing.x(), outgoing.y());
    incoming /= incomingLength;
    outgoing /= outgoingLength;
    QPointF bisector = incoming + outgoing;
    const double bisectorLength = std::hypot(bisector.x(), bisector.y());
    if (bisectorLength <= 1.0e-9) return node.anchor;
    bisector /= bisectorLength;
    const double radius = std::min(node.cornerRadius, maximumCornerRadius(nodeIndex));
    return node.anchor + bisector * radius;
}

bool VectorBezierPath::hasCornerMetadata() const
{
    for (const VectorPathNode &node : nodes) {
        if (node.cornerRadius > 1.0e-6
            || node.cornerStyle != VectorCornerStyle::Rounded) {
            return true;
        }
    }
    return false;
}

bool VectorBezierPath::hasLiveCorners() const
{
    for (int index = 0; index < nodes.size(); ++index) {
        if (nodes.at(index).cornerRadius > 1.0e-6 && cornerableNode(index)) return true;
    }
    return false;
}

QPainterPath VectorBezierPath::painterPath() const
{
    if (!hasLiveCorners()) return basePainterPath();

    struct CornerGeometry {
        bool active = false;
        QPointF start;
        QPointF end;
        QPointF incomingUnit;
        QPointF outgoingUnit;
        QPointF bisector;
        double trim = 0.0;
        double angle = 0.0;
        VectorCornerStyle style = VectorCornerStyle::Rounded;
    };

    QVector<CornerGeometry> corners(nodes.size());
    for (int index = 0; index < nodes.size(); ++index) {
        if (!cornerableNode(index) || nodes.at(index).cornerRadius <= 1.0e-6) continue;
        const int previous = (index - 1 + nodes.size()) % nodes.size();
        const int next = (index + 1) % nodes.size();
        const VectorPathNode &node = nodes.at(index);
        QPointF incoming = nodes.at(previous).anchor - node.anchor;
        QPointF outgoing = nodes.at(next).anchor - node.anchor;
        const double incomingLength = std::hypot(incoming.x(), incoming.y());
        const double outgoingLength = std::hypot(outgoing.x(), outgoing.y());
        incoming /= incomingLength;
        outgoing /= outgoingLength;
        const double cosine = std::clamp(incoming.x() * outgoing.x()
                                             + incoming.y() * outgoing.y(),
                                         -1.0, 1.0);
        CornerGeometry &corner = corners[index];
        corner.angle = std::acos(cosine);
        corner.trim = std::min(node.cornerRadius, maximumCornerRadius(index));
        corner.active = corner.trim > 1.0e-6;
        corner.incomingUnit = incoming;
        corner.outgoingUnit = outgoing;
        corner.start = node.anchor + incoming * corner.trim;
        corner.end = node.anchor + outgoing * corner.trim;
        corner.bisector = incoming + outgoing;
        const double bisectorLength = std::hypot(corner.bisector.x(),
                                                  corner.bisector.y());
        if (bisectorLength > 1.0e-9) corner.bisector /= bisectorLength;
        corner.style = node.cornerStyle;
    }

    const auto drawCorner = [this](QPainterPath *path,
                                      const int index,
                                      const CornerGeometry &corner) {
        if (!corner.active) return;
        const QPointF anchor = nodes.at(index).anchor;
        switch (corner.style) {
        case VectorCornerStyle::Rounded: {
            const double turn = std::numbers::pi - corner.angle;
            const double circleRadius = corner.trim * std::tan(corner.angle * 0.5);
            const double handle = 4.0 / 3.0 * std::tan(turn * 0.25) * circleRadius;
            const QPointF firstControl = corner.start
                - corner.incomingUnit * handle;
            const QPointF secondControl = corner.end
                - corner.outgoingUnit * handle;
            path->cubicTo(firstControl, secondControl, corner.end);
            break;
        }
        case VectorCornerStyle::Chamfer:
            path->lineTo(corner.end);
            break;
        case VectorCornerStyle::Concave: {
            const QPointF notch = anchor + corner.bisector * corner.trim * 1.35;
            const QPointF firstControl = corner.start
                + (notch - corner.start) * 0.72;
            const QPointF secondControl = corner.end
                + (notch - corner.end) * 0.72;
            path->cubicTo(firstControl, secondControl, corner.end);
            break;
        }
        case VectorCornerStyle::Cutout: {
            const QPointF notch = anchor + corner.bisector * corner.trim * 1.2;
            path->lineTo(notch);
            path->lineTo(corner.end);
            break;
        }
        }
    };

    QPainterPath path;
    const QPointF first = corners.constFirst().active
        ? corners.constFirst().end : nodes.constFirst().anchor;
    path.moveTo(first);
    for (int segment = 0; segment < nodes.size(); ++segment) {
        const int next = (segment + 1) % nodes.size();
        const VectorPathNode &left = nodes.at(segment);
        const VectorPathNode &right = nodes.at(next);
        const CornerGeometry &leftCorner = corners.at(segment);
        const CornerGeometry &rightCorner = corners.at(next);
        const QPointF segmentEnd = rightCorner.active
            ? rightCorner.start : right.anchor;
        if (leftCorner.active || rightCorner.active) {
            path.lineTo(segmentEnd);
        } else {
            const QPointF c1 = left.outHandleActive ? left.outHandle : left.anchor;
            const QPointF c2 = right.inHandleActive ? right.inHandle : right.anchor;
            if (c1 == left.anchor && c2 == right.anchor) path.lineTo(right.anchor);
            else path.cubicTo(c1, c2, right.anchor);
        }
        drawCorner(&path, next, rightCorner);
    }
    path.closeSubpath();
    return path;
}

VectorBezierPath VectorBezierPath::fromPainterPath(const QPainterPath &path,
                                                    const bool shouldClose,
                                                    bool *ok)
{
    VectorBezierPath result;
    result.closed = shouldClose;
    bool valid = !path.isEmpty();
    for (int index = 0; valid && index < path.elementCount(); ++index) {
        const QPainterPath::Element element = path.elementAt(index);
        const QPointF point(element.x, element.y);
        if (element.isMoveTo()) {
            if (!result.nodes.isEmpty()) {
                valid = false;
                break;
            }
            VectorPathNode node;
            node.anchor = point;
            node.inHandle = point;
            node.outHandle = point;
            result.nodes.push_back(node);
        } else if (element.isLineTo()) {
            if (result.nodes.isEmpty()) {
                valid = false;
                break;
            }
            if (shouldClose && result.nodes.size() >= 2
                && QLineF(point, result.nodes.constFirst().anchor).length() <= 1.0e-6
                && index == path.elementCount() - 1) {
                continue;
            }
            VectorPathNode node;
            node.anchor = point;
            node.inHandle = point;
            node.outHandle = point;
            result.nodes.push_back(node);
        } else if (element.type == QPainterPath::CurveToElement) {
            if (result.nodes.isEmpty() || index + 2 >= path.elementCount()) {
                valid = false;
                break;
            }
            const QPainterPath::Element control2Element = path.elementAt(index + 1);
            const QPainterPath::Element endElement = path.elementAt(index + 2);
            if (control2Element.type != QPainterPath::CurveToDataElement
                || endElement.type != QPainterPath::CurveToDataElement) {
                valid = false;
                break;
            }
            VectorPathNode &previous = result.nodes.last();
            previous.outHandle = point;
            previous.outHandleActive = QLineF(previous.anchor, point).length() > 1.0e-6;
            const QPointF control2(control2Element.x, control2Element.y);
            const QPointF end(endElement.x, endElement.y);
            if (shouldClose && result.nodes.size() >= 2
                && QLineF(end, result.nodes.constFirst().anchor).length() <= 1.0e-6
                && index + 2 == path.elementCount() - 1) {
                VectorPathNode &first = result.nodes.first();
                first.inHandle = control2;
                first.inHandleActive = QLineF(first.anchor, control2).length() > 1.0e-6;
            } else {
                VectorPathNode node;
                node.anchor = end;
                node.inHandle = control2;
                node.inHandleActive = QLineF(end, control2).length() > 1.0e-6;
                node.outHandle = end;
                result.nodes.push_back(node);
            }
            index += 2;
        } else if (element.type == QPainterPath::CurveToDataElement) {
            valid = false;
        }
        valid = valid && result.nodes.size() <= MaximumNodeCount;
    }
    result.normalise();
    valid = valid && result.nodes.size() >= 2 && result.isSafe();
    if (ok) *ok = valid;
    return valid ? result : VectorBezierPath();
}

bool VectorBezierPath::bakeCorners()
{
    if (!hasLiveCorners()) return true;
    bool converted = false;
    VectorBezierPath baked = fromPainterPath(painterPath(), closed, &converted);
    if (!converted) return false;
    *this = std::move(baked);
    return true;
}

QRectF VectorBezierPath::contentBounds() const
{
    if (nodes.isEmpty()) return {};
    QRectF bounds = painterPath().boundingRect().normalized();
    if (bounds.width() <= 0.0 && bounds.height() <= 0.0) {
        bounds = QRectF(nodes.constFirst().anchor, QSizeF(0.01, 0.01));
    } else {
        if (bounds.width() <= 0.0) bounds.setWidth(0.01);
        if (bounds.height() <= 0.0) bounds.setHeight(0.01);
    }
    return bounds;
}

QVector<QPointF> VectorBezierPath::snapPoints() const
{
    QVector<QPointF> points;
    points.reserve(nodes.size());
    for (const VectorPathNode &node : nodes) points.push_back(node.anchor);
    return points;
}

bool VectorBezierPath::insertNodeOnSegment(const int segmentIndex, const double rawT,
                                            int *insertedIndex)
{
    if (segmentIndex < 0 || segmentIndex >= segmentCount()
        || nodes.size() >= MaximumNodeCount) return false;
    const double t = std::clamp(rawT, 1.0e-5, 1.0 - 1.0e-5);
    const int nextIndex = (segmentIndex + 1) % static_cast<int>(nodes.size());
    VectorPathNode &left = nodes[segmentIndex];
    VectorPathNode &right = nodes[nextIndex];
    const QPointF p0 = left.anchor;
    const QPointF p1 = left.outHandleActive ? left.outHandle : left.anchor;
    const QPointF p2 = right.inHandleActive ? right.inHandle : right.anchor;
    const QPointF p3 = right.anchor;
    const auto lerp = [t](const QPointF &a, const QPointF &b) { return a + (b - a) * t; };
    const QPointF a = lerp(p0, p1);
    const QPointF b = lerp(p1, p2);
    const QPointF c = lerp(p2, p3);
    const QPointF d = lerp(a, b);
    const QPointF e = lerp(b, c);
    const QPointF point = lerp(d, e);
    left.outHandle = a;
    left.outHandleActive = QLineF(left.anchor, a).length() > 1.0e-6;
    right.inHandle = c;
    right.inHandleActive = QLineF(right.anchor, c).length() > 1.0e-6;
    VectorPathNode inserted;
    inserted.anchor = point;
    inserted.inHandle = d;
    inserted.outHandle = e;
    inserted.inHandleActive = QLineF(point, d).length() > 1.0e-6;
    inserted.outHandleActive = QLineF(point, e).length() > 1.0e-6;
    inserted.mode = VectorNodeMode::Smooth;
    const int insertion = nextIndex == 0
        ? static_cast<int>(nodes.size()) : nextIndex;
    nodes.insert(insertion, inserted);
    normalise();
    if (insertedIndex) *insertedIndex = insertion;
    return true;
}

QJsonObject VectorBezierPath::toJson(bool *ok) const
{
    if (!isSafe()) {
        if (ok) *ok = false;
        return {};
    }
    QJsonArray array;
    for (const VectorPathNode &node : nodes) {
        bool nodeOk = false;
        const QJsonObject object = node.toJson(&nodeOk);
        if (!nodeOk) {
            if (ok) *ok = false;
            return {};
        }
        array.push_back(object);
    }
    QJsonObject object;
    object.insert(QStringLiteral("closed"), closed);
    object.insert(QStringLiteral("nodes"), array);
    if (ok) *ok = true;
    return object;
}

VectorBezierPath VectorBezierPath::fromJson(const QJsonObject &object, bool *ok)
{
    VectorBezierPath result;
    result.closed = object.value(QStringLiteral("closed")).toBool(false);
    const QJsonValue nodesValue = object.value(QStringLiteral("nodes"));
    bool valid = nodesValue.isArray();
    if (valid) {
        const QJsonArray array = nodesValue.toArray();
        valid = array.size() <= MaximumNodeCount
            && (!result.closed || array.size() >= 2);
        if (valid) {
            QSet<QUuid> nodeIds;
            result.nodes.reserve(array.size());
            for (const QJsonValue &value : array) {
                if (!value.isObject()) { valid = false; break; }
                bool nodeOk = false;
                VectorPathNode node = VectorPathNode::fromJson(value.toObject(), &nodeOk);
                if (!nodeOk || nodeIds.contains(node.id)) {
                    valid = false;
                    break;
                }
                nodeIds.insert(node.id);
                result.nodes.push_back(node);
            }
        }
    }
    result.normalise();
    if (ok) *ok = valid && result.isSafe();
    return result;
}

void VectorFill::normalise()
{
    if (!colour.isValid()) colour = QColor(Qt::black);
    opacity = std::clamp(std::isfinite(opacity) ? opacity : 1.0, 0.0, 1.0);
}

QJsonObject VectorFill::toJson(bool *ok) const
{
    const bool valid = colour.isValid() && std::isfinite(opacity)
        && opacity >= 0.0 && opacity <= 1.0;
    if (ok) *ok = valid;
    if (!valid) return {};
    QJsonObject object;
    object.insert(QStringLiteral("enabled"), enabled);
    object.insert(QStringLiteral("colour"), colourToJson(colour));
    object.insert(QStringLiteral("opacity"), opacity);
    return object;
}

VectorFill VectorFill::fromJson(const QJsonObject &object, bool *ok)
{
    bool colourOk = false;
    VectorFill result;
    result.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    result.colour = colourFromJson(object.value(QStringLiteral("colour")).toObject(), &colourOk);
    result.opacity = object.value(QStringLiteral("opacity")).toDouble(1.0);
    const bool valid = colourOk && std::isfinite(result.opacity)
        && result.opacity >= 0.0 && result.opacity <= 1.0;
    result.normalise();
    if (ok) *ok = valid;
    return result;
}

bool VectorFill::operator==(const VectorFill &other) const
{
    const QRgba64 left = colour.rgba64();
    const QRgba64 right = other.colour.rgba64();
    return enabled == other.enabled
        && left.red() == right.red()
        && left.green() == right.green()
        && left.blue() == right.blue()
        && left.alpha() == right.alpha()
        && opacity == other.opacity;
}

void VectorStroke::normalise(const bool)
{
    if (!colour.isValid()) colour = QColor(Qt::black);
    opacity = std::clamp(std::isfinite(opacity) ? opacity : 1.0, 0.0, 1.0);
    width = std::clamp(std::isfinite(width) ? width : 1.0,
                       MinimumStrokeWidth, MaximumStrokeWidth);
    miterLimit = std::clamp(std::isfinite(miterLimit) ? miterLimit : 4.0,
                            1.0, 1000.0);
    if (!validStrokeAlignment(alignment)) {
        alignment = VectorStrokeAlignment::Centre;
    }
    if (!validStrokeCap(cap)) cap = VectorStrokeCap::Butt;
    if (!validStrokeJoin(join)) join = VectorStrokeJoin::Miter;
    if (!validStrokePattern(pattern)) pattern = VectorStrokePattern::Solid;
    dashLength = std::clamp(std::isfinite(dashLength) ? dashLength : 8.0,
                            MinimumDashLength, MaximumDashLength);
    gapLength = std::clamp(std::isfinite(gapLength) ? gapLength : 8.0,
                           MinimumDashLength, MaximumDashLength);
    dashOffset = std::clamp(std::isfinite(dashOffset) ? dashOffset : 0.0,
                            -MaximumDashOffset, MaximumDashOffset);
    if (!validArrowheadType(startArrowhead)) startArrowhead = VectorArrowheadType::None;
    if (!validArrowheadType(endArrowhead)) endArrowhead = VectorArrowheadType::None;
    startArrowScale = std::clamp(std::isfinite(startArrowScale) ? startArrowScale : 1.0,
                                 MinimumArrowScale, MaximumArrowScale);
    endArrowScale = std::clamp(std::isfinite(endArrowScale) ? endArrowScale : 1.0,
                               MinimumArrowScale, MaximumArrowScale);
}

bool VectorStroke::isSafe(const bool) const
{
    return colour.isValid() && std::isfinite(opacity)
        && opacity >= 0.0 && opacity <= 1.0
        && std::isfinite(width) && width >= MinimumStrokeWidth
        && width <= MaximumStrokeWidth
        && validStrokeAlignment(alignment)
        && validStrokeCap(cap) && validStrokeJoin(join)
        && std::isfinite(miterLimit) && miterLimit >= 1.0 && miterLimit <= 1000.0
        && validStrokePattern(pattern)
        && std::isfinite(dashLength) && dashLength >= MinimumDashLength
        && dashLength <= MaximumDashLength
        && std::isfinite(gapLength) && gapLength >= MinimumDashLength
        && gapLength <= MaximumDashLength
        && std::isfinite(dashOffset) && std::abs(dashOffset) <= MaximumDashOffset
        && validArrowheadType(startArrowhead) && validArrowheadType(endArrowhead)
        && std::isfinite(startArrowScale) && startArrowScale >= MinimumArrowScale
        && startArrowScale <= MaximumArrowScale
        && std::isfinite(endArrowScale) && endArrowScale >= MinimumArrowScale
        && endArrowScale <= MaximumArrowScale;
}

QJsonObject VectorStroke::toJson(bool *ok) const
{
    const bool valid = isSafe(false);
    if (ok) *ok = valid;
    if (!valid) return {};
    return {{QStringLiteral("enabled"), enabled},
            {QStringLiteral("colour"), colourToJson(colour)},
            {QStringLiteral("opacity"), opacity},
            {QStringLiteral("width"), width},
            {QStringLiteral("alignment"), strokeAlignmentToString(alignment)},
            {QStringLiteral("cap"), strokeCapToString(cap)},
            {QStringLiteral("join"), strokeJoinToString(join)},
            {QStringLiteral("miterLimit"), miterLimit},
            {QStringLiteral("pattern"), strokePatternToString(pattern)},
            {QStringLiteral("dashLength"), dashLength},
            {QStringLiteral("gapLength"), gapLength},
            {QStringLiteral("dashOffset"), dashOffset},
            {QStringLiteral("startArrowhead"), vectorArrowheadTypeToString(startArrowhead)},
            {QStringLiteral("endArrowhead"), vectorArrowheadTypeToString(endArrowhead)},
            {QStringLiteral("startArrowScale"), startArrowScale},
            {QStringLiteral("endArrowScale"), endArrowScale}};
}

VectorStroke VectorStroke::fromJson(const QJsonObject &object,
                                    const bool openPath,
                                    bool *ok)
{
    VectorStroke result;
    if (object.isEmpty()) {
        result.normalise(openPath);
        if (ok) *ok = true;
        return result;
    }
    bool colourOk = false;
    bool alignmentOk = false;
    bool capOk = false;
    bool joinOk = false;
    bool patternOk = false;
    bool startArrowOk = true;
    bool endArrowOk = true;
    result.enabled = object.value(QStringLiteral("enabled")).toBool(false);
    result.colour = colourFromJson(object.value(QStringLiteral("colour")).toObject(), &colourOk);
    result.opacity = object.value(QStringLiteral("opacity")).toDouble(1.0);
    result.width = object.value(QStringLiteral("width")).toDouble(1.0);
    result.alignment = strokeAlignmentFromString(
        object.value(QStringLiteral("alignment")).toString(QStringLiteral("centre")),
        &alignmentOk);
    result.cap = strokeCapFromString(
        object.value(QStringLiteral("cap")).toString(QStringLiteral("butt")), &capOk);
    result.join = strokeJoinFromString(
        object.value(QStringLiteral("join")).toString(QStringLiteral("miter")), &joinOk);
    result.miterLimit = object.value(QStringLiteral("miterLimit")).toDouble(4.0);
    result.pattern = strokePatternFromString(
        object.value(QStringLiteral("pattern")).toString(QStringLiteral("solid")),
        &patternOk);
    result.dashLength = object.value(QStringLiteral("dashLength")).toDouble(8.0);
    result.gapLength = object.value(QStringLiteral("gapLength")).toDouble(8.0);
    result.dashOffset = object.value(QStringLiteral("dashOffset")).toDouble(0.0);
    const QJsonValue startArrowValue = object.value(QStringLiteral("startArrowhead"));
    const QJsonValue endArrowValue = object.value(QStringLiteral("endArrowhead"));
    if (startArrowValue.isString()) {
        result.startArrowhead = vectorArrowheadTypeFromString(startArrowValue.toString(), &startArrowOk);
    } else if (!startArrowValue.isUndefined()) {
        startArrowOk = false;
    }
    if (endArrowValue.isString()) {
        result.endArrowhead = vectorArrowheadTypeFromString(endArrowValue.toString(), &endArrowOk);
    } else if (!endArrowValue.isUndefined()) {
        endArrowOk = false;
    }
    const QJsonValue startScaleValue = object.value(QStringLiteral("startArrowScale"));
    const QJsonValue endScaleValue = object.value(QStringLiteral("endArrowScale"));
    const bool startScaleOk = startScaleValue.isUndefined() || startScaleValue.isDouble();
    const bool endScaleOk = endScaleValue.isUndefined() || endScaleValue.isDouble();
    result.startArrowScale = startScaleValue.isDouble() ? startScaleValue.toDouble() : 1.0;
    result.endArrowScale = endScaleValue.isDouble() ? endScaleValue.toDouble() : 1.0;
    const bool valid = colourOk && alignmentOk && capOk && joinOk && patternOk
        && startArrowOk && endArrowOk && startScaleOk && endScaleOk
        && result.isSafe(openPath);
    result.normalise(openPath);
    if (ok) *ok = valid;
    return result;
}

bool VectorStroke::operator==(const VectorStroke &other) const
{
    const QRgba64 left = colour.rgba64();
    const QRgba64 right = other.colour.rgba64();
    return enabled == other.enabled
        && left.red() == right.red()
        && left.green() == right.green()
        && left.blue() == right.blue()
        && left.alpha() == right.alpha()
        && opacity == other.opacity
        && width == other.width
        && alignment == other.alignment
        && cap == other.cap
        && join == other.join
        && miterLimit == other.miterLimit
        && pattern == other.pattern
        && dashLength == other.dashLength
        && gapLength == other.gapLength
        && dashOffset == other.dashOffset
        && startArrowhead == other.startArrowhead
        && endArrowhead == other.endArrowhead
        && startArrowScale == other.startArrowScale
        && endArrowScale == other.endArrowScale;
}

void VectorCornerRadii::setAll(const double radius)
{
    topLeft = topRight = bottomRight = bottomLeft = radius;
}

void VectorCornerRadii::sanitise()
{
    const auto sanitiseValue = [](const double value) {
        return std::clamp(std::isfinite(value) ? value : 0.0, 0.0, MaximumExtent);
    };
    topLeft = sanitiseValue(topLeft);
    topRight = sanitiseValue(topRight);
    bottomRight = sanitiseValue(bottomRight);
    bottomLeft = sanitiseValue(bottomLeft);
}

void VectorCornerRadii::normalise(const QSizeF &boundsSize)
{
    sanitise();
    const double width = std::max(0.0, boundsSize.width());
    const double height = std::max(0.0, boundsSize.height());
    double scale = 1.0;
    const auto constrain = [&scale](const double side, const double sum) {
        if (sum > side && sum > 0.0) scale = std::min(scale, side / sum);
    };
    constrain(width, topLeft + topRight);
    constrain(width, bottomLeft + bottomRight);
    constrain(height, topLeft + bottomLeft);
    constrain(height, topRight + bottomRight);
    if (scale < 1.0) {
        topLeft *= scale;
        topRight *= scale;
        bottomRight *= scale;
        bottomLeft *= scale;
    }
}

QSizeF VectorCornerRadii::minimumSize() const
{
    return QSizeF(std::max(topLeft + topRight, bottomLeft + bottomRight),
                  std::max(topLeft + bottomLeft, topRight + bottomRight));
}

bool VectorCornerRadii::isSafe() const
{
    const auto safe = [](const double value) {
        return std::isfinite(value) && value >= 0.0 && value <= MaximumExtent;
    };
    return safe(topLeft) && safe(topRight) && safe(bottomRight) && safe(bottomLeft);
}

bool VectorCornerRadii::allEqual(const double epsilon) const
{
    return std::abs(topLeft - topRight) <= epsilon
        && std::abs(topLeft - bottomRight) <= epsilon
        && std::abs(topLeft - bottomLeft) <= epsilon;
}

QJsonObject VectorCornerRadii::toJson(bool *ok) const
{
    const bool valid = isSafe();
    if (ok) *ok = valid;
    if (!valid) return {};
    return {{QStringLiteral("topLeft"), topLeft},
            {QStringLiteral("topRight"), topRight},
            {QStringLiteral("bottomRight"), bottomRight},
            {QStringLiteral("bottomLeft"), bottomLeft}};
}

VectorCornerRadii VectorCornerRadii::fromJson(const QJsonObject &object, bool *ok)
{
    VectorCornerRadii result;
    result.topLeft = object.value(QStringLiteral("topLeft")).toDouble(-1.0);
    result.topRight = object.value(QStringLiteral("topRight")).toDouble(-1.0);
    result.bottomRight = object.value(QStringLiteral("bottomRight")).toDouble(-1.0);
    result.bottomLeft = object.value(QStringLiteral("bottomLeft")).toDouble(-1.0);
    const bool valid = result.isSafe();
    if (ok) *ok = valid;
    return valid ? result : VectorCornerRadii {};
}

bool VectorShape::isOpenPath() const
{
    return type == VectorShapeType::Line
        || (type == VectorShapeType::Path
            && additionalBezierPaths.isEmpty()
            && !bezierPath.closed);
}

void VectorShape::normalise()
{
    if (!validShapeType(type)) type = VectorShapeType::Rectangle;
    if (!validPathFillRule(pathFillRule)) {
        pathFillRule = VectorPathFillRule::EvenOdd;
    }
    if (type == VectorShapeType::Path) {
        bezierPath.normalise();
        for (VectorBezierPath &path : additionalBezierPaths) path.normalise();
        // Compound paths are fill contours, so every contour must remain
        // closed. Open Pen paths intentionally stay single-contour.
        if (!additionalBezierPaths.isEmpty()) {
            bezierPath.closed = true;
            for (VectorBezierPath &path : additionalBezierPaths) path.closed = true;
        }
        // Node UUIDs are selection identities and must be unique across the
        // complete compound path, not merely inside each contour.
        QSet<QUuid> pathNodeIds;
        const auto normaliseIds = [&pathNodeIds](VectorBezierPath &path) {
            for (VectorPathNode &node : path.nodes) {
                if (node.id.isNull() || pathNodeIds.contains(node.id)) {
                    node.id = QUuid::createUuid();
                }
                pathNodeIds.insert(node.id);
            }
        };
        normaliseIds(bezierPath);
        for (VectorBezierPath &path : additionalBezierPaths) normaliseIds(path);
        bounds = bezierPath.contentBounds();
        for (const VectorBezierPath &path : std::as_const(additionalBezierPaths)) {
            const QRectF contourBounds = path.contentBounds();
            if (!contourBounds.isEmpty()) {
                bounds = bounds.isEmpty() ? contourBounds : bounds.united(contourBounds);
            }
        }
        if (bounds.isEmpty() && !bezierPath.nodes.isEmpty()) {
            bounds = QRectF(bezierPath.nodes.constFirst().anchor, QSizeF(0.01, 0.01));
        }
    } else if (type == VectorShapeType::Line) {
        additionalBezierPaths.clear();
        pathFillRule = VectorPathFillRule::EvenOdd;
        if (!safePoint(lineStart)) lineStart = QPointF(0.0, 0.0);
        if (!safePoint(lineEnd)) lineEnd = QPointF(100.0, 100.0);
        if (QLineF(lineStart, lineEnd).length() <= 1.0e-6) {
            lineEnd = lineStart + QPointF(1.0, 0.0);
        }
        bounds = QRectF(lineStart, lineEnd).normalized();
    } else {
        additionalBezierPaths.clear();
        pathFillRule = VectorPathFillRule::EvenOdd;
        bounds = bounds.normalized();
        if (bounds.width() <= 0.0) bounds.setWidth(0.01);
        if (bounds.height() <= 0.0) bounds.setHeight(0.01);
    }
    polygonSides = std::clamp(polygonSides, 3, 64);
    starInnerRatio = std::clamp(std::isfinite(starInnerRatio) ? starInnerRatio : 0.5,
                                0.01, 0.99);
    vertexRotationDegrees = std::isfinite(vertexRotationDegrees)
        ? std::remainder(vertexRotationDegrees, 360.0) : -90.0;
    arrowHeadLengthRatio = std::clamp(
        std::isfinite(arrowHeadLengthRatio) ? arrowHeadLengthRatio : 0.35,
        0.1, 0.9);
    arrowShaftWidthRatio = std::clamp(
        std::isfinite(arrowShaftWidthRatio) ? arrowShaftWidthRatio : 0.35,
        0.05, 0.95);
    if (cornerRadiiLinked) cornerRadii.setAll(cornerRadii.topLeft);
    cornerRadii.sanitise();
    fill.normalise();
    stroke.normalise(isOpenPath());
    if (type == VectorShapeType::Line) {
        // A Line has no fillable interior and must retain a visible,
        // editable stroke. Keep this canonical in memory as well as in the UI.
        fill.enabled = false;
        stroke.enabled = true;
        stroke.alignment = VectorStrokeAlignment::Centre;
    } else if (type == VectorShapeType::Path && !bezierPath.closed) {
        // Preserve the user's fill preference while the path is open so that
        // reopening and reclosing a path is non-destructive. Rendering still
        // suppresses fill coverage for open paths.
        stroke.enabled = true;
    }
    revision = std::max<quint64>(1, revision);
    if (id.isNull()) id = QUuid::createUuid();
    if (!transform.isInvertible() || !transformMatrixIsFiniteAndBounded(transform)) {
        transform.reset();
    }
}

bool VectorShape::isSafe() const
{
    const QRectF safeBounds = bounds.normalized();
    bool pathGeometrySafe = bezierPath.isSafe();
    qsizetype totalPathNodes = bezierPath.nodes.size();
    QSet<QUuid> pathNodeIds;
    if (type == VectorShapeType::Path && pathGeometrySafe) {
        for (const VectorPathNode &node : bezierPath.nodes) {
            if (pathNodeIds.contains(node.id)) pathGeometrySafe = false;
            pathNodeIds.insert(node.id);
        }
        for (const VectorBezierPath &path : additionalBezierPaths) {
            totalPathNodes += path.nodes.size();
            if (!path.closed || !path.isSafe()) pathGeometrySafe = false;
            for (const VectorPathNode &node : path.nodes) {
                if (pathNodeIds.contains(node.id)) pathGeometrySafe = false;
                pathNodeIds.insert(node.id);
            }
        }
        if (!additionalBezierPaths.isEmpty() && !bezierPath.closed) {
            pathGeometrySafe = false;
        }
        if (totalPathNodes > VectorBezierPath::MaximumNodeCount) {
            pathGeometrySafe = false;
        }
    }
    const bool geometrySafe = type == VectorShapeType::Path
        ? pathGeometrySafe
        : type == VectorShapeType::Line
            ? safePoint(lineStart) && safePoint(lineEnd)
                && QLineF(lineStart, lineEnd).length() > 1.0e-6
            : finiteBounded(safeBounds.left()) && finiteBounded(safeBounds.top())
            && finiteBounded(safeBounds.right()) && finiteBounded(safeBounds.bottom())
            && std::isfinite(safeBounds.width()) && std::isfinite(safeBounds.height())
            && safeBounds.width() > 0.0 && safeBounds.height() > 0.0
            && safeBounds.width() <= MaximumExtent && safeBounds.height() <= MaximumExtent;
    return !id.isNull() && validShapeType(type) && geometrySafe
        && validPathFillRule(pathFillRule)
        && (type == VectorShapeType::Path || additionalBezierPaths.isEmpty())
        && (type == VectorShapeType::Path
            || pathFillRule == VectorPathFillRule::EvenOdd)
        && polygonSides >= 3 && polygonSides <= 64
        && std::isfinite(starInnerRatio) && starInnerRatio >= 0.01 && starInnerRatio <= 0.99
        && std::isfinite(vertexRotationDegrees)
        && std::isfinite(arrowHeadLengthRatio)
        && arrowHeadLengthRatio >= 0.1 && arrowHeadLengthRatio <= 0.9
        && std::isfinite(arrowShaftWidthRatio)
        && arrowShaftWidthRatio >= 0.05 && arrowShaftWidthRatio <= 0.95
        && cornerRadii.isSafe()
        && fill.colour.isValid() && std::isfinite(fill.opacity)
        && fill.opacity >= 0.0 && fill.opacity <= 1.0
        && stroke.isSafe(isOpenPath())
        && (!isOpenPath() || stroke.enabled)
        && transform.isInvertible() && transformMatrixIsFiniteAndBounded(transform)
        && revision >= 1;
}

QPainterPath VectorShape::geometryPath() const
{
    const QRectF rect = bounds.normalized();
    QPainterPath result;
    switch (type) {
    case VectorShapeType::Rectangle:
        result.addRect(rect);
        break;
    case VectorShapeType::RoundedRectangle:
        result = roundedRectPath(rect, cornerRadii);
        break;
    case VectorShapeType::Ellipse:
        result.addEllipse(rect);
        break;
    case VectorShapeType::Line:
        result.moveTo(lineStart);
        result.lineTo(lineEnd);
        break;
    case VectorShapeType::Polygon:
        result = polygonPath(regularVertices(rect, polygonSides,
                                             vertexRotationDegrees, false, 1.0));
        break;
    case VectorShapeType::Star:
        result = polygonPath(regularVertices(rect, polygonSides,
                                             vertexRotationDegrees, true,
                                             starInnerRatio));
        break;
    case VectorShapeType::Arrow:
        result = blockArrowPath(rect, arrowHeadLengthRatio, arrowShaftWidthRatio);
        break;
    case VectorShapeType::Path: {
        const Qt::FillRule fillRule = pathFillRule == VectorPathFillRule::NonZero
            ? Qt::WindingFill : Qt::OddEvenFill;
        result = bezierPath.painterPath();
        result.setFillRule(fillRule);
        for (const VectorBezierPath &path : additionalBezierPaths) {
            result.addPath(path.painterPath());
        }
        result.setFillRule(fillRule);
        break;
    }
    }
    return result;
}

QPainterPath VectorShape::path() const
{
    return transform.map(geometryPath());
}

QPainterPath VectorShape::pathForWorldTransform(const QTransform &worldTransform) const
{
    if (type != VectorShapeType::RoundedRectangle
        || worldTransform.type() == QTransform::TxProject
        || transform.type() == QTransform::TxProject) {
        return worldTransform.map(path());
    }

    const QRectF rect = bounds.normalized();
    const auto mapPoint = [this, &worldTransform](const QPointF &point) {
        return worldTransform.map(transform.map(point));
    };
    const QPointF topLeft = mapPoint(rect.topLeft());
    const QPointF topRight = mapPoint(rect.topRight());
    const QPointF bottomLeft = mapPoint(rect.bottomLeft());
    const QPointF bottomRight = mapPoint(rect.bottomRight());
    const QPointF horizontal = topRight - topLeft;
    const QPointF vertical = bottomLeft - topLeft;
    const double width = std::hypot(horizontal.x(), horizontal.y());
    const double height = std::hypot(vertical.x(), vertical.y());
    const QPointF affineBottomRight = topLeft + horizontal + vertical;
    const double affineError = std::hypot(bottomRight.x() - affineBottomRight.x(),
                                          bottomRight.y() - affineBottomRight.y());
    const double dot = horizontal.x() * vertical.x() + horizontal.y() * vertical.y();
    const double orthogonalTolerance = std::max(1.0, width * height) * 1.0e-8;
    const double affineTolerance = std::max({1.0, width, height}) * 1.0e-8;
    if (width <= 1.0e-12 || height <= 1.0e-12
        || std::abs(dot) > orthogonalTolerance || affineError > affineTolerance) {
        return worldTransform.map(path());
    }

    VectorCornerRadii documentRadii = cornerRadii;
    documentRadii.normalise(QSizeF(width, height));
    return roundedParallelogramPath(topLeft, horizontal, vertical, documentRadii);
}

QPainterPath VectorShape::strokeOutlineForWorldTransform(
    const QTransform &worldTransform,
    const double widthMultiplier) const
{
    if (!stroke.enabled || !std::isfinite(widthMultiplier)
        || widthMultiplier <= 0.0) {
        return {};
    }
    VectorStroke outlineStyle = stroke;
    outlineStyle.width *= widthMultiplier;
    if (!std::isfinite(outlineStyle.width) || outlineStyle.width <= 0.0) return {};
    return strokeOutline(pathForWorldTransform(worldTransform), outlineStyle);
}

QPainterPath VectorShape::strokePathForWorldTransform(
    const QTransform &worldTransform) const
{
    if (!stroke.enabled) return {};
    const QPainterPath base = pathForWorldTransform(worldTransform);
    // A declared 8 px Inside or Outside stroke occupies the full 8 px on its
    // chosen side. QPainterPathStroker centres its width on the source path, so
    // double the temporary outline before clipping/subtracting it.
    const double widthMultiplier = !isOpenPath()
            && stroke.alignment != VectorStrokeAlignment::Centre
        ? 2.0 : 1.0;
    QPainterPath outline = strokeOutlineForWorldTransform(
        worldTransform, widthMultiplier);
    if (isOpenPath()) {
        if (stroke.startArrowhead != VectorArrowheadType::None) {
            outline = clipStrokeCapAtEndpoint(
                outline, base, true, stroke.width, stroke.cap);
        }
        if (stroke.endArrowhead != VectorArrowheadType::None) {
            outline = clipStrokeCapAtEndpoint(
                outline, base, false, stroke.width, stroke.cap);
        }
        const QPainterPath startMarker = arrowheadPath(
            base, stroke.startArrowhead, stroke.startArrowScale, stroke.width, true);
        const QPainterPath endMarker = arrowheadPath(
            base, stroke.endArrowhead, stroke.endArrowScale, stroke.width, false);
        if (!startMarker.isEmpty()) outline = outline.united(startMarker);
        if (!endMarker.isEmpty()) outline = outline.united(endMarker);
        outline.setFillRule(Qt::WindingFill);
        return outline;
    }
    if (outline.isEmpty() || stroke.alignment == VectorStrokeAlignment::Centre) {
        return outline;
    }
    if (stroke.alignment == VectorStrokeAlignment::Inside) {
        return base.intersected(outline);
    }
    return outline.subtracted(base);
}

QPainterPath VectorShape::styledPathForWorldTransform(
    const QTransform &worldTransform) const
{
    const QPainterPath base = pathForWorldTransform(worldTransform);
    QPainterPath result;
    if (fill.enabled && !isOpenPath()) result = base;
    if (!stroke.enabled) return result;

    // An inside stroke is guaranteed to remain within the semantic shape.
    // Returning the base path as the conservative coverage path avoids Qt path
    // boolean edge cases on concave stars from shrinking tile-culling and
    // transform bounds. The actual stroke is still clipped precisely when
    // rasterised.
    if (!isOpenPath() && stroke.alignment == VectorStrokeAlignment::Inside) {
        return base;
    }

    const QPainterPath outline = strokePathForWorldTransform(worldTransform);
    if (!outline.isEmpty()) result = result.isEmpty() ? outline : result.united(outline);
    return result;
}

QSizeF VectorShape::orthogonalWorldSize(const QTransform &worldTransform, bool *ok) const
{
    if (ok) *ok = false;
    if (type != VectorShapeType::RoundedRectangle
        || worldTransform.type() == QTransform::TxProject
        || transform.type() == QTransform::TxProject) return {};
    const QRectF rect = bounds.normalized();
    const auto mapPoint = [this, &worldTransform](const QPointF &point) {
        return worldTransform.map(transform.map(point));
    };
    const QPointF topLeft = mapPoint(rect.topLeft());
    const QPointF topRight = mapPoint(rect.topRight());
    const QPointF bottomLeft = mapPoint(rect.bottomLeft());
    const QPointF bottomRight = mapPoint(rect.bottomRight());
    const QPointF horizontal = topRight - topLeft;
    const QPointF vertical = bottomLeft - topLeft;
    const double width = std::hypot(horizontal.x(), horizontal.y());
    const double height = std::hypot(vertical.x(), vertical.y());
    const QPointF affineBottomRight = topLeft + horizontal + vertical;
    const double affineError = std::hypot(bottomRight.x() - affineBottomRight.x(),
                                          bottomRight.y() - affineBottomRight.y());
    const double dot = horizontal.x() * vertical.x() + horizontal.y() * vertical.y();
    const double orthogonalTolerance = std::max(1.0, width * height) * 1.0e-8;
    const double affineTolerance = std::max({1.0, width, height}) * 1.0e-8;
    const bool valid = width > 1.0e-12 && height > 1.0e-12
        && std::abs(dot) <= orthogonalTolerance && affineError <= affineTolerance;
    if (ok) *ok = valid;
    return valid ? QSizeF(width, height) : QSizeF();
}

bool VectorShape::cornerRadiiFitWorldTransform(const QTransform &worldTransform,
                                                const double epsilon) const
{
    if (type != VectorShapeType::RoundedRectangle) return true;
    bool orthogonal = false;
    const QSizeF size = orthogonalWorldSize(worldTransform, &orthogonal);
    if (!orthogonal) return true;
    const QSizeF required = cornerRadii.minimumSize();
    return size.width() + epsilon >= required.width()
        && size.height() + epsilon >= required.height();
}

bool VectorShape::convertToPath(const QTransform &worldTransform)
{
    if (type == VectorShapeType::Path) return true;
    if (!isSafe() || !worldTransform.isInvertible()) return false;

    const bool closedPath = type != VectorShapeType::Line;
    QPainterPath sourcePath = geometryPath();

    bool documentPixelCorners = false;
    if (type == VectorShapeType::RoundedRectangle) {
        orthogonalWorldSize(worldTransform, &documentPixelCorners);
    }
    if (documentPixelCorners) {
        // RoundedRectangle is the one semantic primitive whose visible path is
        // not always just its local geometry mapped by the transforms. Its
        // radii are stored in document pixels, so ordinary orthogonal scaling
        // deliberately keeps the corners visually circular and unscaled.
        // Capture that exact visible outline and map it back through both
        // transforms before turning it into editable local Bézier nodes.
        bool worldInverseOk = false;
        bool shapeInverseOk = false;
        const QTransform inverseWorld = worldTransform.inverted(&worldInverseOk);
        const QTransform inverseShape = transform.inverted(&shapeInverseOk);
        if (!worldInverseOk || !shapeInverseOk) return false;
        sourcePath = inverseShape.map(
            inverseWorld.map(pathForWorldTransform(worldTransform)));
    }

    bool converted = false;
    VectorBezierPath path = VectorBezierPath::fromPainterPath(
        sourcePath, closedPath, &converted);
    if (!converted || !path.isSafe()) return false;

    type = VectorShapeType::Path;
    bezierPath = std::move(path);
    additionalBezierPaths.clear();
    pathFillRule = VectorPathFillRule::EvenOdd;
    bounds = bezierPath.contentBounds();
    revision = revision == std::numeric_limits<quint64>::max()
        ? revision : revision + 1;
    normalise();
    return isSafe();
}


bool VectorShape::expandedStrokePath(const QTransform &worldTransform,
                                     VectorShape *expanded) const
{
    if (!expanded || !isSafe() || !stroke.enabled
        || !worldTransform.isInvertible()
        || !transformMatrixIsFiniteAndBounded(worldTransform)) {
        return false;
    }

    if (stroke.pattern == VectorStrokePattern::Dashed) {
        const QPainterPath centreLine = pathForWorldTransform(worldTransform);
        const double pathLength = centreLine.length();
        const double period = stroke.dashLength + stroke.gapLength;
        if (!std::isfinite(pathLength) || !std::isfinite(period)
            || period <= 0.0
            || pathLength / period + 2.0 > MaximumExpandedDashCount) {
            return false;
        }
    }

    const QPainterPath visibleStroke = strokePathForWorldTransform(worldTransform);
    if (visibleStroke.isEmpty()
        || visibleStroke.elementCount() > MaximumExpandedStrokeElements) {
        return false;
    }

    bool inverseOk = false;
    const QTransform inverseWorld = worldTransform.inverted(&inverseOk);
    if (!inverseOk || !transformMatrixIsFiniteAndBounded(inverseWorld)) return false;
    QPainterPath layerLocalStroke = inverseWorld.map(visibleStroke);
    if (layerLocalStroke.isEmpty()) return false;
    layerLocalStroke.setFillRule(visibleStroke.fillRule());

    bool contourOk = false;
    const QVector<QPainterPath> editableContours = fillEquivalentContours(
        layerLocalStroke, &contourOk);
    if (!contourOk || editableContours.isEmpty()) return false;

    QVector<VectorBezierPath> paths;
    paths.reserve(editableContours.size());
    qsizetype totalNodes = 0;
    qsizetype totalElements = 0;
    for (const QPainterPath &contour : editableContours) {
        totalElements += contour.elementCount();
        if (totalElements > MaximumExpandedStrokeElements) return false;
        bool pathOk = false;
        VectorBezierPath path = VectorBezierPath::fromPainterPath(
            contour, true, &pathOk);
        if (!pathOk || !path.isSafe()) return false;
        totalNodes += path.nodes.size();
        if (totalNodes > VectorBezierPath::MaximumNodeCount) return false;
        paths.push_back(std::move(path));
    }
    if (paths.isEmpty()) return false;

    VectorShape result;
    result.type = VectorShapeType::Path;
    result.bezierPath = std::move(paths.first());
    paths.removeFirst();
    result.additionalBezierPaths = std::move(paths);
    result.pathFillRule = layerLocalStroke.fillRule() == Qt::WindingFill
        ? VectorPathFillRule::NonZero
        : VectorPathFillRule::EvenOdd;
    result.bounds = result.bezierPath.contentBounds();
    for (const VectorBezierPath &path : std::as_const(result.additionalBezierPaths)) {
        const QRectF contourBounds = path.contentBounds();
        if (!contourBounds.isEmpty()) {
            result.bounds = result.bounds.isEmpty()
                ? contourBounds : result.bounds.united(contourBounds);
        }
    }
    result.fill.enabled = true;
    result.fill.colour = stroke.colour;
    result.fill.opacity = stroke.opacity;
    result.stroke = stroke;
    result.stroke.enabled = false;
    result.transform.reset();
    result.revision = revision == std::numeric_limits<quint64>::max()
        ? revision : revision + 1;
    result.normalise();
    if (!result.isSafe()) return false;

    // Verify the serialisable compound geometry still covers the exact visible
    // stroke. This catches malformed contour extraction without ever falling
    // back to artificial connector spokes or seam-producing bridge lines.
    const QPainterPath outputCoverage = result.geometryPath();
    QPainterPath expectedCoverage = layerLocalStroke;
    if (!equivalentFillCoverage(expectedCoverage, outputCoverage)) return false;

    *expanded = std::move(result);
    return true;
}

QRectF VectorShape::contentBounds() const
{
    return styledPathForWorldTransform(QTransform()).boundingRect();
}

QVector<QPointF> VectorShape::snapPoints(const QTransform &worldTransform) const
{
    QVector<QPointF> local;
    if (type == VectorShapeType::Path) {
        local = bezierPath.snapPoints();
        for (const VectorBezierPath &path : additionalBezierPaths) {
            local += path.snapPoints();
        }
    } else if (type == VectorShapeType::Line) {
        local = {lineStart, lineEnd, (lineStart + lineEnd) * 0.5};
    } else if (type == VectorShapeType::Polygon || type == VectorShapeType::Star) {
        local = regularVertices(bounds.normalized(), polygonSides,
                                vertexRotationDegrees,
                                type == VectorShapeType::Star,
                                starInnerRatio);
        local.push_back(bounds.center());
    } else if (type == VectorShapeType::Arrow) {
        const QPainterPath arrow = blockArrowPath(bounds.normalized(),
                                                   arrowHeadLengthRatio,
                                                   arrowShaftWidthRatio);
        const QList<QPolygonF> polygons = arrow.toSubpathPolygons();
        if (!polygons.isEmpty()) {
            for (const QPointF &point : polygons.constFirst()) local.push_back(point);
        }
        local.push_back(bounds.center());
    } else {
        const QRectF rect = bounds.normalized();
        local = {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft(),
                 QPointF(rect.center().x(), rect.top()),
                 QPointF(rect.right(), rect.center().y()),
                 QPointF(rect.center().x(), rect.bottom()),
                 QPointF(rect.left(), rect.center().y()), rect.center()};
    }
    QVector<QPointF> result;
    result.reserve(local.size());
    for (const QPointF &point : local) {
        result.push_back(worldTransform.map(transform.map(point)));
    }
    return result;
}

QJsonObject VectorShape::toJson(bool *ok) const
{
    if (!isSafe()) {
        if (ok) *ok = false;
        return {};
    }
    bool fillOk = false;
    bool strokeOk = false;
    bool radiiOk = false;
    bool pathOk = type != VectorShapeType::Path;
    QJsonObject pathObject;
    QJsonArray additionalPathArray;
    if (type == VectorShapeType::Path) {
        pathObject = bezierPath.toJson(&pathOk);
        for (const VectorBezierPath &path : additionalBezierPaths) {
            bool additionalOk = false;
            const QJsonObject additionalObject = path.toJson(&additionalOk);
            if (!additionalOk) {
                pathOk = false;
                break;
            }
            additionalPathArray.append(additionalObject);
        }
    }
    const QJsonObject fillObject = fill.toJson(&fillOk);
    const QJsonObject strokeObject = stroke.toJson(&strokeOk);
    const QJsonObject radiiObject = cornerRadii.toJson(&radiiOk);
    if (!fillOk || !strokeOk || !radiiOk || !pathOk) {
        if (ok) *ok = false;
        return {};
    }
    QJsonObject object;
    object.insert(QStringLiteral("id"), id.toString(QUuid::WithoutBraces));
    object.insert(QStringLiteral("kind"), vectorShapeTypeToString(type));
    object.insert(QStringLiteral("bounds"), rectToJson(bounds));
    object.insert(QStringLiteral("lineStart"), pointToJson(lineStart));
    object.insert(QStringLiteral("lineEnd"), pointToJson(lineEnd));
    object.insert(QStringLiteral("polygonSides"), polygonSides);
    object.insert(QStringLiteral("starInnerRatio"), starInnerRatio);
    object.insert(QStringLiteral("vertexRotationDegrees"), vertexRotationDegrees);
    object.insert(QStringLiteral("arrowHeadLengthRatio"), arrowHeadLengthRatio);
    object.insert(QStringLiteral("arrowShaftWidthRatio"), arrowShaftWidthRatio);
    if (type == VectorShapeType::Path) {
        object.insert(QStringLiteral("path"), pathObject);
        object.insert(QStringLiteral("fillRule"),
                      pathFillRuleToString(pathFillRule));
        if (!additionalPathArray.isEmpty()) {
            object.insert(QStringLiteral("additionalPaths"), additionalPathArray);
        }
    }
    object.insert(QStringLiteral("cornerRadius"), cornerRadii.topLeft);
    object.insert(QStringLiteral("cornerRadii"), radiiObject);
    object.insert(QStringLiteral("cornerRadiiLinked"), cornerRadiiLinked);
    object.insert(QStringLiteral("fill"), fillObject);
    object.insert(QStringLiteral("stroke"), strokeObject);
    object.insert(QStringLiteral("transform"), transformToJson(transform));
    object.insert(QStringLiteral("revision"), QString::number(revision));
    if (ok) *ok = true;
    return object;
}

VectorShape VectorShape::fromJson(const QJsonObject &object, bool *ok)
{
    VectorShape result;
    const QUuid id(object.value(QStringLiteral("id")).toString());
    bool typeOk = false;
    result.id = id;
    result.type = vectorShapeTypeFromString(object.value(QStringLiteral("kind")).toString(),
                                            &typeOk);
    bool boundsOk = false;
    result.bounds = rectFromJson(object.value(QStringLiteral("bounds")).toObject(),
                                 result.type == VectorShapeType::Line
                                     || result.type == VectorShapeType::Path,
                                 &boundsOk);
    bool lineStartOk = true;
    bool lineEndOk = true;
    const QJsonValue lineStartValue = object.value(QStringLiteral("lineStart"));
    const QJsonValue lineEndValue = object.value(QStringLiteral("lineEnd"));
    if (result.type == VectorShapeType::Line) {
        result.lineStart = pointFromJson(lineStartValue.toObject(), &lineStartOk);
        result.lineEnd = pointFromJson(lineEndValue.toObject(), &lineEndOk);
    } else if (!lineStartValue.isUndefined() || !lineEndValue.isUndefined()) {
        // The serializer has historically carried these dormant primitive fields
        // for every shape kind, and they participate in VectorShape equality and
        // cache fingerprints. Preserve them when present so a save/load or a
        // primitive-to-path conversion cannot silently acquire the constructor
        // defaults on Windows or any other platform. Older payloads that omit
        // both fields remain compatible because non-Line rendering never consumes
        // them. A half-present pair is malformed rather than ambiguous.
        lineStartOk = lineStartValue.isObject() && lineEndValue.isObject();
        lineEndOk = lineStartOk;
        if (lineStartOk) {
            result.lineStart = pointFromJson(lineStartValue.toObject(), &lineStartOk);
            result.lineEnd = pointFromJson(lineEndValue.toObject(), &lineEndOk);
        }
    }
    result.polygonSides = object.value(QStringLiteral("polygonSides")).toInt(5);
    result.starInnerRatio = object.value(QStringLiteral("starInnerRatio")).toDouble(0.5);
    result.vertexRotationDegrees = object.value(
        QStringLiteral("vertexRotationDegrees")).toDouble(-90.0);
    const QJsonValue arrowHeadValue = object.value(
        QStringLiteral("arrowHeadLengthRatio"));
    const QJsonValue arrowShaftValue = object.value(
        QStringLiteral("arrowShaftWidthRatio"));
    const bool arrowHeadValueOk = arrowHeadValue.isUndefined() || arrowHeadValue.isDouble();
    const bool arrowShaftValueOk = arrowShaftValue.isUndefined() || arrowShaftValue.isDouble();
    result.arrowHeadLengthRatio = arrowHeadValue.isDouble()
        ? arrowHeadValue.toDouble() : 0.35;
    result.arrowShaftWidthRatio = arrowShaftValue.isDouble()
        ? arrowShaftValue.toDouble() : 0.35;
    bool pathOk = result.type == VectorShapeType::Path
        || (!object.contains(QStringLiteral("additionalPaths"))
            && !object.contains(QStringLiteral("fillRule")));
    bool pathFillRuleOk = true;
    if (result.type == VectorShapeType::Path) {
        const QJsonValue fillRuleValue = object.value(QStringLiteral("fillRule"));
        if (fillRuleValue.isUndefined()) {
            result.pathFillRule = VectorPathFillRule::EvenOdd;
        } else if (fillRuleValue.isString()) {
            result.pathFillRule = pathFillRuleFromString(
                fillRuleValue.toString(), &pathFillRuleOk);
        } else {
            pathFillRuleOk = false;
        }
        const QJsonValue pathValue = object.value(QStringLiteral("path"));
        pathOk = pathValue.isObject();
        if (pathOk) {
            result.bezierPath = VectorBezierPath::fromJson(
                pathValue.toObject(), &pathOk);
        }
        const QJsonValue additionalValue = object.value(
            QStringLiteral("additionalPaths"));
        if (pathOk && !additionalValue.isUndefined()) {
            pathOk = additionalValue.isArray();
            if (pathOk) {
                const QJsonArray paths = additionalValue.toArray();
                if (paths.size() > VectorBezierPath::MaximumNodeCount) {
                    pathOk = false;
                } else {
                    result.additionalBezierPaths.reserve(paths.size());
                    qsizetype totalNodeCount = result.bezierPath.nodes.size();
                    for (const QJsonValue &value : paths) {
                        if (!value.isObject()) {
                            pathOk = false;
                            break;
                        }
                        bool additionalOk = false;
                        VectorBezierPath additional = VectorBezierPath::fromJson(
                            value.toObject(), &additionalOk);
                        if (!additionalOk || !additional.closed
                            || additional.nodes.size()
                                > VectorBezierPath::MaximumNodeCount - totalNodeCount) {
                            pathOk = false;
                            break;
                        }
                        totalNodeCount += additional.nodes.size();
                        result.additionalBezierPaths.push_back(
                            std::move(additional));
                    }
                }
            }
        }
    }

    bool radiiOk = false;
    const QJsonValue radiiValue = object.value(QStringLiteral("cornerRadii"));
    if (radiiValue.isObject()) {
        result.cornerRadii = VectorCornerRadii::fromJson(radiiValue.toObject(), &radiiOk);
        result.cornerRadiiLinked = object.value(
            QStringLiteral("cornerRadiiLinked")).toBool(result.cornerRadii.allEqual());
    } else if (radiiValue.isUndefined()) {
        const double legacyRadius = object.value(QStringLiteral("cornerRadius")).toDouble(0.0);
        radiiOk = std::isfinite(legacyRadius) && legacyRadius >= 0.0;
        result.cornerRadii.setAll(legacyRadius);
        result.cornerRadiiLinked = true;
    }

    bool fillOk = false;
    result.fill = VectorFill::fromJson(object.value(QStringLiteral("fill")).toObject(), &fillOk);
    bool strokeOk = false;
    result.stroke = VectorStroke::fromJson(object.value(QStringLiteral("stroke")).toObject(),
                                           result.type == VectorShapeType::Line
                                               || (result.type == VectorShapeType::Path
                                                   && result.additionalBezierPaths.isEmpty()
                                                   && !result.bezierPath.closed),
                                           &strokeOk);
    bool transformOk = false;
    result.transform = transformFromJson(object.value(QStringLiteral("transform")).toObject(),
                                         &transformOk);
    bool revisionOk = false;
    result.revision = object.value(QStringLiteral("revision")).toString().toULongLong(&revisionOk);
    if (!revisionOk) {
        result.revision = static_cast<quint64>(
            object.value(QStringLiteral("revision")).toDouble(1.0));
    }
    const bool valid = !id.isNull() && typeOk && boundsOk && lineStartOk && lineEndOk
        && pathOk && pathFillRuleOk && radiiOk && fillOk && strokeOk
        && arrowHeadValueOk && arrowShaftValueOk && transformOk
        && result.polygonSides >= 3 && result.polygonSides <= 64
        && std::isfinite(result.starInnerRatio)
        && result.starInnerRatio >= 0.01 && result.starInnerRatio <= 0.99
        && std::isfinite(result.vertexRotationDegrees)
        && std::isfinite(result.arrowHeadLengthRatio)
        && result.arrowHeadLengthRatio >= 0.1 && result.arrowHeadLengthRatio <= 0.9
        && std::isfinite(result.arrowShaftWidthRatio)
        && result.arrowShaftWidthRatio >= 0.05 && result.arrowShaftWidthRatio <= 0.95;
    result.normalise();
    if (ok) *ok = valid && result.isSafe();
    return result;
}

void VectorAppearance::normalise()
{
    schema = CurrentSchema;
    fill.normalise();
    stroke.normalise(true);
}

bool VectorAppearance::isSafe() const
{
    return schema == CurrentSchema
        && fill.colour.isValid() && std::isfinite(fill.opacity)
        && fill.opacity >= 0.0 && fill.opacity <= 1.0
        && stroke.isSafe(true);
}

void VectorAppearance::applyTo(VectorShape &shape) const
{
    if (!isSafe()) return;
    shape.fill = fill;
    shape.stroke = stroke;
    shape.normalise();
}

void VectorAppearance::swapFillAndStroke()
{
    const bool fillEnabled = fill.enabled;
    const QColor fillColour = fill.colour;
    const double fillOpacity = fill.opacity;
    fill.enabled = stroke.enabled;
    fill.colour = stroke.colour;
    fill.opacity = stroke.opacity;
    stroke.enabled = fillEnabled;
    stroke.colour = fillColour;
    stroke.opacity = fillOpacity;
    normalise();
}

QJsonObject VectorAppearance::toJson(bool *ok) const
{
    if (!isSafe()) {
        if (ok) *ok = false;
        return {};
    }
    bool fillOk = false;
    bool strokeOk = false;
    const QJsonObject fillObject = fill.toJson(&fillOk);
    const QJsonObject strokeObject = stroke.toJson(&strokeOk);
    const bool valid = fillOk && strokeOk;
    if (ok) *ok = valid;
    if (!valid) return {};
    return {{QStringLiteral("format"), QStringLiteral("VFXPhotoLabVectorAppearance")},
            {QStringLiteral("schema"), static_cast<int>(schema)},
            {QStringLiteral("fill"), fillObject},
            {QStringLiteral("stroke"), strokeObject}};
}

VectorAppearance VectorAppearance::fromJson(const QJsonObject &object, bool *ok)
{
    VectorAppearance result;
    const bool formatOk = object.value(QStringLiteral("format")).toString()
        == QStringLiteral("VFXPhotoLabVectorAppearance");
    const int schemaValue = object.value(QStringLiteral("schema")).toInt(-1);
    bool fillOk = false;
    bool strokeOk = false;
    const QJsonValue fillValue = object.value(QStringLiteral("fill"));
    const QJsonValue strokeValue = object.value(QStringLiteral("stroke"));
    if (fillValue.isObject()) {
        result.fill = VectorFill::fromJson(fillValue.toObject(), &fillOk);
    }
    if (strokeValue.isObject()) {
        result.stroke = VectorStroke::fromJson(strokeValue.toObject(), false,
                                               &strokeOk);
    }
    // Schema 1 appearances predate arrowheads. The stroke decoder supplies
    // None/1.0 defaults for those omitted fields, then the payload is migrated
    // to the current schema before the normal safety check.
    result.schema = CurrentSchema;
    const bool valid = formatOk && schemaValue >= 1
        && schemaValue <= static_cast<int>(CurrentSchema)
        && fillOk && strokeOk && result.isSafe();
    result.normalise();
    if (ok) *ok = valid;
    return result;
}

VectorAppearance VectorAppearance::fromShape(const VectorShape &shape)
{
    VectorAppearance result;
    result.fill = shape.fill;
    result.stroke = shape.stroke;
    result.normalise();
    return result;
}

VectorAppearance VectorAppearance::sensibleDefaults(
    const QColor &primaryColour,
    const QColor &secondaryColour,
    const bool openPath)
{
    VectorAppearance result;
    result.fill.enabled = !openPath;
    result.fill.colour = secondaryColour.isValid()
        ? secondaryColour : QColor(Qt::white);
    result.fill.opacity = 1.0;
    result.stroke.enabled = openPath;
    result.stroke.colour = primaryColour.isValid()
        ? primaryColour : QColor(Qt::black);
    result.stroke.opacity = 1.0;
    result.stroke.width = 2.0;
    result.stroke.alignment = VectorStrokeAlignment::Centre;
    result.stroke.cap = VectorStrokeCap::Round;
    result.stroke.join = VectorStrokeJoin::Miter;
    result.stroke.miterLimit = 4.0;
    result.stroke.pattern = VectorStrokePattern::Solid;
    result.stroke.dashLength = 12.0;
    result.stroke.gapLength = 8.0;
    result.stroke.dashOffset = 0.0;
    result.normalise();
    return result;
}

void VectorLayerData::normalise()
{
    schema = CurrentSchema;
    for (VectorShape &object : objects) object.normalise();
}

bool VectorLayerData::isSafe() const
{
    if (schema != CurrentSchema
        || !std::isfinite(featherRadius)
        || featherRadius < 0.0
        || featherRadius > MaximumFeatherRadius
        || objects.size() > MaximumObjectCount) {
        return false;
    }
    QSet<QUuid> ids;
    qsizetype totalPathNodes = 0;
    for (const VectorShape &object : objects) {
        if (!object.isSafe() || ids.contains(object.id)) return false;
        ids.insert(object.id);
        if (object.type == VectorShapeType::Path) {
            qsizetype objectNodes = object.bezierPath.nodes.size();
            for (const VectorBezierPath &path : object.additionalBezierPaths) {
                if (path.nodes.size() > MaximumTotalNodeCount - objectNodes) return false;
                objectNodes += path.nodes.size();
            }
            if (objectNodes > MaximumTotalNodeCount - totalPathNodes) return false;
            totalPathNodes += objectNodes;
        }
    }
    return true;
}

QRectF VectorLayerData::contentBounds() const
{
    QRectF result;
    for (const VectorShape &object : objects) {
        const QRectF bounds = object.contentBounds();
        if (!bounds.isEmpty()) result = result.isEmpty() ? bounds : result.united(bounds);
    }
    return result;
}

QVector<QPointF> VectorLayerData::snapPoints(const QTransform &worldTransform) const
{
    constexpr qsizetype MaximumSnapPointCount = 65536;
    QVector<QPointF> result;
    for (const VectorShape &object : objects) {
        if (result.size() >= MaximumSnapPointCount) break;
        const QVector<QPointF> points = object.snapPoints(worldTransform);
        const qsizetype remaining = MaximumSnapPointCount - result.size();
        for (qsizetype index = 0; index < std::min(points.size(), remaining); ++index) {
            result.push_back(points.at(index));
        }
    }
    return result;
}

quint64 VectorLayerData::fingerprint() const
{
    quint64 hash = FnvOffset;
    hashValue(hash, schema);
    hashValue(hash, featherRadius);
    const qsizetype count = objects.size();
    hashValue(hash, count);
    for (const VectorShape &object : objects) {
        const QByteArray idBytes = object.id.toRfc4122();
        hashBytes(hash, idBytes.constData(), idBytes.size());
        hashValue(hash, static_cast<int>(object.type));
        hashValue(hash, object.bounds.x()); hashValue(hash, object.bounds.y());
        hashValue(hash, object.bounds.width()); hashValue(hash, object.bounds.height());
        hashValue(hash, object.lineStart.x()); hashValue(hash, object.lineStart.y());
        hashValue(hash, object.lineEnd.x()); hashValue(hash, object.lineEnd.y());
        hashValue(hash, object.polygonSides);
        hashValue(hash, object.starInnerRatio);
        hashValue(hash, object.vertexRotationDegrees);
        hashValue(hash, object.arrowHeadLengthRatio);
        hashValue(hash, object.arrowShaftWidthRatio);
        const auto hashPath = [&hash](const VectorBezierPath &path) {
            hashValue(hash, path.closed);
            const qsizetype pathNodeCount = path.nodes.size();
            hashValue(hash, pathNodeCount);
            for (const VectorPathNode &node : path.nodes) {
                const QByteArray nodeId = node.id.toRfc4122();
                hashBytes(hash, nodeId.constData(), nodeId.size());
                hashValue(hash, node.anchor.x()); hashValue(hash, node.anchor.y());
                hashValue(hash, node.inHandle.x()); hashValue(hash, node.inHandle.y());
                hashValue(hash, node.outHandle.x()); hashValue(hash, node.outHandle.y());
                hashValue(hash, node.inHandleActive); hashValue(hash, node.outHandleActive);
                hashValue(hash, static_cast<int>(node.mode));
                hashValue(hash, node.cornerRadius);
                hashValue(hash, static_cast<int>(node.cornerStyle));
            }
        };
        hashPath(object.bezierPath);
        const qsizetype additionalPathCount = object.additionalBezierPaths.size();
        hashValue(hash, additionalPathCount);
        for (const VectorBezierPath &path : object.additionalBezierPaths) {
            hashPath(path);
        }
        hashValue(hash, static_cast<int>(object.pathFillRule));
        hashValue(hash, object.cornerRadii.topLeft);
        hashValue(hash, object.cornerRadii.topRight);
        hashValue(hash, object.cornerRadii.bottomRight);
        hashValue(hash, object.cornerRadii.bottomLeft);
        hashValue(hash, object.cornerRadiiLinked);
        hashValue(hash, object.fill.enabled);
        const QRgba64 fillRgba = object.fill.colour.rgba64();
        hashValue(hash, fillRgba.red()); hashValue(hash, fillRgba.green());
        hashValue(hash, fillRgba.blue()); hashValue(hash, fillRgba.alpha());
        hashValue(hash, object.fill.opacity);
        hashValue(hash, object.stroke.enabled);
        const QRgba64 strokeRgba = object.stroke.colour.rgba64();
        hashValue(hash, strokeRgba.red()); hashValue(hash, strokeRgba.green());
        hashValue(hash, strokeRgba.blue()); hashValue(hash, strokeRgba.alpha());
        hashValue(hash, object.stroke.opacity); hashValue(hash, object.stroke.width);
        hashValue(hash, static_cast<int>(object.stroke.alignment));
        hashValue(hash, static_cast<int>(object.stroke.cap));
        hashValue(hash, static_cast<int>(object.stroke.join));
        hashValue(hash, object.stroke.miterLimit);
        hashValue(hash, static_cast<int>(object.stroke.pattern));
        hashValue(hash, object.stroke.dashLength);
        hashValue(hash, object.stroke.gapLength);
        hashValue(hash, object.stroke.dashOffset);
        hashValue(hash, static_cast<int>(object.stroke.startArrowhead));
        hashValue(hash, static_cast<int>(object.stroke.endArrowhead));
        hashValue(hash, object.stroke.startArrowScale);
        hashValue(hash, object.stroke.endArrowScale);
        hashValue(hash, object.transform.m11()); hashValue(hash, object.transform.m12());
        hashValue(hash, object.transform.m13()); hashValue(hash, object.transform.m21());
        hashValue(hash, object.transform.m22()); hashValue(hash, object.transform.m23());
        hashValue(hash, object.transform.m31()); hashValue(hash, object.transform.m32());
        hashValue(hash, object.transform.m33()); hashValue(hash, object.revision);
    }
    return hash == 0 ? 1 : hash;
}

qint64 VectorLayerData::estimatedBytes() const
{
    const qint64 capacity = std::max<qsizetype>(objects.capacity(), objects.size());
    qint64 bytes = static_cast<qint64>(sizeof(VectorLayerData))
        + capacity * static_cast<qint64>(sizeof(VectorShape));
    for (const VectorShape &shape : objects) {
        bytes += std::max<qsizetype>(shape.bezierPath.nodes.capacity(),
                                     shape.bezierPath.nodes.size())
            * static_cast<qint64>(sizeof(VectorPathNode));
        bytes += std::max<qsizetype>(shape.additionalBezierPaths.capacity(),
                                     shape.additionalBezierPaths.size())
            * static_cast<qint64>(sizeof(VectorBezierPath));
        for (const VectorBezierPath &path : shape.additionalBezierPaths) {
            bytes += std::max<qsizetype>(path.nodes.capacity(), path.nodes.size())
                * static_cast<qint64>(sizeof(VectorPathNode));
        }
    }
    return bytes;
}

QJsonObject VectorLayerData::toJson(bool *ok) const
{
    if (!isSafe()) {
        if (ok) *ok = false;
        return {};
    }
    QJsonArray objectArray;
    for (const VectorShape &shape : objects) {
        bool shapeOk = false;
        const QJsonObject shapeObject = shape.toJson(&shapeOk);
        if (!shapeOk) {
            if (ok) *ok = false;
            return {};
        }
        objectArray.append(shapeObject);
    }
    if (ok) *ok = true;
    return {{QStringLiteral("schema"), static_cast<int>(CurrentSchema)},
            {QStringLiteral("featherRadius"), featherRadius},
            {QStringLiteral("objects"), objectArray}};
}

VectorLayerData VectorLayerData::fromJson(const QJsonObject &object, bool *ok)
{
    VectorLayerData result;
    const int schemaValue = object.value(QStringLiteral("schema")).toInt(-1);
    const QJsonValue featherValue = object.value(QStringLiteral("featherRadius"));
    const bool hasFeatherMetadata = !featherValue.isUndefined();
    const QJsonValue objectsValue = object.value(QStringLiteral("objects"));
    if ((schemaValue < 1 || schemaValue > static_cast<int>(CurrentSchema))
        || !objectsValue.isArray()
        || (schemaValue < 8 && hasFeatherMetadata)
        || (schemaValue >= 8 && (!hasFeatherMetadata || !featherValue.isDouble()))) {
        if (ok) *ok = false;
        return result;
    }
    if (schemaValue >= 8) {
        result.featherRadius = featherValue.toDouble();
        if (!std::isfinite(result.featherRadius)
            || result.featherRadius < 0.0
            || result.featherRadius > MaximumFeatherRadius) {
            if (ok) *ok = false;
            return result;
        }
    }
    const QJsonArray objects = objectsValue.toArray();
    if (objects.size() > MaximumObjectCount) {
        if (ok) *ok = false;
        return result;
    }
    result.objects.reserve(objects.size());
    qsizetype totalPathNodes = 0;
    for (const QJsonValue &value : objects) {
        if (!value.isObject()) {
            if (ok) *ok = false;
            return {};
        }
        const QJsonObject shapeObject = value.toObject();
        const QJsonObject strokeObject = shapeObject.value(
            QStringLiteral("stroke")).toObject();
        const bool hasDashMetadata = strokeObject.contains(QStringLiteral("pattern"))
            || strokeObject.contains(QStringLiteral("dashLength"))
            || strokeObject.contains(QStringLiteral("gapLength"))
            || strokeObject.contains(QStringLiteral("dashOffset"));
        const bool hasCompoundPathMetadata = shapeObject.contains(
            QStringLiteral("additionalPaths"));
        const bool hasPathFillRuleMetadata = shapeObject.contains(
            QStringLiteral("fillRule"));
        const bool hasArrowheadMetadata = strokeObject.contains(
            QStringLiteral("startArrowhead"))
            || strokeObject.contains(QStringLiteral("endArrowhead"))
            || strokeObject.contains(QStringLiteral("startArrowScale"))
            || strokeObject.contains(QStringLiteral("endArrowScale"));
        bool shapeOk = false;
        VectorShape shape = VectorShape::fromJson(shapeObject, &shapeOk);
        const bool hasArrowShapeMetadata = shapeObject.contains(
            QStringLiteral("arrowHeadLengthRatio"))
            || shapeObject.contains(QStringLiteral("arrowShaftWidthRatio"))
            || shape.type == VectorShapeType::Arrow;
        qsizetype shapePathNodes = 0;
        if (shapeOk && shape.type == VectorShapeType::Path) {
            shapePathNodes = shape.bezierPath.nodes.size();
            for (const VectorBezierPath &path : shape.additionalBezierPaths) {
                if (path.nodes.size() > MaximumTotalNodeCount - shapePathNodes) {
                    shapeOk = false;
                    break;
                }
                shapePathNodes += path.nodes.size();
            }
            if (shapeOk && shapePathNodes > MaximumTotalNodeCount - totalPathNodes) {
                shapeOk = false;
            }
        }
        if (!shapeOk
            || (schemaValue < 2 && shape.type == VectorShapeType::Path)
            || (schemaValue < 3 && shape.type == VectorShapeType::Path
                && shape.bezierPath.hasCornerMetadata())
            || (schemaValue < 4 && hasDashMetadata)
            || (schemaValue < 5 && hasCompoundPathMetadata)
            || (schemaValue < 6 && hasPathFillRuleMetadata)
            || (schemaValue < 7 && (hasArrowheadMetadata || hasArrowShapeMetadata))) {
            if (ok) *ok = false;
            return {};
        }
        totalPathNodes += shapePathNodes;
        result.objects.push_back(std::move(shape));
    }
    result.normalise();
    const bool valid = result.isSafe();
    if (ok) *ok = valid;
    return result;
}

} // namespace vfx
