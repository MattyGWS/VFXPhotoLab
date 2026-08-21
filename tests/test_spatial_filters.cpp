#include "Adjustment.h"
#include "ImageProcessor.h"
#include "SpatialFilter.h"

#include <QByteArray>
#include <QColorSpace>
#include <QRgba64>
#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

using namespace vfx;

namespace {

bool exactImageBytes(const QImage &left, const QImage &right)
{
    if (left.isNull() || right.isNull() || left.size() != right.size()
        || left.format() != right.format() || left.depth() != right.depth()) {
        return false;
    }
    const qsizetype active = static_cast<qsizetype>(left.width()) * left.depth() / 8;
    for (int y = 0; y < left.height(); ++y) {
        if (std::memcmp(left.constScanLine(y), right.constScanLine(y),
                        static_cast<std::size_t>(active)) != 0) {
            return false;
        }
    }
    return true;
}

int maximumPremultipliedDifference(const QImage &left, const QImage &right)
{
    if (left.isNull() || right.isNull() || left.size() != right.size()) return 255;
    const QImage a = left.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage b = right.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int maximum = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto *ar = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *br = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            maximum = std::max({maximum,
                                std::abs(qRed(ar[x]) - qRed(br[x])),
                                std::abs(qGreen(ar[x]) - qGreen(br[x])),
                                std::abs(qBlue(ar[x]) - qBlue(br[x])),
                                std::abs(qAlpha(ar[x]) - qAlpha(br[x]))});
        }
    }
    return maximum;
}

QImage patternedEightBit(const QSize &size)
{
    QImage image(size, QImage::Format_RGBA8888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            uchar *pixel = row + x * 4;
            pixel[0] = static_cast<uchar>((x * 37 + y * 11) & 255);
            pixel[1] = static_cast<uchar>((x * 13 + y * 47) & 255);
            pixel[2] = static_cast<uchar>((x * 71 + y * 5) & 255);
            pixel[3] = static_cast<uchar>((x + y) % 5 == 0 ? 0 : 31 + ((x * 17 + y * 19) & 223));
        }
    }
    return image;
}

QImage exactSixteenBitExpansion(const QImage &source)
{
    const QImage rgba = source.convertToFormat(QImage::Format_RGBA8888);
    QImage result(rgba.size(), QImage::Format_RGBA64);
    result.setColorSpace(rgba.colorSpace());
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar *input = rgba.constScanLine(y);
        auto *output = reinterpret_cast<QRgba64 *>(result.scanLine(y));
        for (int x = 0; x < rgba.width(); ++x) {
            const uchar *pixel = input + x * 4;
            output[x] = QRgba64::fromRgba64(pixel[0] * 257u,
                                             pixel[1] * 257u,
                                             pixel[2] * 257u,
                                             pixel[3] * 257u);
        }
    }
    return result;
}

} // namespace

class SpatialFilterTests final : public QObject {
    Q_OBJECT

private slots:
    void contractNormalisesAndPacksStableGpuLayout();
    void edgeCoordinatesAreDefinedDeterministically();
    void tilePlansScaleRadiiAndDescribeInvalidation();
    void haloExtractionPreservesHiddenRgbAndPrecision();
    void boxFixtureMatchesFullRenderAcrossTileBoundaries();
    void alphaContractsAndBitDepthsRemainConsistent();
    void cancellationAndLargeRadiusGuardsAreCooperative();
    void gaussianApproximationIsDeterministicAndTileSafe();
    void parallelRowProcessorPreservesExactReferenceOutput();
    void adjustmentSchemasRoundTripWithoutReinterpretingOldIds();
    void blurSharpenEssentialsPreserveAlphaAndMatchTiledRendering();
    void blurSharpenEightAndSixteenBitReferencesStayConsistent();
};

