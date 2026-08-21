#include "Adjustment.h"
#include "AdjustmentPresetStore.h"
#include "ImageProcessor.h"

#include <QColorSpace>
#include <QJsonObject>
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
            pixel[3] = static_cast<uchar>(((x + y) % 7 == 0) ? 0 : ((x * 19 + y * 23) & 255));
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

QImage render(const QImage &source, const AdjustmentData &data)
{
    return ImageProcessor::renderPreservingHiddenRgb(
        source, {adjustmentLayer(data), baseLayer(source)}, nullptr, source.size());
}

} // namespace

class OpticalSpatialColourEffectsTests final : public QObject {
    Q_OBJECT

private slots:
    void appendOnlyIdentityAndSchema();
    void roundTripsAndRejectsLegacySchema();
    void vignetteUsesDocumentGeometryAndPreservesAlpha();
    void expandedVignetteCanDarkenCornersWithoutDarkeningEdgeCentres();
    void vignetteMatchesRegionRendering();
    void rgbSplitPreservesAlphaAndMatchesRegionRendering();
    void chromaticCorrectionIdentityAndRegionRendering();
    void effectsAreConsistentAcrossEightAndSixteenBit();
    void cancellationIsCooperative();
    void builtInPresetsAreSelfContained();
};

void OpticalSpatialColourEffectsTests::appendOnlyIdentityAndSchema()
{
    QCOMPARE(static_cast<int>(AdjustmentType::Vignette), 23);
    QCOMPARE(static_cast<int>(AdjustmentType::RgbSplit), 24);
    QCOMPARE(static_cast<int>(AdjustmentType::ChromaticAberrationCorrection), 25);
    QCOMPARE(AdjustmentData::CurrentSchema, 16u);
    QCOMPARE(adjustmentTypeToString(AdjustmentType::RgbSplit), QStringLiteral("rgb-split"));
    QVERIFY(!adjustmentIsSpatial(AdjustmentType::Vignette));
    QVERIFY(adjustmentIsSpatial(AdjustmentType::RgbSplit));
    QVERIFY(adjustmentIsSpatial(AdjustmentType::ChromaticAberrationCorrection));

    AdjustmentData split;
    split.reset(AdjustmentType::RgbSplit);
    QVERIFY(adjustmentSpatialRadius(split) >= 7);
}

void OpticalSpatialColourEffectsTests::roundTripsAndRejectsLegacySchema()
{
    QVector<AdjustmentData> fixtures;

    AdjustmentData vignette;
    vignette.reset(AdjustmentType::Vignette);
    VignetteParameters vignetteParameters;
    vignetteParameters.amount = -37.0;
    vignetteParameters.size = 187.0;
    vignetteParameters.midpoint = 61.0;
    vignetteParameters.roundness = 44.0;
    vignetteParameters.feather = 83.0;
    vignetteParameters.centreX = 12.0;
    vignetteParameters.centreY = -9.0;
    vignetteParameters.rotation = 27.0;
    vignetteParameters.highlightProtection = 48.0;
    vignetteParameters.inverted = true;
    vignetteParameters.tintEnabled = true;
    vignetteParameters.tint = QColor(73, 41, 19);
    vignette.parameters = vignetteParameters;
    fixtures.push_back(vignette);

    AdjustmentData split;
    split.reset(AdjustmentType::RgbSplit);
    split.parameters = RgbSplitParameters {-12.5, 4.0, 9.5, -3.0};
    fixtures.push_back(split);

    AdjustmentData correction;
    correction.reset(AdjustmentType::ChromaticAberrationCorrection);
    correction.parameters = ChromaticAberrationCorrectionParameters {2.25, -1.75, 7.0, -5.0, 1.45};
    fixtures.push_back(correction);

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

        if (expected.type == AdjustmentType::Vignette) {
            QJsonObject legacy = object;
            legacy.insert(QStringLiteral("schema"), 15);
            QJsonObject parametersObject = legacy.value(QStringLiteral("parameters")).toObject();
            parametersObject.remove(QStringLiteral("size"));
            legacy.insert(QStringLiteral("parameters"), parametersObject);
            readOk = false;
            const AdjustmentData restoredLegacy = AdjustmentData::fromJson(
                legacy, AdjustmentType::Exposure, &readOk);
            QVERIFY(readOk);
            QCOMPARE(std::get<VignetteParameters>(restoredLegacy.parameters).size, 100.0);
        }

        object.insert(QStringLiteral("schema"), 13);
        readOk = true;
        AdjustmentData::fromJson(object, AdjustmentType::Exposure, &readOk);
        QVERIFY(!readOk);
    }
}

