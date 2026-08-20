#include "ExportProfileStore.h"

#include "ExportNamingTemplate.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <optional>
#include <utility>

namespace vfx {
namespace {

constexpr qint64 MaximumProfileFileBytes = 32LL * 1024LL * 1024LL;
constexpr int MaximumProfileCount = 2048;

void setError(QString *error, const QString &message)
{
    if (error) *error = message;
}

QString cleanName(const QString &name)
{
    return name.trimmed().left(PresetStore::MaximumNameLength);
}

QString normalisedSuffix(QString suffix)
{
    suffix = suffix.trimmed().toLower();
    if (suffix == QStringLiteral("jpeg")) suffix = QStringLiteral("jpg");
    if (suffix == QStringLiteral("tif")) suffix = QStringLiteral("tiff");
    return suffix;
}

QString bitDepthToken(const ImageExportBitDepth depth)
{
    return depth == ImageExportBitDepth::Sixteen
        ? QStringLiteral("16") : QStringLiteral("8");
}

QString ditherToken(const ImageExportDither dither)
{
    return dither == ImageExportDither::BlueNoise64
        ? QStringLiteral("blue-noise-64") : QStringLiteral("none");
}

QString alphaToken(const ImageExportAlphaMode mode)
{
    return mode == ImageExportAlphaMode::FlattenToMatte
        ? QStringLiteral("flatten-to-matte")
        : QStringLiteral("preserve-when-supported");
}

bool managedPath(const QString &path)
{
    return PresetStore::pathIsManagedByDirectory(
        path, ExportProfileStore::storageDirectory());
}

std::optional<ExportProfile> profileWithName(
    const QVector<ExportProfile> &profiles,
    const QString &name,
    const QString &exceptId = {})
{
    const QString folded = cleanName(name).toCaseFolded();
    for (const ExportProfile &profile : profiles) {
        if (profile.builtIn || (!exceptId.isEmpty()
            && profile.metadata.id == exceptId)) continue;
        if (profile.name.toCaseFolded() == folded) return profile;
    }
    return std::nullopt;
}

int userProfileCount(const QVector<ExportProfile> &profiles)
{
    return static_cast<int>(std::count_if(
        profiles.cbegin(), profiles.cend(),
        [](const ExportProfile &profile) { return !profile.builtIn; }));
}

QString uniqueImportedName(const QVector<ExportProfile> &profiles,
                           const QString &inputName)
{
    const QString requested = cleanName(inputName);
    if (!profileWithName(profiles, requested).has_value()) return requested;
    for (int suffix = 2; suffix < MaximumProfileCount + 2; ++suffix) {
        const QString suffixText = QStringLiteral(" (%1)").arg(suffix);
        const qsizetype limit = std::max<qsizetype>(
            1, PresetStore::MaximumNameLength - suffixText.size());
        const QString candidate = requested.left(limit).trimmed() + suffixText;
        if (!profileWithName(profiles, candidate).has_value()) return candidate;
    }
    return {};
}

ExportProfile builtIn(const QString &name,
                      const QString &category,
                      const QStringList &tags,
                      ExportProfileData data)
{
    ExportProfile profile;
    profile.name = name;
    profile.data = std::move(data);
    profile.builtIn = true;
    profile.metadata.id = PresetStore::stableBuiltInId(
        PresetKind::ExportProfile, QStringLiteral("production-export"), name);
    profile.metadata.name = name;
    profile.metadata.category = category;
    profile.metadata.tags = tags;
    profile.metadata.builtIn = true;
    const PresetUsageMetadata usage = PresetUsageStore::usageFor(
        profile.metadata.id);
    profile.metadata.favourite = usage.favourite;
    profile.metadata.lastUsedUtcMs = usage.lastUsedUtcMs;
    profile.metadata.useCount = usage.useCount;
    return profile;
}

QJsonObject payloadFor(const ExportProfileData &data, QString *error)
{
    QJsonObject payload;
    payload.insert(QStringLiteral("profileVersion"),
                   ExportProfileData::JsonSchemaVersion);
    const QJsonObject settings = data.toJson(error);
    if (settings.isEmpty()) return {};
    payload.insert(QStringLiteral("settings"), settings);
    return payload;
}

bool dataFromEnvelope(const PresetEnvelope &envelope,
                      ExportProfileData *data,
                      QString *error)
{
    if (!data || envelope.kind != PresetKind::ExportProfile
        || envelope.payload.value(QStringLiteral("profileVersion")).toInt(-1)
            != ExportProfileData::JsonSchemaVersion
        || !envelope.payload.value(QStringLiteral("settings")).isObject()) {
        setError(error,
                 QStringLiteral("The export profile payload is missing or uses an unsupported version."));
        return false;
    }
    return ExportProfileData::fromJson(
        envelope.payload.value(QStringLiteral("settings")).toObject(),
        data, error);
}

bool writeUserProfile(const PresetMetadata &inputMetadata,
                      const ExportProfileData &data,
                      const QString &oldPath,
                      QString *error)
{
    if (error) error->clear();
    QString validationError;
    if (!data.isValid(&validationError)) {
        setError(error, validationError);
        return false;
    }
    PresetMetadata metadata = inputMetadata;
    metadata.name = cleanName(metadata.name);
    metadata.category = metadata.category.trimmed();
    if (metadata.category.isEmpty()) metadata.category = QStringLiteral("Export");
    metadata.builtIn = false;
    const qint64 now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    if (metadata.createdUtcMs <= 0) metadata.createdUtcMs = now;
    metadata.modifiedUtcMs = now;

    const QJsonObject payload = payloadFor(data, error);
    if (payload.isEmpty()) return false;
    PresetEnvelope envelope;
    envelope.kind = PresetKind::ExportProfile;
    envelope.metadata = metadata;
    envelope.payload = payload;
    const QString path = PresetStore::filePathForId(
        ExportProfileStore::storageDirectory(), metadata.id);
    if (!PresetStore::writeFile(path, envelope, MaximumProfileFileBytes, error)) {
        return false;
    }
    if (!oldPath.isEmpty()
        && QFileInfo(oldPath).absoluteFilePath()
            != QFileInfo(path).absoluteFilePath()) {
        QFile oldFile(oldPath);
        if (managedPath(oldPath) && oldFile.exists() && !oldFile.remove()) {
            QFile::remove(path);
            setError(error,
                     QStringLiteral("The updated profile was written, but the previous profile could not be removed."));
            return false;
        }
    }
    return true;
}

} // namespace

bool ExportProfileData::isValid(QString *errorMessage) const
{
    if (errorMessage) errorMessage->clear();
    const QString suffix = normalisedSuffix(formatSuffix);
    const ImageExportCapabilities capabilities =
        imageExportCapabilitiesForPath(QStringLiteral("output.%1").arg(suffix));
    if (!capabilities.valid) {
        setError(errorMessage,
                 QStringLiteral("The export profile uses an unsupported file format."));
        return false;
    }
    if (bitDepth != ImageExportBitDepth::Eight
        && bitDepth != ImageExportBitDepth::Sixteen) {
        setError(errorMessage,
                 QStringLiteral("The export profile uses an invalid bit depth."));
        return false;
    }
    if (bitDepth == ImageExportBitDepth::Sixteen
        && !capabilities.supportsSixteenBit) {
        setError(errorMessage,
                 QStringLiteral("%1 does not support 16-bit integer export.")
                     .arg(capabilities.displayName));
        return false;
    }
    if (dither != ImageExportDither::None
        && dither != ImageExportDither::BlueNoise64) {
        setError(errorMessage,
                 QStringLiteral("The export profile uses an invalid dither mode."));
        return false;
    }
    if (bitDepth == ImageExportBitDepth::Sixteen
        && dither != ImageExportDither::None) {
        setError(errorMessage,
                 QStringLiteral("Dithering must be disabled for a 16-bit export profile."));
        return false;
    }
    if (alphaMode != ImageExportAlphaMode::PreserveWhenSupported
        && alphaMode != ImageExportAlphaMode::FlattenToMatte) {
        setError(errorMessage,
                 QStringLiteral("The export profile uses an invalid Alpha mode."));
        return false;
    }
    if (!capabilities.supportsAlpha
        && alphaMode != ImageExportAlphaMode::FlattenToMatte) {
        setError(errorMessage,
                 QStringLiteral("This format requires transparency to be flattened to a matte."));
        return false;
    }
    if (quality < 0 || quality > 100) {
        setError(errorMessage,
                 QStringLiteral("The export quality must be between 0 and 100."));
        return false;
    }
    if (!matteColour.isValid()) {
        setError(errorMessage,
                 QStringLiteral("The export matte colour is invalid."));
        return false;
    }
    if (!output.profile.isValid()) {
        setError(errorMessage,
                 QStringLiteral("The export output-profile descriptor is invalid."));
        return false;
    }
    if (convertToOutputProfile && output.profile.isUntagged()) {
        setError(errorMessage,
                 QStringLiteral("A converting export profile must contain a tagged ICC or OCIO output space."));
        return false;
    }
    return ExportNamingTemplate::validate(namingTemplate, errorMessage);
}

QJsonObject ExportProfileData::toJson(QString *errorMessage) const
{
    QString validationError;
    if (!isValid(&validationError)) {
        setError(errorMessage, validationError);
        return {};
    }
    QJsonObject object;
    object.insert(QStringLiteral("format"), normalisedSuffix(formatSuffix));
    object.insert(QStringLiteral("bitDepth"), bitDepthToken(bitDepth));
    object.insert(QStringLiteral("dither"), ditherToken(dither));
    object.insert(QStringLiteral("alphaMode"), alphaToken(alphaMode));
    object.insert(QStringLiteral("convertToOutputProfile"),
                  convertToOutputProfile);
    object.insert(QStringLiteral("output"), output.toJson());
    object.insert(QStringLiteral("quality"), quality);
    object.insert(QStringLiteral("matteColour"),
                  matteColour.name(QColor::HexRgb));
    object.insert(QStringLiteral("namingTemplate"), namingTemplate);
    return object;
}

bool ExportProfileData::fromJson(const QJsonObject &object,
                                 ExportProfileData *profile,
                                 QString *errorMessage)
{
    if (!profile) {
        setError(errorMessage,
                 QStringLiteral("The export profile destination is missing."));
        return false;
    }
    const struct RequiredField { const char *name; QJsonValue::Type type; } required[] = {
        {"format", QJsonValue::String},
        {"bitDepth", QJsonValue::String},
        {"dither", QJsonValue::String},
        {"alphaMode", QJsonValue::String},
        {"convertToOutputProfile", QJsonValue::Bool},
        {"output", QJsonValue::Object},
        {"quality", QJsonValue::Double},
        {"matteColour", QJsonValue::String},
        {"namingTemplate", QJsonValue::String},
    };
    for (const RequiredField &field : required) {
        const QJsonValue value = object.value(QString::fromLatin1(field.name));
        if (value.type() != field.type) {
            setError(errorMessage,
                     QStringLiteral("The export profile field ‘%1’ is missing or has the wrong type.")
                         .arg(QString::fromLatin1(field.name)));
            return false;
        }
    }

    ExportProfileData decoded;
    decoded.formatSuffix = normalisedSuffix(
        object.value(QStringLiteral("format")).toString());
    const QString depth = object.value(QStringLiteral("bitDepth")).toString();
    if (depth == QStringLiteral("8")) {
        decoded.bitDepth = ImageExportBitDepth::Eight;
    } else if (depth == QStringLiteral("16")) {
        decoded.bitDepth = ImageExportBitDepth::Sixteen;
    } else {
        setError(errorMessage,
                 QStringLiteral("The export profile has an unknown bit depth."));
        return false;
    }
    const QString dither = object.value(QStringLiteral("dither")).toString();
    if (dither == QStringLiteral("none")) {
        decoded.dither = ImageExportDither::None;
    } else if (dither == QStringLiteral("blue-noise-64")) {
        decoded.dither = ImageExportDither::BlueNoise64;
    } else {
        setError(errorMessage,
                 QStringLiteral("The export profile has an unknown dither mode."));
        return false;
    }
    const QString alpha = object.value(QStringLiteral("alphaMode")).toString();
    if (alpha == QStringLiteral("preserve-when-supported")) {
        decoded.alphaMode = ImageExportAlphaMode::PreserveWhenSupported;
    } else if (alpha == QStringLiteral("flatten-to-matte")) {
        decoded.alphaMode = ImageExportAlphaMode::FlattenToMatte;
    } else {
        setError(errorMessage,
                 QStringLiteral("The export profile has an unknown Alpha mode."));
        return false;
    }
    decoded.convertToOutputProfile = object.value(
        QStringLiteral("convertToOutputProfile")).toBool(true);
    if (!object.value(QStringLiteral("output")).isObject()) {
        setError(errorMessage,
                 QStringLiteral("The export profile has no output-colour settings."));
        return false;
    }
    const auto output = OutputColourSettings::fromJson(
        object.value(QStringLiteral("output")).toObject(), errorMessage);
    if (!output) return false;
    decoded.output = *output;
    decoded.quality = object.value(QStringLiteral("quality")).toInt(95);
    decoded.matteColour = QColor(
        object.value(QStringLiteral("matteColour")).toString());
    decoded.namingTemplate = object.value(
        QStringLiteral("namingTemplate")).toString();
    QString validationError;
    if (!decoded.isValid(&validationError)) {
        setError(errorMessage, validationError);
        return false;
    }
    *profile = std::move(decoded);
    return true;
}

QVector<ExportProfile> ExportProfileStore::builtInProfiles()
{
    const ColourSpaceDescriptor srgb = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));

    ExportProfileData webPng;
    webPng.formatSuffix = QStringLiteral("png");
    webPng.bitDepth = ImageExportBitDepth::Eight;
    webPng.dither = ImageExportDither::BlueNoise64;
    webPng.alphaMode = ImageExportAlphaMode::PreserveWhenSupported;
    webPng.convertToOutputProfile = true;
    webPng.output.profile = srgb;
    webPng.output.renderingIntent = ColourRenderingIntent::RelativeColorimetric;
    webPng.output.blackPointCompensation = true;
    webPng.output.embedProfile = true;
    webPng.namingTemplate = QStringLiteral("{document}-web");

    ExportProfileData webJpeg = webPng;
    webJpeg.formatSuffix = QStringLiteral("jpg");
    webJpeg.quality = 92;
    webJpeg.alphaMode = ImageExportAlphaMode::FlattenToMatte;
    webJpeg.namingTemplate = QStringLiteral("{document}-web");

    ExportProfileData masterPng = webPng;
    masterPng.bitDepth = ImageExportBitDepth::Sixteen;
    masterPng.dither = ImageExportDither::None;
    masterPng.convertToOutputProfile = false;
    masterPng.namingTemplate = QStringLiteral("{document}-master-{bit_depth}");

    ExportProfileData workingTiff = masterPng;
    workingTiff.formatSuffix = QStringLiteral("tiff");
    workingTiff.namingTemplate = QStringLiteral("{document}-working-{bit_depth}");

    ExportProfileData alphaTga = webPng;
    alphaTga.formatSuffix = QStringLiteral("tga");
    alphaTga.convertToOutputProfile = false;
    alphaTga.output.embedProfile = false;
    alphaTga.namingTemplate = QStringLiteral("{document}-alpha");

    return {
        builtIn(QStringLiteral("Web PNG — sRGB 8-bit"),
                QStringLiteral("Web"),
                {QStringLiteral("png"), QStringLiteral("srgb"),
                 QStringLiteral("alpha")}, webPng),
        builtIn(QStringLiteral("Web JPEG — sRGB 8-bit"),
                QStringLiteral("Web"),
                {QStringLiteral("jpeg"), QStringLiteral("srgb"),
                 QStringLiteral("flattened")}, webJpeg),
        builtIn(QStringLiteral("PNG Master — Working 16-bit"),
                QStringLiteral("Master"),
                {QStringLiteral("png"), QStringLiteral("16-bit"),
                 QStringLiteral("working")}, masterPng),
        builtIn(QStringLiteral("TIFF Master — Working 16-bit"),
                QStringLiteral("Master"),
                {QStringLiteral("tiff"), QStringLiteral("16-bit"),
                 QStringLiteral("working")}, workingTiff),
        builtIn(QStringLiteral("TGA Alpha — Working 8-bit"),
                QStringLiteral("Interchange"),
                {QStringLiteral("tga"), QStringLiteral("alpha"),
                 QStringLiteral("working")}, alphaTga)
    };
}