void SpatialFilterTests::contractNormalisesAndPacksStableGpuLayout()
{
    SpatialFilterContract contract;
    contract.version = 99;
    contract.documentRadius = QSize(-10, 9000);
    contract.maximumRadius = 9000;
    contract.safetyPadding = 900;
    contract.edgeMode = SpatialEdgeMode::Mirror;
    contract.alphaMode = SpatialAlphaMode::CoverageAwareRgba;
    contract.quality = SpatialPreviewQuality::Interactive;
    contract.normalise();

    QCOMPARE(contract.version, SpatialFilterContract::CurrentVersion);
    QCOMPARE(contract.documentRadius, QSize(0, 4096));
    QCOMPARE(contract.maximumRadius, 4096);
    QCOMPARE(contract.safetyPadding, 64);
    QVERIFY(contract.fingerprint() != 0);
    QCOMPARE(contract.fingerprint(), contract.fingerprint());

    SpatialFilterContract plannedContract = contract;
    plannedContract.documentRadius = QSize(40, 20);
    plannedContract.safetyPadding = 2;
    const SpatialFilterTilePlan plan = SpatialFilterFoundation::plan(
        QRect(100, 50, 80, 40), QSize(500, 250), QSize(1000, 500), plannedContract);
    QVERIFY2(plan.valid, qPrintable(plan.failureReason));
    const SpatialFilterGpuContract gpu = plan.gpuContract(plannedContract, QSize(500, 250));
    QCOMPARE(gpu.sourceWidth, quint32(500));
    QCOMPARE(gpu.sourceHeight, quint32(250));
    QCOMPARE(gpu.outputOriginX, 100);
    QCOMPARE(gpu.outputOriginY, 50);
    QCOMPARE(gpu.outputWidth, quint32(80));
    QCOMPARE(gpu.outputHeight, quint32(40));
    QCOMPARE(gpu.radiusX, quint32(22));
    QCOMPARE(gpu.radiusY, quint32(12));
    QCOMPARE(gpu.edgeMode, quint32(SpatialEdgeMode::Mirror));
    QCOMPARE(gpu.alphaMode, quint32(SpatialAlphaMode::CoverageAwareRgba));
    QCOMPARE(gpu.quality, quint32(SpatialPreviewQuality::Interactive));
    QCOMPARE(sizeof(gpu), std::size_t(64));
}

void SpatialFilterTests::edgeCoordinatesAreDefinedDeterministically()
{
    bool inside = false;
    QCOMPARE(SpatialFilterFoundation::mappedCoordinate(-3, 5, SpatialEdgeMode::Clamp, &inside), 0);
    QVERIFY(inside);
    QCOMPARE(SpatialFilterFoundation::mappedCoordinate(7, 5, SpatialEdgeMode::Clamp), 4);

    QCOMPARE(SpatialFilterFoundation::mappedCoordinate(-1, 5, SpatialEdgeMode::Wrap), 4);
    QCOMPARE(SpatialFilterFoundation::mappedCoordinate(5, 5, SpatialEdgeMode::Wrap), 0);
    QCOMPARE(SpatialFilterFoundation::mappedCoordinate(12, 5, SpatialEdgeMode::Wrap), 2);

    QCOMPARE(SpatialFilterFoundation::mappedCoordinate(-1, 5, SpatialEdgeMode::Mirror), 0);
    QCOMPARE(SpatialFilterFoundation::mappedCoordinate(-2, 5, SpatialEdgeMode::Mirror), 1);
    QCOMPARE(SpatialFilterFoundation::mappedCoordinate(5, 5, SpatialEdgeMode::Mirror), 4);
    QCOMPARE(SpatialFilterFoundation::mappedCoordinate(6, 5, SpatialEdgeMode::Mirror), 3);

    inside = true;
    QCOMPARE(SpatialFilterFoundation::mappedCoordinate(-1, 5, SpatialEdgeMode::Transparent, &inside), 0);
    QVERIFY(!inside);
    inside = false;
    QCOMPARE(SpatialFilterFoundation::mappedCoordinate(3, 5, SpatialEdgeMode::Transparent, &inside), 3);
    QVERIFY(inside);
}

