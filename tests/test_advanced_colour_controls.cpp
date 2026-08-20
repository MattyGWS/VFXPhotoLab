#include "Adjustment.h"
#include "AdjustmentPresetStore.h"
#include "ImageProcessor.h"

#include <QColorSpace>
#include <QFile>
#include <QJsonObject>
#include <QRgba64>
#include <QtTest/QtTest>

#include <array>
#include <cmath>
#include <cstring>

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

QImage render(const QImage &source, const AdjustmentData &data)
{
    return ImageProcessor::renderPreservingHiddenRgb(
        source, {adjustmentLayer(data), baseLayer(source)});
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
                    static_cast<quint16>(((x + y) % 5 == 0)
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
                pixel[3] = static_cast<uchar>(((x + y) % 5 == 0)
                                                  ? 0
                                                  : ((x * 17 + y * 19) & 255));
            }
        }
    }
    return image;
}

} // namespace

class AdvancedColourControlsTests final : public QObject {
    Q_OBJECT

private slots:
    void appendOnlyIdentifierAndSchemaAreStable();
    void allAdvancedControlsRoundTrip();
    void selectiveColourIdentityPreservesExactPixelsAtBothDepths();
    void selectiveColourIsConsistentAcrossEightAndSixteenBit();
    void selectiveColourFamiliesAndTonalRangesAreDirectional();
    void relativeAndAbsoluteMethodsRemainDistinct();
    void selectiveColourPreservesAlphaAndHiddenRgb();
    void selectiveColourMatchesFullFrameAndRegionRendering();
    void existingAdvancedControlsRetainAlphaAndDefaultMixerIdentity();
    void advancedBuiltInPresetsAreSelfContained();
    void shaderPublishesSelectiveColourContract();
};

void AdvancedColourControlsTests::appendOnlyIdentifierAndSchemaAreStable()
{
    QCOMPARE(static_cast<int>(AdjustmentType::Invert), 20);
    QCOMPARE(static_cast<int>(AdjustmentType::PhotoFilter), 21);
    QCOMPARE(static_cast<int>(AdjustmentType::SelectiveColour), 22);
    QCOMPARE(AdjustmentData::CurrentSchema, 16u);
    QCOMPARE(adjustmentTypeToString(AdjustmentType::SelectiveColour),
             QStringLiteral("selective-colour"));
    QCOMPARE(defaultAdjustmentName(AdjustmentType::SelectiveColour),
             QStringLiteral("Selective Colour"));

    AdjustmentData data;
    data.reset(AdjustmentType::SelectiveColour);
    QCOMPARE(adjustmentProcessingDomain(data),
             AdjustmentProcessingDomain::EncodedSrgb);
    QVERIFY(adjustmentRequiresManagedDomainTransform(data));
    QVERIFY(!adjustmentIsSpatial(AdjustmentType::SelectiveColour));
}

void AdvancedColourControlsTests::allAdvancedControlsRoundTrip()
{
    QVector<AdjustmentData> fixtures;

    AdjustmentData balance;
    balance.reset(AdjustmentType::ColourBalance);
    auto balanceParameters = std::get<ColourBalanceParameters>(balance.parameters);
    balanceParameters.range(ColourBalanceRange::Shadows) = {-18.0, 7.0, 13.0};
    balanceParameters.range(ColourBalanceRange::Highlights) = {11.0, -9.0, -14.0};
    balanceParameters.preserveLuminosity = false;
    balance.parameters = balanceParameters;
    fixtures.push_back(balance);

    AdjustmentData mixer;
    mixer.reset(AdjustmentType::ChannelMixer);
    auto mixerParameters = std::get<ChannelMixerParameters>(mixer.parameters);
    mixerParameters.output(ChannelMixerOutput::Red) = {112.0, -8.0, 4.0, -3.0};
    mixerParameters.monochrome = {120.0, 45.0, -35.0, 2.0};
    mixerParameters.monochromeEnabled = true;
    mixer.parameters = mixerParameters;
    fixtures.push_back(mixer);

    AdjustmentData blackWhite;
    blackWhite.reset(AdjustmentType::BlackAndWhite);
    auto blackWhiteParameters = std::get<BlackAndWhiteParameters>(blackWhite.parameters);
    blackWhiteParameters.colourWeights = {145.0, 78.0, 116.0, 67.0, 132.0, 88.0};
    blackWhiteParameters.tintEnabled = true;
    blackWhiteParameters.tintHue = 38.0;
    blackWhiteParameters.tintSaturation = 24.0;
    blackWhite.parameters = blackWhiteParameters;
    fixtures.push_back(blackWhite);

    AdjustmentData selective;
    selective.reset(AdjustmentType::SelectiveColour);
    SelectiveColourParameters selectiveParameters;
    selectiveParameters.range(SelectiveColourRange::Reds) = {18.0, -9.0, 12.0, 4.0};
    selectiveParameters.range(SelectiveColourRange::Whites).black = -6.0;
    selectiveParameters.range(SelectiveColourRange::Neutrals) = {3.0, -5.0, 7.0, 4.0};
    selectiveParameters.range(SelectiveColourRange::Blacks) = {-4.0, 6.0, 2.0, 11.0};
    selectiveParameters.method = SelectiveColourMethod::Absolute;
    selective.parameters = selectiveParameters;
    fixtures.push_back(selective);

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
    }

    bool writeOk = false;
    QJsonObject dishonest = selective.toJson(&writeOk);
    QVERIFY(writeOk);
    dishonest.insert(QStringLiteral("schema"), 12);
    bool readOk = true;
    AdjustmentData::fromJson(dishonest, AdjustmentType::Exposure, &readOk);
    QVERIFY(!readOk);
}

