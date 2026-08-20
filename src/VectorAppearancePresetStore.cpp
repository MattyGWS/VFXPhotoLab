#include "VectorAppearancePresetStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

#include <algorithm>
#include <optional>
#include <utility>

namespace vfx {
namespace {

constexpr auto LegacyPresetFormat = "vfxphotolab-preset";
constexpr auto LegacyPresetCategory = "vector-appearance";
constexpr quint64 MaximumJsonInteger = 9007199254740991ULL;

QString cleanName(const QString &input)
{
    return input.trimmed().left(PresetStore::MaximumNameLength);
}

QByteArray canonicalAppearancePayload(const VectorAppearance &input)
{
    VectorAppearance appearance = input;
    appearance.normalise();
    bool ok = false;
    const QJsonObject object = appearance.toJson(&ok);
    return ok ? QJsonDocument(object).toJson(QJsonDocument::Compact)
              : QByteArray();
}

PresetMetadata metadataForLegacy(const QString &name,
                                 const VectorAppearance &appearance)
{
    PresetMetadata metadata;
    metadata.id = PresetStore::stableLegacyId(
        PresetKind::VectorAppearance, QStringLiteral("vector-appearance"),
        name, canonicalAppearancePayload(appearance));
    metadata.name = name;
    metadata.category = QStringLiteral("Vector Appearance");
    return metadata;
}

QJsonObject appearancePayload(const VectorAppearance &input, bool *ok)
{
    VectorAppearance appearance = input;
    appearance.normalise();
    bool appearanceOk = false;
    const QJsonObject appearanceJson = appearance.toJson(&appearanceOk);
    if (ok) *ok = appearanceOk && appearance.isSafe();
    if (!appearanceOk || !appearance.isSafe()) return {};
    QJsonObject payload;
    payload.insert(QStringLiteral("appearance"), appearanceJson);
    return payload;
}

bool appearanceFromEnvelope(const PresetEnvelope &envelope,
                            VectorAppearance *appearance,
                            QString *error)
{
    if (!appearance) {
        if (error) *error = QStringLiteral("The vector appearance destination is missing");
        return false;
    }
    if (envelope.kind != PresetKind::VectorAppearance
        || !envelope.payload.value(QStringLiteral("appearance")).isObject()) {
        if (error) *error = QStringLiteral("The preset does not contain a user vector appearance");
        return false;
    }
    bool ok = false;
    VectorAppearance decoded = VectorAppearance::fromJson(
        envelope.payload.value(QStringLiteral("appearance")).toObject(), &ok);
    decoded.normalise();
    if (!ok || !decoded.isSafe()) {
        if (error) *error = QStringLiteral("The vector appearance preset payload is invalid");
        return false;
    }
    *appearance = std::move(decoded);
    return true;
}

bool managedPath(const QString &path)
{
    return PresetStore::pathIsManagedByDirectory(
        path, VectorAppearancePresetStore::storageDirectory());
}

std::optional<VectorAppearancePreset> presetWithName(
    const QVector<VectorAppearancePreset> &presets,
    const QString &name,
    const QString &exceptId = {})
{
    const QString folded = cleanName(name).toCaseFolded();
    for (const VectorAppearancePreset &preset : presets) {
        if (!exceptId.isEmpty() && preset.metadata.id == exceptId) continue;
        if (preset.name.toCaseFolded() == folded) return preset;
    }
    return std::nullopt;
}

QString uniqueImportedName(const QVector<VectorAppearancePreset> &presets,
                           const QString &inputName)
{
    const QString requested = cleanName(inputName);
    if (!presetWithName(presets, requested).has_value()) return requested;

    for (int suffix = 2;
         suffix < VectorAppearancePresetStore::MaximumPresetCount + 2;
         ++suffix) {
        const QString suffixText = QStringLiteral(" (%1)").arg(suffix);
        const qsizetype baseLimit = std::max<qsizetype>(
            1, PresetStore::MaximumNameLength - suffixText.size());
        const QString candidate = requested.left(baseLimit).trimmed()
            + suffixText;
        if (!presetWithName(presets, candidate).has_value()) return candidate;
    }
    return {};
}

bool readLegacyPreset(const QFileInfo &info,
                      VectorAppearancePreset *preset,
                      QString *error)
{
    if (error) error->clear();
    if (!preset || info.size() <= 0
        || info.size() > VectorAppearancePresetStore::MaximumPresetBytes) {
        if (error) *error = QStringLiteral("The legacy vector appearance preset exceeds the safety limit");
        return false;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Could not read the vector appearance preset");
        return false;
    }
    const QByteArray bytes = file.read(
        VectorAppearancePresetStore::MaximumPresetBytes + 1);
    if (bytes.size() != info.size()) {
        if (error) *error = QStringLiteral("Could not read the complete vector appearance preset");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    const QJsonObject root = document.object();
    bool appearanceOk = false;
    VectorAppearance appearance = VectorAppearance::fromJson(
        root.value(QStringLiteral("appearance")).toObject(), &appearanceOk);
    appearance.normalise();
    const QString name = cleanName(root.value(QStringLiteral("name")).toString());
    const bool valid = parseError.error == QJsonParseError::NoError
        && document.isObject()
        && root.value(QStringLiteral("format")).toString()
            == QString::fromLatin1(LegacyPresetFormat)
        && root.value(QStringLiteral("version")).toInt(-1)
            == VectorAppearancePresetStore::LegacyFileVersion
        && root.value(QStringLiteral("category")).toString()
            == QString::fromLatin1(LegacyPresetCategory)
        && !name.isEmpty() && appearanceOk && appearance.isSafe();
    if (!valid) {
        if (error) *error = QStringLiteral("The legacy vector appearance preset is invalid");
        return false;
    }
    *preset = {name, appearance, info.absoluteFilePath(),
               metadataForLegacy(name, appearance)};
    return true;
}

bool writeUserPreset(const PresetMetadata &inputMetadata,
                     const VectorAppearance &inputAppearance,
                     const QString &oldPath,
                     QString *error)
{
    if (error) error->clear();
    bool payloadOk = false;
    const QJsonObject payload = appearancePayload(inputAppearance, &payloadOk);
    if (!payloadOk) {
        if (error) *error = QStringLiteral("The vector appearance could not be serialised");
        return false;
    }

    PresetMetadata metadata = inputMetadata;
    metadata.name = cleanName(metadata.name);
    metadata.category = metadata.category.trimmed();
    if (metadata.category.isEmpty()) {
        metadata.category = QStringLiteral("Vector Appearance");
    }
    metadata.builtIn = false;
    const qint64 now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    if (metadata.createdUtcMs <= 0) metadata.createdUtcMs = now;
    metadata.modifiedUtcMs = now;

    PresetEnvelope envelope;
    envelope.kind = PresetKind::VectorAppearance;
    envelope.metadata = metadata;
    envelope.payload = payload;
    const QString path = PresetStore::filePathForId(
        VectorAppearancePresetStore::storageDirectory(), metadata.id);
    if (!PresetStore::writeFile(path, envelope,
                                VectorAppearancePresetStore::MaximumPresetBytes,
                                error)) {
        return false;
    }
    if (!oldPath.isEmpty()
        && QFileInfo(oldPath).absoluteFilePath()
            != QFileInfo(path).absoluteFilePath()) {
        QFile oldFile(oldPath);
        if (managedPath(oldPath) && oldFile.exists() && !oldFile.remove()) {
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

} // namespace

QString VectorAppearancePresetStore::storageDirectory()
{
    return PresetStore::storageDirectory(PresetKind::VectorAppearance);
}

QVector<VectorAppearancePreset> VectorAppearancePresetStore::presets(
    QStringList *warnings)
{
    QVector<VectorAppearancePreset> result;
    QSet<QString> loadedIds;
    QSet<QString> loadedNames;
    QDir directory(storageDirectory());
    if (!directory.exists()) return result;

    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files | QDir::Readable,
        QDir::Name | QDir::IgnoreCase);
    if (files.size() > MaximumPresetCount && warnings) {
        warnings->push_back(QStringLiteral(
            "Only the first %1 vector appearance presets were loaded")
                                .arg(MaximumPresetCount));
    }

    const qsizetype count = std::min<qsizetype>(files.size(), MaximumPresetCount);
    for (qsizetype index = 0; index < count; ++index) {
        const QFileInfo &info = files.at(index);
        if (info.size() <= 0 || info.size() > MaximumPresetBytes) {
            if (warnings) {
                warnings->push_back(QStringLiteral("Ignored oversized preset %1")
                                        .arg(info.fileName()));
            }
            continue;
        }
        VectorAppearancePreset entry;
        QString loadError;

        PresetEnvelope envelope;
        if (PresetStore::readFile(info.absoluteFilePath(),
                                  PresetKind::VectorAppearance,
                                  MaximumPresetBytes, &envelope,
                                  &loadError)) {
            VectorAppearance appearance;
            if (envelope.metadata.builtIn
                || !appearanceFromEnvelope(envelope, &appearance, &loadError)) {
                if (warnings) {
                    warnings->push_back(QStringLiteral("Ignored invalid preset %1: %2")
                                            .arg(info.fileName(), loadError));
                }
                continue;
            }
            entry = {envelope.metadata.name, appearance,
                     info.absoluteFilePath(), envelope.metadata};
        } else if (!readLegacyPreset(info, &entry, &loadError)) {
            if (warnings) {
                warnings->push_back(QStringLiteral("Ignored invalid preset %1: %2")
                                        .arg(info.fileName(), loadError));
            }
            continue;
        }

        const QString foldedName = entry.name.toCaseFolded();
        if (loadedIds.contains(entry.metadata.id)
            || loadedNames.contains(foldedName)) {
            if (warnings) {
                warnings->push_back(QStringLiteral(
                    "Ignored duplicate vector appearance preset %1")
                                        .arg(info.fileName()));
            }
            continue;
        }
        loadedIds.insert(entry.metadata.id);
        loadedNames.insert(foldedName);
        result.push_back(std::move(entry));
    }

    std::sort(result.begin(), result.end(),
              [](const VectorAppearancePreset &left,
                 const VectorAppearancePreset &right) {
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

bool VectorAppearancePresetStore::saveUserPreset(
    const QString &inputName,
    const VectorAppearance &inputAppearance,
    QString *error)
{
    if (error) error->clear();
    const QString name = cleanName(inputName);
    VectorAppearance appearance = inputAppearance;
    appearance.normalise();
    if (name.isEmpty()) {
        if (error) *error = QStringLiteral("Enter a preset name");
        return false;
    }
    if (!appearance.isSafe()) {
        if (error) *error = QStringLiteral("The vector appearance is not valid");
        return false;
    }

    const QVector<VectorAppearancePreset> existing = presets();
    const auto match = presetWithName(existing, name);
    if (!match.has_value() && existing.size() >= MaximumPresetCount) {
        if (error) *error = QStringLiteral("The vector appearance preset limit has been reached");
        return false;
    }
    PresetMetadata metadata = match.has_value()
        ? match->metadata : PresetMetadata();
    if (metadata.id.isEmpty()) {
        metadata.id = PresetStore::newUserId(PresetKind::VectorAppearance);
    }
    metadata.name = name;
    return writeUserPreset(metadata, appearance,
                           match.has_value() ? match->storagePath : QString(),
                           error);
}

bool VectorAppearancePresetStore::createUserPreset(
    const QString &inputName,
    const VectorAppearance &inputAppearance,
    const QString &category,
    const QStringList &tags,
    QString *createdId,
    QString *error)
{
    if (error) error->clear();
    const QString name = cleanName(inputName);
    VectorAppearance appearance = inputAppearance;
    appearance.normalise();
    if (name.isEmpty()) {
        if (error) *error = QStringLiteral("Enter a preset name");
        return false;
    }
    if (!appearance.isSafe()) {
        if (error) *error = QStringLiteral("The vector appearance is not valid");
        return false;
    }
    const QVector<VectorAppearancePreset> existing = presets();
    if (presetWithName(existing, name).has_value()) {
        if (error) *error = QStringLiteral("A preset with that name already exists");
        return false;
    }
    if (existing.size() >= MaximumPresetCount) {
        if (error) *error = QStringLiteral("The vector appearance preset limit has been reached");
        return false;
    }
    PresetMetadata metadata;
    metadata.id = PresetStore::newUserId(PresetKind::VectorAppearance);
    metadata.name = name;
    metadata.category = category.trimmed();
    metadata.tags = tags;
    if (!writeUserPreset(metadata, appearance, {}, error)) return false;
    if (createdId) *createdId = metadata.id;
    return true;
}

bool VectorAppearancePresetStore::renameUserPreset(
    const VectorAppearancePreset &preset,
    const QString &inputName,
    QString *error)
{
    if (error) error->clear();
    const QString name = cleanName(inputName);
    if (!managedPath(preset.storagePath) || !preset.appearance.isSafe()
        || name.isEmpty()) {
        if (error) *error = QStringLiteral("The selected preset or new name is not valid");
        return false;
    }
    if (presetWithName(presets(), name, preset.metadata.id).has_value()) {
        if (error) *error = QStringLiteral("A preset with that name already exists");
        return false;
    }
    PresetMetadata metadata = preset.metadata;
    if (metadata.id.isEmpty()) metadata = metadataForLegacy(preset.name, preset.appearance);
    metadata.name = name;
    return writeUserPreset(metadata, preset.appearance, preset.storagePath, error);
}

bool VectorAppearancePresetStore::duplicateUserPreset(
    const VectorAppearancePreset &preset,
    const QString &inputName,
    QString *error)
{
    if (error) error->clear();
    const QString name = cleanName(inputName);
    if (name.isEmpty() || presetWithName(presets(), name).has_value()) {
        if (error) *error = QStringLiteral("Enter a unique preset name");
        return false;
    }
    PresetMetadata metadata;
    metadata.id = PresetStore::newUserId(PresetKind::VectorAppearance);
    metadata.name = name;
    metadata.category = preset.metadata.category;
    metadata.tags = preset.metadata.tags;
    metadata.favourite = preset.metadata.favourite;
    return writeUserPreset(metadata, preset.appearance, {}, error);
}

bool VectorAppearancePresetStore::updateUserPreset(
    const VectorAppearancePreset &preset,
    const VectorAppearance &appearance,
    QString *error)
{
    if (!managedPath(preset.storagePath) || !appearance.isSafe()) {
        if (error) *error = QStringLiteral("The selected preset cannot be updated");
        return false;
    }
    PresetMetadata metadata = preset.metadata;
    if (metadata.id.isEmpty()) metadata = metadataForLegacy(preset.name, preset.appearance);
    metadata.name = preset.name;
    return writeUserPreset(metadata, appearance, preset.storagePath, error);
}

bool VectorAppearancePresetStore::updateMetadata(
    const VectorAppearancePreset &preset,
    const QString &category,
    const QStringList &tags,
    QString *error)
{
    if (!managedPath(preset.storagePath)) {
        if (error) *error = QStringLiteral("The selected preset details cannot be changed");
        return false;
    }
    PresetMetadata metadata = preset.metadata;
    if (metadata.id.isEmpty()) {
        metadata = metadataForLegacy(preset.name, preset.appearance);
    }
    metadata.name = preset.name;
    metadata.category = category.trimmed();
    metadata.tags = tags;
    return writeUserPreset(metadata, preset.appearance, preset.storagePath, error);
}

bool VectorAppearancePresetStore::setFavourite(
    const VectorAppearancePreset &preset,
    const bool favourite,
    QString *error)
{
    if (!managedPath(preset.storagePath)) {
        if (error) *error = QStringLiteral("The selected preset cannot be changed");
        return false;
    }
    PresetMetadata metadata = preset.metadata;
    if (metadata.id.isEmpty()) metadata = metadataForLegacy(preset.name, preset.appearance);
    metadata.name = preset.name;
    metadata.favourite = favourite;
    return writeUserPreset(metadata, preset.appearance, preset.storagePath, error);
}

bool VectorAppearancePresetStore::recordUse(
    const VectorAppearancePreset &preset,
    QString *error)
{
    if (!managedPath(preset.storagePath)) {
        if (error) *error = QStringLiteral("The selected preset cannot be changed");
        return false;
    }
    PresetMetadata metadata = preset.metadata;
    if (metadata.id.isEmpty()) metadata = metadataForLegacy(preset.name, preset.appearance);
    metadata.name = preset.name;
    metadata.lastUsedUtcMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    if (metadata.useCount < MaximumJsonInteger) ++metadata.useCount;
    return writeUserPreset(metadata, preset.appearance, preset.storagePath, error);
}

bool VectorAppearancePresetStore::importPresetFile(
    const QString &sourcePath,
    QString *importedName,
    QString *error)
{
    PresetEnvelope envelope;
    if (!PresetStore::readFile(sourcePath, PresetKind::VectorAppearance,
                               MaximumPresetBytes, &envelope, error)) {
        return false;
    }
    VectorAppearance appearance;
    if (!appearanceFromEnvelope(envelope, &appearance, error)) return false;
    if (envelope.metadata.builtIn) {
        envelope.metadata.id = PresetStore::newUserId(PresetKind::VectorAppearance);
        envelope.metadata.builtIn = false;
    }
    const QVector<VectorAppearancePreset> existing = presets();
    for (const VectorAppearancePreset &entry : existing) {
        if (entry.metadata.id == envelope.metadata.id) {
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
    if (!writeUserPreset(envelope.metadata, appearance, {}, error)) return false;
    if (importedName) *importedName = name;
    return true;
}

bool VectorAppearancePresetStore::exportPresetFile(
    const VectorAppearancePreset &preset,
    const QString &destinationPath,
    QString *error)
{
    if (destinationPath.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Choose a destination preset file");
        return false;
    }
    bool payloadOk = false;
    const QJsonObject payload = appearancePayload(preset.appearance, &payloadOk);
    if (!payloadOk) {
        if (error) *error = QStringLiteral("The vector appearance could not be serialised");
        return false;
    }
    PresetEnvelope envelope;
    envelope.kind = PresetKind::VectorAppearance;
    envelope.metadata = preset.metadata;
    if (envelope.metadata.id.isEmpty()) {
        envelope.metadata = metadataForLegacy(preset.name, preset.appearance);
    }
    envelope.metadata.name = preset.name;
    envelope.metadata.category = envelope.metadata.category.trimmed();
    if (envelope.metadata.category.isEmpty()) {
        envelope.metadata.category = QStringLiteral("Vector Appearance");
    }
    envelope.payload = payload;
    return PresetStore::writeFile(destinationPath, envelope,
                                  MaximumPresetBytes, error);
}

bool VectorAppearancePresetStore::removeUserPreset(
    const VectorAppearancePreset &preset,
    QString *error)
{
    if (error) error->clear();
    if (!managedPath(preset.storagePath)) {
        if (error) *error = QStringLiteral("The selected preset is not valid");
        return false;
    }
    QFile file(preset.storagePath);
    if (!file.remove()) {
        if (error) {
            *error = QStringLiteral("Could not remove the preset: %1")
                         .arg(file.errorString());
        }
        return false;
    }
    return true;
}

} // namespace vfx
