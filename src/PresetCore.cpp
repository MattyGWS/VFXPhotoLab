#include "PresetCore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace vfx {
namespace {

constexpr auto PresetFormat = "vfxphotolab-preset";
constexpr quint64 MaximumJsonInteger = 9007199254740991ULL;

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) *errorMessage = message;
}

QString cleanToken(QString input, const int maximumLength)
{
    input = input.trimmed();
    QString result;
    result.reserve(std::min<qsizetype>(input.size(), maximumLength));
    for (const QChar character : std::as_const(input)) {
        if (result.size() >= maximumLength) break;
        ushort code = character.unicode();
        const bool asciiLetter = (code >= 'a' && code <= 'z')
            || (code >= 'A' && code <= 'Z');
        const bool asciiDigit = code >= '0' && code <= '9';
        if (asciiLetter || asciiDigit || code == '-' || code == '_'
            || code == '.' || code == ':') {
            if (code >= 'A' && code <= 'Z') code += 'a' - 'A';
            result += QChar(code);
        } else if (!result.isEmpty() && !result.endsWith(QLatin1Char('-'))) {
            result += QLatin1Char('-');
        }
    }
    while (result.endsWith(QLatin1Char('-'))) result.chop(1);
    return result;
}

bool identifierIsSafe(const QString &id)
{
    if (id.isEmpty() || id.size() > 160) return false;
    for (const QChar character : id) {
        const ushort code = character.unicode();
        const bool lowerAsciiLetter = code >= 'a' && code <= 'z';
        const bool asciiDigit = code >= '0' && code <= '9';
        if (!lowerAsciiLetter && !asciiDigit
            && code != '-' && code != '_'
            && code != '.' && code != ':') {
            return false;
        }
    }
    return true;
}

QJsonObject metadataToJson(const PresetMetadata &metadata)
{
    QJsonArray tags;
    for (const QString &tag : metadata.tags) tags.push_back(tag);

    QJsonObject object;
    object.insert(QStringLiteral("id"), metadata.id);
    object.insert(QStringLiteral("name"), metadata.name);
    object.insert(QStringLiteral("category"), metadata.category);
    object.insert(QStringLiteral("tags"), tags);
    object.insert(QStringLiteral("favourite"), metadata.favourite);
    object.insert(QStringLiteral("builtIn"), metadata.builtIn);
    object.insert(QStringLiteral("createdUtcMs"), static_cast<double>(metadata.createdUtcMs));
    object.insert(QStringLiteral("modifiedUtcMs"), static_cast<double>(metadata.modifiedUtcMs));
    object.insert(QStringLiteral("lastUsedUtcMs"), static_cast<double>(metadata.lastUsedUtcMs));
    object.insert(QStringLiteral("useCount"), static_cast<double>(metadata.useCount));
    return object;
}

bool jsonInteger(const QJsonValue &value,
                 const quint64 maximum,
                 quint64 *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble(-1.0);
    if (!std::isfinite(number) || number < 0.0
        || number > static_cast<double>(maximum)
        || std::floor(number) != number) {
        return false;
    }
    if (result) *result = static_cast<quint64>(number);
    return true;
}