void AdvancedColourControlsTests::selectiveColourIdentityPreservesExactPixelsAtBothDepths()
{
    AdjustmentData identity;
    identity.reset(AdjustmentType::SelectiveColour);

    const QImage source8 = patternedImage({37, 23}, QImage::Format_RGBA8888);
    const QImage output8 = render(source8, identity).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(exactImageBytes(output8, source8));

    const QImage source16 = patternedImage({31, 19}, QImage::Format_RGBA64);
    const QImage output16 = render(source16, identity).convertToFormat(QImage::Format_RGBA64);
    QVERIFY(exactImageBytes(output16, source16));
}

void AdvancedColourControlsTests::selectiveColourIsConsistentAcrossEightAndSixteenBit()
{
    const QImage source8 = patternedImage({41, 27}, QImage::Format_RGBA8888);
    QImage source16 = source8.convertToFormat(QImage::Format_RGBA64);
    source16.setColorSpace(QColorSpace(QColorSpace::SRgb));

    AdjustmentData data;
    data.reset(AdjustmentType::SelectiveColour);
    SelectiveColourParameters parameters;
    parameters.range(SelectiveColourRange::Reds) = {-22.0, 14.0, 9.0, 6.0};
    parameters.range(SelectiveColourRange::Cyans) = {7.0, -13.0, 18.0, -5.0};
    parameters.range(SelectiveColourRange::Whites).black = -8.0;
    parameters.range(SelectiveColourRange::Neutrals) = {4.0, -6.0, 7.0, 3.0};
    parameters.range(SelectiveColourRange::Blacks).black = 12.0;
    parameters.method = SelectiveColourMethod::Relative;
    data.parameters = parameters;

    const QImage output8 = render(source8, data).convertToFormat(QImage::Format_RGBA8888);
    const QImage output16 = render(source16, data).convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(output8.size(), output16.size());
    for (int y = 0; y < output8.height(); ++y) {
        for (int x = 0; x < output8.width(); ++x) {
            const QColor eight = output8.pixelColor(x, y);
            const QColor sixteen = output16.pixelColor(x, y);
            QVERIFY(std::abs(eight.red() - sixteen.red()) <= 2);
            QVERIFY(std::abs(eight.green() - sixteen.green()) <= 2);
            QVERIFY(std::abs(eight.blue() - sixteen.blue()) <= 2);
            QCOMPARE(eight.alpha(), sixteen.alpha());
        }
    }
}

