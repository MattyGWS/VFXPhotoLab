#include "WarpOperations.h"

#include <QColorSpace>
#include <QLineF>
#include <QPainter>
#include <QPolygonF>
#include <QRect>
#include <QSizeF>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace vfx {
namespace {

constexpr double kEpsilon = 1.0e-9;
constexpr qint64 kMaximumWarpBytes = 1073741824; // one GiB hard allocation ceiling

bool finitePoint(const QPointF &point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

QPointF extendedPoint(const WarpMesh &mesh, int column, int row)
{
    auto pointInExtendedColumn = [&](const int boundedRow) {
        if (column < 0) {
            return mesh.point(0, boundedRow) * 2.0 - mesh.point(1, boundedRow);
        }
        if (column >= mesh.columns) {
            return mesh.point(mesh.columns - 1, boundedRow) * 2.0
                - mesh.point(mesh.columns - 2, boundedRow);
        }
        return mesh.point(column, boundedRow);
    };

    if (row < 0) {
        return pointInExtendedColumn(0) * 2.0 - pointInExtendedColumn(1);
    }
    if (row >= mesh.rows) {
        return pointInExtendedColumn(mesh.rows - 1) * 2.0
            - pointInExtendedColumn(mesh.rows - 2);
    }
    return pointInExtendedColumn(row);
}

QPointF catmullRom(const QPointF &p0,
                   const QPointF &p1,
                   const QPointF &p2,
                   const QPointF &p3,
                   const double t)
{
    const double t2 = t * t;
    const double t3 = t2 * t;
    return (p1 * 2.0
            + (p2 - p0) * t
            + (p0 * 2.0 - p1 * 5.0 + p2 * 4.0 - p3) * t2
            + (-p0 + p1 * 3.0 - p2 * 3.0 + p3) * t3) * 0.5;
}

QPointF catmullRomDerivative(const QPointF &p0,
                             const QPointF &p1,
                             const QPointF &p2,
                             const QPointF &p3,
                             const double t)
{
    const double t2 = t * t;
    return ((p2 - p0)
            + (p0 * 2.0 - p1 * 5.0 + p2 * 4.0 - p3) * (2.0 * t)
            + (-p0 + p1 * 3.0 - p2 * 3.0 + p3) * (3.0 * t2)) * 0.5;
}

struct MeshCoordinate {
    int cell = 0;
    double fraction = 0.0;
};

MeshCoordinate meshCoordinate(const double value, const int count)
{
    // Keep the first/last polynomial cell active beyond the nominal mesh
    // rectangle. This gives off-canvas raster storage a smooth extrapolated
    // mapping instead of clipping it to the visible transform box.
    const double scaled = value * static_cast<double>(count - 1);
    const int cell = std::clamp(static_cast<int>(std::floor(scaled)),
                                0, count - 2);
    return {cell, scaled - static_cast<double>(cell)};
}

void evaluateWithDerivatives(const WarpMesh &mesh,
                             const double u,
                             const double v,
                             QPointF *position,
                             QPointF *derivativeU,
                             QPointF *derivativeV)
{
    const MeshCoordinate x = meshCoordinate(u, mesh.columns);
    const MeshCoordinate y = meshCoordinate(v, mesh.rows);
    std::array<QPointF, 4> rows {};
    std::array<QPointF, 4> rowDerivatives {};
    for (int sampleRow = -1; sampleRow <= 2; ++sampleRow) {
        const int output = sampleRow + 1;
        const QPointF p0 = extendedPoint(mesh, x.cell - 1, y.cell + sampleRow);
        const QPointF p1 = extendedPoint(mesh, x.cell, y.cell + sampleRow);
        const QPointF p2 = extendedPoint(mesh, x.cell + 1, y.cell + sampleRow);
        const QPointF p3 = extendedPoint(mesh, x.cell + 2, y.cell + sampleRow);
        rows[output] = catmullRom(p0, p1, p2, p3, x.fraction);
        rowDerivatives[output] = catmullRomDerivative(
            p0, p1, p2, p3, x.fraction) * static_cast<double>(mesh.columns - 1);
    }
    if (position) {
        *position = catmullRom(rows[0], rows[1], rows[2], rows[3], y.fraction);
    }
    if (derivativeU) {
        *derivativeU = catmullRom(rowDerivatives[0], rowDerivatives[1],
                                  rowDerivatives[2], rowDerivatives[3],
                                  y.fraction);
    }
    if (derivativeV) {
        *derivativeV = catmullRomDerivative(rows[0], rows[1], rows[2], rows[3],
                                            y.fraction)
            * static_cast<double>(mesh.rows - 1);
    }
}

double squaredLength(const QPointF &value)
{
    return QPointF::dotProduct(value, value);
}

bool solveFromSeed(const WarpMesh &mesh,
                   const QPointF &destination,
                   QPointF seed,
                   const QRectF &uvDomain,
                   QPointF *resolvedUv,
                   double *residualSquared)
{
    seed.setX(std::clamp(seed.x(), uvDomain.left(), uvDomain.right()));
    seed.setY(std::clamp(seed.y(), uvDomain.top(), uvDomain.bottom()));
    double previousResidual = std::numeric_limits<double>::max();
    for (int iteration = 0; iteration < 14; ++iteration) {
        QPointF position;
        QPointF du;
        QPointF dv;
        evaluateWithDerivatives(mesh, seed.x(), seed.y(), &position, &du, &dv);
        const QPointF error = position - destination;
        const double residual = squaredLength(error);
        if (residual <= 1.0e-8) {
            break;
        }
        const double determinant = du.x() * dv.y() - du.y() * dv.x();
        if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12) {
            break;
        }
        const QPointF step((error.x() * dv.y() - error.y() * dv.x()) / determinant,
                           (du.x() * error.y() - du.y() * error.x()) / determinant);
        if (!finitePoint(step)) {
            break;
        }

        double amount = 1.0;
        QPointF candidate;
        double candidateResidual = residual;
        for (int lineSearch = 0; lineSearch < 6; ++lineSearch) {
            candidate = seed - step * amount;
            candidate.setX(std::clamp(candidate.x(),
                                          uvDomain.left(), uvDomain.right()));
            candidate.setY(std::clamp(candidate.y(),
                                          uvDomain.top(), uvDomain.bottom()));
            candidateResidual = squaredLength(
                evaluateWarpMesh(mesh, candidate.x(), candidate.y()) - destination);
            if (candidateResidual <= residual || amount <= 0.0625) {
                break;
            }
            amount *= 0.5;
        }
        seed = candidate;
        if (std::abs(previousResidual - candidateResidual) <= 1.0e-12) {
            break;
        }
        previousResidual = candidateResidual;
    }

    const QPointF mapped = evaluateWarpMesh(mesh, seed.x(), seed.y());
    const double residual = squaredLength(mapped - destination);
    if (resolvedUv) {
        *resolvedUv = seed;
    }
    if (residualSquared) {
        *residualSquared = residual;
    }
    return std::isfinite(residual);
}

QRectF referenceDestinationBounds(const QSize &referenceSize,
                                  const QPointF &referenceOrigin,
                                  const QTransform &referenceToDocument,
                                  const WarpMesh &mesh)
{
    if (referenceSize.isEmpty()) {
        return {};
    }
    bool inverseOk = false;
    const QTransform documentToReference = referenceToDocument.inverted(&inverseOk);
    if (!inverseOk) {
        return {};
    }

    QRectF bounds;
    bool first = true;
    constexpr int samples = 36;
    for (int row = 0; row <= samples; ++row) {
        const double localV = row / static_cast<double>(samples);
        for (int column = 0; column <= samples; ++column) {
            const double localU = column / static_cast<double>(samples);
            const QPointF referencePoint(
                referenceOrigin.x() + localU * referenceSize.width(),
                referenceOrigin.y() + localV * referenceSize.height());
            const QPointF sourceDocument = referenceToDocument.map(referencePoint);
            if (!finitePoint(sourceDocument)) {
                continue;
            }
            const double u = (sourceDocument.x() - mesh.sourceBounds.left())
                / mesh.sourceBounds.width();
            const double v = (sourceDocument.y() - mesh.sourceBounds.top())
                / mesh.sourceBounds.height();
            const QPointF destinationReference = documentToReference.map(
                evaluateWarpMesh(mesh, u, v));
            if (!finitePoint(destinationReference)) {
                continue;
            }
            if (first) {
                bounds = QRectF(destinationReference, QSizeF());
                first = false;
            } else {
                bounds = bounds.united(QRectF(destinationReference, QSizeF()));
            }
        }
    }
    return first ? QRectF() : bounds.normalized();
}

inline bool cancelled(const std::atomic_bool *token)
{
    return token && token->load(std::memory_order_acquire);
}

QRgba64 sampleRgba64(const QImage &image, const double x, const double y)
{
    const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, image.width() - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, image.height() - 1);
    const int x1 = std::min(x0 + 1, image.width() - 1);
    const int y1 = std::min(y0 + 1, image.height() - 1);
    const double fx = std::clamp(x - std::floor(x), 0.0, 1.0);
    const double fy = std::clamp(y - std::floor(y), 0.0, 1.0);
    const auto *row0 = reinterpret_cast<const QRgba64 *>(image.constScanLine(y0));
    const auto *row1 = reinterpret_cast<const QRgba64 *>(image.constScanLine(y1));
    const QRgba64 p00 = row0[x0];
    const QRgba64 p10 = row0[x1];
    const QRgba64 p01 = row1[x0];
    const QRgba64 p11 = row1[x1];
    auto component = [&](auto getter) {
        const double top = getter(p00) * (1.0 - fx) + getter(p10) * fx;
        const double bottom = getter(p01) * (1.0 - fx) + getter(p11) * fx;
        return static_cast<quint16>(std::clamp(
            qRound(top * (1.0 - fy) + bottom * fy), 0, 65535));
    };
    return QRgba64::fromRgba64(
        component([](const QRgba64 p) { return p.red(); }),
        component([](const QRgba64 p) { return p.green(); }),
        component([](const QRgba64 p) { return p.blue(); }),
        component([](const QRgba64 p) { return p.alpha(); }));
}

