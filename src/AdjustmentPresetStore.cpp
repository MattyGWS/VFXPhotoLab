#include "AdjustmentPresetStore.h"

#include "PresetCore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <optional>
#include <utility>

namespace vfx {
namespace {

// A maximum 65³ float LUT serialises to roughly 4.5 MiB of base64 JSON. Keep
// enough headroom for combined shapers and metadata while rejecting accidental
// or hostile preset files before readAll() allocates unbounded memory.
constexpr qint64 MaximumPresetFileBytes = 32LL * 1024LL * 1024LL;
constexpr int MaximumPresetCount = 2048;

QString cleanName(const QString &name)
{
    return name.trimmed().left(PresetStore::MaximumNameLength);
}

QString legacyStorageDirectory(const AdjustmentType type)
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.vfxphotolab");
    return QDir(root).filePath(QStringLiteral("adjustment-presets/%1")
                                   .arg(adjustmentTypeToString(type)));
}

AdjustmentPreset preset(const QString &name, AdjustmentData data)
{
    data.normalise();
    return {name, data, true, {}, {}};
}

QByteArray canonicalAdjustmentPayload(const AdjustmentData &input)
{
    AdjustmentData adjustment = input;
    adjustment.normalise();
    bool ok = false;
    const QJsonObject object = adjustment.toJson(&ok);
    return ok ? QJsonDocument(object).toJson(QJsonDocument::Compact) : QByteArray();
}

PresetMetadata metadataForLegacy(const QString &name,
                                 const AdjustmentData &adjustment)
{
    PresetMetadata metadata;
    metadata.id = PresetStore::stableLegacyId(
        PresetKind::Adjustment, adjustmentTypeToString(adjustment.type), name,
        canonicalAdjustmentPayload(adjustment));
    metadata.name = name;
    metadata.category = adjustmentTypeToString(adjustment.type);
    return metadata;
}

QJsonObject adjustmentPayload(const AdjustmentData &adjustment,
                              bool *ok)
{
    bool adjustmentOk = false;
    const QJsonObject adjustmentJson = adjustment.toJson(&adjustmentOk);
    if (ok) *ok = adjustmentOk;
    if (!adjustmentOk) return {};
    QJsonObject payload;
    payload.insert(QStringLiteral("adjustmentType"),
                   adjustmentTypeToString(adjustment.type));
    payload.insert(QStringLiteral("adjustment"), adjustmentJson);
    return payload;
}

bool adjustmentFromEnvelope(const PresetEnvelope &envelope,
                            const AdjustmentType expectedType,
                            AdjustmentData *adjustment,
                            QString *error)
{
    if (!adjustment) {
        if (error) *error = QStringLiteral("The adjustment destination is missing");
        return false;
    }
    if (envelope.kind != PresetKind::Adjustment
        || envelope.payload.value(QStringLiteral("adjustmentType")).toString()
            != adjustmentTypeToString(expectedType)
        || !envelope.payload.value(QStringLiteral("adjustment")).isObject()) {
        if (error) *error = QStringLiteral("The preset does not contain the expected adjustment type");
        return false;
    }
    bool ok = false;
    AdjustmentData decoded = AdjustmentData::fromJson(
        envelope.payload.value(QStringLiteral("adjustment")).toObject(),
        expectedType, &ok);
    if (!ok || decoded.type != expectedType) {
        if (error) *error = QStringLiteral("The adjustment preset payload is invalid");
        return false;
    }
    decoded.normalise();
    *adjustment = std::move(decoded);
    return true;
}

bool managedPath(const QString &path, const AdjustmentType type)
{
    return PresetStore::pathIsManagedByDirectory(
               path, AdjustmentPresetStore::storageDirectory(type))
        || PresetStore::pathIsManagedByDirectory(
               path, legacyStorageDirectory(type));
}

std::optional<AdjustmentPreset> userPresetWithName(
    const QVector<AdjustmentPreset> &presets,
    const QString &name,
    const QString &exceptId = {})
{
    const QString folded = cleanName(name).toCaseFolded();
    for (const AdjustmentPreset &preset : presets) {
        if (preset.builtIn || (!exceptId.isEmpty()
            && preset.metadata.id == exceptId)) continue;
        if (preset.name.toCaseFolded() == folded) return preset;
    }
    return std::nullopt;
}

QString uniqueImportedName(const QVector<AdjustmentPreset> &presets,
                           const QString &inputName)
{
    const QString requested = cleanName(inputName);
    if (!userPresetWithName(presets, requested).has_value()) return requested;

    for (int suffix = 2; suffix < MaximumPresetCount + 2; ++suffix) {
        const QString suffixText = QStringLiteral(" (%1)").arg(suffix);
        const qsizetype baseLimit = std::max<qsizetype>(
            1, PresetStore::MaximumNameLength - suffixText.size());
        const QString candidate = requested.left(baseLimit).trimmed()
            + suffixText;
        if (!userPresetWithName(presets, candidate).has_value()) {
            return candidate;
        }
    }
    return {};
}

bool writeUserPreset(const PresetMetadata &inputMetadata,
                     const AdjustmentData &inputAdjustment,
                     const QString &oldPath,
                     QString *error)
{
    if (error) error->clear();
    AdjustmentData adjustment = inputAdjustment;
    adjustment.normalise();
    bool payloadOk = false;
    const QJsonObject payload = adjustmentPayload(adjustment, &payloadOk);
    if (!payloadOk) {
        if (error) *error = QStringLiteral("The adjustment could not be serialised");
        return false;
    }

    PresetMetadata metadata = inputMetadata;
    metadata.name = cleanName(metadata.name);
    metadata.category = metadata.category.trimmed();
    if (metadata.category.isEmpty()) {
        metadata.category = adjustmentTypeToString(adjustment.type);
    }
    metadata.builtIn = false;
    const qint64 now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    if (metadata.createdUtcMs <= 0) metadata.createdUtcMs = now;
    metadata.modifiedUtcMs = now;

    PresetEnvelope envelope;
    envelope.kind = PresetKind::Adjustment;
    envelope.metadata = metadata;
    envelope.payload = payload;
    const QString path = PresetStore::filePathForId(
        AdjustmentPresetStore::storageDirectory(adjustment.type), metadata.id);
    if (!PresetStore::writeFile(path, envelope, MaximumPresetFileBytes, error)) {
        return false;
    }
    if (!oldPath.isEmpty()
        && QFileInfo(oldPath).absoluteFilePath()
            != QFileInfo(path).absoluteFilePath()) {
        QFile oldFile(oldPath);
        if (managedPath(oldPath, adjustment.type) && oldFile.exists()
            && !oldFile.remove()) {
            QFile::remove(path);
            if (error) {
                *error = QStringLiteral("The new preset was written, but the old preset could not be replaced: %1")
                             .arg(oldFile.errorString());
            }
            return false;
        }
    }
    return true;
}

bool readLegacyPreset(const QFileInfo &info,
                      const AdjustmentType type,
                      AdjustmentPreset *preset,
                      QString *error)
{
    if (error) error->clear();
    if (!preset || info.size() < 0 || info.size() > MaximumPresetFileBytes) {
        if (error) *error = QStringLiteral("The legacy preset exceeds the safety limit");
        return false;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Could not read the preset");
        return false;
    }
    const QByteArray bytes = file.read(MaximumPresetFileBytes + 1);
    if (bytes.size() != info.size()) {
        if (error) *error = QStringLiteral("Could not read the complete preset");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    const QJsonObject root = document.object();
    bool adjustmentOk = false;
    AdjustmentData adjustment = AdjustmentData::fromJson(
        root.value(QStringLiteral("adjustment")).toObject(), type, &adjustmentOk);
    const QString name = cleanName(root.value(QStringLiteral("name")).toString());
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || root.value(QStringLiteral("format")).toString()
            != QStringLiteral("vfxphotolab-adjustment-preset")
        || root.value(QStringLiteral("version")).toInt() != 1
        || name.isEmpty() || !adjustmentOk || adjustment.type != type) {
        if (error) *error = QStringLiteral("The legacy adjustment preset is invalid");
        return false;
    }
    adjustment.normalise();
    *preset = {name, adjustment, false, info.absoluteFilePath(),
               metadataForLegacy(name, adjustment)};
    return true;
}

} // namespace