void AdvancedColourControlsTests::selectiveColourFamiliesAndTonalRangesAreDirectional()
{
    QImage source(4, 1, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.setPixelColor(0, 0, QColor(220, 35, 35, 255));
    source.setPixelColor(1, 0, QColor(35, 55, 220, 255));
    source.setPixelColor(2, 0, QColor(232, 232, 232, 255));
    source.setPixelColor(3, 0, QColor(28, 28, 28, 255));

    AdjustmentData data;
    data.reset(AdjustmentType::SelectiveColour);
    SelectiveColourParameters parameters;
    parameters.range(SelectiveColourRange::Reds).cyan = 45.0;
    parameters.range(SelectiveColourRange::Blues).yellow = 45.0;
    parameters.range(SelectiveColourRange::Whites).black = 35.0;
    parameters.range(SelectiveColourRange::Blacks).black = -35.0;
    data.parameters = parameters;

    const QImage output = render(source, data).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(output.pixelColor(0, 0).red() < source.pixelColor(0, 0).red());
    QVERIFY(output.pixelColor(1, 0).blue() < source.pixelColor(1, 0).blue());
    QVERIFY(output.pixelColor(2, 0).lightness() < source.pixelColor(2, 0).lightness());
    QVERIFY(output.pixelColor(3, 0).lightness() > source.pixelColor(3, 0).lightness());
}

void AdvancedColourControlsTests::relativeAndAbsoluteMethodsRemainDistinct()
{
    QImage source(1, 1, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.setPixelColor(0, 0, QColor(160, 55, 45, 255));

    auto adjustedRed = [&](const SelectiveColourMethod method) {
        AdjustmentData data;
        data.reset(AdjustmentType::SelectiveColour);
        SelectiveColourParameters parameters;
        parameters.method = method;
        parameters.range(SelectiveColourRange::Reds).cyan = 30.0;
        data.parameters = parameters;
        return render(source, data).convertToFormat(QImage::Format_RGBA8888)
            .pixelColor(0, 0).red();
    };

    const int relativeRed = adjustedRed(SelectiveColourMethod::Relative);
    const int absoluteRed = adjustedRed(SelectiveColourMethod::Absolute);
    QVERIFY(relativeRed < source.pixelColor(0, 0).red());
    QVERIFY(absoluteRed < relativeRed);

    QImage white(1, 1, QImage::Format_RGBA8888);
    white.setColorSpace(QColorSpace(QColorSpace::SRgb));
    white.fill(QColor(255, 255, 255, 255));
    AdjustmentData whiteRelative;
    whiteRelative.reset(AdjustmentType::SelectiveColour);
    SelectiveColourParameters whiteRelativeParameters;
    whiteRelativeParameters.method = SelectiveColourMethod::Relative;
    whiteRelativeParameters.range(SelectiveColourRange::Whites).magenta = 50.0;
    whiteRelative.parameters = whiteRelativeParameters;
    QVERIFY(exactImageBytes(render(white, whiteRelative)
                                .convertToFormat(QImage::Format_RGBA8888), white));

    AdjustmentData whiteAbsolute = whiteRelative;
    auto whiteAbsoluteParameters = std::get<SelectiveColourParameters>(
        whiteAbsolute.parameters);
    whiteAbsoluteParameters.method = SelectiveColourMethod::Absolute;
    whiteAbsolute.parameters = whiteAbsoluteParameters;
    const QColor absoluteWhite = render(white, whiteAbsolute)
        .convertToFormat(QImage::Format_RGBA8888).pixelColor(0, 0);
    QCOMPARE(absoluteWhite.red(), 255);
    QVERIFY(absoluteWhite.green() < 255);
    QCOMPARE(absoluteWhite.blue(), 255);
}

void AdvancedColourControlsTests::selectiveColourPreservesAlphaAndHiddenRgb()
{
    QImage source(2, 1, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.setPixelColor(0, 0, QColor(201, 41, 37, 0));
    source.setPixelColor(1, 0, QColor(42, 61, 203, 117));

    AdjustmentData data;
    data.reset(AdjustmentType::SelectiveColour);
    SelectiveColourParameters parameters;
    parameters.range(SelectiveColourRange::Reds) = {35.0, -15.0, 20.0, 8.0};
    parameters.range(SelectiveColourRange::Blues) = {-12.0, 18.0, 30.0, 6.0};
    data.parameters = parameters;

    const QImage output = render(source, data).convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(output.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(output.pixelColor(1, 0).alpha(), 117);
    QVERIFY(output.pixelColor(0, 0).rgb() != source.pixelColor(0, 0).rgb());
    QVERIFY(output.pixelColor(1, 0).rgb() != source.pixelColor(1, 0).rgb());

    QImage managed = source;
    managed.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const QImage managedOutput = render(managed, data)
        .convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(managedOutput.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(managedOutput.pixelColor(1, 0).alpha(), 117);
    QVERIFY(managedOutput.pixelColor(0, 0).rgb()
            != managed.pixelColor(0, 0).rgb());
}

void AdvancedColourControlsTests::selectiveColourMatchesFullFrameAndRegionRendering()
{
    const QImage source = patternedImage({73, 59}, QImage::Format_RGBA8888);
    AdjustmentData data;
    data.reset(AdjustmentType::SelectiveColour);
    SelectiveColourParameters parameters;
    parameters.range(SelectiveColourRange::Reds) = {-18.0, 11.0, 7.0, 3.0};
    parameters.range(SelectiveColourRange::Greens) = {12.0, -9.0, 6.0, -4.0};
    parameters.range(SelectiveColourRange::Neutrals) = {4.0, -3.0, 8.0, 5.0};
    data.parameters = parameters;
    const QVector<LayerNode> layers {adjustmentLayer(data), baseLayer(source)};

    const QImage full = ImageProcessor::renderPreservingHiddenRgb(source, layers)
                            .convertToFormat(QImage::Format_RGBA8888);
    const QRect region(11, 9, 37, 31);
    const QImage cropped = ImageProcessor::renderRegion(
        source, layers, region, source.size()).convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(cropped.size(), region.size());
    QVERIFY(exactImageBytes(cropped, full.copy(region)));
}

void AdvancedColourControlsTests::existingAdvancedControlsRetainAlphaAndDefaultMixerIdentity()
{
    const QImage source = patternedImage({29, 17}, QImage::Format_RGBA8888);

    AdjustmentData mixer;
    mixer.reset(AdjustmentType::ChannelMixer);
    const QImage mixed = render(source, mixer).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(exactImageBytes(mixed, source));

    const std::array<AdjustmentType, 3> types {
        AdjustmentType::ColourBalance,
        AdjustmentType::ChannelMixer,
        AdjustmentType::BlackAndWhite
    };
    for (const AdjustmentType type : types) {
        AdjustmentData data;
        data.reset(type);
        if (type == AdjustmentType::ColourBalance) {
            auto parameters = std::get<ColourBalanceParameters>(data.parameters);
            parameters.range(ColourBalanceRange::Midtones).cyanRed = 25.0;
            data.parameters = parameters;
        } else if (type == AdjustmentType::ChannelMixer) {
            auto parameters = std::get<ChannelMixerParameters>(data.parameters);
            parameters.output(ChannelMixerOutput::Red).green = 20.0;
            data.parameters = parameters;
        } else {
            auto parameters = std::get<BlackAndWhiteParameters>(data.parameters);
            parameters.colourWeights[0] = 140.0;
            data.parameters = parameters;
        }
        const QImage output = render(source, data).convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                QCOMPARE(output.pixelColor(x, y).alpha(), source.pixelColor(x, y).alpha());
            }
        }
    }
}

void AdvancedColourControlsTests::advancedBuiltInPresetsAreSelfContained()
{
    for (const AdjustmentType type : {AdjustmentType::ColourBalance,
                                      AdjustmentType::ChannelMixer,
                                      AdjustmentType::BlackAndWhite,
                                      AdjustmentType::SelectiveColour}) {
        const QVector<AdjustmentPreset> presets = AdjustmentPresetStore::builtInPresets(type);
        QVERIFY2(!presets.isEmpty(), qPrintable(defaultAdjustmentName(type)));
        for (const AdjustmentPreset &preset : presets) {
            QCOMPARE(preset.adjustment.type, type);
            QCOMPARE(preset.adjustment.schema, AdjustmentData::CurrentSchema);
            bool ok = false;
            const QJsonObject object = preset.adjustment.toJson(&ok);
            QVERIFY(ok);
            QVERIFY(!object.isEmpty());
        }
    }
}

void AdvancedColourControlsTests::shaderPublishesSelectiveColourContract()
{
#ifdef VFXPHOTOLAB_TEST_SOURCE_DIR
    QFile shader(QStringLiteral(VFXPHOTOLAB_TEST_SOURCE_DIR)
                 + QStringLiteral("/shaders/adjustment_tile.wgsl"));
    QVERIFY(shader.open(QIODevice::ReadOnly));
    const QByteArray source = shader.readAll();
    QVERIFY(source.contains("case 22u: { return apply_selective_colour(input); }"));
    QVERIFY(source.contains("selective_colour_ranges: array<vec4<f32>, 9>"));
    QVERIFY(source.contains("selective_colour_options: vec4<f32>"));
    QVERIFY(source.contains("fn apply_selective_colour"));
#endif
}

QTEST_MAIN(AdvancedColourControlsTests)
#include "test_advanced_colour_controls.moc"
