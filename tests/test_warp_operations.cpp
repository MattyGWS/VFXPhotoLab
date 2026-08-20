#include "WarpOperations.h"

#include <QtTest/QtTest>

#include <QLineF>

using namespace vfx;

class WarpOperationsTests final : public QObject {
    Q_OBJECT

private slots:
    void identityMeshEvaluatesExactly();
    void interiorPointCreatesSmoothDeformation();
    void resamplingRetainsCurrentShape();
    void foldedMeshIsRejected();
    void interactiveValidationRejectsObviousFold();
    void mirroredMeshRemainsValid();
    void inverseMappingRoundTrips();
    void offCanvasStorageSurvivesIdentityWarp();
    void hiddenRgbSurvivesStraightComponentWarp();
    void rgbaSurfaceNeverCollapsesToGrayscaleStorage();
    void rgba64ComponentsRemainExactAtControlSamples();
};

void WarpOperationsTests::identityMeshEvaluatesExactly()
{
    const QRectF bounds(10.0, 20.0, 120.0, 80.0);
    const WarpMesh mesh = identityWarpMesh(bounds, 3, 3);
    QVERIFY(validateWarpMesh(mesh));
    QVERIFY(warpMeshIsIdentity(mesh));
    QCOMPARE(evaluateWarpMesh(mesh, 0.0, 0.0), bounds.topLeft());
    QCOMPARE(evaluateWarpMesh(mesh, 1.0, 1.0), bounds.bottomRight());
    const QPointF centre = evaluateWarpMesh(mesh, 0.5, 0.5);
    QVERIFY(QLineF(centre, bounds.center()).length() < 1.0e-8);
}

void WarpOperationsTests::interiorPointCreatesSmoothDeformation()
{
    WarpMesh mesh = identityWarpMesh(QRectF(0.0, 0.0, 100.0, 100.0), 3, 3);
    mesh.setPoint(1, 1, mesh.point(1, 1) + QPointF(15.0, -12.0));
    QVERIFY(validateWarpMesh(mesh));
    QVERIFY(!warpMeshIsIdentity(mesh));
    const QPointF centre = evaluateWarpMesh(mesh, 0.5, 0.5);
    QVERIFY(QLineF(centre, QPointF(65.0, 38.0)).length() < 1.0e-6);
    const QPointF nearCentre = evaluateWarpMesh(mesh, 0.45, 0.5);
    QVERIFY(nearCentre.x() > 45.0);
    QVERIFY(nearCentre.x() < centre.x());
}

void WarpOperationsTests::resamplingRetainsCurrentShape()
{
    WarpMesh source = identityWarpMesh(QRectF(0.0, 0.0, 200.0, 100.0), 3, 3);
    source.setPoint(1, 1, source.point(1, 1) + QPointF(20.0, 10.0));
    const WarpMesh dense = resampledWarpMesh(source, 4, 4);
    QVERIFY(validateWarpMesh(dense));
    for (int y = 0; y <= 8; ++y) {
        for (int x = 0; x <= 8; ++x) {
            const double u = x / 8.0;
            const double v = y / 8.0;
            QVERIFY(QLineF(evaluateWarpMesh(source, u, v),
                           evaluateWarpMesh(dense, u, v)).length() < 2.5);
        }
    }
}

void WarpOperationsTests::foldedMeshIsRejected()
{
    WarpMesh mesh = identityWarpMesh(QRectF(0.0, 0.0, 100.0, 100.0), 2, 2);
    const QPointF topLeft = mesh.point(0, 0);
    mesh.setPoint(0, 0, mesh.point(1, 0));
    mesh.setPoint(1, 0, topLeft);
    QString error;
    QVERIFY(!validateWarpMesh(mesh, &error));
    QVERIFY(!error.isEmpty());
}


void WarpOperationsTests::interactiveValidationRejectsObviousFold()
{
    WarpMesh identity = identityWarpMesh(
        QRectF(0.0, 0.0, 320.0, 180.0), 4, 4);
    QVERIFY(validateWarpMeshInteractive(identity));

    WarpMesh folded = identity;
    folded.setPoint(1, 1, QPointF(290.0, 150.0));
    folded.setPoint(2, 2, QPointF(30.0, 30.0));
    QString error;
    QVERIFY(!validateWarpMeshInteractive(folded, &error));
    QVERIFY(!error.isEmpty());
}

void WarpOperationsTests::mirroredMeshRemainsValid()
{
    WarpMesh mesh = identityWarpMesh(QRectF(0.0, 0.0, 100.0, 60.0), 3, 3);
    for (QPointF &point : mesh.points) {
        point.setX(100.0 - point.x());
    }
    QVERIFY(validateWarpMesh(mesh));
    QVERIFY(!warpMeshIsIdentity(mesh));
}

void WarpOperationsTests::inverseMappingRoundTrips()
{
    WarpMesh mesh = identityWarpMesh(QRectF(5.0, 7.0, 140.0, 90.0), 4, 4);
    mesh.setPoint(1, 1, mesh.point(1, 1) + QPointF(8.0, -4.0));
    mesh.setPoint(2, 2, mesh.point(2, 2) + QPointF(-6.0, 7.0));
    QVERIFY(validateWarpMesh(mesh));
    const QPointF source(60.0, 52.0);
    const double u = (source.x() - mesh.sourceBounds.left())
        / mesh.sourceBounds.width();
    const double v = (source.y() - mesh.sourceBounds.top())
        / mesh.sourceBounds.height();
    const QPointF destination = evaluateWarpMesh(mesh, u, v);
    QPointF recovered;
    QVERIFY(invertWarpMesh(mesh, destination, &recovered));
    QVERIFY(QLineF(source, recovered).length() < 0.02);
}