void SpatialFilterTests::tilePlansScaleRadiiAndDescribeInvalidation()
{
    SpatialFilterContract contract;
    contract.documentRadius = QSize(20, 30);
    contract.edgeMode = SpatialEdgeMode::Clamp;
    contract.safetyPadding = 2;
    const SpatialFilterTilePlan plan = SpatialFilterFoundation::plan(
        QRect(0, 0, 100, 80), QSize(500, 250), QSize(1000, 500), contract);
    QVERIFY2(plan.valid, qPrintable(plan.failureReason));
    QCOMPARE(plan.scaledRadius, QSize(12, 17));
    QCOMPARE(plan.samplingRect, QRect(-12, -17, 124, 114));
    QCOMPARE(plan.dependencyBounds, QRect(0, 0, 112, 97));
    QCOMPARE(plan.cropOffset, QPoint(12, 17));
    QVERIFY(plan.cacheFingerprint != 0);

    const QVector<QRect> ordinary = SpatialFilterFoundation::affectedOutputRegions(
        QRect(50, 50, 10, 10), QSize(100, 100), QSize(4, 6), SpatialEdgeMode::Clamp);
    QVERIFY(ordinary == QVector<QRect> {QRect(46, 44, 18, 22)});

    const QVector<QRect> wrapped = SpatialFilterFoundation::affectedOutputRegions(
        QRect(0, 40, 3, 10), QSize(100, 100), QSize(5, 2), SpatialEdgeMode::Wrap);
    QVERIFY(wrapped.contains(QRect(0, 38, 8, 14)));
    QVERIFY(wrapped.contains(QRect(95, 38, 5, 14)));

    const QVector<QRect> mirrored = SpatialFilterFoundation::affectedOutputRegions(
        QRect(0, 40, 3, 10), QSize(100, 100), QSize(5, 2), SpatialEdgeMode::Mirror);
    QVERIFY(mirrored == QVector<QRect> {QRect(0, 0, 100, 100)});

    SpatialFilterContract identity;
    identity.documentRadius = {};
    const SpatialFilterTilePlan identityPlan = SpatialFilterFoundation::plan(
        QRect(10, 20, 30, 40), QSize(100, 100), QSize(100, 100), identity);
    QVERIFY(identityPlan.valid);
    QCOMPARE(identityPlan.scaledRadius, QSize(0, 0));
    QCOMPARE(identityPlan.dependencyBounds, QRect(10, 20, 30, 40));

    SpatialFilterContract excessive;
    excessive.documentRadius = QSize(4097, 1);
    const SpatialFilterTilePlan excessivePlan = SpatialFilterFoundation::plan(
        QRect(10, 20, 30, 40), QSize(100, 100), QSize(100, 100), excessive);
    QVERIFY(!excessivePlan.valid);
    QVERIFY(excessivePlan.failureReason.contains(QStringLiteral("4096-pixel")));
}

void SpatialFilterTests::haloExtractionPreservesHiddenRgbAndPrecision()
{
    QImage source8(2, 1, QImage::Format_RGBA8888);
    uchar *row8 = source8.scanLine(0);
    const uchar values8[8] {10, 20, 30, 0, 200, 150, 100, 255};
    std::memcpy(row8, values8, sizeof(values8));

    SpatialFilterContract contract;
    contract.documentRadius = QSize(1, 0);
    contract.safetyPadding = 0;
    contract.edgeMode = SpatialEdgeMode::Clamp;
    const SpatialFilterTilePlan plan = SpatialFilterFoundation::plan(
        QRect(0, 0, 1, 1), source8.size(), source8.size(), contract);
    QVERIFY(plan.valid);
    const QImage clampHalo = SpatialFilterFoundation::extractHalo(
        source8, plan, SpatialEdgeMode::Clamp);
    QCOMPARE(clampHalo.size(), QSize(3, 1));
    const uchar *clamp = clampHalo.constScanLine(0);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(clamp), 12),
             QByteArray::fromRawData("\x0a\x14\x1e\x00\x0a\x14\x1e\x00\xc8\x96\x64\xff", 12));

    const QImage transparentHalo = SpatialFilterFoundation::extractHalo(
        source8, plan, SpatialEdgeMode::Transparent);
    const uchar *transparent = transparentHalo.constScanLine(0);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(transparent), 4), QByteArray(4, '\0'));
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(transparent + 4), 8),
             QByteArray::fromRawData("\x0a\x14\x1e\x00\xc8\x96\x64\xff", 8));

    const QImage source16 = exactSixteenBitExpansion(source8);
    const QImage halo16 = SpatialFilterFoundation::extractHalo(
        source16, plan, SpatialEdgeMode::Clamp);
    QVERIFY(!halo16.isNull());
    QCOMPARE(halo16.format(), QImage::Format_RGBA64);
    const auto *pixels16 = reinterpret_cast<const QRgba64 *>(halo16.constScanLine(0));
    QCOMPARE(pixels16[0].red(), quint16(10 * 257));
    QCOMPARE(pixels16[0].green(), quint16(20 * 257));
    QCOMPARE(pixels16[0].blue(), quint16(30 * 257));
    QCOMPARE(pixels16[0].alpha(), quint16(0));
    QCOMPARE(pixels16[2].red(), quint16(200 * 257));
}