QRgb sampleRgba8(const QImage &image, const double x, const double y)
{
    const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, image.width() - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, image.height() - 1);
    const int x1 = std::min(x0 + 1, image.width() - 1);
    const int y1 = std::min(y0 + 1, image.height() - 1);
    const double fx = std::clamp(x - std::floor(x), 0.0, 1.0);
    const double fy = std::clamp(y - std::floor(y), 0.0, 1.0);
    const uchar *row0 = image.constScanLine(y0);
    const uchar *row1 = image.constScanLine(y1);
    auto value = [&](const int channel) {
        const double top = row0[x0 * 4 + channel] * (1.0 - fx)
            + row0[x1 * 4 + channel] * fx;
        const double bottom = row1[x0 * 4 + channel] * (1.0 - fx)
            + row1[x1 * 4 + channel] * fx;
        return std::clamp(qRound(top * (1.0 - fy) + bottom * fy), 0, 255);
    };
    return qRgba(value(0), value(1), value(2), value(3));
}

quint16 sampleGray16(const QImage &image, const double x, const double y)
{
    const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, image.width() - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, image.height() - 1);
    const int x1 = std::min(x0 + 1, image.width() - 1);
    const int y1 = std::min(y0 + 1, image.height() - 1);
    const double fx = std::clamp(x - std::floor(x), 0.0, 1.0);
    const double fy = std::clamp(y - std::floor(y), 0.0, 1.0);
    const auto *row0 = reinterpret_cast<const quint16 *>(image.constScanLine(y0));
    const auto *row1 = reinterpret_cast<const quint16 *>(image.constScanLine(y1));
    const double top = row0[x0] * (1.0 - fx) + row0[x1] * fx;
    const double bottom = row1[x0] * (1.0 - fx) + row1[x1] * fx;
    return static_cast<quint16>(std::clamp(
        qRound(top * (1.0 - fy) + bottom * fy), 0, 65535));
}

