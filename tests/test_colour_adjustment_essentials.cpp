#include "Adjustment.h"
#include "AdjustmentPresetStore.h"
#include "ImageProcessor.h"

#include <QColorSpace>
#include <QFile>
#include <QJsonObject>
#include <QRgba64>
#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

using namespace vfx;

namespace {

LayerNode baseLayer(const QImage &image)
{
    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = image;
    base.rasterReferenceSize = image.size();
    return base;
}

LayerNode adjustmentLayer(const AdjustmentData &data)
{
    LayerNode layer;
    layer.type = LayerType::Adjustment;
    layer.setAdjustmentData(data);
    return layer;
}

bool exactImageBytes(const QImage &left, const QImage &right)
{
    if (left.isNull() || right.isNull() || left.size() != right.size()
        || left.format() != right.format() || left.depth() != right.depth()) {
        return false;
    }
    const qsizetype activeBytes = static_cast<qsizetype>(left.width())
        * left.depth() / 8;
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

double encodedLuminance(const QColor &colour)
{
    return (0.2126 * colour.redF()) + (0.7152 * colour.greenF())
        + (0.0722 * colour.blueF());
}

QImage patternedImage(const QSize &size, const QImage::Format format)
{
    QImage image(size, format);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    if (format == QImage::Format_RGBA64) {
        for (int y = 0; y < size.height(); ++y) {
            auto *row = reinterpret_cast<QRgba64 *>(image.scanLine(y));
            for (int x = 0; x < size.width(); ++x) {
                row[x] = QRgba64::fromRgba64(
                    static_cast<quint16>((x * 3701 + y * 1103) & 0xffff),
                    static_cast<quint16>((x * 1301 + y * 4703) & 0xffff),
                    static_cast<quint16>((x * 7103 + y * 509) & 0xffff),
                    static_cast<quint16>(((x + y) % 7 == 0)
                                             ? 0
                                             : ((x * 1709 + y * 1901) & 0xffff)));
            }
        }
    } else {
        for (int y = 0; y < size.height(); ++y) {
            uchar *row = image.scanLine(y);
            for (int x = 0; x < size.width(); ++x) {
                uchar *pixel = row + x * 4;
                pixel[0] = static_cast<uchar>((x * 37 + y * 11) & 255);
                pixel[1] = static_cast<uchar>((x * 13 + y * 47) & 255);
                pixel[2] = static_cast<uchar>((x * 71 + y * 5) & 255);
                pixel[3] = static_cast<uchar>(((x + y) % 7 == 0)
                                                  ? 0
                                                  : ((x * 17 + y * 19) & 255));
            }
        }
    }
    return image;
}

} // namespace

class ColourAdjustmentEssentialsTests final : public QObject {
    Q_OBJECT

private slots:
    void appendOnlyIdentifiersAndSchemaRemainStable();
    void invertAndPhotoFilterRoundTripWithoutLegacyReinterpretation();
    void invertPreservesStraightAlphaAndHiddenRgbAtBothDepths();
    void photoFilterIdentityTintAndLuminosityContracts();
    void photoFilterIsConsistentAcrossEightAndSixteenBit();
    void managedPhotoFilterPreservesHiddenRgb();
    void essentialsMatchFullFrameAndRegionRendering();
    void existingVibranceThresholdAndPosteriseContractsRemainStable();
    void builtInPhotoFilterPresetsAreSelfContained();
    void shaderPublishesMatchingAdjustmentIdentifiers();
};

void ColourAdjustmentEssentialsTests::appendOnlyIdentifiersAndSchemaRemainStable()
{
    QCOMPARE(static_cast<int>(AdjustmentType::Exposure), 0);
    QCOMPARE(static_cast<int>(AdjustmentType::ShadowsHighlights), 15);
    QCOMPARE(static_cast<int>(AdjustmentType::GaussianBlur), 16);
    QCOMPARE(static_cast<int>(AdjustmentType::BoxBlur), 17);
    QCOMPARE(static_cast<int>(AdjustmentType::UnsharpMask), 18);
    QCOMPARE(static_cast<int>(AdjustmentType::HighPass), 19);
    QCOMPARE(static_cast<int>(AdjustmentType::Invert), 20);
    QCOMPARE(static_cast<int>(AdjustmentType::PhotoFilter), 21);
    QCOMPARE(AdjustmentData::CurrentSchema, 16u);

    QCOMPARE(adjustmentTypeToString(AdjustmentType::Invert), QStringLiteral("invert"));
    QCOMPARE(adjustmentTypeToString(AdjustmentType::PhotoFilter),
             QStringLiteral("photo-filter"));
    AdjustmentData invert;
    invert.reset(AdjustmentType::Invert);
    QCOMPARE(adjustmentProcessingDomain(invert),
             AdjustmentProcessingDomain::EncodedWorking);

    AdjustmentData photo;
    photo.reset(AdjustmentType::PhotoFilter);
    QCOMPARE(adjustmentProcessingDomain(photo),
             AdjustmentProcessingDomain::EncodedSrgb);
    QVERIFY(!adjustmentIsSpatial(AdjustmentType::Invert));
    QVERIFY(!adjustmentIsSpatial(AdjustmentType::PhotoFilter));
}

void ColourAdjustmentEssentialsTests::invertAndPhotoFilterRoundTripWithoutLegacyReinterpretation()
{
    QVector<AdjustmentData> fixtures;
    AdjustmentData invert;
    invert.reset(AdjustmentType::Invert);
    fixtures.push_back(invert);

    AdjustmentData photo;
    photo.reset(AdjustmentType::PhotoFilter);
    PhotoFilterParameters photoParameters;
    photoParameters.colour = QColor(55, 145, 218, 63);
    photoParameters.density = 47.5;
    photoParameters.preserveLuminosity = false;
    photo.parameters = photoParameters;
    fixtures.push_back(photo);

    for (AdjustmentData expected : fixtures) {
        expected.normalise();
        bool writeOk = false;
        const QJsonObject object = expected.toJson(&writeOk);
        QVERIFY(writeOk);
        QCOMPARE(object.value(QStringLiteral("schema")).toInt(), 16);
        bool readOk = false;
        const AdjustmentData restored = AdjustmentData::fromJson(
            object, AdjustmentType::Exposure, &readOk);
        QVERIFY(readOk);
        QVERIFY(restored == expected);

        QJsonObject dishonest = object;
        dishonest.insert(QStringLiteral("schema"), 11);
        readOk = true;
        AdjustmentData::fromJson(dishonest, AdjustmentType::Exposure, &readOk);
        QVERIFY(!readOk);
    }

    PhotoFilterParameters malformed;
    malformed.colour = QColor();
    malformed.density = std::numeric_limits<double>::quiet_NaN();
    malformed.normalise();
    QCOMPARE(malformed.colour, QColor(236, 138, 0));
    QCOMPARE(malformed.colour.alpha(), 255);
    QCOMPARE(malformed.density, 25.0);
}

void ColourAdjustmentEssentialsTests::invertPreservesStraightAlphaAndHiddenRgbAtBothDepths()
{
    AdjustmentData invertData;
    invertData.reset(AdjustmentType::Invert);
    const LayerNode invert = adjustmentLayer(invertData);

    QImage source8(2, 1, QImage::Format_RGBA8888);
    source8.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source8.setPixelColor(0, 0, QColor(17, 91, 203, 0));
    source8.setPixelColor(1, 0, QColor(255, 1, 128, 173));
    const QImage output8 = ImageProcessor::renderPreservingHiddenRgb(
        source8, {invert, baseLayer(source8)}).convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(output8.pixelColor(0, 0), QColor(238, 164, 52, 0));
    QCOMPARE(output8.pixelColor(1, 0), QColor(0, 254, 127, 173));

    QImage source16(2, 1, QImage::Format_RGBA64);
    source16.setColorSpace(QColorSpace(QColorSpace::SRgb));
    auto *sourcePixels = reinterpret_cast<QRgba64 *>(source16.bits());
    sourcePixels[0] = QRgba64::fromRgba64(1'337, 19'753, 61'121, 0);
    sourcePixels[1] = QRgba64::fromRgba64(65'535, 1, 32'768, 49'123);
    const QImage output16 = ImageProcessor::renderPreservingHiddenRgb(
        source16, {invert, baseLayer(source16)}).convertToFormat(QImage::Format_RGBA64);
    const auto *outputPixels = reinterpret_cast<const QRgba64 *>(output16.constBits());
    QCOMPARE(outputPixels[0], QRgba64::fromRgba64(64'198, 45'782, 4'414, 0));
    QCOMPARE(outputPixels[1], QRgba64::fromRgba64(0, 65'534, 32'767, 49'123));
}

void ColourAdjustmentEssentialsTests::photoFilterIdentityTintAndLuminosityContracts()
{
    QImage source(3, 1, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.setPixelColor(0, 0, QColor(128, 128, 128, 0));
    source.setPixelColor(1, 0, QColor(128, 128, 128, 91));
    source.setPixelColor(2, 0, QColor(83, 147, 211, 203));

    AdjustmentData identity;
    identity.reset(AdjustmentType::PhotoFilter);
    auto identityParameters = std::get<PhotoFilterParameters>(identity.parameters);
    identityParameters.density = 0.0;
    identity.parameters = identityParameters;
    const QImage identityOutput = ImageProcessor::renderPreservingHiddenRgb(
        source, {adjustmentLayer(identity), baseLayer(source)})
                                      .convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(exactImageBytes(identityOutput, source));

    auto renderNeutral = [&](const QColor &filter, const bool preserve) {
        AdjustmentData data;
        data.reset(AdjustmentType::PhotoFilter);
        PhotoFilterParameters parameters;
        parameters.colour = filter;
        parameters.density = 65.0;
        parameters.preserveLuminosity = preserve;
        data.parameters = parameters;
        return ImageProcessor::renderPreservingHiddenRgb(
                   source, {adjustmentLayer(data), baseLayer(source)})
            .convertToFormat(QImage::Format_RGBA8888);
    };

    const QImage warming = renderNeutral(QColor(236, 138, 0), true);
    const QImage cooling = renderNeutral(QColor(0, 109, 255), true);
    const QColor warmNeutral = warming.pixelColor(1, 0);
    const QColor coolNeutral = cooling.pixelColor(1, 0);
    QVERIFY(warmNeutral.red() > warmNeutral.blue());
    QVERIFY(coolNeutral.blue() > coolNeutral.red());
    QCOMPARE(warming.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(warming.pixelColor(1, 0).alpha(), 91);
    QCOMPARE(warming.pixelColor(2, 0).alpha(), 203);

    const QImage unpreserved = renderNeutral(QColor(236, 138, 0), false);
    const double sourceLuma = encodedLuminance(source.pixelColor(1, 0));
    const double preservedDelta = std::abs(
        encodedLuminance(warming.pixelColor(1, 0)) - sourceLuma);
    const double unpreservedDelta = std::abs(
        encodedLuminance(unpreserved.pixelColor(1, 0)) - sourceLuma);
    QVERIFY(preservedDelta <= unpreservedDelta + (2.0 / 255.0));
}

void ColourAdjustmentEssentialsTests::photoFilterIsConsistentAcrossEightAndSixteenBit()
{
    QImage source8(4, 1, QImage::Format_RGBA8888);
    source8.setColorSpace(QColorSpace(QColorSpace::SRgb));
    const std::array<QColor, 4> colours {
        QColor(37, 128, 219, 0),
        QColor(211, 83, 19, 17),
        QColor(128, 128, 128, 128),
        QColor(250, 244, 231, 255)
    };
    for (int x = 0; x < source8.width(); ++x) {
        source8.setPixelColor(x, 0, colours[static_cast<std::size_t>(x)]);
    }

    QImage source16(4, 1, QImage::Format_RGBA64);
    source16.setColorSpace(QColorSpace(QColorSpace::SRgb));
    auto *source16Pixels = reinterpret_cast<QRgba64 *>(source16.bits());
    for (int x = 0; x < source16.width(); ++x) {
        const QColor colour = colours[static_cast<std::size_t>(x)];
        source16Pixels[x] = QRgba64::fromRgba64(
            static_cast<quint16>(colour.red() * 257),
            static_cast<quint16>(colour.green() * 257),
            static_cast<quint16>(colour.blue() * 257),
            static_cast<quint16>(colour.alpha() * 257));
    }

    AdjustmentData data;
    data.reset(AdjustmentType::PhotoFilter);
    PhotoFilterParameters parameters;
    parameters.colour = QColor(55, 145, 218);
    parameters.density = 63.0;
    parameters.preserveLuminosity = true;
    data.parameters = parameters;

    const LayerNode filter = adjustmentLayer(data);
    const QImage output8 = ImageProcessor::renderPreservingHiddenRgb(
        source8, {filter, baseLayer(source8)}).convertToFormat(QImage::Format_RGBA8888);
    const QImage output16 = ImageProcessor::renderPreservingHiddenRgb(
        source16, {filter, baseLayer(source16)}).convertToFormat(QImage::Format_RGBA64);
    const auto *output16Pixels = reinterpret_cast<const QRgba64 *>(output16.constBits());

    for (int x = 0; x < output8.width(); ++x) {
        const QColor low = output8.pixelColor(x, 0);
        const QRgba64 high = output16Pixels[x];
        QVERIFY(std::abs(low.red() - qRound(high.red() / 257.0)) <= 1);
        QVERIFY(std::abs(low.green() - qRound(high.green() / 257.0)) <= 1);
        QVERIFY(std::abs(low.blue() - qRound(high.blue() / 257.0)) <= 1);
        QCOMPARE(high.alpha(), static_cast<quint16>(low.alpha() * 257));
    }
}

void ColourAdjustmentEssentialsTests::managedPhotoFilterPreservesHiddenRgb()
{
    const QColorSpace displayP3(QColorSpace::DisplayP3);
    QVERIFY(displayP3.isValid());

    QImage transparent(1, 1, QImage::Format_RGBA8888);
    transparent.setColorSpace(displayP3);
    transparent.setPixelColor(0, 0, QColor(41, 129, 217, 0));
    QImage opaque = transparent;
    opaque.setPixelColor(0, 0, QColor(41, 129, 217, 255));

    AdjustmentData data;
    data.reset(AdjustmentType::PhotoFilter);
    PhotoFilterParameters parameters;
    parameters.colour = QColor(236, 138, 0);
    parameters.density = 52.0;
    parameters.preserveLuminosity = true;
    data.parameters = parameters;
    const LayerNode filter = adjustmentLayer(data);

    const QImage transparentOutput = ImageProcessor::renderPreservingHiddenRgb(
        transparent, {filter, baseLayer(transparent)}, nullptr, {},
        ColourProcessingCompatibility::ManagedV1).convertToFormat(
            QImage::Format_RGBA8888);
    const QImage opaqueOutput = ImageProcessor::renderPreservingHiddenRgb(
        opaque, {filter, baseLayer(opaque)}, nullptr, {},
        ColourProcessingCompatibility::ManagedV1).convertToFormat(
            QImage::Format_RGBA8888);
    QVERIFY(!transparentOutput.isNull());
    QVERIFY(!opaqueOutput.isNull());
    const QColor hidden = transparentOutput.pixelColor(0, 0);
    const QColor visible = opaqueOutput.pixelColor(0, 0);
    QCOMPARE(hidden.alpha(), 0);
    QCOMPARE(visible.alpha(), 255);
    QCOMPARE(hidden.red(), visible.red());
    QCOMPARE(hidden.green(), visible.green());
    QCOMPARE(hidden.blue(), visible.blue());
}

void ColourAdjustmentEssentialsTests::essentialsMatchFullFrameAndRegionRendering()
{
    for (const QImage::Format format : {QImage::Format_RGBA8888,
                                        QImage::Format_RGBA64}) {
        const QImage source = patternedImage(QSize(73, 59), format);
        QVector<AdjustmentData> adjustments;

        AdjustmentData invert;
        invert.reset(AdjustmentType::Invert);
        adjustments.push_back(invert);

        AdjustmentData photo;
        photo.reset(AdjustmentType::PhotoFilter);
        PhotoFilterParameters parameters;
        parameters.colour = QColor(55, 145, 218);
        parameters.density = 38.0;
        photo.parameters = parameters;
        adjustments.push_back(photo);

        for (const AdjustmentData &data : adjustments) {
            const QVector<LayerNode> layers {adjustmentLayer(data), baseLayer(source)};
            const QImage full = ImageProcessor::renderPreservingHiddenRgb(source, layers);
            const QRect region(17, 13, 31, 29);
            const QImage tiled = ImageProcessor::renderRegionPreservingHiddenRgb(
                source, layers, region, source.size());
            QVERIFY(!full.isNull());
            QVERIFY(!tiled.isNull());
            QVERIFY2(maximumPremultipliedDifference(full.copy(region), tiled) <= 1,
                     qPrintable(defaultAdjustmentName(data.type)));
        }
    }
}

void ColourAdjustmentEssentialsTests::existingVibranceThresholdAndPosteriseContractsRemainStable()
{
    QCOMPARE(static_cast<int>(AdjustmentType::Vibrance), 6);
    QCOMPARE(static_cast<int>(AdjustmentType::Posterise), 12);
    QCOMPARE(static_cast<int>(AdjustmentType::Threshold), 13);

    for (const AdjustmentType type : {AdjustmentType::Vibrance,
                                      AdjustmentType::Threshold,
                                      AdjustmentType::Posterise}) {
        AdjustmentData data;
        data.reset(type);
        bool writeOk = false;
        const QJsonObject object = data.toJson(&writeOk);
        QVERIFY(writeOk);
        bool readOk = false;
        const AdjustmentData restored = AdjustmentData::fromJson(
            object, AdjustmentType::Exposure, &readOk);
        QVERIFY(readOk);
        QVERIFY(restored == data);
    }
}

void ColourAdjustmentEssentialsTests::builtInPhotoFilterPresetsAreSelfContained()
{
    const QVector<AdjustmentPreset> photoPresets =
        AdjustmentPresetStore::builtInPresets(AdjustmentType::PhotoFilter);
    QVERIFY(photoPresets.size() >= 5);
    for (const AdjustmentPreset &preset : photoPresets) {
        QCOMPARE(preset.adjustment.type, AdjustmentType::PhotoFilter);
        QCOMPARE(preset.adjustment.schema, AdjustmentData::CurrentSchema);
        const auto parameters = std::get<PhotoFilterParameters>(
            preset.adjustment.parameters);
        QVERIFY(parameters.colour.isValid());
        QCOMPARE(parameters.colour.alpha(), 255);
        QVERIFY(parameters.density > 0.0);
    }
    QVERIFY(AdjustmentPresetStore::builtInPresets(AdjustmentType::Invert).isEmpty());
}

void ColourAdjustmentEssentialsTests::shaderPublishesMatchingAdjustmentIdentifiers()
{
    QFile shader(QStringLiteral(VFXPHOTOLAB_TEST_SOURCE_DIR)
                 + QStringLiteral("/shaders/adjustment_tile.wgsl"));
    QVERIFY(shader.open(QIODevice::ReadOnly));
    const QByteArray source = shader.readAll();
    QVERIFY(source.contains("case 20u: { return vec3<f32>(1.0) - input; }"));
    QVERIFY(source.contains("case 21u: { return apply_photo_filter(input); }"));
    QVERIFY(source.contains("photo_filter_params: vec4<f32>"));
}

QTEST_MAIN(ColourAdjustmentEssentialsTests)
#include "test_colour_adjustment_essentials.moc"