void OpticalSpatialColourEffectsTests::vignetteUsesDocumentGeometryAndPreservesAlpha()
{
    QImage source(101, 81, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.fill(QColor(180, 180, 180, 0));

    AdjustmentData data;
    data.reset(AdjustmentType::Vignette);
    VignetteParameters parameters;
    parameters.amount = 65.0;
    parameters.midpoint = 42.0;
    parameters.feather = 70.0;
    data.parameters = parameters;

    const QImage output = render(source, data).convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(output.pixelColor(50, 40).alpha(), 0);
    QCOMPARE(output.pixelColor(0, 0).alpha(), 0);
    QVERIFY(output.pixelColor(0, 0).red() < output.pixelColor(50, 40).red());
    QVERIFY(output.pixelColor(0, 0).red() != source.pixelColor(0, 0).red());
}

void OpticalSpatialColourEffectsTests::expandedVignetteCanDarkenCornersWithoutDarkeningEdgeCentres()
{
    QImage source(201, 121, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.fill(QColor(180, 180, 180, 255));

    AdjustmentData data;
    data.reset(AdjustmentType::Vignette);
    VignetteParameters parameters;
    parameters.amount = 100.0;
    parameters.size = 250.0;
    parameters.midpoint = 50.0;
    parameters.feather = 50.0;
    data.parameters = parameters;

    const QImage output = render(source, data).convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(output.pixelColor(100, 0), source.pixelColor(100, 0));
    QCOMPARE(output.pixelColor(0, 60), source.pixelColor(0, 60));
    QVERIFY(output.pixelColor(0, 0).red() < source.pixelColor(0, 0).red());
    QVERIFY(output.pixelColor(200, 120).red() < source.pixelColor(200, 120).red());
}

void OpticalSpatialColourEffectsTests::vignetteMatchesRegionRendering()
{
    const QImage source = patternedImage({97, 73});
    AdjustmentData data;
    data.reset(AdjustmentType::Vignette);
    VignetteParameters parameters;
    parameters.amount = 52.0;
    parameters.size = 172.0;
    parameters.midpoint = 39.0;
    parameters.roundness = -35.0;
    parameters.feather = 74.0;
    parameters.centreX = 11.0;
    parameters.centreY = -8.0;
    parameters.rotation = 23.0;
    parameters.highlightProtection = 31.0;
    parameters.tintEnabled = true;
    parameters.tint = QColor(45, 27, 16);
    data.parameters = parameters;
    const QVector<LayerNode> layers {adjustmentLayer(data), baseLayer(source)};

    const QImage full = ImageProcessor::renderPreservingHiddenRgb(
        source, layers, nullptr, source.size()).convertToFormat(QImage::Format_RGBA8888);
    const QRect region(21, 15, 43, 35);
    const QImage tile = ImageProcessor::renderRegionPreservingHiddenRgb(
        source, layers, region, source.size()).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(maximumPremultipliedDifference(tile, full.copy(region)) <= 1);
}

void OpticalSpatialColourEffectsTests::rgbSplitPreservesAlphaAndMatchesRegionRendering()
{
    const QImage source = patternedImage({93, 71});
    AdjustmentData data;
    data.reset(AdjustmentType::RgbSplit);
    data.parameters = RgbSplitParameters {-8.5, 3.0, 7.0, -4.5};
    const QVector<LayerNode> layers {adjustmentLayer(data), baseLayer(source)};

    const QImage full = ImageProcessor::renderPreservingHiddenRgb(
        source, layers, nullptr, source.size()).convertToFormat(QImage::Format_RGBA8888);
    const QRect region(19, 13, 47, 39);
    const QImage tile = ImageProcessor::renderRegionPreservingHiddenRgb(
        source, layers, region, source.size()).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(maximumPremultipliedDifference(tile, full.copy(region)) <= 1);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            QCOMPARE(full.pixelColor(x, y).alpha(), source.pixelColor(x, y).alpha());
        }
    }
}