bool metadataFromJson(const QJsonObject &object,
                      PresetMetadata *metadata,
                      QString *errorMessage)
{
    if (!metadata) {
        setError(errorMessage, QStringLiteral("The preset metadata destination is missing."));
        return false;
    }

    const QJsonValue idValue = object.value(QStringLiteral("id"));
    const QJsonValue nameValue = object.value(QStringLiteral("name"));
    const QJsonValue categoryValue = object.value(QStringLiteral("category"));
    const QJsonValue favouriteValue = object.value(QStringLiteral("favourite"));
    const QJsonValue builtInValue = object.value(QStringLiteral("builtIn"));
    if (!idValue.isString() || !nameValue.isString()
        || (!categoryValue.isUndefined() && !categoryValue.isString())
        || (!favouriteValue.isUndefined() && !favouriteValue.isBool())
        || (!builtInValue.isUndefined() && !builtInValue.isBool())) {
        setError(errorMessage, QStringLiteral("The preset metadata contains invalid field types."));
        return false;
    }

    PresetMetadata result;
    result.id = idValue.toString();
    result.name = nameValue.toString();
    result.category = categoryValue.toString();
    result.favourite = favouriteValue.toBool(false);
    result.builtIn = builtInValue.toBool(false);

    const QJsonValue tagsValue = object.value(QStringLiteral("tags"));
    if (!tagsValue.isUndefined() && !tagsValue.isArray()) {
        setError(errorMessage, QStringLiteral("The preset tag list is damaged."));
        return false;
    }
    const QJsonArray tags = tagsValue.toArray();
    if (tags.size() > PresetStore::MaximumTagCount) {
        setError(errorMessage, QStringLiteral("The preset contains too many tags."));
        return false;
    }
    for (const QJsonValue &value : tags) {
        if (!value.isString()) {
            setError(errorMessage, QStringLiteral("The preset contains an invalid tag."));
            return false;
        }
        result.tags.push_back(value.toString());
    }

    quint64 created = 0;
    quint64 modified = 0;
    quint64 lastUsed = 0;
    quint64 useCount = 0;
    const auto readOptional = [&](const QString &key, quint64 *target) {
        const QJsonValue value = object.value(key);
        return value.isUndefined() || jsonInteger(value, MaximumJsonInteger, target);
    };
    if (!readOptional(QStringLiteral("createdUtcMs"), &created)
        || !readOptional(QStringLiteral("modifiedUtcMs"), &modified)
        || !readOptional(QStringLiteral("lastUsedUtcMs"), &lastUsed)
        || !readOptional(QStringLiteral("useCount"), &useCount)
        || created > static_cast<quint64>(std::numeric_limits<qint64>::max())
        || modified > static_cast<quint64>(std::numeric_limits<qint64>::max())
        || lastUsed > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        setError(errorMessage, QStringLiteral("The preset usage metadata is damaged."));
        return false;
    }
    result.createdUtcMs = static_cast<qint64>(created);
    result.modifiedUtcMs = static_cast<qint64>(modified);
    result.lastUsedUtcMs = static_cast<qint64>(lastUsed);
    result.useCount = useCount;

    QString validationError;
    if (!result.isValid(&validationError)) {
        setError(errorMessage, validationError);
        return false;
    }
    *metadata = std::move(result);
    return true;
}

QString hashSuffix(const QByteArray &bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
            .toHex().left(20));
}

constexpr auto UsageFormat = "vfxphotolab-preset-usage";
constexpr int UsageVersion = 1;
constexpr qint64 MaximumUsageFileBytes = 1024 * 1024;
constexpr int MaximumUsageEntries = 4096;

QString usageFilePath()
{
    return QDir(PresetStore::storageRoot()).filePath(
        QStringLiteral("usage.vfxpreset.json"));
}

QJsonObject readUsageEntries()
{
    QFile file(usageFilePath());
    if (!file.exists()) return {};
    const QFileInfo info(file);
    if (!info.isFile() || !info.isReadable() || info.size() <= 0
        || info.size() > MaximumUsageFileBytes
        || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        file.read(MaximumUsageFileBytes + 1), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString()
            != QString::fromLatin1(UsageFormat)
        || root.value(QStringLiteral("version")).toInt(-1) != UsageVersion
        || !root.value(QStringLiteral("entries")).isObject()) {
        return {};
    }
    const QJsonObject entries = root.value(QStringLiteral("entries")).toObject();
    return entries.size() <= MaximumUsageEntries ? entries : QJsonObject();
}

bool writeUsageEntries(const QJsonObject &entries, QString *errorMessage)
{
    if (entries.size() > MaximumUsageEntries) {
        setError(errorMessage, QStringLiteral("The preset usage store is full."));
        return false;
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), QString::fromLatin1(UsageFormat));
    root.insert(QStringLiteral("version"), UsageVersion);
    root.insert(QStringLiteral("entries"), entries);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (bytes.size() > MaximumUsageFileBytes) {
        setError(errorMessage, QStringLiteral("The preset usage store exceeds its safety limit."));
        return false;
    }
    const QFileInfo info(usageFilePath());
    QDir directory;
    if (!directory.mkpath(info.absolutePath())) {
        setError(errorMessage, QStringLiteral("Could not create the preset usage directory."));
        return false;
    }
    QSaveFile file(info.absoluteFilePath());
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(bytes) != bytes.size() || !file.commit()) {
        setError(errorMessage,
                 QStringLiteral("Could not save preset usage data: %1")
                     .arg(file.errorString()));
        return false;
    }
    return true;
}

} // namespace