QString AdjustmentPresetStore::storageDirectory(const AdjustmentType type)
{
    return PresetStore::storageDirectory(
        PresetKind::Adjustment, adjustmentTypeToString(type));
}

QVector<AdjustmentPreset> AdjustmentPresetStore::builtInPresets(const AdjustmentType type)
{
    QVector<AdjustmentPreset> result;
    const auto make = [type](const auto &parameters) {
        AdjustmentData data;
        data.reset(type);
        data.parameters = parameters;
        data.normalise();
        return data;
    };
    switch (type) {
    case AdjustmentType::Exposure: {
        ExposureParameters plus; plus.exposure = 1.0;
        ExposureParameters minus; minus.exposure = -1.0;
        ExposureParameters lift; lift.offset = 0.05; lift.gamma = 0.9;
        result << preset(QStringLiteral("+1 EV"), make(plus))
               << preset(QStringLiteral("-1 EV"), make(minus))
               << preset(QStringLiteral("Lift Shadows"), make(lift));
        break;
    }
    case AdjustmentType::Contrast: {
        ContrastParameters soft; soft.contrast = -20.0;
        ContrastParameters punch; punch.contrast = 25.0; punch.pivot = 0.45;
        result << preset(QStringLiteral("Soft Contrast"), make(soft))
               << preset(QStringLiteral("Punchy"), make(punch));
        break;
    }
    case AdjustmentType::Saturation: {
        SaturationParameters muted; muted.saturation = -35.0;
        SaturationParameters rich; rich.saturation = 25.0;
        result << preset(QStringLiteral("Muted"), make(muted))
               << preset(QStringLiteral("Rich Colour"), make(rich));
        break;
    }
    case AdjustmentType::Levels: {
        LevelsParameters strong;
        strong.channel(AdjustmentChannel::Rgb).inputBlack = 0.04;
        strong.channel(AdjustmentChannel::Rgb).inputWhite = 0.96;
        strong.channel(AdjustmentChannel::Rgb).gamma = 0.95;
        LevelsParameters fade;
        fade.channel(AdjustmentChannel::Rgb).outputBlack = 0.06;
        fade.channel(AdjustmentChannel::Rgb).outputWhite = 0.95;
        result << preset(QStringLiteral("Tighten Range"), make(strong))
               << preset(QStringLiteral("Faded Output"), make(fade));
        break;
    }
    case AdjustmentType::Curves: {
        CurvesParameters contrast;
        contrast.channel(AdjustmentChannel::Rgb).points = {{0.0, 0.0}, {0.25, 0.18}, {0.75, 0.82}, {1.0, 1.0}};
        CurvesParameters matte;
        matte.channel(AdjustmentChannel::Rgb).points = {{0.0, 0.06}, {0.35, 0.32}, {0.75, 0.78}, {1.0, 0.96}};
        CurvesParameters crossProcess;
        crossProcess.channel(AdjustmentChannel::Rgb).points = {
            {0.0, 0.03}, {0.28, 0.22}, {0.72, 0.82}, {1.0, 0.98}
        };
        crossProcess.channel(AdjustmentChannel::Red).points = {
            {0.0, 0.02}, {0.48, 0.57}, {1.0, 1.0}
        };
        crossProcess.channel(AdjustmentChannel::Blue).points = {
            {0.0, 0.08}, {0.52, 0.45}, {1.0, 0.94}
        };
        result << preset(QStringLiteral("Gentle S-Curve"), make(contrast))
               << preset(QStringLiteral("Matte Fade"), make(matte))
               << preset(QStringLiteral("Cross Process"), make(crossProcess));
        break;
    }
    case AdjustmentType::HueSaturation: {
        HueSaturationParameters vivid; vivid.saturation = 18.0;
        HueSaturationParameters cool; cool.hue = -8.0; cool.saturation = 8.0;
        HueSaturationParameters muteGreens;
        muteGreens.range(HueSaturationRange::Greens).saturation = -35.0;
        muteGreens.range(HueSaturationRange::Greens).lightness = -4.0;
        HueSaturationParameters warmReds;
        warmReds.range(HueSaturationRange::Reds).hue = 8.0;
        warmReds.range(HueSaturationRange::Reds).saturation = 16.0;
        warmReds.range(HueSaturationRange::Yellows).hue = -5.0;
        result << preset(QStringLiteral("Vivid"), make(vivid))
               << preset(QStringLiteral("Cool Shift"), make(cool))
               << preset(QStringLiteral("Mute Greens"), make(muteGreens))
               << preset(QStringLiteral("Warm Reds"), make(warmReds));
        break;
    }
    case AdjustmentType::Vibrance: {
        VibranceParameters natural; natural.vibrance = 30.0; natural.skinProtection = 75.0;
        VibranceParameters subdued; subdued.vibrance = -25.0; subdued.saturation = -5.0;
        result << preset(QStringLiteral("Natural Boost"), make(natural))
               << preset(QStringLiteral("Subdued"), make(subdued));
        break;
    }
    case AdjustmentType::WhiteBalance: {
        WhiteBalanceParameters warm; warm.temperature = 20.0; warm.tint = 3.0;
        WhiteBalanceParameters cool; cool.temperature = -20.0; cool.tint = -3.0;
        result << preset(QStringLiteral("Warm"), make(warm))
               << preset(QStringLiteral("Cool"), make(cool));
        break;
    }
    case AdjustmentType::ColourBalance: {
        ColourBalanceParameters cinematic;
        cinematic.range(ColourBalanceRange::Shadows).cyanRed = -12.0;
        cinematic.range(ColourBalanceRange::Shadows).yellowBlue = 12.0;
        cinematic.range(ColourBalanceRange::Highlights).cyanRed = 10.0;
        cinematic.range(ColourBalanceRange::Highlights).yellowBlue = -8.0;
        ColourBalanceParameters warmMidtones;
        warmMidtones.range(ColourBalanceRange::Midtones).cyanRed = 9.0;
        warmMidtones.range(ColourBalanceRange::Midtones).yellowBlue = -12.0;
        ColourBalanceParameters coolClean;
        coolClean.range(ColourBalanceRange::Shadows).cyanRed = -8.0;
        coolClean.range(ColourBalanceRange::Midtones).yellowBlue = 6.0;
        coolClean.range(ColourBalanceRange::Highlights).yellowBlue = 10.0;
        result << preset(QStringLiteral("Cool Shadows, Warm Highlights"), make(cinematic))
               << preset(QStringLiteral("Warm Midtones"), make(warmMidtones))
               << preset(QStringLiteral("Cool Clean"), make(coolClean));
        break;
    }
    case AdjustmentType::ChannelMixer: {
        ChannelMixerParameters swap;
        swap.outputs[0] = {0.0, 0.0, 100.0, 0.0};
        swap.outputs[2] = {100.0, 0.0, 0.0, 0.0};
        ChannelMixerParameters mono;
        mono.monochromeEnabled = true;
        ChannelMixerParameters infrared;
        infrared.monochromeEnabled = true;
        infrared.monochrome = {120.0, 45.0, -35.0, 0.0};
        result << preset(QStringLiteral("Swap Red and Blue"), make(swap))
               << preset(QStringLiteral("Balanced Monochrome"), make(mono))
               << preset(QStringLiteral("Infrared Monochrome"), make(infrared));
        break;
    }
    case AdjustmentType::BlackAndWhite: {
        BlackAndWhiteParameters high;
        high.colourWeights = {120.0, 110.0, 90.0, 80.0, 70.0, 105.0};
        BlackAndWhiteParameters sepia;
        sepia.tintEnabled = true; sepia.tintHue = 35.0; sepia.tintSaturation = 24.0;
        BlackAndWhiteParameters redFilter;
        redFilter.colourWeights = {175.0, 130.0, 90.0, 75.0, 55.0, 120.0};
        BlackAndWhiteParameters blueFilter;
        blueFilter.colourWeights = {70.0, 85.0, 105.0, 130.0, 165.0, 95.0};
        result << preset(QStringLiteral("High Contrast"), make(high))
               << preset(QStringLiteral("Red Filter"), make(redFilter))
               << preset(QStringLiteral("Blue Filter"), make(blueFilter))
               << preset(QStringLiteral("Sepia"), make(sepia));
        break;
    }
    case AdjustmentType::GradientMap: {
        GradientMapParameters sepia;
        sepia.stops = {{0.0, QColor(QStringLiteral("#24170f"))},
                       {0.45, QColor(QStringLiteral("#9b6b43"))},
                       {1.0, QColor(QStringLiteral("#f3e4c7"))}};
        GradientMapParameters tealOrange;
        tealOrange.stops = {{0.0, QColor(QStringLiteral("#062b38"))},
                            {0.5, QColor(QStringLiteral("#77706a"))},
                            {1.0, QColor(QStringLiteral("#ffb45f"))}};
        GradientMapParameters moonlight;
        moonlight.stops = {{0.0, QColor(QStringLiteral("#070b1b"))},
                           {0.42, QColor(QStringLiteral("#263f72"))},
                           {0.78, QColor(QStringLiteral("#8eb4cf"))},
                           {1.0, QColor(QStringLiteral("#f2f4e8"))}};
        moonlight.interpolation = GradientInterpolation::Smooth;
        GradientMapParameters copperCyan;
        copperCyan.stops = {{0.0, QColor(QStringLiteral("#071f24"))},
                            {0.38, QColor(QStringLiteral("#1f6971"))},
                            {0.64, QColor(QStringLiteral("#a86c45"))},
                            {1.0, QColor(QStringLiteral("#f2d2a2"))}};
        copperCyan.interpolation = GradientInterpolation::Smooth;
        result << preset(QStringLiteral("Sepia"), make(sepia))
               << preset(QStringLiteral("Teal and Orange"), make(tealOrange))
               << preset(QStringLiteral("Moonlight"), make(moonlight))
               << preset(QStringLiteral("Copper and Cyan"), make(copperCyan));
        break;
    }
    case AdjustmentType::Posterise: {
        PosteriseParameters four; four.levels = 4;
        PosteriseParameters eight; eight.levels = 8;
        PosteriseParameters sixteen; sixteen.levels = 16;
        result << preset(QStringLiteral("4 Levels"), make(four))
               << preset(QStringLiteral("8 Levels"), make(eight))
               << preset(QStringLiteral("16 Levels"), make(sixteen));
        break;
    }
    case AdjustmentType::Threshold: {
        ThresholdParameters midpoint;
        ThresholdParameters shadows; shadows.threshold = 0.25;
        ThresholdParameters highlights; highlights.threshold = 0.75;
        result << preset(QStringLiteral("Midpoint"), make(midpoint))
               << preset(QStringLiteral("Shadow Cut"), make(shadows))
               << preset(QStringLiteral("Highlight Cut"), make(highlights));
        break;
    }
    case AdjustmentType::Invert:
        // The adjustment itself is the complete operation; a preset would be
        // indistinguishable from creating a fresh Invert layer.
        break;
    case AdjustmentType::PhotoFilter: {
        PhotoFilterParameters warming85;
        warming85.colour = QColor(236, 138, 0);
        warming85.density = 25.0;
        PhotoFilterParameters warmingLba;
        warmingLba.colour = QColor(250, 150, 0);
        warmingLba.density = 25.0;
        PhotoFilterParameters warming81;
        warming81.colour = QColor(235, 177, 90);
        warming81.density = 25.0;
        PhotoFilterParameters cooling80;
        cooling80.colour = QColor(0, 109, 255);
        cooling80.density = 25.0;
        PhotoFilterParameters coolingLbb;
        coolingLbb.colour = QColor(0, 93, 194);
        coolingLbb.density = 25.0;
        PhotoFilterParameters cooling82;
        cooling82.colour = QColor(55, 145, 218);
        cooling82.density = 25.0;
        PhotoFilterParameters sepia;
        sepia.colour = QColor(172, 122, 51);
        sepia.density = 35.0;
        result << preset(QStringLiteral("Warming Filter (85)"), make(warming85))
               << preset(QStringLiteral("Warming Filter (LBA)"), make(warmingLba))
               << preset(QStringLiteral("Warming Filter (81)"), make(warming81))
               << preset(QStringLiteral("Cooling Filter (80)"), make(cooling80))
               << preset(QStringLiteral("Cooling Filter (LBB)"), make(coolingLbb))
               << preset(QStringLiteral("Cooling Filter (82)"), make(cooling82))
               << preset(QStringLiteral("Sepia Filter"), make(sepia));
        break;
    }
    case AdjustmentType::SelectiveColour: {
        SelectiveColourParameters punchyReds;
        punchyReds.range(SelectiveColourRange::Reds) = {-18.0, 10.0, 14.0, 3.0};
        SelectiveColourParameters deepBlues;
        deepBlues.range(SelectiveColourRange::Blues) = {12.0, 8.0, -20.0, 12.0};
        deepBlues.range(SelectiveColourRange::Cyans) = {7.0, 2.0, -9.0, 5.0};
        SelectiveColourParameters cleanNeutrals;
        cleanNeutrals.range(SelectiveColourRange::Whites).black = -7.0;
        cleanNeutrals.range(SelectiveColourRange::Neutrals) = {-3.0, -2.0, 4.0, -3.0};
        cleanNeutrals.range(SelectiveColourRange::Blacks).black = 9.0;
        SelectiveColourParameters absolutePrint;
        absolutePrint.method = SelectiveColourMethod::Absolute;
        absolutePrint.range(SelectiveColourRange::Yellows) = {-4.0, 3.0, 8.0, 0.0};
        absolutePrint.range(SelectiveColourRange::Blacks).black = 5.0;
        result << preset(QStringLiteral("Punchy Reds"), make(punchyReds))
               << preset(QStringLiteral("Deep Blues"), make(deepBlues))
               << preset(QStringLiteral("Clean Neutrals"), make(cleanNeutrals))
               << preset(QStringLiteral("Absolute Print Tweak"), make(absolutePrint));
        break;
    }
    case AdjustmentType::Vignette: {
        VignetteParameters classic;
        classic.amount = 45.0; classic.midpoint = 45.0; classic.feather = 65.0;
        VignetteParameters portrait;
        portrait.amount = 24.0; portrait.midpoint = 58.0; portrait.roundness = 18.0;
        portrait.feather = 82.0; portrait.highlightProtection = 40.0;
        VignetteParameters glow;
        glow.amount = -28.0; glow.midpoint = 52.0; glow.feather = 78.0; glow.inverted = true;
        VignetteParameters warm;
        warm.amount = 34.0; warm.midpoint = 50.0; warm.feather = 72.0;
        warm.tintEnabled = true; warm.tint = QColor(61, 35, 20);
        VignetteParameters corners;
        corners.amount = 42.0; corners.size = 225.0; corners.midpoint = 48.0;
        corners.feather = 62.0; corners.highlightProtection = 20.0;
        result << preset(QStringLiteral("Classic Dark"), make(classic))
               << preset(QStringLiteral("Soft Portrait"), make(portrait))
               << preset(QStringLiteral("Corners Only"), make(corners))
               << preset(QStringLiteral("Centre Glow"), make(glow))
               << preset(QStringLiteral("Warm Edge Tint"), make(warm));
        break;
    }
    case AdjustmentType::RgbSplit: {
        RgbSplitParameters horizontal;
        horizontal.redOffsetX = 6.0; horizontal.blueOffsetX = -6.0;
        RgbSplitParameters vertical;
        vertical.redOffsetX = 0.0; vertical.blueOffsetX = 0.0;
        vertical.redOffsetY = 6.0; vertical.blueOffsetY = -6.0;
        RgbSplitParameters diagonal;
        diagonal.redOffsetX = 5.0; diagonal.redOffsetY = -3.0;
        diagonal.blueOffsetX = -5.0; diagonal.blueOffsetY = 3.0;
        result << preset(QStringLiteral("Horizontal Split"), make(horizontal))
               << preset(QStringLiteral("Vertical Split"), make(vertical))
               << preset(QStringLiteral("Digital Diagonal"), make(diagonal));
        break;
    }
    case AdjustmentType::ChromaticAberrationCorrection: {
        ChromaticAberrationCorrectionParameters redOut;
        redOut.redEdgeShift = 1.5; redOut.blueEdgeShift = -1.5;
        ChromaticAberrationCorrectionParameters redIn;
        redIn.redEdgeShift = -1.5; redIn.blueEdgeShift = 1.5;
        ChromaticAberrationCorrectionParameters strong;
        strong.redEdgeShift = 3.0; strong.blueEdgeShift = -3.0; strong.falloff = 1.5;
        result << preset(QStringLiteral("Red Out, Blue In"), make(redOut))
               << preset(QStringLiteral("Red In, Blue Out"), make(redIn))
               << preset(QStringLiteral("Strong Radial Correction"), make(strong));
        break;
    }
    case AdjustmentType::SurfaceBlur: {
        SurfaceBlurParameters portrait; portrait.radius = 10.0; portrait.threshold = 18.0;
        SurfaceBlurParameters smooth; smooth.radius = 22.0; smooth.threshold = 32.0;
        SurfaceBlurParameters broad; broad.radius = 45.0; broad.threshold = 55.0;
        result << preset(QStringLiteral("Portrait Smoothing"), make(portrait))
               << preset(QStringLiteral("Smooth Surfaces"), make(smooth))
               << preset(QStringLiteral("Broad Edge-Preserving Blur"), make(broad));
        break;
    }
    case AdjustmentType::MotionBlur: {
        MotionBlurParameters horizontal; horizontal.distance = 24.0; horizontal.angle = 0.0;
        MotionBlurParameters diagonal; diagonal.distance = 36.0; diagonal.angle = 35.0; diagonal.samples = 32;
        MotionBlurParameters vertical; vertical.distance = 48.0; vertical.angle = 90.0; vertical.samples = 32;
        result << preset(QStringLiteral("Horizontal Motion"), make(horizontal))
               << preset(QStringLiteral("Diagonal Motion"), make(diagonal))
               << preset(QStringLiteral("Vertical Motion"), make(vertical));
        break;
    }
    case AdjustmentType::RadialBlur: {
        RadialBlurParameters spin; spin.mode = RadialBlurMode::Spin; spin.amount = 22.0;
        RadialBlurParameters strongSpin; strongSpin.mode = RadialBlurMode::Spin; strongSpin.amount = 55.0; strongSpin.samples = 40;
        RadialBlurParameters zoom; zoom.mode = RadialBlurMode::Zoom; zoom.amount = 38.0; zoom.samples = 32;
        result << preset(QStringLiteral("Gentle Spin"), make(spin))
               << preset(QStringLiteral("Strong Spin"), make(strongSpin))
               << preset(QStringLiteral("Zoom Burst"), make(zoom));
        break;
    }
    case AdjustmentType::Lut:
        break;
    case AdjustmentType::ShadowsHighlights: {
        ShadowsHighlightsParameters balanced;
        balanced.shadowAmount = 35.0;
        balanced.highlightAmount = 25.0;
        balanced.radius = 90.0;
        balanced.midtoneContrast = 8.0;
        ShadowsHighlightsParameters strong;
        strong.shadowAmount = 60.0;
        strong.shadowTonalWidth = 65.0;
        strong.highlightAmount = 45.0;
        strong.highlightTonalWidth = 60.0;
        strong.radius = 130.0;
        strong.midtoneContrast = 12.0;
        strong.colourCorrection = 30.0;
        result << preset(QStringLiteral("Balanced Recovery"), make(balanced))
               << preset(QStringLiteral("Strong Recovery"), make(strong));
        break;
    }
    case AdjustmentType::GaussianBlur: {
        GaussianBlurParameters subtle; subtle.radius = 3.0;
        GaussianBlurParameters soft; soft.radius = 12.0;
        GaussianBlurParameters dreamy; dreamy.radius = 35.0;
        result << preset(QStringLiteral("Subtle Softening"), make(subtle))
               << preset(QStringLiteral("Soft Focus"), make(soft))
               << preset(QStringLiteral("Dreamy Blur"), make(dreamy));
        break;
    }
    case AdjustmentType::BoxBlur: {
        BoxBlurParameters small; small.radius = 2.0;
        BoxBlurParameters medium; medium.radius = 8.0;
        result << preset(QStringLiteral("Small Box"), make(small))
               << preset(QStringLiteral("Medium Box"), make(medium));
        break;
    }
    case AdjustmentType::UnsharpMask: {
        UnsharpMaskParameters capture; capture.radius = 1.0; capture.amount = 80.0; capture.threshold = 2.0;
        UnsharpMaskParameters general; general.radius = 2.0; general.amount = 120.0; general.threshold = 4.0;
        UnsharpMaskParameters local; local.radius = 18.0; local.amount = 35.0; local.threshold = 0.0;
        result << preset(QStringLiteral("Capture Sharpening"), make(capture))
               << preset(QStringLiteral("General Sharpening"), make(general))
               << preset(QStringLiteral("Local Contrast"), make(local));
        break;
    }
    case AdjustmentType::HighPass: {
        HighPassParameters fine; fine.radius = 2.0;
        HighPassParameters medium; medium.radius = 8.0;
        HighPassParameters mono; mono.radius = 4.0; mono.monochrome = true;
        result << preset(QStringLiteral("Fine Detail"), make(fine))
               << preset(QStringLiteral("Medium Detail"), make(medium))
               << preset(QStringLiteral("Monochrome Detail"), make(mono));
        break;
    }
    }
    for (AdjustmentPreset &entry : result) {
        entry.metadata.id = PresetStore::stableBuiltInId(
            PresetKind::Adjustment, adjustmentTypeToString(type), entry.name);
        entry.metadata.name = entry.name;
        entry.metadata.category = adjustmentTypeToString(type);
        entry.metadata.builtIn = true;
        const PresetUsageMetadata usage = PresetUsageStore::usageFor(
            entry.metadata.id);
        entry.metadata.favourite = usage.favourite;
        entry.metadata.lastUsedUtcMs = usage.lastUsedUtcMs;
        entry.metadata.useCount = usage.useCount;
    }
    return result;
}