void OpticalSpatialColourEffectsTests::chromaticCorrectionIdentityAndRegionRendering()
{
    const QImage source = patternedImage({89, 67});
    AdjustmentData identity;
    identity.reset(AdjustmentType::ChromaticAberrationCorrection);
    QVERIFY(exactBytes(render(source, identity).convertToFormat(QImage::Format_RGBA8888), source));

    AdjustmentData data;
    data.reset(AdjustmentType::ChromaticAberrationCorrection);
    data.parameters = ChromaticAberrationCorrectionParameters {3.25, -2.75, 6.0, -4.0, 1.6};
    const QVector<LayerNode> layers {adjustmentLayer(data), baseLayer(source)};
    const QImage full = ImageProcessor::renderPreservingHiddenRgb(
        source, layers, nullptr, source.size()).convertToFormat(QImage::Format_RGBA8888);
    const QRect region(17, 11, 41, 37);
    const QImage tile = ImageProcessor::renderRegionPreservingHiddenRgb(
        source, layers, region, source.size()).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(exactBytes(tile, full.copy(region)));
}

void OpticalSpatialColourEffectsTests::effectsAreConsistentAcrossEightAndSixteenBit()
{
    const QImage source8 = patternedImage({37, 29});
    QImage source16 = source8.convertToFormat(QImage::Format_RGBA64);
    source16.setColorSpace(source8.colorSpace());

    QVector<AdjustmentData> fixtures;
    AdjustmentData vignette;
    vignette.reset(AdjustmentType::Vignette);
    VignetteParameters vignetteParameters;
    vignetteParameters.amount = 43.0;
    vignetteParameters.size = 163.0;
    vignetteParameters.midpoint = 47.0;
    vignetteParameters.roundness = 28.0;
    vignetteParameters.feather = 68.0;
    vignetteParameters.centreX = -7.0;
    vignetteParameters.centreY = 9.0;
    vignetteParameters.rotation = -16.0;
    vignetteParameters.tintEnabled = true;
    vignetteParameters.tint = QColor(62, 38, 21);
    vignette.parameters = vignetteParameters;
    fixtures.push_back(vignette);

    AdjustmentData split;
    split.reset(AdjustmentType::RgbSplit);
    split.parameters = RgbSplitParameters {-4.25, 2.5, 5.75, -3.0};
    fixtures.push_back(split);

    AdjustmentData correction;
    correction.reset(AdjustmentType::ChromaticAberrationCorrection);
    correction.parameters = ChromaticAberrationCorrectionParameters {2.4, -1.8, 5.0, -6.0, 1.35};
    fixtures.push_back(correction);

    for (const AdjustmentData &data : fixtures) {
        const QImage output8 = render(source8, data).convertToFormat(QImage::Format_RGBA8888);
        const QImage output16 = render(source16, data).convertToFormat(QImage::Format_RGBA64);
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

void OpticalSpatialColourEffectsTests::cancellationIsCooperative()
{
    const QImage source = patternedImage({127, 91});
    std::atomic_bool cancelled {true};
    for (const AdjustmentType type : {AdjustmentType::Vignette,
                                      AdjustmentType::RgbSplit,
                                      AdjustmentType::ChromaticAberrationCorrection}) {
        AdjustmentData data;
        data.reset(type);
        const QImage output = ImageProcessor::renderPreservingHiddenRgb(
            source, {adjustmentLayer(data), baseLayer(source)}, &cancelled, source.size());
        QVERIFY(output.isNull());
    }
}

void OpticalSpatialColourEffectsTests::builtInPresetsAreSelfContained()
{
    for (const AdjustmentType type : {AdjustmentType::Vignette,
                                      AdjustmentType::RgbSplit,
                                      AdjustmentType::ChromaticAberrationCorrection}) {
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

    const QVector<AdjustmentPreset> vignettePresets =
        AdjustmentPresetStore::builtInPresets(AdjustmentType::Vignette);
    const auto corners = std::find_if(
        vignettePresets.cbegin(), vignettePresets.cend(),
        [](const AdjustmentPreset &preset) {
            return preset.name == QStringLiteral("Corners Only");
        });
    QVERIFY(corners != vignettePresets.cend());
    QVERIFY(std::get<VignetteParameters>(corners->adjustment.parameters).size > 100.0);
}

QTEST_MAIN(OpticalSpatialColourEffectsTests)
#include "test_optical_spatial_colour_effects.moc"