void SpatialFilterTests::boxFixtureMatchesFullRenderAcrossTileBoundaries()
{
    const QImage source = patternedEightBit(QSize(521, 301));
    const QSize radius(4, 3);
    const QVector<QRect> outputRects {
        QRect(0, 0, 17, 14),
        QRect(247, 129, 25, 31) // crosses the compositor's 256-pixel tile edge
    };
    for (const SpatialEdgeMode edgeMode : {
             SpatialEdgeMode::Clamp,
             SpatialEdgeMode::Mirror,
             SpatialEdgeMode::Wrap,
             SpatialEdgeMode::Transparent}) {
        const QImage full = SpatialFilterFoundation::boxBlurReference(
            source, radius, edgeMode, SpatialAlphaMode::StraightRgba);
        QVERIFY(!full.isNull());

        for (const QRect &outputRect : outputRects) {
            SpatialFilterContract contract;
            contract.documentRadius = radius;
            contract.edgeMode = edgeMode;
            contract.alphaMode = SpatialAlphaMode::StraightRgba;
            contract.safetyPadding = 0;
            const SpatialFilterTilePlan plan = SpatialFilterFoundation::plan(
                outputRect, source.size(), source.size(), contract);
            QVERIFY2(plan.valid, qPrintable(plan.failureReason));
            const QImage halo = SpatialFilterFoundation::extractHalo(source, plan, edgeMode);
            QVERIFY(!halo.isNull());
            const QImage filteredHalo = SpatialFilterFoundation::boxBlurReference(
                halo, radius, SpatialEdgeMode::Clamp, SpatialAlphaMode::StraightRgba);
            QVERIFY(!filteredHalo.isNull());
            const QImage tile = filteredHalo.copy(QRect(plan.cropOffset, outputRect.size()));
            QVERIFY2(exactImageBytes(tile, full.copy(outputRect)),
                     qPrintable(QStringLiteral("Tile mismatch for %1 at %2,%3")
                                    .arg(SpatialFilterFoundation::edgeModeName(edgeMode))
                                    .arg(outputRect.x()).arg(outputRect.y())));
        }
    }
}

void SpatialFilterTests::alphaContractsAndBitDepthsRemainConsistent()
{
    QImage source(6, 1, QImage::Format_RGBA8888);
    uchar *row = source.scanLine(0);
    const uchar values[24] {
        255, 0, 0, 0,
        0, 255, 0, 0,
        0, 0, 255, 255,
        255, 255, 0, 0,
        255, 0, 255, 0,
        0, 255, 255, 128
    };
    std::memcpy(row, values, sizeof(values));

    const QImage preserve = SpatialFilterFoundation::boxBlurReference(
        source, QSize(1, 0), SpatialEdgeMode::Clamp,
        SpatialAlphaMode::PreserveSourceAlpha);
    QVERIFY(!preserve.isNull());
    for (int x = 0; x < source.width(); ++x) {
        QCOMPARE(preserve.constScanLine(0)[x * 4 + 3], row[x * 4 + 3]);
    }
    // Hidden RGB is filtered even where source alpha remains zero.
    QVERIFY(preserve.constScanLine(0)[4] != 0 || preserve.constScanLine(0)[5] != 255);

    const QImage straight = SpatialFilterFoundation::boxBlurReference(
        source, QSize(1, 0), SpatialEdgeMode::Clamp,
        SpatialAlphaMode::StraightRgba);
    QVERIFY(!straight.isNull());
    QVERIFY(straight.constScanLine(0)[1 * 4 + 3] > 0);

    const QImage coverage = SpatialFilterFoundation::boxBlurReference(
        source, QSize(0, 0), SpatialEdgeMode::Clamp,
        SpatialAlphaMode::CoverageAwareRgba);
    QVERIFY(exactImageBytes(coverage, source));

    const QImage source16 = exactSixteenBitExpansion(source);
    const QImage blurred8 = SpatialFilterFoundation::boxBlurReference(
        source, QSize(2, 0), SpatialEdgeMode::Mirror,
        SpatialAlphaMode::PreserveSourceAlpha);
    const QImage blurred16 = SpatialFilterFoundation::boxBlurReference(
        source16, QSize(2, 0), SpatialEdgeMode::Mirror,
        SpatialAlphaMode::PreserveSourceAlpha);
    QVERIFY(!blurred8.isNull());
    QVERIFY(!blurred16.isNull());
    const auto *row16 = reinterpret_cast<const QRgba64 *>(blurred16.constScanLine(0));
    const uchar *out8 = blurred8.constScanLine(0);
    for (int x = 0; x < source.width(); ++x) {
        const int values16[4] {row16[x].red(), row16[x].green(), row16[x].blue(), row16[x].alpha()};
        for (int component = 0; component < 4; ++component) {
            const int reduced = static_cast<int>(std::lround(values16[component] / 257.0));
            QVERIFY(std::abs(reduced - int(out8[x * 4 + component])) <= 1);
        }
    }
}