QVector<AdjustmentPreset> AdjustmentPresetStore::presets(const AdjustmentType type,
                                                         QStringList *warnings)
{
    QVector<AdjustmentPreset> result = builtInPresets(type);
    QSet<QString> loadedIds;
    QSet<QString> loadedUserNames;
    for (const AdjustmentPreset &entry : result) loadedIds.insert(entry.metadata.id);

    auto addUserPreset = [&](AdjustmentPreset entry, const QString &fileName) {
        const QString foldedName = entry.name.toCaseFolded();
        if (loadedIds.contains(entry.metadata.id)
            || loadedUserNames.contains(foldedName)) {
            if (warnings) {
                warnings->push_back(QStringLiteral("Ignored duplicate adjustment preset %1")
                                        .arg(fileName));
            }
            return;
        }
        loadedIds.insert(entry.metadata.id);
        loadedUserNames.insert(foldedName);
        result.push_back(std::move(entry));
    };

    QDir directory(storageDirectory(type));
    const QFileInfoList unifiedFiles = directory.exists()
        ? directory.entryInfoList({QStringLiteral("*.json")},
                                  QDir::Files | QDir::Readable,
                                  QDir::Name | QDir::IgnoreCase)
        : QFileInfoList();
    const qsizetype unifiedCount = std::min<qsizetype>(
        unifiedFiles.size(), MaximumPresetCount);
    if (unifiedFiles.size() > MaximumPresetCount && warnings) {
        warnings->push_back(QStringLiteral("Only the first %1 adjustment presets were loaded")
                                .arg(MaximumPresetCount));
    }
    for (qsizetype index = 0; index < unifiedCount; ++index) {
        const QFileInfo &info = unifiedFiles.at(index);
        if (info.size() <= 0 || info.size() > MaximumPresetFileBytes) {
            if (warnings) {
                warnings->push_back(QStringLiteral("Ignored oversized preset %1")
                                        .arg(info.fileName()));
            }
            continue;
        }
        PresetEnvelope envelope;
        QString loadError;
        if (!PresetStore::readFile(info.absoluteFilePath(),
                                   PresetKind::Adjustment,
                                   MaximumPresetFileBytes,
                                   &envelope,
                                   &loadError)) {
            if (warnings) {
                warnings->push_back(QStringLiteral("Ignored invalid preset %1: %2")
                                        .arg(info.fileName(), loadError));
            }
            continue;
        }
        AdjustmentData adjustment;
        if (envelope.metadata.builtIn
            || !adjustmentFromEnvelope(envelope, type, &adjustment, &loadError)) {
            if (warnings) {
                warnings->push_back(QStringLiteral("Ignored invalid preset %1: %2")
                                        .arg(info.fileName(), loadError));
            }
            continue;
        }
        addUserPreset({envelope.metadata.name, adjustment, false,
                       info.absoluteFilePath(), envelope.metadata},
                      info.fileName());
    }

    QDir legacyDirectory(legacyStorageDirectory(type));
    const QFileInfoList legacyFiles = legacyDirectory.exists()
        ? legacyDirectory.entryInfoList({QStringLiteral("*.json")},
                                        QDir::Files | QDir::Readable,
                                        QDir::Name | QDir::IgnoreCase)
        : QFileInfoList();
    const qsizetype remainingLegacyCapacity = std::max<qsizetype>(
        0, MaximumPresetCount - unifiedCount);
    const qsizetype legacyCount = std::min<qsizetype>(
        legacyFiles.size(), remainingLegacyCapacity);
    if (legacyFiles.size() > legacyCount && warnings) {
        warnings->push_back(QStringLiteral("Only the first %1 total adjustment presets were loaded")
                                .arg(MaximumPresetCount));
    }
    for (qsizetype index = 0; index < legacyCount; ++index) {
        const QFileInfo &info = legacyFiles.at(index);
        if (info.size() <= 0 || info.size() > MaximumPresetFileBytes) {
            if (warnings) {
                warnings->push_back(QStringLiteral("Ignored oversized legacy preset %1")
                                        .arg(info.fileName()));
            }
            continue;
        }
        AdjustmentPreset legacy;
        QString loadError;
        if (!readLegacyPreset(info, type, &legacy, &loadError)) {
            if (warnings) {
                warnings->push_back(QStringLiteral("Ignored invalid legacy preset %1: %2")
                                        .arg(info.fileName(), loadError));
            }
            continue;
        }
        addUserPreset(std::move(legacy), info.fileName());
    }

    const qsizetype builtInCount = builtInPresets(type).size();
    std::sort(result.begin() + builtInCount, result.end(),
              [](const AdjustmentPreset &left,
                 const AdjustmentPreset &right) {
                  if (left.metadata.favourite != right.metadata.favourite) {
                      return left.metadata.favourite;
                  }
                  if (left.metadata.lastUsedUtcMs != right.metadata.lastUsedUtcMs) {
                      return left.metadata.lastUsedUtcMs > right.metadata.lastUsedUtcMs;
                  }
                  return QString::localeAwareCompare(left.name, right.name) < 0;
              });
    return result;
}

