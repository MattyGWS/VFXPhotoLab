#include "Adjustment.h"
#include "AdjustmentPresetStore.h"
#include "ImageProcessor.h"

#include <QColorSpace>
#include <QJsonObject>
#include <QRgba64>
#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

using namespace vfx;

namespace {

LayerNode baseLayer(const QImage &image)
{
    LayerNode layer;
    layer.type = LayerType::BaseImage;
    layer.rasterImage = image;
    layer.rasterReferenceSize = image.size();
    return layer;
}

LayerNode adjustmentLayer(const AdjustmentData &data)
{
    LayerNode layer;
    layer.type = LayerType::Adjustment;
    layer.setAdjustmentData(data);
    return layer;
}

QImage patternedImage(const QSize &size)
{
    QImage image(size, QImage::Format_RGBA8888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            uchar *pixel = row + x * 4;
            pixel[0] = static_cast<uchar>((x * 31 + y * 7) & 255);
            pixel[1] = static_cast<uchar>((x * 11 + y * 43) & 255);
            pixel[2] = static_cast<uchar>((x * 53 + y * 17) & 255);
            pixel[3] = static_cast<uchar>(((x + y) % 9 == 0)
                ? 0 : (48 + ((x * 19 + y * 23) % 208)));
        }
    }
    return image;
}

bool exactBytes(const QImage &left, const QImage &right)
{
    if (left.isNull() || right.isNull() || left.size() != right.size()
        || left.format() != right.format() || left.depth() != right.depth()) {
        return false;
    }
    const qsizetype activeBytes = static_cast<qsizetype>(left.width()) * left.depth() / 8;
    for (int y = 0; y < left.height(); ++y) {
        if (std::memcmp(left.constScanLine(y), right.constScanLine(y),
                        static_cast<std::size_t>(activeBytes)) != 0) {
            return false;
        }
    }
    return true;
}

QImage render(const QImage &source, const AdjustmentData &data,
              const std::atomic_bool *cancelRequested = nullptr)
{
    return ImageProcessor::renderPreservingHiddenRgb(
        source, {adjustmentLayer(data), baseLayer(source)},
        cancelRequested, source.size());
}

} // namespace

class AdditionalSpatialFiltersTests final : public QObject {
    Q_OBJECT

private slots:
    void appendOnlyIdentitySchemaAndAnisotropicHalos();
    void roundTripsAndRejectsLegacySchema();
    void surfaceBlurPreservesEdgesAlphaAndTiles();
    void motionBlurIsDirectionalAlphaSafeAndTiles();
    void radialBlurModesRespectDocumentGeometryAndTiles();
    void filtersAreConsistentAcrossEightAndSixteenBit();
    void cancellationIsCooperative();
    void builtInPresetsAreSelfContained();
};

void AdditionalSpatialFiltersTests::appendOnlyIdentitySchemaAndAnisotropicHalos()
{
    QCOMPARE(static_cast<int>(AdjustmentType::SurfaceBlur), 26);
    QCOMPARE(static_cast<int>(AdjustmentType::MotionBlur), 27);
    QCOMPARE(static_cast<int>(AdjustmentType::RadialBlur), 28);
    QCOMPARE(AdjustmentData::CurrentSchema, 16u);
    QCOMPARE(adjustmentTypeToString(AdjustmentType::SurfaceBlur), QStringLiteral("surface-blur"));
    QCOMPARE(adjustmentTypeToString(AdjustmentType::MotionBlur), QStringLiteral("motion-blur"));
    QCOMPARE(adjustmentTypeToString(AdjustmentType::RadialBlur), QStringLiteral("radial-blur"));
    QVERIFY(adjustmentIsSpatial(AdjustmentType::SurfaceBlur));
    QVERIFY(adjustmentIsSpatial(AdjustmentType::MotionBlur));
    QVERIFY(adjustmentIsSpatial(AdjustmentType::RadialBlur));

    AdjustmentData horizontal;
    horizontal.reset(AdjustmentType::MotionBlur);
    MotionBlurParameters horizontalParameters;
    horizontalParameters.distance = 40.0;
    horizontalParameters.angle = 0.0;
    horizontal.parameters = horizontalParameters;
    QCOMPARE(adjustmentSpatialRadius2D(horizontal), QSize(21, 0));

    AdjustmentData vertical = horizontal;
    auto verticalParameters = horizontalParameters;
    verticalParameters.angle = 90.0;
    vertical.parameters = verticalParameters;
    QCOMPARE(adjustmentSpatialRadius2D(vertical), QSize(0, 21));

    AdjustmentData surface;
    surface.reset(AdjustmentType::SurfaceBlur);
    SurfaceBlurParameters surfaceParameters;
    surfaceParameters.radius = 12.0;
    surface.parameters = surfaceParameters;
    const QSize cumulative = maximumSpatialAdjustmentRadius2D(
        {adjustmentLayer(horizontal), adjustmentLayer(surface)});
    QCOMPARE(cumulative, QSize(33, 12));
}