void SpatialFilterTests::cancellationAndLargeRadiusGuardsAreCooperative()
{
    std::atomic_bool cancelled {true};
    const QImage source = patternedEightBit(QSize(32, 32));
    QVERIFY(SpatialFilterFoundation::boxBlurReference(
        source, QSize(5, 5), SpatialEdgeMode::Clamp,
        SpatialAlphaMode::StraightRgba, &cancelled).isNull());
    QVERIFY(SpatialFilterFoundation::gaussianBlurReference(
        source, QSize(5, 5), SpatialEdgeMode::Clamp,
        SpatialAlphaMode::StraightRgba, &cancelled).isNull());

    QVector<float> plane(32 * 32, 0.5f);
    QVERIFY(!SpatialFilterFoundation::blurSparseThirteenTap(
        &plane, 32, 32, QSize(10, 10), SpatialEdgeMode::Clamp, true, &cancelled));

    SpatialFilterContract contract;
    contract.documentRadius = QSize(4096, 4096);
    contract.safetyPadding = 0;
    const SpatialFilterTilePlan unsafePlan = SpatialFilterFoundation::plan(
        QRect(10000, 10000, 256, 256), QSize(20000, 20000),
        QSize(20000, 20000), contract);
    QVERIFY(!unsafePlan.valid);
    QVERIFY(unsafePlan.failureReason.contains(QStringLiteral("512 MiB")));
}


void SpatialFilterTests::gaussianApproximationIsDeterministicAndTileSafe()
{
    const QImage source = patternedEightBit(QSize(541, 333));
    const QSize radius(23, 17);
    const QImage first = SpatialFilterFoundation::gaussianBlurReference(
        source, radius, SpatialEdgeMode::Clamp,
        SpatialAlphaMode::PreserveSourceAlpha);
    const QImage second = SpatialFilterFoundation::gaussianBlurReference(
        source, radius, SpatialEdgeMode::Clamp,
        SpatialAlphaMode::PreserveSourceAlpha);
    QVERIFY(!first.isNull());
    QVERIFY(exactImageBytes(first, second));

    const QRect outputRect(241, 249, 47, 39);
    SpatialFilterContract contract;
    contract.documentRadius = radius;
    contract.edgeMode = SpatialEdgeMode::Clamp;
    contract.alphaMode = SpatialAlphaMode::PreserveSourceAlpha;
    contract.safetyPadding = 0;
    const SpatialFilterTilePlan plan = SpatialFilterFoundation::plan(
        outputRect, source.size(), source.size(), contract);
    QVERIFY2(plan.valid, qPrintable(plan.failureReason));
    const QImage halo = SpatialFilterFoundation::extractHalo(
        source, plan, SpatialEdgeMode::Clamp);
    QVERIFY(!halo.isNull());
    const QImage filteredHalo = SpatialFilterFoundation::gaussianBlurReference(
        halo, radius, SpatialEdgeMode::Clamp,
        SpatialAlphaMode::PreserveSourceAlpha);
    QVERIFY(!filteredHalo.isNull());
    const QImage tile = filteredHalo.copy(QRect(plan.cropOffset, outputRect.size()));
    QVERIFY(exactImageBytes(tile, first.copy(outputRect)));

    for (int x = 0; x < source.width(); ++x) {
        QCOMPARE(first.constScanLine(7)[x * 4 + 3],
                 source.constScanLine(7)[x * 4 + 3]);
    }
}