bool AdjustmentPresetStore::saveUserPreset(const QString &inputName,
                                           const AdjustmentData &inputAdjustment,
                                           QString *error,
                                           const bool overwrite)
{
    if (error) error->clear();
    const QString name = cleanName(inputName);
    if (name.isEmpty()) {
        if (error) *error = QStringLiteral("Enter a preset name");
        return false;
    }
    AdjustmentData adjustment = inputAdjustment;
    adjustment.normalise();
    const QVector<AdjustmentPreset> existing = presets(adjustment.type);
    const auto match = userPresetWithName(existing, name);
    if (match.has_value() && !overwrite) {
        if (error) *error = QStringLiteral("A user preset with this name already exists");
        return false;
    }
    if (!match.has_value() && existing.size() >= MaximumPresetCount) {
        if (error) *error = QStringLiteral("The adjustment preset limit has been reached");
        return false;
    }

    PresetMetadata metadata = match.has_value()
        ? match->metadata : PresetMetadata();
    if (metadata.id.isEmpty()) metadata.id = PresetStore::newUserId(PresetKind::Adjustment);
    metadata.name = name;
    return writeUserPreset(metadata, adjustment,
                           match.has_value() ? match->storagePath : QString(),
                           error);
}

bool AdjustmentPresetStore::createUserPreset(
    const QString &inputName,
    const AdjustmentData &inputAdjustment,
    const QString &category,
    const QStringList &tags,
    QString *createdId,
    QString *error)
{
    if (error) error->clear();
    const QString name = cleanName(inputName);
    AdjustmentData adjustment = inputAdjustment;
    adjustment.normalise();
    if (name.isEmpty()) {
        if (error) *error = QStringLiteral("Enter a preset name");
        return false;
    }
    const QVector<AdjustmentPreset> existing = presets(adjustment.type);
    if (userPresetWithName(existing, name).has_value()) {
        if (error) *error = QStringLiteral("A user preset with this name already exists");
        return false;
    }
    if (existing.size() >= MaximumPresetCount) {
        if (error) *error = QStringLiteral("The adjustment preset limit has been reached");
        return false;
    }
    PresetMetadata metadata;
    metadata.id = PresetStore::newUserId(PresetKind::Adjustment);
    metadata.name = name;
    metadata.category = category.trimmed();
    metadata.tags = tags;
    if (!writeUserPreset(metadata, adjustment, {}, error)) return false;
    if (createdId) *createdId = metadata.id;
    return true;
}