uchar sampleGray8(const QImage &image, const double x, const double y)
{
    const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, image.width() - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, image.height() - 1);
    const int x1 = std::min(x0 + 1, image.width() - 1);
    const int y1 = std::min(y0 + 1, image.height() - 1);
    const double fx = std::clamp(x - std::floor(x), 0.0, 1.0);
    const double fy = std::clamp(y - std::floor(y), 0.0, 1.0);
    const uchar *row0 = image.constScanLine(y0);
    const uchar *row1 = image.constScanLine(y1);
    const double top = row0[x0] * (1.0 - fx) + row0[x1] * fx;
    const double bottom = row1[x0] * (1.0 - fx) + row1[x1] * fx;
    return static_cast<uchar>(std::clamp(
        qRound(top * (1.0 - fy) + bottom * fy), 0, 255));
}

} // namespace

bool WarpMesh::isStructurallyValid() const
{
    return columns >= 2 && columns <= 4 && rows >= 2 && rows <= 4
        && sourceBounds.isValid() && !sourceBounds.isEmpty()
        && points.size() == columns * rows;
}

int WarpMesh::pointIndex(const int column, const int row) const
{
    return row * columns + column;
}

QPointF WarpMesh::point(const int column, const int row) const
{
    if (column < 0 || column >= columns || row < 0 || row >= rows
        || points.size() != columns * rows) {
        return {};
    }
    return points.at(pointIndex(column, row));
}

void WarpMesh::setPoint(const int column, const int row, const QPointF &position)
{
    if (column < 0 || column >= columns || row < 0 || row >= rows
        || points.size() != columns * rows) {
        return;
    }
    points[pointIndex(column, row)] = position;
}