QString ExportProfileStore::storageDirectory()
{
    return PresetStore::storageDirectory(PresetKind::ExportProfile);
}

QVector<ExportProfile> ExportProfileStore::profiles(QStringList *warnings)
{
    if (warnings) warnings->clear();
    QVector<ExportProfile> result = builtInProfiles();
    const qsizetype builtInCount = result.size();
    QSet<QString> seenIds;
    QSet<QString> seenNames;
    for (const ExportProfile &profile : std::as_const(result)) {
        seenIds.insert(profile.metadata.id);
        seenNames.insert(profile.name.toCaseFolded());
    }

    QDir directory(storageDirectory());
    if (!directory.exists()) return result;
    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    int accepted = 0;
    for (const QFileInfo &info : files) {
        if (accepted >= MaximumProfileCount) {
            if (warnings) warnings->push_back(
                QStringLiteral("Additional export profiles were ignored because the profile limit was reached."));
            break;
        }
        PresetEnvelope envelope;
        QString error;
        if (!PresetStore::readFile(info.absoluteFilePath(),
                                   PresetKind::ExportProfile,
                                   MaximumProfileFileBytes,
                                   &envelope, &error)) {
            if (warnings) warnings->push_back(
                QStringLiteral("Ignored invalid export profile %1: %2")
                    .arg(info.fileName(), error));
            continue;
        }
        ExportProfileData data;
        if (!dataFromEnvelope(envelope, &data, &error)) {
            if (warnings) warnings->push_back(
                QStringLiteral("Ignored invalid export profile %1: %2")
                    .arg(info.fileName(), error));
            continue;
        }
        if (envelope.metadata.builtIn
            || seenIds.contains(envelope.metadata.id)
            || seenNames.contains(envelope.metadata.name.toCaseFolded())) {
            if (warnings) warnings->push_back(
                QStringLiteral("Ignored duplicate export profile %1.")
                    .arg(info.fileName()));
            continue;
        }
        ExportProfile profile;
        profile.name = envelope.metadata.name;
        profile.data = std::move(data);
        profile.builtIn = false;
        profile.storagePath = info.absoluteFilePath();
        profile.metadata = envelope.metadata;
        result.push_back(std::move(profile));
        seenIds.insert(envelope.metadata.id);
        seenNames.insert(envelope.metadata.name.toCaseFolded());
        ++accepted;
    }

    std::sort(result.begin() + builtInCount, result.end(),
              [](const ExportProfile &left, const ExportProfile &right) {
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

bool ExportProfileStore::createUserProfile(
    const QString &inputName,
    const ExportProfileData &data,
    const QString &category,
    const QStringList &tags,
    QString *createdId,
    QString *error)
{
    if (error) error->clear();
    const QString name = cleanName(inputName);
    const QVector<ExportProfile> existing = profiles();
    if (name.isEmpty()) {
        setError(error, QStringLiteral("Enter an export profile name."));
        return false;
    }
    if (profileWithName(existing, name).has_value()) {
        setError(error,
                 QStringLiteral("A user export profile with this name already exists."));
        return false;
    }
    if (userProfileCount(existing) >= MaximumProfileCount) {
        setError(error, QStringLiteral("The export profile limit has been reached."));
        return false;
    }
    PresetMetadata metadata;
    metadata.id = PresetStore::newUserId(PresetKind::ExportProfile);
    metadata.name = name;
    metadata.category = category.trimmed();
    metadata.tags = tags;
    if (!writeUserProfile(metadata, data, {}, error)) return false;
    if (createdId) *createdId = metadata.id;
    return true;
}

bool ExportProfileStore::renameUserProfile(const ExportProfile &profile,
                                           const QString &inputName,
                                           QString *error)
{
    const QString name = cleanName(inputName);
    if (profile.builtIn || !managedPath(profile.storagePath) || name.isEmpty()) {
        setError(error,
                 QStringLiteral("The selected export profile or new name is invalid."));
        return false;
    }
    if (profileWithName(profiles(), name, profile.metadata.id).has_value()) {
        setError(error,
                 QStringLiteral("An export profile with that name already exists."));
        return false;
    }
    PresetMetadata metadata = profile.metadata;
    metadata.name = name;
    return writeUserProfile(metadata, profile.data, profile.storagePath, error);
}

bool ExportProfileStore::duplicateUserProfile(const ExportProfile &profile,
                                              const QString &inputName,
                                              QString *createdId,
                                              QString *error)
{
    const QString name = cleanName(inputName);
    const QVector<ExportProfile> existing = profiles();
    if (name.isEmpty() || profileWithName(existing, name).has_value()) {
        setError(error, QStringLiteral("Enter a unique export profile name."));
        return false;
    }
    if (userProfileCount(existing) >= MaximumProfileCount) {
        setError(error, QStringLiteral("The export profile limit has been reached."));
        return false;
    }
    PresetMetadata metadata;
    metadata.id = PresetStore::newUserId(PresetKind::ExportProfile);
    metadata.name = name;
    metadata.category = profile.metadata.category;
    metadata.tags = profile.metadata.tags;
    metadata.favourite = profile.metadata.favourite;
    if (!writeUserProfile(metadata, profile.data, {}, error)) return false;
    if (createdId) *createdId = metadata.id;
    return true;
}

bool ExportProfileStore::updateUserProfile(const ExportProfile &profile,
                                           const ExportProfileData &data,
                                           QString *error)
{
    if (profile.builtIn || !managedPath(profile.storagePath)) {
        setError(error,
                 QStringLiteral("The selected export profile cannot be updated."));
        return false;
    }
    PresetMetadata metadata = profile.metadata;
    metadata.name = profile.name;
    return writeUserProfile(metadata, data, profile.storagePath, error);
}

bool ExportProfileStore::updateMetadata(const ExportProfile &profile,
                                        const QString &category,
                                        const QStringList &tags,
                                        QString *error)
{
    if (profile.builtIn || !managedPath(profile.storagePath)) {
        setError(error,
                 QStringLiteral("The selected export profile details cannot be changed."));
        return false;
    }
    PresetMetadata metadata = profile.metadata;
    metadata.name = profile.name;
    metadata.category = category.trimmed();
    metadata.tags = tags;
    return writeUserProfile(metadata, profile.data, profile.storagePath, error);
}

bool ExportProfileStore::setFavourite(const ExportProfile &profile,
                                      const bool favourite,
                                      QString *error)
{
    if (profile.builtIn) {
        return PresetUsageStore::setFavourite(
            profile.metadata.id, favourite, error);
    }
    if (!managedPath(profile.storagePath)) {
        setError(error,
                 QStringLiteral("The selected export profile cannot be changed."));
        return false;
    }
    PresetMetadata metadata = profile.metadata;
    metadata.name = profile.name;
    metadata.favourite = favourite;
    return writeUserProfile(metadata, profile.data, profile.storagePath, error);
}

bool ExportProfileStore::recordUse(const ExportProfile &profile,
                                   QString *error)
{
    if (profile.builtIn) {
        return PresetUsageStore::recordUse(profile.metadata.id, error);
    }
    if (!managedPath(profile.storagePath)) {
        setError(error,
                 QStringLiteral("The selected export profile cannot be changed."));
        return false;
    }
    PresetMetadata metadata = profile.metadata;
    metadata.name = profile.name;
    metadata.lastUsedUtcMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    if (metadata.useCount < 9007199254740991ULL) ++metadata.useCount;
    return writeUserProfile(metadata, profile.data, profile.storagePath, error);
}

bool ExportProfileStore::removeUserProfile(const ExportProfile &profile,
                                           QString *error)
{
    if (profile.builtIn || !managedPath(profile.storagePath)) {
        setError(error,
                 QStringLiteral("The selected export profile cannot be deleted."));
        return false;
    }
    QFile file(profile.storagePath);
    if (!file.remove()) {
        setError(error, file.errorString());
        return false;
    }
    return true;
}

bool ExportProfileStore::importProfileFile(const QString &sourcePath,
                                           QString *importedId,
                                           QString *error)
{
    PresetEnvelope envelope;
    if (!PresetStore::readFile(sourcePath, PresetKind::ExportProfile,
                               MaximumProfileFileBytes, &envelope, error)) {
        return false;
    }
    ExportProfileData data;
    if (!dataFromEnvelope(envelope, &data, error)) return false;
    if (envelope.metadata.builtIn) {
        envelope.metadata.id = PresetStore::newUserId(PresetKind::ExportProfile);
        envelope.metadata.builtIn = false;
    }
    const QVector<ExportProfile> existing = profiles();
    if (userProfileCount(existing) >= MaximumProfileCount) {
        setError(error, QStringLiteral("The export profile limit has been reached."));
        return false;
    }
    for (const ExportProfile &entry : existing) {
        if (entry.metadata.id == envelope.metadata.id) {
            setError(error,
                     QStringLiteral("This export profile has already been imported."));
            return false;
        }
    }
    const QString importedName = uniqueImportedName(
        existing, envelope.metadata.name);
    if (importedName.isEmpty()) {
        setError(error,
                 QStringLiteral("A unique name could not be assigned to the imported export profile."));
        return false;
    }
    envelope.metadata.name = importedName;
    envelope.metadata.builtIn = false;
    if (!writeUserProfile(envelope.metadata, data, {}, error)) return false;
    if (importedId) *importedId = envelope.metadata.id;
    return true;
}

bool ExportProfileStore::exportProfileFile(
    const ExportProfile &profile,
    const QString &destinationPath,
    QString *error)
{
    PresetMetadata metadata = profile.metadata;
    metadata.name = profile.name;
    PresetEnvelope envelope;
    envelope.kind = PresetKind::ExportProfile;
    envelope.metadata = metadata;
    envelope.payload = payloadFor(profile.data, error);
    if (envelope.payload.isEmpty()) return false;
    return PresetStore::writeFile(destinationPath, envelope,
                                  MaximumProfileFileBytes, error);
}

} // namespace vfx