bool AdjustmentPresetStore::userPresetExists(const QString &inputName,
                                              const AdjustmentType type)
{
    const QString name = cleanName(inputName);
    if (name.isEmpty()) return false;
    return userPresetWithName(presets(type), name).has_value();
}

bool AdjustmentPresetStore::renameUserPreset(const AdjustmentPreset &preset,
                                             const QString &inputName,
                                             QString *error)
{
    if (error) error->clear();
    const QString name = cleanName(inputName);
    if (preset.builtIn || !managedPath(preset.storagePath, preset.adjustment.type)
        || name.isEmpty()) {
        if (error) *error = QStringLiteral("The selected preset or new name is not valid");
        return false;
    }
    if (userPresetWithName(presets(preset.adjustment.type), name,
                           preset.metadata.id).has_value()) {
        if (error) *error = QStringLiteral("A preset with that name already exists");
        return false;
    }
    PresetMetadata metadata = preset.metadata;
    if (metadata.id.isEmpty()) {
        metadata = metadataForLegacy(preset.name, preset.adjustment);
    }
    metadata.name = name;
    return writeUserPreset(metadata, preset.adjustment, preset.storagePath, error);
}

bool AdjustmentPresetStore::duplicateUserPreset(const AdjustmentPreset &preset,
                                                const QString &inputName,
                                                QString *error)
{
    if (error) error->clear();
    const QString name = cleanName(inputName);
    if (name.isEmpty() || userPresetWithName(
            presets(preset.adjustment.type), name).has_value()) {
        if (error) *error = QStringLiteral("Enter a unique preset name");
        return false;
    }
    PresetMetadata metadata;
    metadata.id = PresetStore::newUserId(PresetKind::Adjustment);
    metadata.name = name;
    metadata.category = preset.metadata.category.trimmed();
    if (metadata.category.isEmpty()) {
        metadata.category = adjustmentTypeToString(preset.adjustment.type);
    }
    metadata.tags = preset.metadata.tags;
    metadata.favourite = preset.metadata.favourite;
    return writeUserPreset(metadata, preset.adjustment, {}, error);
}