WarpMesh identityWarpMesh(const QRectF &sourceBounds,
                          const int columns,
                          const int rows)
{
    WarpMesh mesh;
    mesh.columns = std::clamp(columns, 2, 4);
    mesh.rows = std::clamp(rows, 2, 4);
    mesh.sourceBounds = sourceBounds.normalized();
    mesh.points.reserve(mesh.columns * mesh.rows);
    for (int row = 0; row < mesh.rows; ++row) {
        const double v = row / static_cast<double>(mesh.rows - 1);
        for (int column = 0; column < mesh.columns; ++column) {
            const double u = column / static_cast<double>(mesh.columns - 1);
            mesh.points.push_back(QPointF(
                mesh.sourceBounds.left() + u * mesh.sourceBounds.width(),
                mesh.sourceBounds.top() + v * mesh.sourceBounds.height()));
        }
    }
    return mesh;
}

WarpMesh resampledWarpMesh(const WarpMesh &mesh, const int columns, const int rows)
{
    if (!mesh.isStructurallyValid()) {
        return identityWarpMesh(mesh.sourceBounds, columns, rows);
    }
    WarpMesh result = identityWarpMesh(mesh.sourceBounds, columns, rows);
    for (int row = 0; row < result.rows; ++row) {
        const double v = row / static_cast<double>(result.rows - 1);
        for (int column = 0; column < result.columns; ++column) {
            const double u = column / static_cast<double>(result.columns - 1);
            result.setPoint(column, row, evaluateWarpMesh(mesh, u, v));
        }
    }
    return result;
}

QPointF evaluateWarpMesh(const WarpMesh &mesh, const double u, const double v)
{
    QPointF position;
    if (!mesh.isStructurallyValid()) {
        return position;
    }
    evaluateWithDerivatives(mesh, u, v, &position, nullptr, nullptr);
    return position;
}

QPointF evaluateWarpMeshDerivativeU(const WarpMesh &mesh,
                                    const double u,
                                    const double v)
{
    QPointF derivative;
    if (mesh.isStructurallyValid()) {
        evaluateWithDerivatives(mesh, u, v, nullptr, &derivative, nullptr);
    }
    return derivative;
}

QPointF evaluateWarpMeshDerivativeV(const WarpMesh &mesh,
                                    const double u,
                                    const double v)
{
    QPointF derivative;
    if (mesh.isStructurallyValid()) {
        evaluateWithDerivatives(mesh, u, v, nullptr, nullptr, &derivative);
    }
    return derivative;
}