PresetUsageMetadata PresetUsageStore::usageFor(const QString &presetId)
{
    PresetUsageMetadata usage;
    if (!identifierIsSafe(presetId)) return usage;
    const QJsonValue value = readUsageEntries().value(presetId);
    if (!value.isObject()) return usage;
    const QJsonObject object = value.toObject();
    const QJsonValue favourite = object.value(QStringLiteral("favourite"));
    quint64 lastUsed = 0;
    quint64 useCount = 0;
    const auto readOptional = [&object](const QString &key,
                                        quint64 *target) {
        const QJsonValue field = object.value(key);
        return field.isUndefined()
            || jsonInteger(field, MaximumJsonInteger, target);
    };
    if ((!favourite.isUndefined() && !favourite.isBool())
        || !readOptional(QStringLiteral("lastUsedUtcMs"), &lastUsed)
        || !readOptional(QStringLiteral("useCount"), &useCount)
        || lastUsed > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        return {};
    }
    usage.favourite = favourite.toBool(false);
    usage.lastUsedUtcMs = static_cast<qint64>(lastUsed);
    usage.useCount = useCount;
    return usage;
}

bool PresetUsageStore::setFavourite(const QString &presetId,
                                     const bool favourite,
                                     QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (!identifierIsSafe(presetId)) {
        setError(errorMessage, QStringLiteral("The preset identifier is invalid."));
        return false;
    }
    QJsonObject entries = readUsageEntries();
    QJsonObject entry = entries.value(presetId).toObject();
    entry.insert(QStringLiteral("favourite"), favourite);
    if (!entry.contains(QStringLiteral("lastUsedUtcMs"))) {
        entry.insert(QStringLiteral("lastUsedUtcMs"), 0);
    }
    if (!entry.contains(QStringLiteral("useCount"))) {
        entry.insert(QStringLiteral("useCount"), 0);
    }
    entries.insert(presetId, entry);
    return writeUsageEntries(entries, errorMessage);
}

bool PresetUsageStore::recordUse(const QString &presetId,
                                  QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (!identifierIsSafe(presetId)) {
        setError(errorMessage, QStringLiteral("The preset identifier is invalid."));
        return false;
    }
    QJsonObject entries = readUsageEntries();
    QJsonObject entry = entries.value(presetId).toObject();
    quint64 useCount = 0;
    if (!jsonInteger(entry.value(QStringLiteral("useCount")),
                     MaximumJsonInteger, &useCount)) {
        useCount = 0;
    }
    if (useCount < MaximumJsonInteger) ++useCount;
    entry.insert(QStringLiteral("favourite"),
                 entry.value(QStringLiteral("favourite")).toBool(false));
    entry.insert(QStringLiteral("lastUsedUtcMs"),
                 static_cast<double>(
                     QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()));
    entry.insert(QStringLiteral("useCount"), static_cast<double>(useCount));
    entries.insert(presetId, entry);
    return writeUsageEntries(entries, errorMessage);
}

bool PresetMetadata::isValid(QString *errorMessage) const
{
    if (!identifierIsSafe(id)) {
        setError(errorMessage, QStringLiteral("The preset identifier is missing or invalid."));
        return false;
    }
    const QString cleanName = name.trimmed();
    if (cleanName.isEmpty() || cleanName.size() > PresetStore::MaximumNameLength) {
        setError(errorMessage, QStringLiteral("The preset name is missing or too long."));
        return false;
    }
    if (category.size() > PresetStore::MaximumCategoryLength) {
        setError(errorMessage, QStringLiteral("The preset category is too long."));
        return false;
    }
    if (tags.size() > PresetStore::MaximumTagCount) {
        setError(errorMessage, QStringLiteral("The preset contains too many tags."));
        return false;
    }
    for (const QString &tag : tags) {
        if (tag.trimmed().isEmpty()
            || tag.size() > PresetStore::MaximumTagLength) {
            setError(errorMessage, QStringLiteral("The preset contains an invalid tag."));
            return false;
        }
    }
    if (createdUtcMs < 0 || modifiedUtcMs < 0 || lastUsedUtcMs < 0
        || useCount > MaximumJsonInteger) {
        setError(errorMessage, QStringLiteral("The preset usage metadata is invalid."));
        return false;
    }
    return true;
}