bool AdjustmentPresetStore::updateUserPreset(const AdjustmentPreset &preset,
                                             const AdjustmentData &adjustment,
                                             QString *error)
{
    if (preset.builtIn || preset.adjustment.type != adjustment.type
        || !managedPath(preset.storagePath, adjustment.type)) {
        if (error) *error = QStringLiteral("The selected preset cannot be updated");
        return false;
    }
    PresetMetadata metadata = preset.metadata;
    if (metadata.id.isEmpty()) metadata = metadataForLegacy(preset.name, preset.adjustment);
    metadata.name = preset.name;
    return writeUserPreset(metadata, adjustment, preset.storagePath, error);
}

bool AdjustmentPresetStore::updateMetadata(
    const AdjustmentPreset &preset,
    const QString &category,
    const QStringList &tags,
    QString *error)
{
    if (preset.builtIn
        || !managedPath(preset.storagePath, preset.adjustment.type)) {
        if (error) *error = QStringLiteral("The selected preset details cannot be changed");
        return false;
    }
    PresetMetadata metadata = preset.metadata;
    if (metadata.id.isEmpty()) {
        metadata = metadataForLegacy(preset.name, preset.adjustment);
    }
    metadata.name = preset.name;
    metadata.category = category.trimmed();
    metadata.tags = tags;
    return writeUserPreset(metadata, preset.adjustment, preset.storagePath, error);
}