void AdditionalSpatialFiltersTests::roundTripsAndRejectsLegacySchema()
{
    QVector<AdjustmentData> fixtures;

    AdjustmentData surface;
    surface.reset(AdjustmentType::SurfaceBlur);
    surface.parameters = SurfaceBlurParameters {17.5, 31.0};
    fixtures.push_back(surface);

    AdjustmentData motion;
    motion.reset(AdjustmentType::MotionBlur);
    motion.parameters = MotionBlurParameters {48.0, -37.0, 40, false};
    fixtures.push_back(motion);

    AdjustmentData radial;
    radial.reset(AdjustmentType::RadialBlur);
    radial.parameters = RadialBlurParameters {
        RadialBlurMode::Zoom, 73.0, 12.0, -8.0, 52, false};
    fixtures.push_back(radial);

    for (AdjustmentData expected : fixtures) {
        expected.normalise();
        bool writeOk = false;
        QJsonObject object = expected.toJson(&writeOk);
        QVERIFY(writeOk);
        QCOMPARE(object.value(QStringLiteral("schema")).toInt(), 16);

        bool readOk = false;
        const AdjustmentData restored = AdjustmentData::fromJson(
            object, AdjustmentType::Exposure, &readOk);
        QVERIFY(readOk);
        QVERIFY(restored == expected);

        object.insert(QStringLiteral("schema"), 14);
        readOk = true;
        AdjustmentData::fromJson(object, AdjustmentType::Exposure, &readOk);
        QVERIFY(!readOk);
    }
}

void AdditionalSpatialFiltersTests::surfaceBlurPreservesEdgesAlphaAndTiles()
{
    QImage edge(65, 41, QImage::Format_RGBA8888);
    edge.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < edge.height(); ++y) {
        uchar *row = edge.scanLine(y);
        for (int x = 0; x < edge.width(); ++x) {
            uchar *pixel = row + x * 4;
            const int value = x < edge.width() / 2 ? 24 : 232;
            const int texture = ((x + y) & 1) ? 9 : -9;
            pixel[0] = static_cast<uchar>(std::clamp(value + texture, 0, 255));
            pixel[1] = pixel[0];
            pixel[2] = pixel[0];
            pixel[3] = static_cast<uchar>((x + y) % 7 == 0 ? 0 : 173);
        }
    }

    AdjustmentData data;
    data.reset(AdjustmentType::SurfaceBlur);
    data.parameters = SurfaceBlurParameters {8.0, 24.0};
    const QImage output = render(edge, data).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!output.isNull());
    QVERIFY(std::abs(output.pixelColor(31, 20).red() - output.pixelColor(33, 20).red()) > 100);
    QVERIFY(std::abs(output.pixelColor(8, 20).red() - output.pixelColor(9, 20).red())
            < std::abs(edge.pixelColor(8, 20).red() - edge.pixelColor(9, 20).red()));
    for (int y = 0; y < edge.height(); ++y) {
        for (int x = 0; x < edge.width(); ++x) {
            QCOMPARE(output.pixelColor(x, y).alpha(), edge.pixelColor(x, y).alpha());
        }
    }

    const QImage source = patternedImage({113, 89});
    const QVector<LayerNode> layers {adjustmentLayer(data), baseLayer(source)};
    const QImage full = ImageProcessor::renderPreservingHiddenRgb(
        source, layers, nullptr, source.size()).convertToFormat(QImage::Format_RGBA8888);
    const QRect region(29, 17, 53, 47);
    const QImage tile = ImageProcessor::renderRegionPreservingHiddenRgb(
        source, layers, region, source.size()).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(exactBytes(tile, full.copy(region)));
}