void SpatialFilterTests::parallelRowProcessorPreservesExactReferenceOutput()
{
    const QImage source = patternedEightBit(QSize(389, 277));
    std::atomic_int invocations {0};
    std::atomic_int processedRows {0};
    const SpatialRowProcessor reverseRows = [&invocations, &processedRows](
        const int width,
        const int height,
        const std::function<void(int)> &processRow) {
        QVERIFY(width > 0);
        QVERIFY(height > 0);
        invocations.fetch_add(1, std::memory_order_relaxed);
        for (int row = height - 1; row >= 0; --row) {
            processRow(row);
            processedRows.fetch_add(1, std::memory_order_relaxed);
        }
    };

    const QImage sequentialBox = SpatialFilterFoundation::boxBlurReference(
        source, QSize(31, 17), SpatialEdgeMode::Clamp,
        SpatialAlphaMode::CoverageAwareRgba);
    const QImage scheduledBox = SpatialFilterFoundation::boxBlurReference(
        source, QSize(31, 17), SpatialEdgeMode::Clamp,
        SpatialAlphaMode::CoverageAwareRgba, nullptr, reverseRows);
    QVERIFY(!sequentialBox.isNull());
    QVERIFY(exactImageBytes(sequentialBox, scheduledBox));

    const QImage sequentialGaussian = SpatialFilterFoundation::gaussianBlurReference(
        source, QSize(29, 23), SpatialEdgeMode::Clamp,
        SpatialAlphaMode::PreserveSourceAlpha);
    const QImage scheduledGaussian = SpatialFilterFoundation::gaussianBlurReference(
        source, QSize(29, 23), SpatialEdgeMode::Clamp,
        SpatialAlphaMode::PreserveSourceAlpha, nullptr, reverseRows);
    QVERIFY(!sequentialGaussian.isNull());
    QVERIFY(exactImageBytes(sequentialGaussian, scheduledGaussian));
    QVERIFY(invocations.load(std::memory_order_relaxed) >= 20);
    QVERIFY(processedRows.load(std::memory_order_relaxed) > source.height());
}

void SpatialFilterTests::adjustmentSchemasRoundTripWithoutReinterpretingOldIds()
{
    QCOMPARE(static_cast<int>(AdjustmentType::ShadowsHighlights), 15);
    QCOMPARE(static_cast<int>(AdjustmentType::GaussianBlur), 16);
    QCOMPARE(static_cast<int>(AdjustmentType::BoxBlur), 17);
    QCOMPARE(static_cast<int>(AdjustmentType::UnsharpMask), 18);
    QCOMPARE(static_cast<int>(AdjustmentType::HighPass), 19);

    QVector<AdjustmentData> fixtures;
    AdjustmentData gaussian;
    gaussian.reset(AdjustmentType::GaussianBlur);
    auto gaussianParameters = std::get<GaussianBlurParameters>(gaussian.parameters);
    gaussianParameters.radius = 73.5;
    gaussianParameters.affectAlpha = false;
    gaussian.parameters = gaussianParameters;
    fixtures.push_back(gaussian);

    AdjustmentData box;
    box.reset(AdjustmentType::BoxBlur);
    auto boxParameters = std::get<BoxBlurParameters>(box.parameters);
    boxParameters.radius = 14.0;
    boxParameters.affectAlpha = true;
    box.parameters = boxParameters;
    fixtures.push_back(box);

    AdjustmentData unsharp;
    unsharp.reset(AdjustmentType::UnsharpMask);
    auto unsharpParameters = std::get<UnsharpMaskParameters>(unsharp.parameters);
    unsharpParameters.radius = 5.5;
    unsharpParameters.amount = 212.0;
    unsharpParameters.threshold = 11.0;
    unsharp.parameters = unsharpParameters;
    fixtures.push_back(unsharp);

    AdjustmentData highPass;
    highPass.reset(AdjustmentType::HighPass);
    auto highPassParameters = std::get<HighPassParameters>(highPass.parameters);
    highPassParameters.radius = 31.0;
    highPassParameters.monochrome = true;
    highPass.parameters = highPassParameters;
    fixtures.push_back(highPass);

    for (AdjustmentData expected : fixtures) {
        expected.normalise();
        bool writeOk = false;
        const QJsonObject object = expected.toJson(&writeOk);
        QVERIFY(writeOk);
        QCOMPARE(object.value(QStringLiteral("schema")).toInt(),
                 static_cast<int>(AdjustmentData::CurrentSchema));
        bool readOk = false;
        const AdjustmentData restored = AdjustmentData::fromJson(
            object, AdjustmentType::Exposure, &readOk);
        QVERIFY(readOk);
        QVERIFY(restored == expected);
        QCOMPARE(adjustmentProcessingDomain(restored),
                 AdjustmentProcessingDomain::EncodedWorking);
        QVERIFY(adjustmentIsSpatial(restored.type));
    }

    QVector<LayerNode> stack;
    for (const AdjustmentData &data : fixtures) {
        LayerNode layer;
        layer.type = LayerType::Adjustment;
        layer.setAdjustmentData(data);
        stack.push_back(layer);
    }
    QCOMPARE(maximumSpatialAdjustmentRadius(stack), 125);
}