bool AdjustmentPresetStore::setFavourite(const AdjustmentPreset &preset,
                                         const bool favourite,
                                         QString *error)
{
    if (preset.builtIn) {
        return PresetUsageStore::setFavourite(
            preset.metadata.id, favourite, error);
    }
    if (!managedPath(preset.storagePath, preset.adjustment.type)) {
        if (error) *error = QStringLiteral("The selected preset cannot be changed");
        return false;
    }
    PresetMetadata metadata = preset.metadata;
    if (metadata.id.isEmpty()) metadata = metadataForLegacy(preset.name, preset.adjustment);
    metadata.name = preset.name;
    metadata.favourite = favourite;
    return writeUserPreset(metadata, preset.adjustment, preset.storagePath, error);
}

bool AdjustmentPresetStore::recordUse(const AdjustmentPreset &preset,
                                      QString *error)
{
    if (preset.builtIn) {
        return PresetUsageStore::recordUse(preset.metadata.id, error);
    }
    if (!managedPath(preset.storagePath, preset.adjustment.type)) {
        if (error) *error = QStringLiteral("The selected preset cannot be changed");
        return false;
    }
    PresetMetadata metadata = preset.metadata;
    if (metadata.id.isEmpty()) metadata = metadataForLegacy(preset.name, preset.adjustment);
    metadata.name = preset.name;
    metadata.lastUsedUtcMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    if (metadata.useCount < 9007199254740991ULL) ++metadata.useCount;
    return writeUserPreset(metadata, preset.adjustment, preset.storagePath, error);
}