void AdditionalSpatialFiltersTests::motionBlurIsDirectionalAlphaSafeAndTiles()
{
    QImage impulse(81, 61, QImage::Format_RGBA8888);
    impulse.setColorSpace(QColorSpace(QColorSpace::SRgb));
    impulse.fill(QColor(0, 0, 0, 0));
    impulse.setPixelColor(40, 30, QColor(255, 180, 60, 255));

    AdjustmentData horizontal;
    horizontal.reset(AdjustmentType::MotionBlur);
    MotionBlurParameters parameters;
    parameters.distance = 30.0;
    parameters.angle = 0.0;
    parameters.samples = 48;
    parameters.affectAlpha = true;
    horizontal.parameters = parameters;
    const QImage horizontalOutput = render(impulse, horizontal).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(horizontalOutput.pixelColor(29, 30).alpha() > 0);
    QCOMPARE(horizontalOutput.pixelColor(40, 20).alpha(), 0);

    const QImage source = patternedImage({121, 93});
    parameters.distance = 37.0;
    parameters.angle = 31.0;
    parameters.samples = 40;
    parameters.affectAlpha = false;
    horizontal.parameters = parameters;
    const QVector<LayerNode> layers {adjustmentLayer(horizontal), baseLayer(source)};
    const QImage full = ImageProcessor::renderPreservingHiddenRgb(
        source, layers, nullptr, source.size()).convertToFormat(QImage::Format_RGBA8888);
    const QRect region(31, 19, 57, 49);
    const QImage tile = ImageProcessor::renderRegionPreservingHiddenRgb(
        source, layers, region, source.size()).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(exactBytes(tile, full.copy(region)));
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            QCOMPARE(full.pixelColor(x, y).alpha(), source.pixelColor(x, y).alpha());
        }
    }
}

void AdditionalSpatialFiltersTests::radialBlurModesRespectDocumentGeometryAndTiles()
{
    const QImage source = patternedImage({119, 91});
    for (const RadialBlurMode mode : {RadialBlurMode::Spin, RadialBlurMode::Zoom}) {
        AdjustmentData data;
        data.reset(AdjustmentType::RadialBlur);
        RadialBlurParameters parameters;
        parameters.mode = mode;
        parameters.amount = 42.0;
        parameters.centreX = 13.0;
        parameters.centreY = -11.0;
        parameters.samples = 44;
        parameters.affectAlpha = false;
        data.parameters = parameters;
        const QVector<LayerNode> layers {adjustmentLayer(data), baseLayer(source)};
        const QImage full = ImageProcessor::renderPreservingHiddenRgb(
            source, layers, nullptr, source.size()).convertToFormat(QImage::Format_RGBA8888);
        const QRect region(27, 21, 61, 43);
        const QImage tile = ImageProcessor::renderRegionPreservingHiddenRgb(
            source, layers, region, source.size()).convertToFormat(QImage::Format_RGBA8888);
        QVERIFY2(exactBytes(tile, full.copy(region)),
                 mode == RadialBlurMode::Spin ? "Spin tile mismatch" : "Zoom tile mismatch");
        QVERIFY(!exactBytes(full, source));
    }

    AdjustmentData identity;
    identity.reset(AdjustmentType::RadialBlur);
    RadialBlurParameters identityParameters;
    identityParameters.amount = 0.0;
    identity.parameters = identityParameters;
    QVERIFY(exactBytes(render(source, identity).convertToFormat(QImage::Format_RGBA8888), source));
}