void WarpOperationsTests::offCanvasStorageSurvivesIdentityWarp()
{
    QImage source(9, 5, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    uchar *pixel = source.scanLine(2);
    pixel[0] = 17;
    pixel[1] = 111;
    pixel[2] = 239;
    pixel[3] = 255;

    // The visible transform box covers x=[2,7], while the raster storage starts
    // at x=0. Identity warp must extrapolate the boundary mesh and preserve it.
    const WarpMesh mesh = identityWarpMesh(QRectF(2.0, 0.0, 5.0, 5.0), 3, 3);
    const WarpedSurface warped = warpReferenceSurface(
        source, source.size(), QPointF(), QTransform(), mesh);
    QVERIFY2(warped.isValid(), qPrintable(warped.error));
    const int x = qRound(0.0 - warped.referenceOrigin.x());
    const int y = qRound(2.0 - warped.referenceOrigin.y());
    QVERIFY(x >= 0 && x < warped.image.width());
    QVERIFY(y >= 0 && y < warped.image.height());
    const uchar *result = warped.image.constScanLine(y) + x * 4;
    QCOMPARE(result[0], static_cast<uchar>(17));
    QCOMPARE(result[1], static_cast<uchar>(111));
    QCOMPARE(result[2], static_cast<uchar>(239));
    QCOMPARE(result[3], static_cast<uchar>(255));
}

void WarpOperationsTests::hiddenRgbSurvivesStraightComponentWarp()
{
    QImage source(5, 5, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    uchar *pixel = source.scanLine(2) + 2 * 4;
    pixel[0] = 211;
    pixel[1] = 37;
    pixel[2] = 91;
    pixel[3] = 0;
    const WarpMesh mesh = identityWarpMesh(QRectF(0.0, 0.0, 5.0, 5.0), 3, 3);
    const WarpedSurface warped = warpReferenceSurface(
        source, source.size(), QPointF(), QTransform(), mesh);
    QVERIFY2(warped.isValid(), qPrintable(warped.error));
    const int x = qRound(2.0 - warped.referenceOrigin.x());
    const int y = qRound(2.0 - warped.referenceOrigin.y());
    const uchar *result = warped.image.constScanLine(y) + x * 4;
    QCOMPARE(result[0], static_cast<uchar>(211));
    QCOMPARE(result[1], static_cast<uchar>(37));
    QCOMPARE(result[2], static_cast<uchar>(91));
    QCOMPARE(result[3], static_cast<uchar>(0));
}


void WarpOperationsTests::rgbaSurfaceNeverCollapsesToGrayscaleStorage()
{
    QImage source(3, 3, QImage::Format_RGBA8888);
    source.fill(qRgba(128, 128, 128, 77));
    const WarpMesh mesh = identityWarpMesh(QRectF(0.0, 0.0, 3.0, 3.0), 3, 3);
    const WarpedSurface warped = warpReferenceSurface(
        source, source.size(), QPointF(), QTransform(), mesh);
    QVERIFY2(warped.isValid(), qPrintable(warped.error));
    QCOMPARE(warped.image.format(), QImage::Format_RGBA8888);
    const int x = qRound(1.0 - warped.referenceOrigin.x());
    const int y = qRound(1.0 - warped.referenceOrigin.y());
    const uchar *result = warped.image.constScanLine(y) + x * 4;
    QCOMPARE(result[0], static_cast<uchar>(128));
    QCOMPARE(result[1], static_cast<uchar>(128));
    QCOMPARE(result[2], static_cast<uchar>(128));
    QCOMPARE(result[3], static_cast<uchar>(77));
}

void WarpOperationsTests::rgba64ComponentsRemainExactAtControlSamples()
{
    QImage source(3, 3, QImage::Format_RGBA64);
    source.fill(Qt::transparent);
    auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(1));
    row[1] = QRgba64::fromRgba64(60001, 41003, 22007, 0);
    const WarpMesh mesh = identityWarpMesh(QRectF(0.0, 0.0, 3.0, 3.0), 3, 3);
    const WarpedSurface warped = warpReferenceSurface(
        source, source.size(), QPointF(), QTransform(), mesh);
    QVERIFY2(warped.isValid(), qPrintable(warped.error));
    const int x = qRound(1.0 - warped.referenceOrigin.x());
    const int y = qRound(1.0 - warped.referenceOrigin.y());
    const auto *result = reinterpret_cast<const QRgba64 *>(
        warped.image.constScanLine(y));
    QCOMPARE(result[x].red(), static_cast<quint16>(60001));
    QCOMPARE(result[x].green(), static_cast<quint16>(41003));
    QCOMPARE(result[x].blue(), static_cast<quint16>(22007));
    QCOMPARE(result[x].alpha(), static_cast<quint16>(0));
}

QTEST_MAIN(WarpOperationsTests)
#include "test_warp_operations.moc"