namespace {

bool validateWarpMeshSamples(const WarpMesh &mesh,
                             const int requestedSamples,
                             QString *error)
{
    if (error) {
        error->clear();
    }
    if (!mesh.isStructurallyValid()) {
        if (error) *error = QStringLiteral("Warp mesh dimensions or source bounds are invalid");
        return false;
    }
    for (const QPointF &point : mesh.points) {
        if (!finitePoint(point) || std::abs(point.x()) > 1.0e9
            || std::abs(point.y()) > 1.0e9) {
            if (error) *error = QStringLiteral("Warp mesh contains a non-finite or extreme control point");
            return false;
        }
    }

    const double sourceArea = std::max(1.0,
        mesh.sourceBounds.width() * mesh.sourceBounds.height());
    const double minimumJacobian = sourceArea * 1.0e-8;
    const int samples = std::clamp(requestedSamples, 4, 32);
    double orientation = 0.0;
    for (int row = 0; row <= samples; ++row) {
        const double v = row / static_cast<double>(samples);
        for (int column = 0; column <= samples; ++column) {
            const double u = column / static_cast<double>(samples);
            QPointF du;
            QPointF dv;
            // Compute both derivatives in one tensor evaluation. The previous
            // implementation called the U and V helpers separately, repeating
            // all Catmull-Rom work twice per sample.
            evaluateWithDerivatives(mesh, u, v, nullptr, &du, &dv);
            const double jacobian = du.x() * dv.y() - du.y() * dv.x();
            if (!std::isfinite(jacobian)
                || std::abs(jacobian) <= minimumJacobian) {
                if (error) {
                    *error = QStringLiteral(
                        "Warp mesh would fold, cross or collapse the image surface");
                }
                return false;
            }
            const double sampleOrientation = jacobian > 0.0 ? 1.0 : -1.0;
            if (orientation == 0.0) {
                orientation = sampleOrientation;
            } else if (sampleOrientation != orientation) {
                if (error) {
                    *error = QStringLiteral(
                        "Warp mesh would fold or cross the image surface");
                }
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool validateWarpMeshInteractive(const WarpMesh &mesh, QString *error)
{
    return validateWarpMeshSamples(mesh, 7, error);
}

bool validateWarpMesh(const WarpMesh &mesh, QString *error)
{
    return validateWarpMeshSamples(mesh, 24, error);
}

bool warpMeshIsIdentity(const WarpMesh &mesh, const double tolerance)
{
    if (!mesh.isStructurallyValid()) {
        return true;
    }
    const WarpMesh identity = identityWarpMesh(mesh.sourceBounds,
                                               mesh.columns,
                                               mesh.rows);
    for (int index = 0; index < mesh.points.size(); ++index) {
        if (QLineF(mesh.points.at(index), identity.points.at(index)).length()
            > tolerance) {
            return false;
        }
    }
    return true;
}

QRectF warpMeshDestinationBounds(const WarpMesh &mesh, const int samplesPerAxis)
{
    if (!mesh.isStructurallyValid()) {
        return {};
    }
    const int samples = std::clamp(samplesPerAxis, 8, 128);
    QRectF bounds;
    bool first = true;
    for (int row = 0; row <= samples; ++row) {
        const double v = row / static_cast<double>(samples);
        for (int column = 0; column <= samples; ++column) {
            const double u = column / static_cast<double>(samples);
            const QPointF point = evaluateWarpMesh(mesh, u, v);
            if (first) {
                bounds = QRectF(point, QSizeF());
                first = false;
            } else {
                bounds = bounds.united(QRectF(point, QSizeF()));
            }
        }
    }
    return bounds.normalized();
}

static QRectF warpDomainDestinationBounds(const WarpMesh &mesh,
                                            const QRectF &requestedUvDomain,
                                            const int samplesPerAxis)
{
    if (!mesh.isStructurallyValid() || !requestedUvDomain.isValid()
        || requestedUvDomain.isEmpty()) {
        return {};
    }
    const QRectF uvDomain = requestedUvDomain.normalized();
    const int samples = std::clamp(samplesPerAxis, 8, 128);
    QRectF bounds;
    bool first = true;
    for (int row = 0; row <= samples; ++row) {
        const double v = uvDomain.top()
            + uvDomain.height() * row / static_cast<double>(samples);
        for (int column = 0; column <= samples; ++column) {
            const double u = uvDomain.left()
                + uvDomain.width() * column / static_cast<double>(samples);
            const QPointF point = evaluateWarpMesh(mesh, u, v);
            if (!finitePoint(point)) continue;
            if (first) {
                bounds = QRectF(point, QSizeF());
                first = false;
            } else {
                bounds = bounds.united(QRectF(point, QSizeF()));
            }
        }
    }
    return first ? QRectF() : bounds.normalized().adjusted(-4.0, -4.0, 4.0, 4.0);
}

static bool invertWarpMeshInDomain(const WarpMesh &mesh,
                                   const QPointF &destinationDocument,
                                   const QRectF &requestedUvDomain,
                                   const QRectF &destinationBounds,
                                   QPointF *sourceDocument,
                                   QPointF *resolvedUv,
                                   const QPointF *initialUv)
{
    if (!sourceDocument || !mesh.isStructurallyValid()
        || !finitePoint(destinationDocument)
        || !requestedUvDomain.isValid() || requestedUvDomain.isEmpty()
        || !destinationBounds.isValid() || destinationBounds.isEmpty()) {
        return false;
    }
    const QRectF uvDomain = requestedUvDomain.normalized();
    if (!destinationBounds.contains(destinationDocument)) {
        return false;
    }

    const auto accepted = [&uvDomain](const QPointF &uv,
                                      const double residual) {
        constexpr double margin = 1.0e-5;
        return residual <= 0.35 * 0.35
            && uv.x() >= uvDomain.left() - margin
            && uv.x() <= uvDomain.right() + margin
            && uv.y() >= uvDomain.top() - margin
            && uv.y() <= uvDomain.bottom() + margin;
    };
    const auto publish = [&](QPointF uv) {
        uv.setX(std::clamp(uv.x(), uvDomain.left(), uvDomain.right()));
        uv.setY(std::clamp(uv.y(), uvDomain.top(), uvDomain.bottom()));
        *sourceDocument = QPointF(
            mesh.sourceBounds.left() + uv.x() * mesh.sourceBounds.width(),
            mesh.sourceBounds.top() + uv.y() * mesh.sourceBounds.height());
        if (resolvedUv) *resolvedUv = uv;
    };

    if (initialUv && finitePoint(*initialUv)) {
        QPointF coherentUv;
        double coherentResidual = std::numeric_limits<double>::max();
        if (solveFromSeed(mesh, destinationDocument, *initialUv, uvDomain,
                          &coherentUv, &coherentResidual)
            && accepted(coherentUv, coherentResidual)) {
            publish(coherentUv);
            return true;
        }
    }

    std::array<QPointF, 4> seeds;
    seeds[0] = QPointF(
        destinationBounds.width() > kEpsilon
            ? uvDomain.left() + uvDomain.width()
                * (destinationDocument.x() - destinationBounds.left())
                / destinationBounds.width()
            : uvDomain.center().x(),
        destinationBounds.height() > kEpsilon
            ? uvDomain.top() + uvDomain.height()
                * (destinationDocument.y() - destinationBounds.top())
                / destinationBounds.height()
            : uvDomain.center().y());

    int nearestIndex = 0;
    double nearestDistance = std::numeric_limits<double>::max();
    for (int index = 0; index < mesh.points.size(); ++index) {
        const double distance = squaredLength(mesh.points.at(index)
                                              - destinationDocument);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearestIndex = index;
        }
    }
    seeds[1] = QPointF(
        (nearestIndex % mesh.columns) / static_cast<double>(mesh.columns - 1),
        (nearestIndex / mesh.columns) / static_cast<double>(mesh.rows - 1));
    seeds[2] = uvDomain.center();
    seeds[3] = QPointF(
        std::clamp((destinationDocument.x() - mesh.sourceBounds.left())
                       / mesh.sourceBounds.width(),
                   uvDomain.left(), uvDomain.right()),
        std::clamp((destinationDocument.y() - mesh.sourceBounds.top())
                       / mesh.sourceBounds.height(),
                   uvDomain.top(), uvDomain.bottom()));

    QPointF bestUv;
    double bestResidual = std::numeric_limits<double>::max();
    for (const QPointF &seed : seeds) {
        QPointF candidate;
        double residual = std::numeric_limits<double>::max();
        if (solveFromSeed(mesh, destinationDocument, seed, uvDomain,
                          &candidate, &residual)
            && residual < bestResidual) {
            bestResidual = residual;
            bestUv = candidate;
        }
    }
    if (!accepted(bestUv, bestResidual)) {
        return false;
    }
    publish(bestUv);
    return true;
}

bool invertWarpMesh(const WarpMesh &mesh,
                    const QPointF &destinationDocument,
                    QPointF *sourceDocument,
                    QPointF *resolvedUv,
                    const QPointF *initialUv)
{
    const QRectF uvDomain(0.0, 0.0, 1.0, 1.0);
    const QRectF destinationBounds = warpDomainDestinationBounds(mesh, uvDomain, 18);
    return invertWarpMeshInDomain(mesh,
                                  destinationDocument,
                                  uvDomain,
                                  destinationBounds,
                                  sourceDocument,
                                  resolvedUv,
                                  initialUv);
}

WarpedSurface warpReferenceSurface(
    const QImage &source,
    const QSize &sourceReferenceSize,
    const QPointF &sourceReferenceOrigin,
    const QTransform &referenceToDocument,
    const WarpMesh &mesh,
    const bool outsideWhite,
    const std::atomic_bool *cancelRequested)
{
    WarpedSurface result;
    if (source.isNull() || sourceReferenceSize.isEmpty()) {
        result.error = QStringLiteral("Warp source surface is empty");
        return result;
    }
    QString meshError;
    if (!validateWarpMesh(mesh, &meshError)) {
        result.error = meshError;
        return result;
    }
    bool inverseOk = false;
    const QTransform documentToReference = referenceToDocument.inverted(&inverseOk);
    if (!inverseOk) {
        result.error = QStringLiteral("Layer reference transform is singular");
        return result;
    }
    QPolygonF sourceReferencePolygon;
    const QRectF sourceReferenceRect(sourceReferenceOrigin,
                                     QSizeF(sourceReferenceSize));
    sourceReferencePolygon << sourceReferenceRect.topLeft()
                           << sourceReferenceRect.topRight()
                           << sourceReferenceRect.bottomRight()
                           << sourceReferenceRect.bottomLeft();
    const QRectF sourceDocumentBounds = referenceToDocument
        .map(sourceReferencePolygon).boundingRect().normalized();
    QRectF uvDomain(
        (sourceDocumentBounds.left() - mesh.sourceBounds.left())
            / mesh.sourceBounds.width(),
        (sourceDocumentBounds.top() - mesh.sourceBounds.top())
            / mesh.sourceBounds.height(),
        sourceDocumentBounds.width() / mesh.sourceBounds.width(),
        sourceDocumentBounds.height() / mesh.sourceBounds.height());
    uvDomain = uvDomain.normalized().adjusted(-1.0e-5, -1.0e-5,
                                               1.0e-5, 1.0e-5);
    constexpr double maximumExtrapolation = 16.0;
    uvDomain.setLeft(std::max(-maximumExtrapolation, uvDomain.left()));
    uvDomain.setTop(std::max(-maximumExtrapolation, uvDomain.top()));
    uvDomain.setRight(std::min(1.0 + maximumExtrapolation, uvDomain.right()));
    uvDomain.setBottom(std::min(1.0 + maximumExtrapolation, uvDomain.bottom()));
    if (!uvDomain.isValid() || uvDomain.isEmpty()) {
        result.error = QStringLiteral("Warp source coordinate domain is invalid");
        return result;
    }
    const QRectF domainDestinationBounds = warpDomainDestinationBounds(
        mesh, uvDomain, 36);
    if (domainDestinationBounds.isEmpty()) {
        result.error = QStringLiteral("Warped source domain has no destination bounds");
        return result;
    }
    if (cancelled(cancelRequested)) {
        result.cancelled = true;
        return result;
    }

    const QRectF destinationReference = referenceDestinationBounds(
        sourceReferenceSize, sourceReferenceOrigin, referenceToDocument, mesh);
    if (destinationReference.isEmpty()) {
        result.error = QStringLiteral("Warped surface has no representable destination bounds");
        return result;
    }
    const double leftValue = std::floor(destinationReference.left()) - 1.0;
    const double topValue = std::floor(destinationReference.top()) - 1.0;
    const double rightValue = std::ceil(destinationReference.right()) + 1.0;
    const double bottomValue = std::ceil(destinationReference.bottom()) + 1.0;
    if (!std::isfinite(leftValue) || !std::isfinite(topValue)
        || !std::isfinite(rightValue) || !std::isfinite(bottomValue)
        || leftValue < std::numeric_limits<int>::min()
        || topValue < std::numeric_limits<int>::min()
        || rightValue > std::numeric_limits<int>::max()
        || bottomValue > std::numeric_limits<int>::max()) {
        result.error = QStringLiteral("Warped surface coordinates exceed supported storage");
        return result;
    }
    const int left = static_cast<int>(leftValue);
    const int top = static_cast<int>(topValue);
    const int width = static_cast<int>(rightValue - leftValue);
    const int height = static_cast<int>(bottomValue - topValue);
    const qint64 pixels = static_cast<qint64>(width) * height;
    const bool grayscale = source.format() == QImage::Format_Grayscale8
        || source.format() == QImage::Format_Grayscale16;
    const bool sixteenBit = source.depth() > 32
        || source.format() == QImage::Format_Grayscale16;
    const qint64 bytesPerPixel = grayscale
        ? (sixteenBit ? 2 : 1)
        : (sixteenBit ? 8 : 4);
    if (width <= 0 || height <= 0 || pixels <= 0
        || pixels > kMaximumWarpBytes / bytesPerPixel) {
        result.error = QStringLiteral("Warped surface would exceed the safe allocation limit");
        return result;
    }

    QImage prepared = source.convertToFormat(
        grayscale
            ? (sixteenBit ? QImage::Format_Grayscale16
                          : QImage::Format_Grayscale8)
            : (sixteenBit ? QImage::Format_RGBA64
                          : QImage::Format_RGBA8888));
    if (prepared.isNull()) {
        result.error = QStringLiteral("Warp source conversion failed");
        return result;
    }
    QImage output(QSize(width, height), prepared.format());
    if (output.isNull()) {
        result.error = QStringLiteral("Warp destination allocation failed");
        return result;
    }
    if (grayscale) {
        output.fill(outsideWhite ? Qt::white : Qt::black);
    } else {
        output.fill(Qt::transparent);
    }
    output.setColorSpace(source.colorSpace());
    output.detach();

    QVector<int> rowIndices(height);
    for (int row = 0; row < height; ++row) {
        rowIndices[row] = row;
    }
    std::atomic_bool workerCancelled {false};
    QtConcurrent::blockingMap(rowIndices, [&](const int outputY) {
        if (workerCancelled.load(std::memory_order_relaxed)
            || cancelled(cancelRequested)) {
            workerCancelled.store(true, std::memory_order_relaxed);
            return;
        }
        QPointF previousUv;
        bool previousUvValid = false;
        for (int outputX = 0; outputX < width; ++outputX) {
            if ((outputX & 255) == 0 && cancelled(cancelRequested)) {
                workerCancelled.store(true, std::memory_order_relaxed);
                break;
            }
            const QPointF destinationReferencePoint(
                left + outputX + 0.5,
                top + outputY + 0.5);
            const QPointF destinationDocument = referenceToDocument.map(
                destinationReferencePoint);
            QPointF sourceDocument;
            QPointF uv;
            const bool inverted = invertWarpMeshInDomain(
                mesh, destinationDocument, uvDomain, domainDestinationBounds,
                &sourceDocument, &uv,
                previousUvValid ? &previousUv : nullptr);
            previousUv = uv;
            previousUvValid = inverted;
            if (!inverted) {
                continue;
            }
            const QPointF sourceReference = documentToReference.map(sourceDocument);
            const double sourceX = (sourceReference.x() - sourceReferenceOrigin.x())
                    * prepared.width() / static_cast<double>(sourceReferenceSize.width())
                - 0.5;
            const double sourceY = (sourceReference.y() - sourceReferenceOrigin.y())
                    * prepared.height() / static_cast<double>(sourceReferenceSize.height())
                - 0.5;
            if (sourceX < -0.5 || sourceY < -0.5
                || sourceX > prepared.width() - 0.5
                || sourceY > prepared.height() - 0.5) {
                continue;
            }
            if (grayscale) {
                if (sixteenBit) {
                    auto *line = reinterpret_cast<quint16 *>(output.scanLine(outputY));
                    line[outputX] = sampleGray16(prepared, sourceX, sourceY);
                } else {
                    output.scanLine(outputY)[outputX] = sampleGray8(
                        prepared, sourceX, sourceY);
                }
            } else if (sixteenBit) {
                auto *line = reinterpret_cast<QRgba64 *>(output.scanLine(outputY));
                line[outputX] = sampleRgba64(prepared, sourceX, sourceY);
            } else {
                uchar *pixel = output.scanLine(outputY) + outputX * 4;
                const QRgb sample = sampleRgba8(prepared, sourceX, sourceY);
                pixel[0] = static_cast<uchar>(qRed(sample));
                pixel[1] = static_cast<uchar>(qGreen(sample));
                pixel[2] = static_cast<uchar>(qBlue(sample));
                pixel[3] = static_cast<uchar>(qAlpha(sample));
            }
        }
    });

    if (workerCancelled.load(std::memory_order_relaxed)
        || cancelled(cancelRequested)) {
        result.cancelled = true;
        return result;
    }
    result.image = output;
    result.referenceSize = output.size();
    result.referenceOrigin = QPointF(left, top);
    return result;
}

QImage warpCompositePreviewCpu(const QImage &background,
                               const QImage &foreground,
                               const QSize &documentSize,
                               const WarpMesh &documentMesh,
                               const std::atomic_bool *cancelRequested,
                               QString *error)
{
    if (error) {
        error->clear();
    }
    if (background.isNull() || foreground.isNull()
        || background.size() != foreground.size() || documentSize.isEmpty()) {
        if (error) *error = QStringLiteral("Warp preview requires matching surfaces");
        return {};
    }
    WarpMesh previewMesh = documentMesh;
    const double scaleX = background.width()
        / static_cast<double>(documentSize.width());
    const double scaleY = background.height()
        / static_cast<double>(documentSize.height());
    previewMesh.sourceBounds = QRectF(
        documentMesh.sourceBounds.left() * scaleX,
        documentMesh.sourceBounds.top() * scaleY,
        documentMesh.sourceBounds.width() * scaleX,
        documentMesh.sourceBounds.height() * scaleY);
    for (QPointF &point : previewMesh.points) {
        point = QPointF(point.x() * scaleX, point.y() * scaleY);
    }
    const QRect foregroundRect = previewMesh.sourceBounds
        .adjusted(-2.0, -2.0, 2.0, 2.0)
        .toAlignedRect()
        .intersected(foreground.rect());
    QImage result = background.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (foregroundRect.isEmpty()) {
        result.setColorSpace(background.colorSpace());
        return result;
    }
    const QImage foregroundCrop = foreground.copy(foregroundRect);
    const WarpedSurface warped = warpReferenceSurface(
        foregroundCrop,
        foregroundRect.size(),
        QPointF(foregroundRect.topLeft()),
        QTransform(),
        previewMesh,
        false,
        cancelRequested);
    if (!warped.isValid()) {
        if (error) *error = warped.error;
        return {};
    }
    QPainter painter(&result);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.drawImage(warped.referenceOrigin,
                      warped.image.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
    painter.end();
    result.setColorSpace(background.colorSpace());
    return result;
}

} // namespace vfx