void AdditionalSpatialFiltersTests::filtersAreConsistentAcrossEightAndSixteenBit()
{
    const QImage source8 = patternedImage({43, 35});
    QImage source16 = source8.convertToFormat(QImage::Format_RGBA64);
    source16.setColorSpace(source8.colorSpace());

    QVector<AdjustmentData> fixtures;
    AdjustmentData surface;
    surface.reset(AdjustmentType::SurfaceBlur);
    surface.parameters = SurfaceBlurParameters {5.0, 28.0};
    fixtures.push_back(surface);

    AdjustmentData motion;
    motion.reset(AdjustmentType::MotionBlur);
    motion.parameters = MotionBlurParameters {13.0, -24.0, 24, false};
    fixtures.push_back(motion);

    AdjustmentData radial;
    radial.reset(AdjustmentType::RadialBlur);
    radial.parameters = RadialBlurParameters {
        RadialBlurMode::Zoom, 18.0, -8.0, 7.0, 24, false};
    fixtures.push_back(radial);

    for (const AdjustmentData &data : fixtures) {
        const QImage output8 = render(source8, data).convertToFormat(QImage::Format_RGBA8888);
        const QImage output16 = render(source16, data).convertToFormat(QImage::Format_RGBA64);
        QVERIFY(!output8.isNull());
        QVERIFY(!output16.isNull());
        QCOMPARE(output8.size(), output16.size());
        for (int y = 0; y < output8.height(); ++y) {
            const auto *high = reinterpret_cast<const QRgba64 *>(output16.constScanLine(y));
            for (int x = 0; x < output8.width(); ++x) {
                const QColor low = output8.pixelColor(x, y);
                QVERIFY(std::abs(low.red() - qRound(high[x].red() / 257.0)) <= 2);
                QVERIFY(std::abs(low.green() - qRound(high[x].green() / 257.0)) <= 2);
                QVERIFY(std::abs(low.blue() - qRound(high[x].blue() / 257.0)) <= 2);
                QCOMPARE(high[x].alpha(), static_cast<quint16>(low.alpha() * 257));
            }
        }
    }
}

void AdditionalSpatialFiltersTests::cancellationIsCooperative()
{
    const QImage source = patternedImage({127, 97});
    std::atomic_bool cancelled {true};
    for (const AdjustmentType type : {AdjustmentType::SurfaceBlur,
                                      AdjustmentType::MotionBlur,
                                      AdjustmentType::RadialBlur}) {
        AdjustmentData data;
        data.reset(type);
        const QImage output = render(source, data, &cancelled);
        QVERIFY(output.isNull());
    }
}

void AdditionalSpatialFiltersTests::builtInPresetsAreSelfContained()
{
    for (const AdjustmentType type : {AdjustmentType::SurfaceBlur,
                                      AdjustmentType::MotionBlur,
                                      AdjustmentType::RadialBlur}) {
        const QVector<AdjustmentPreset> presets = AdjustmentPresetStore::builtInPresets(type);
        QVERIFY(!presets.isEmpty());
        for (const AdjustmentPreset &preset : presets) {
            QCOMPARE(preset.adjustment.type, type);
            QCOMPARE(preset.adjustment.schema, AdjustmentData::CurrentSchema);
            bool ok = false;
            QVERIFY(!preset.adjustment.toJson(&ok).isEmpty());
            QVERIFY(ok);
        }
    }
}

QTEST_MAIN(AdditionalSpatialFiltersTests)
#include "test_additional_spatial_filters.moc"
