#pragma once

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QTransform>
#include <QVector>
#include <QString>

#include <atomic>

namespace vfx {

// Smooth document-space transform mesh. columns/rows describe control points,
// not cells: a 3x3 mesh therefore has one interior point. sourceBounds is the
// immutable pre-transform document rectangle; points are the corresponding
// deformed document-space positions.
struct WarpMesh {
    int columns = 3;
    int rows = 3;
    QRectF sourceBounds;
    QVector<QPointF> points;

    bool isStructurallyValid() const;
    int pointIndex(int column, int row) const;
    QPointF point(int column, int row) const;
    void setPoint(int column, int row, const QPointF &position);
};

WarpMesh identityWarpMesh(const QRectF &sourceBounds,
                          int columns = 3,
                          int rows = 3);
WarpMesh resampledWarpMesh(const WarpMesh &mesh, int columns, int rows);

// Catmull-Rom tensor interpolation with linearly extrapolated boundary control
// points. Identity and every affine grid remain exact while interior points
// produce a smooth (C1 inside the domain) deformation.
QPointF evaluateWarpMesh(const WarpMesh &mesh, double u, double v);
QPointF evaluateWarpMeshDerivativeU(const WarpMesh &mesh, double u, double v);
QPointF evaluateWarpMeshDerivativeV(const WarpMesh &mesh, double u, double v);

// Lightweight fold check for live pointer interaction. It samples the same
// Jacobian as the authoritative validator at a lower density so the UI never
// performs the full 24x24 proof for every mouse event. Apply/release still use
// validateWarpMesh().
bool validateWarpMeshInteractive(const WarpMesh &mesh, QString *error = nullptr);
bool validateWarpMesh(const WarpMesh &mesh, QString *error = nullptr);
bool warpMeshIsIdentity(const WarpMesh &mesh, double tolerance = 1.0e-6);
QRectF warpMeshDestinationBounds(const WarpMesh &mesh, int samplesPerAxis = 48);

// Invert one destination document position. The optional UV seed allows image
// resampling to retain scanline coherence; returned UV is clamped to [0, 1].
bool invertWarpMesh(const WarpMesh &mesh,
                    const QPointF &destinationDocument,
                    QPointF *sourceDocument,
                    QPointF *resolvedUv = nullptr,
                    const QPointF *initialUv = nullptr);

struct WarpedSurface {
    QImage image;
    QSize referenceSize;
    QPointF referenceOrigin;
    bool cancelled = false;
    QString error;

    bool isValid() const { return !image.isNull() && referenceSize.isValid(); }
};

// Warp one straight-component stored surface while keeping it in the same
// layer-local reference coordinate system. referenceToDocument is the node's
// world transform before applying raster/mask storage origin. RGB and Alpha are
// sampled independently, so hidden RGB beneath zero Alpha is preserved.
WarpedSurface warpReferenceSurface(
    const QImage &source,
    const QSize &sourceReferenceSize,
    const QPointF &sourceReferenceOrigin,
    const QTransform &referenceToDocument,
    const WarpMesh &mesh,
    bool outsideWhite = false,
    const std::atomic_bool *cancelRequested = nullptr);

// Preview helper for document-sized foreground/background images. Mesh
// coordinates are document-space coordinates matching documentSize.
QImage warpCompositePreviewCpu(const QImage &background,
                               const QImage &foreground,
                               const QSize &documentSize,
                               const WarpMesh &documentMesh,
                               const std::atomic_bool *cancelRequested = nullptr,
                               QString *error = nullptr);

} // namespace vfx