bool AdjustmentPresetStore::importPresetFile(const QString &sourcePath,
                                             const AdjustmentType expectedType,
                                             QString *importedName,
                                             QString *error)
{
    PresetEnvelope envelope;
    if (!PresetStore::readFile(sourcePath, PresetKind::Adjustment,
                               MaximumPresetFileBytes, &envelope, error)) {
        return false;
    }
    AdjustmentData adjustment;
    if (!adjustmentFromEnvelope(envelope, expectedType, &adjustment, error)) return false;
    if (envelope.metadata.builtIn) {
        envelope.metadata.id = PresetStore::newUserId(PresetKind::Adjustment);
        envelope.metadata.builtIn = false;
    }
    const QVector<AdjustmentPreset> existing = presets(expectedType);
    for (const AdjustmentPreset &entry : existing) {
        if (!entry.builtIn && entry.metadata.id == envelope.metadata.id) {
            if (error) *error = QStringLiteral("This preset has already been imported");
            return false;
        }
    }
    const QString name = uniqueImportedName(existing, envelope.metadata.name);
    if (name.isEmpty()) {
        if (error) *error = QStringLiteral("A unique imported preset name could not be created");
        return false;
    }
    envelope.metadata.name = name;
    envelope.metadata.createdUtcMs = 0;
    envelope.metadata.modifiedUtcMs = 0;
    envelope.metadata.lastUsedUtcMs = 0;
    envelope.metadata.useCount = 0;
    if (!writeUserPreset(envelope.metadata, adjustment, {}, error)) return false;
    if (importedName) *importedName = name;
    return true;
}

bool AdjustmentPresetStore::exportPresetFile(const AdjustmentPreset &preset,
                                             const QString &destinationPath,
                                             QString *error)
{
    if (destinationPath.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Choose a destination preset file");
        return false;
    }
    bool payloadOk = false;
    const QJsonObject payload = adjustmentPayload(preset.adjustment, &payloadOk);
    if (!payloadOk) {
        if (error) *error = QStringLiteral("The adjustment could not be serialised");
        return false;
    }
    PresetEnvelope envelope;
    envelope.kind = PresetKind::Adjustment;
    envelope.metadata = preset.metadata;
    if (envelope.metadata.id.isEmpty()) {
        envelope.metadata = preset.builtIn
            ? PresetMetadata() : metadataForLegacy(preset.name, preset.adjustment);
        if (preset.builtIn) {
            envelope.metadata.id = PresetStore::stableBuiltInId(
                PresetKind::Adjustment,
                adjustmentTypeToString(preset.adjustment.type), preset.name);
            envelope.metadata.builtIn = true;
        }
    }
    envelope.metadata.name = preset.name;
    envelope.metadata.category = envelope.metadata.category.trimmed();
    if (envelope.metadata.category.isEmpty()) {
        envelope.metadata.category = adjustmentTypeToString(
            preset.adjustment.type);
    }
    envelope.payload = payload;
    return PresetStore::writeFile(destinationPath, envelope,
                                  MaximumPresetFileBytes, error);
}

bool AdjustmentPresetStore::removeUserPreset(const AdjustmentPreset &preset,
                                             QString *error)
{
    if (error) error->clear();
    if (preset.builtIn || !managedPath(preset.storagePath, preset.adjustment.type)) {
        if (error) *error = QStringLiteral("Built-in or unmanaged presets cannot be removed");
        return false;
    }
    QFile file(preset.storagePath);
    if (!file.remove()) {
        if (error) *error = QStringLiteral("Could not remove the preset: %1").arg(file.errorString());
        return false;
    }
    return true;
}

} // namespace vfx