void SpatialFilterTests::blurSharpenEssentialsPreserveAlphaAndMatchTiledRendering()
{
    const QSize size(572, 417);
    QImage source = patternedEightBit(size);
    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = size;

    // Hidden RGB must be processed independently of coverage when these
    // spatial operators are configured not to affect alpha. Build an otherwise
    // identical opaque reference so the test checks that contract directly
    // instead of assuming every filtered transparent sample remains non-black.
    QImage opaqueSource = source;
    opaqueSource.detach();
    for (int y = 0; y < opaqueSource.height(); ++y) {
        uchar *row = opaqueSource.scanLine(y);
        for (int x = 0; x < opaqueSource.width(); ++x) {
            row[x * 4 + 3] = 255;
        }
    }
    LayerNode opaqueBase = base;
    opaqueBase.rasterImage = opaqueSource;

    const QVector<AdjustmentType> types {
        AdjustmentType::GaussianBlur,
        AdjustmentType::BoxBlur,
        AdjustmentType::UnsharpMask,
        AdjustmentType::HighPass
    };
    for (const AdjustmentType type : types) {
        LayerNode adjustment;
        adjustment.type = LayerType::Adjustment;
        adjustment.resetAdjustmentParameters(type);
        switch (type) {
        case AdjustmentType::GaussianBlur: {
            GaussianBlurParameters parameters;
            parameters.radius = 37.0;
            parameters.affectAlpha = false;
            adjustment.setGaussianBlurParameters(parameters);
            break;
        }
        case AdjustmentType::BoxBlur: {
            BoxBlurParameters parameters;
            parameters.radius = 29.0;
            parameters.affectAlpha = false;
            adjustment.setBoxBlurParameters(parameters);
            break;
        }
        case AdjustmentType::UnsharpMask: {
            UnsharpMaskParameters parameters;
            parameters.radius = 17.0;
            parameters.amount = 165.0;
            parameters.threshold = 3.0;
            adjustment.setUnsharpMaskParameters(parameters);
            break;
        }
        case AdjustmentType::HighPass: {
            HighPassParameters parameters;
            parameters.radius = 41.0;
            parameters.monochrome = true;
            adjustment.setHighPassParameters(parameters);
            break;
        }
        default:
            QFAIL("Unexpected fixture adjustment type");
        }

        const QVector<LayerNode> layers {adjustment, base};
        const QImage full = ImageProcessor::renderRegionPreservingHiddenRgb(
            source, layers, source.rect(), size);
        QVERIFY2(!full.isNull(), qPrintable(adjustmentTypeToString(type)));
        QCOMPARE(full.size(), size);

        QImage assembled(size, full.format());
        assembled.fill(Qt::transparent);
        assembled.setColorSpace(full.colorSpace());
        for (int top = 0; top < size.height(); top += 256) {
            for (int left = 0; left < size.width(); left += 256) {
                const QRect region(left, top,
                                   std::min(256, size.width() - left),
                                   std::min(256, size.height() - top));
                const QImage tile = ImageProcessor::renderRegionPreservingHiddenRgb(
                    source, layers, region, size);
                QVERIFY2(!tile.isNull(), qPrintable(adjustmentTypeToString(type)));
                QCOMPARE(tile.size(), region.size());
                for (int y = 0; y < tile.height(); ++y) {
                    std::memcpy(assembled.scanLine(region.y() + y)
                                    + static_cast<qsizetype>(region.x()) * 4,
                                tile.constScanLine(y),
                                static_cast<std::size_t>(tile.width()) * 4);
                }
            }
        }
        QVERIFY2(maximumPremultipliedDifference(assembled, full) <= 2,
                 qPrintable(QStringLiteral("Tiled mismatch for %1")
                                .arg(adjustmentTypeToString(type))));

        const QImage opaqueFull = ImageProcessor::renderRegionPreservingHiddenRgb(
            opaqueSource, {adjustment, opaqueBase}, opaqueSource.rect(), size);
        QVERIFY2(!opaqueFull.isNull(), qPrintable(adjustmentTypeToString(type)));
        QCOMPARE(opaqueFull.size(), size);

        for (int y = 0; y < size.height(); y += 23) {
            const uchar *before = source.constScanLine(y);
            const uchar *after = full.constScanLine(y);
            const uchar *opaqueAfter = opaqueFull.constScanLine(y);
            for (int x = 0; x < size.width(); x += 29) {
                QCOMPARE(after[x * 4 + 3], before[x * 4 + 3]);
                if (before[x * 4 + 3] == 0) {
                    QCOMPARE(after[x * 4], opaqueAfter[x * 4]);
                    QCOMPARE(after[x * 4 + 1], opaqueAfter[x * 4 + 1]);
                    QCOMPARE(after[x * 4 + 2], opaqueAfter[x * 4 + 2]);
                }
            }
        }
    }

    LayerNode alphaBlur;
    alphaBlur.type = LayerType::Adjustment;
    alphaBlur.resetAdjustmentParameters(AdjustmentType::GaussianBlur);
    GaussianBlurParameters alphaParameters;
    alphaParameters.radius = 19.0;
    alphaParameters.affectAlpha = true;
    alphaBlur.setGaussianBlurParameters(alphaParameters);
    const QImage alphaResult = ImageProcessor::renderRegionPreservingHiddenRgb(
        source, {alphaBlur, base}, source.rect(), size);
    QVERIFY(!alphaResult.isNull());
    bool alphaChanged = false;
    for (int y = 0; y < size.height() && !alphaChanged; y += 7) {
        const uchar *before = source.constScanLine(y);
        const uchar *after = alphaResult.constScanLine(y);
        for (int x = 0; x < size.width(); x += 11) {
            if (after[x * 4 + 3] != before[x * 4 + 3]) {
                alphaChanged = true;
                break;
            }
        }
    }
    QVERIFY(alphaChanged);
}