bool PresetEnvelope::isValid(QString *errorMessage) const
{
    if (!metadata.isValid(errorMessage)) return false;
    if (payload.isEmpty()) {
        setError(errorMessage, QStringLiteral("The preset payload is empty."));
        return false;
    }
    return true;
}

QString PresetStore::kindName(const PresetKind kind)
{
    switch (kind) {
    case PresetKind::Adjustment: return QStringLiteral("adjustment");
    case PresetKind::VectorAppearance: return QStringLiteral("vector-appearance");
    case PresetKind::ExportProfile: return QStringLiteral("export-profile");
    }
    return QStringLiteral("unknown");
}

bool PresetStore::kindFromName(const QString &name, PresetKind *kind)
{
    const QString folded = name.trimmed().toCaseFolded();
    if (folded == QStringLiteral("adjustment")) {
        if (kind) *kind = PresetKind::Adjustment;
        return true;
    }
    if (folded == QStringLiteral("vector-appearance")) {
        if (kind) *kind = PresetKind::VectorAppearance;
        return true;
    }
    if (folded == QStringLiteral("export-profile")) {
        if (kind) *kind = PresetKind::ExportProfile;
        return true;
    }
    return false;
}

QString PresetStore::storageRoot()
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.vfxphotolab");
    return QDir(root).filePath(QStringLiteral("presets"));
}

QString PresetStore::storageDirectory(const PresetKind kind,
                                      const QString &subtype)
{
    QDir directory(storageRoot());
    QString path = directory.filePath(kindName(kind));
    const QString cleanedSubtype = cleanToken(subtype, 80);
    if (!cleanedSubtype.isEmpty()) path = QDir(path).filePath(cleanedSubtype);
    return path;
}

QString PresetStore::stableBuiltInId(const PresetKind kind,
                                     const QString &scope,
                                     const QString &name)
{
    const QByteArray identity = kindName(kind).toUtf8() + '\n'
        + scope.trimmed().toCaseFolded().toUtf8() + '\n'
        + name.trimmed().toCaseFolded().toUtf8();
    QString readable = cleanToken(scope + QLatin1Char('-') + name, 72);
    if (readable.isEmpty()) readable = QStringLiteral("preset");
    return QStringLiteral("builtin.%1.%2.%3")
        .arg(kindName(kind), readable, hashSuffix(identity));
}

QString PresetStore::stableLegacyId(const PresetKind kind,
                                    const QString &scope,
                                    const QString &name,
                                    const QByteArray &canonicalPayload)
{
    const QByteArray identity = kindName(kind).toUtf8() + '\n'
        + scope.trimmed().toCaseFolded().toUtf8() + '\n'
        + name.trimmed().toCaseFolded().toUtf8() + '\n'
        + canonicalPayload;
    return QStringLiteral("legacy.%1.%2")
        .arg(kindName(kind), hashSuffix(identity));
}

QString PresetStore::newUserId(const PresetKind kind)
{
    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return QStringLiteral("user.%1.%2").arg(kindName(kind), uuid);
}