void SpatialFilterTests::blurSharpenEightAndSixteenBitReferencesStayConsistent()
{
    const QImage source8 = patternedEightBit(QSize(73, 61));
    const QImage source16 = exactSixteenBitExpansion(source8);
    const QVector<AdjustmentType> types {
        AdjustmentType::GaussianBlur,
        AdjustmentType::BoxBlur,
        AdjustmentType::UnsharpMask,
        AdjustmentType::HighPass
    };
    for (const AdjustmentType type : types) {
        LayerNode adjustment;
        adjustment.type = LayerType::Adjustment;
        adjustment.resetAdjustmentParameters(type);
        if (type == AdjustmentType::GaussianBlur) {
            GaussianBlurParameters parameters;
            parameters.radius = 9.0;
            parameters.affectAlpha = false;
            adjustment.setGaussianBlurParameters(parameters);
        } else if (type == AdjustmentType::BoxBlur) {
            BoxBlurParameters parameters;
            parameters.radius = 7.0;
            parameters.affectAlpha = false;
            adjustment.setBoxBlurParameters(parameters);
        } else if (type == AdjustmentType::UnsharpMask) {
            UnsharpMaskParameters parameters;
            parameters.radius = 5.0;
            parameters.amount = 140.0;
            parameters.threshold = 0.0;
            adjustment.setUnsharpMaskParameters(parameters);
        } else {
            HighPassParameters parameters;
            parameters.radius = 11.0;
            parameters.monochrome = false;
            adjustment.setHighPassParameters(parameters);
        }

        LayerNode base8;
        base8.type = LayerType::BaseImage;
        base8.rasterImage = source8;
        base8.rasterReferenceSize = source8.size();
        LayerNode base16 = base8;
        base16.rasterImage = source16;

        const QImage result8 = ImageProcessor::renderRegionPreservingHiddenRgb(
            source8, {adjustment, base8}, source8.rect(), source8.size())
                                   .convertToFormat(QImage::Format_RGBA8888);
        const QImage result16 = ImageProcessor::renderRegionPreservingHiddenRgb(
            source16, {adjustment, base16}, source16.rect(), source16.size())
                                    .convertToFormat(QImage::Format_RGBA64);
        QVERIFY(!result8.isNull());
        QVERIFY(!result16.isNull());
        for (int y = 0; y < result8.height(); ++y) {
            const uchar *row8 = result8.constScanLine(y);
            const auto *row16 = reinterpret_cast<const QRgba64 *>(
                result16.constScanLine(y));
            for (int x = 0; x < result8.width(); ++x) {
                const int components16[4] {
                    row16[x].red(), row16[x].green(),
                    row16[x].blue(), row16[x].alpha()
                };
                for (int component = 0; component < 4; ++component) {
                    const int reduced = static_cast<int>(std::lround(
                        components16[component] / 257.0));
                    QVERIFY2(std::abs(reduced - int(row8[x * 4 + component])) <= 3,
                             qPrintable(QStringLiteral("8/16 mismatch for %1 channel %2")
                                            .arg(adjustmentTypeToString(type))
                                            .arg(component)));
                }
            }
        }
    }
}

QTEST_APPLESS_MAIN(SpatialFilterTests)
#include "test_spatial_filters.moc"