QByteArray PresetStore::serialise(const PresetEnvelope &input,
                                  QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    PresetEnvelope envelope = input;
    envelope.metadata.name = envelope.metadata.name.trimmed();
    envelope.metadata.category = envelope.metadata.category.trimmed();
    for (QString &tag : envelope.metadata.tags) tag = tag.trimmed();
    envelope.metadata.tags.removeDuplicates();

    QString validationError;
    if (!envelope.isValid(&validationError)) {
        setError(errorMessage, validationError);
        return {};
    }

    QJsonObject root;
    root.insert(QStringLiteral("format"), QString::fromLatin1(PresetFormat));
    root.insert(QStringLiteral("version"), CurrentSchemaVersion);
    root.insert(QStringLiteral("kind"), kindName(envelope.kind));
    root.insert(QStringLiteral("metadata"), metadataToJson(envelope.metadata));
    root.insert(QStringLiteral("payload"), envelope.payload);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool PresetStore::parse(const QByteArray &bytes,
                        const PresetKind expectedKind,
                        PresetEnvelope *envelope,
                        QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (!envelope) {
        setError(errorMessage, QStringLiteral("The preset destination is missing."));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(errorMessage, QStringLiteral("The preset file is not valid JSON."));
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString()
            != QString::fromLatin1(PresetFormat)
        || root.value(QStringLiteral("version")).toInt(-1)
            != CurrentSchemaVersion) {
        setError(errorMessage, QStringLiteral("The preset file uses an unsupported format or version."));
        return false;
    }
    PresetKind actualKind;
    if (!kindFromName(root.value(QStringLiteral("kind")).toString(),
                      &actualKind)
        || actualKind != expectedKind) {
        setError(errorMessage, QStringLiteral("The preset file is for a different feature type."));
        return false;
    }
    if (!root.value(QStringLiteral("metadata")).isObject()
        || !root.value(QStringLiteral("payload")).isObject()) {
        setError(errorMessage, QStringLiteral("The preset metadata or payload is damaged."));
        return false;
    }

    PresetEnvelope result;
    result.kind = actualKind;
    if (!metadataFromJson(root.value(QStringLiteral("metadata")).toObject(),
                          &result.metadata, errorMessage)) {
        return false;
    }
    result.payload = root.value(QStringLiteral("payload")).toObject();
    QString validationError;
    if (!result.isValid(&validationError)) {
        setError(errorMessage, validationError);
        return false;
    }
    *envelope = std::move(result);
    return true;
}

bool PresetStore::readFile(const QString &path,
                           const PresetKind expectedKind,
                           const qint64 maximumBytes,
                           PresetEnvelope *envelope,
                           QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable() || info.size() <= 0
        || maximumBytes <= 0 || info.size() > maximumBytes) {
        setError(errorMessage, QStringLiteral("The preset file is missing, unreadable or exceeds the safety limit."));
        return false;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage, QStringLiteral("Could not read the preset: %1").arg(file.errorString()));
        return false;
    }
    const QByteArray bytes = file.read(maximumBytes + 1);
    if (bytes.size() != info.size() || bytes.size() > maximumBytes) {
        setError(errorMessage, QStringLiteral("The preset could not be read completely."));
        return false;
    }
    if (!parse(bytes, expectedKind, envelope, errorMessage)) return false;
    envelope->storagePath = info.absoluteFilePath();
    return true;
}

bool PresetStore::writeFile(const QString &path,
                            const PresetEnvelope &envelope,
                            const qint64 maximumBytes,
                            QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    const QByteArray bytes = serialise(envelope, errorMessage);
    if (bytes.isEmpty()) return false;
    if (maximumBytes <= 0 || bytes.size() > maximumBytes) {
        setError(errorMessage, QStringLiteral("The preset exceeds the safe storage limit."));
        return false;
    }
    const QFileInfo info(path);
    QDir directory;
    if (!directory.mkpath(info.absolutePath())) {
        setError(errorMessage, QStringLiteral("Could not create the preset directory."));
        return false;
    }
    QSaveFile file(info.absoluteFilePath());
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(bytes) != bytes.size()
        || !file.commit()) {
        setError(errorMessage,
                 QStringLiteral("Could not save the preset: %1")
                     .arg(file.errorString()));
        return false;
    }
    return true;
}

QString PresetStore::filePathForId(const QString &directory,
                                   const QString &id)
{
    QString safe = cleanToken(id, 150);
    if (safe.isEmpty()) safe = QStringLiteral("preset");
    return QDir(directory).filePath(safe + QStringLiteral(".vfxpreset.json"));
}

bool PresetStore::pathIsManagedByDirectory(const QString &path,
                                           const QString &directory)
{
    if (path.isEmpty() || directory.isEmpty()) return false;
    const QFileInfo info(path);
    QString parent = QDir::cleanPath(info.absolutePath());
    QString expected = QDir::cleanPath(QDir(directory).absolutePath());
#ifdef Q_OS_WIN
    parent = parent.toCaseFolded();
    expected = expected.toCaseFolded();
#endif
    const QString fileName = info.fileName().toLower();
    return parent == expected
        && (fileName.endsWith(QStringLiteral(".json"))
            || fileName.endsWith(QStringLiteral(".vfxpreset")));
}

} // namespace vfx
