#include "AdjustmentPresetStore.h"
#include "PresetCore.h"
#include "VectorAppearancePresetStore.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <variant>
#include <QtTest/QtTest>

using namespace vfx;

namespace {

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QDir directory;
    if (!directory.mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(bytes) == bytes.size();
}

QString legacyAdjustmentDirectory(const AdjustmentType type)
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.vfxphotolab");
    return QDir(root).filePath(QStringLiteral("adjustment-presets/%1")
                                   .arg(adjustmentTypeToString(type)));
}

} // namespace

class PresetTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void unifiedEnvelopeRoundTripsDeterministically();
    void malformedOrWrongKindEnvelopeIsRejected();
    void stableIdentifiersAreDeterministicAndDistinct();
    void legacyAdjustmentPresetLoadsAndMigratesOnlyOnExplicitUpdate();
    void legacyVectorAppearanceLoadsAndMigratesOnlyOnExplicitUpdate();
    void exportedAdjustmentPresetCanBeImportedWithStableIdentity();
    void builtInUsageMetadataPersistsSeparately();
    void adjustmentManagementMetadataRoundTrips();
    void vectorManagementMetadataRoundTrips();
};

void PresetTests::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("VFXPhotoLabTests"));
    QCoreApplication::setApplicationName(QStringLiteral("PresetArchitecture"));
    cleanup();
}

void PresetTests::cleanup()
{
    const QString root = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    if (!root.isEmpty()) QDir(root).removeRecursively();
}

void PresetTests::unifiedEnvelopeRoundTripsDeterministically()
{
    PresetEnvelope input;
    input.kind = PresetKind::ExportProfile;
    input.metadata.id = QStringLiteral("user.export-profile.12345678-1234-1234-1234-123456789abc");
    input.metadata.name = QStringLiteral("Web Delivery");
    input.metadata.category = QStringLiteral("Delivery");
    input.metadata.tags = {QStringLiteral("web"), QStringLiteral("srgb")};
    input.metadata.favourite = true;
    input.metadata.createdUtcMs = 1700000000000;
    input.metadata.modifiedUtcMs = 1700000000100;
    input.metadata.lastUsedUtcMs = 1700000000200;
    input.metadata.useCount = 7;
    input.payload.insert(QStringLiteral("format"), QStringLiteral("png"));
    input.payload.insert(QStringLiteral("bitDepth"), 8);

    QString error;
    const QByteArray first = PresetStore::serialise(input, &error);
    QVERIFY2(!first.isEmpty(), qPrintable(error));
    PresetEnvelope decoded;
    QVERIFY2(PresetStore::parse(first, PresetKind::ExportProfile,
                                &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.metadata.id, input.metadata.id);
    QCOMPARE(decoded.metadata.name, input.metadata.name);
    QCOMPARE(decoded.metadata.tags, input.metadata.tags);
    QCOMPARE(decoded.metadata.favourite, input.metadata.favourite);
    QCOMPARE(decoded.metadata.useCount, input.metadata.useCount);
    QCOMPARE(decoded.payload, input.payload);
    QCOMPARE(PresetStore::serialise(decoded, &error), first);
}

void PresetTests::malformedOrWrongKindEnvelopeIsRejected()
{
    PresetEnvelope input;
    input.kind = PresetKind::Adjustment;
    input.metadata.id = QStringLiteral("user.adjustment.12345678-1234-1234-1234-123456789abc");
    input.metadata.name = QStringLiteral("Test");
    input.payload.insert(QStringLiteral("adjustment"), QJsonObject{{QStringLiteral("type"), QStringLiteral("Exposure")}});
    QString error;
    const QByteArray bytes = PresetStore::serialise(input, &error);
    QVERIFY2(!bytes.isEmpty(), qPrintable(error));

    PresetEnvelope decoded;
    QVERIFY(!PresetStore::parse(bytes, PresetKind::VectorAppearance,
                                &decoded, &error));
    QVERIFY(!error.isEmpty());

    QJsonObject root = QJsonDocument::fromJson(bytes).object();
    root.insert(QStringLiteral("version"), 999);
    QVERIFY(!PresetStore::parse(QJsonDocument(root).toJson(),
                                PresetKind::Adjustment, &decoded, &error));

    root = QJsonDocument::fromJson(bytes).object();
    QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
    metadata.insert(QStringLiteral("id"), QStringLiteral("../../unsafe"));
    root.insert(QStringLiteral("metadata"), metadata);
    QVERIFY(!PresetStore::parse(QJsonDocument(root).toJson(),
                                PresetKind::Adjustment, &decoded, &error));

    root = QJsonDocument::fromJson(bytes).object();
    metadata = root.value(QStringLiteral("metadata")).toObject();
    metadata.insert(QStringLiteral("id"), QStringLiteral("user.adjustment.Uppercase"));
    root.insert(QStringLiteral("metadata"), metadata);
    QVERIFY(!PresetStore::parse(QJsonDocument(root).toJson(),
                                PresetKind::Adjustment, &decoded, &error));

    root = QJsonDocument::fromJson(bytes).object();
    metadata = root.value(QStringLiteral("metadata")).toObject();
    metadata.insert(QStringLiteral("category"), 17);
    root.insert(QStringLiteral("metadata"), metadata);
    QVERIFY(!PresetStore::parse(QJsonDocument(root).toJson(),
                                PresetKind::Adjustment, &decoded, &error));
}

void PresetTests::stableIdentifiersAreDeterministicAndDistinct()
{
    const QString first = PresetStore::stableBuiltInId(
        PresetKind::Adjustment, QStringLiteral("Exposure"),
        QStringLiteral("+1 EV"));
    QCOMPARE(first, PresetStore::stableBuiltInId(
        PresetKind::Adjustment, QStringLiteral("Exposure"),
        QStringLiteral("+1 EV")));
    QVERIFY(first != PresetStore::stableBuiltInId(
        PresetKind::Adjustment, QStringLiteral("Exposure"),
        QStringLiteral("-1 EV")));
    QVERIFY(first.startsWith(QStringLiteral("builtin.adjustment.")));
    const QString unicodeNameId = PresetStore::stableBuiltInId(
        PresetKind::Adjustment, QStringLiteral("Café"),
        QStringLiteral("Épreuve douce"));
    QVERIFY(QRegularExpression(QStringLiteral("^[a-z0-9._:-]+$"))
                .match(unicodeNameId).hasMatch());

    const QString user = PresetStore::newUserId(PresetKind::VectorAppearance);
    QVERIFY(user.startsWith(QStringLiteral("user.vector-appearance.")));
    QVERIFY(user != PresetStore::newUserId(PresetKind::VectorAppearance));
}

void PresetTests::legacyAdjustmentPresetLoadsAndMigratesOnlyOnExplicitUpdate()
{
    AdjustmentData adjustment;
    adjustment.reset(AdjustmentType::Exposure);
    auto parameters = std::get<ExposureParameters>(adjustment.parameters);
    parameters.exposure = 1.25;
    adjustment.parameters = parameters;
    adjustment.normalise();
    bool adjustmentOk = false;
    const QJsonObject adjustmentJson = adjustment.toJson(&adjustmentOk);
    QVERIFY(adjustmentOk);

    QJsonObject legacy;
    legacy.insert(QStringLiteral("format"), QStringLiteral("vfxphotolab-adjustment-preset"));
    legacy.insert(QStringLiteral("version"), 1);
    legacy.insert(QStringLiteral("name"), QStringLiteral("Legacy Exposure"));
    legacy.insert(QStringLiteral("adjustment"), adjustmentJson);
    const QString legacyPath = QDir(legacyAdjustmentDirectory(adjustment.type))
        .filePath(QStringLiteral("Legacy_Exposure.json"));
    QVERIFY(writeBytes(legacyPath, QJsonDocument(legacy).toJson()));

    QStringList warnings;
    QVector<AdjustmentPreset> loaded = AdjustmentPresetStore::presets(
        adjustment.type, &warnings);
    const auto match = std::find_if(loaded.cbegin(), loaded.cend(),
        [](const AdjustmentPreset &preset) {
            return !preset.builtIn
                && preset.name == QStringLiteral("Legacy Exposure");
        });
    QVERIFY(match != loaded.cend());
    QVERIFY(match->metadata.id.startsWith(QStringLiteral("legacy.adjustment.")));
    QCOMPARE(QFileInfo(match->storagePath).absoluteFilePath(),
             QFileInfo(legacyPath).absoluteFilePath());
    QVERIFY(QFileInfo::exists(legacyPath));

    QString error;
    QVERIFY2(AdjustmentPresetStore::saveUserPreset(
                 match->name, match->adjustment, &error, true),
             qPrintable(error));
    QVERIFY(!QFileInfo::exists(legacyPath));
    loaded = AdjustmentPresetStore::presets(adjustment.type, &warnings);
    const auto migrated = std::find_if(loaded.cbegin(), loaded.cend(),
        [](const AdjustmentPreset &preset) {
            return !preset.builtIn
                && preset.name == QStringLiteral("Legacy Exposure");
        });
    QVERIFY(migrated != loaded.cend());
    QVERIFY(migrated->storagePath.startsWith(
        AdjustmentPresetStore::storageDirectory(adjustment.type)));
    QFile migratedFile(migrated->storagePath);
    QVERIFY(migratedFile.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(migratedFile.readAll()).object();
    QCOMPARE(root.value(QStringLiteral("version")).toInt(),
             PresetStore::CurrentSchemaVersion);
    QCOMPARE(root.value(QStringLiteral("kind")).toString(),
             QStringLiteral("adjustment"));
}

void PresetTests::legacyVectorAppearanceLoadsAndMigratesOnlyOnExplicitUpdate()
{
    VectorAppearance appearance;
    appearance.fill.enabled = true;
    appearance.fill.colour = QColor(QStringLiteral("#336699"));
    appearance.stroke.enabled = true;
    appearance.stroke.colour = QColor(QStringLiteral("#ffcc00"));
    appearance.stroke.width = 7.5;
    appearance.normalise();
    bool appearanceOk = false;
    const QJsonObject appearanceJson = appearance.toJson(&appearanceOk);
    QVERIFY(appearanceOk);

    QJsonObject legacy;
    legacy.insert(QStringLiteral("format"), QStringLiteral("vfxphotolab-preset"));
    legacy.insert(QStringLiteral("version"), 1);
    legacy.insert(QStringLiteral("category"), QStringLiteral("vector-appearance"));
    legacy.insert(QStringLiteral("name"), QStringLiteral("Legacy Vector"));
    legacy.insert(QStringLiteral("appearance"), appearanceJson);
    const QString legacyPath = QDir(VectorAppearancePresetStore::storageDirectory())
        .filePath(QStringLiteral("legacy-vector.json"));
    QVERIFY(writeBytes(legacyPath, QJsonDocument(legacy).toJson()));

    QStringList warnings;
    QVector<VectorAppearancePreset> loaded =
        VectorAppearancePresetStore::presets(&warnings);
    const auto match = std::find_if(loaded.cbegin(), loaded.cend(),
        [](const VectorAppearancePreset &preset) {
            return preset.name == QStringLiteral("Legacy Vector");
        });
    QVERIFY(match != loaded.cend());
    QVERIFY(match->metadata.id.startsWith(
        QStringLiteral("legacy.vector-appearance.")));
    QVERIFY(QFileInfo::exists(legacyPath));

    QString error;
    QVERIFY2(VectorAppearancePresetStore::saveUserPreset(
                 match->name, match->appearance, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(legacyPath));
    loaded = VectorAppearancePresetStore::presets(&warnings);
    const auto migrated = std::find_if(loaded.cbegin(), loaded.cend(),
        [](const VectorAppearancePreset &preset) {
            return preset.name == QStringLiteral("Legacy Vector");
        });
    QVERIFY(migrated != loaded.cend());
    QFile migratedFile(migrated->storagePath);
    QVERIFY(migratedFile.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(migratedFile.readAll()).object();
    QCOMPARE(root.value(QStringLiteral("version")).toInt(),
             PresetStore::CurrentSchemaVersion);
    QCOMPARE(root.value(QStringLiteral("kind")).toString(),
             QStringLiteral("vector-appearance"));
}

void PresetTests::exportedAdjustmentPresetCanBeImportedWithStableIdentity()
{
    AdjustmentData adjustment;
    adjustment.reset(AdjustmentType::Contrast);
    auto parameters = std::get<ContrastParameters>(adjustment.parameters);
    parameters.contrast = 31.0;
    adjustment.parameters = parameters;
    adjustment.normalise();
    QString error;
    QString createdId;
    QVERIFY2(AdjustmentPresetStore::createUserPreset(
                 QStringLiteral("Studio Contrast"), adjustment,
                 QStringLiteral("Studio"),
                 {QStringLiteral("contrast"), QStringLiteral("delivery")},
                 &createdId, &error), qPrintable(error));
    QVector<AdjustmentPreset> presets = AdjustmentPresetStore::presets(
        adjustment.type);
    const auto saved = std::find_if(presets.cbegin(), presets.cend(),
        [](const AdjustmentPreset &preset) {
            return !preset.builtIn
                && preset.name == QStringLiteral("Studio Contrast");
        });
    QVERIFY(saved != presets.cend());

    QTemporaryDir exchange;
    QVERIFY(exchange.isValid());
    const QString exportedPath = exchange.filePath(
        QStringLiteral("studio-contrast.vfxpreset.json"));
    QVERIFY2(AdjustmentPresetStore::exportPresetFile(
                 *saved, exportedPath, &error), qPrintable(error));
    const QString stableId = saved->metadata.id;
    QCOMPARE(stableId, createdId);
    QVERIFY2(AdjustmentPresetStore::removeUserPreset(*saved, &error),
             qPrintable(error));

    QString importedName;
    QVERIFY2(AdjustmentPresetStore::importPresetFile(
                 exportedPath, adjustment.type, &importedName, &error),
             qPrintable(error));
    QCOMPARE(importedName, QStringLiteral("Studio Contrast"));
    presets = AdjustmentPresetStore::presets(adjustment.type);
    const auto imported = std::find_if(presets.cbegin(), presets.cend(),
        [](const AdjustmentPreset &preset) {
            return !preset.builtIn
                && preset.name == QStringLiteral("Studio Contrast");
        });
    QVERIFY(imported != presets.cend());
    QCOMPARE(imported->metadata.id, stableId);
    QCOMPARE(imported->metadata.category, QStringLiteral("Studio"));
    QCOMPARE(imported->metadata.tags,
             QStringList({QStringLiteral("contrast"),
                          QStringLiteral("delivery")}));
}


void PresetTests::builtInUsageMetadataPersistsSeparately()
{
    QVector<AdjustmentPreset> available =
        AdjustmentPresetStore::builtInPresets(AdjustmentType::Exposure);
    QVERIFY(!available.isEmpty());
    const AdjustmentPreset builtIn = available.first();
    QVERIFY(builtIn.builtIn);
    QVERIFY(builtIn.storagePath.isEmpty());

    QString error;
    QVERIFY2(AdjustmentPresetStore::setFavourite(builtIn, true, &error),
             qPrintable(error));
    QVERIFY2(AdjustmentPresetStore::recordUse(builtIn, &error),
             qPrintable(error));

    available = AdjustmentPresetStore::builtInPresets(
        AdjustmentType::Exposure);
    const auto reloaded = std::find_if(
        available.cbegin(), available.cend(),
        [&builtIn](const AdjustmentPreset &preset) {
            return preset.metadata.id == builtIn.metadata.id;
        });
    QVERIFY(reloaded != available.cend());
    QVERIFY(reloaded->metadata.favourite);
    QVERIFY(reloaded->metadata.lastUsedUtcMs > 0);
    QCOMPARE(reloaded->metadata.useCount, quint64(1));
    QVERIFY(reloaded->storagePath.isEmpty());

    const QDir userDirectory(
        AdjustmentPresetStore::storageDirectory(AdjustmentType::Exposure));
    QCOMPARE(userDirectory.entryList(
                 {QStringLiteral("*.json")}, QDir::Files).size(), 0);
}

void PresetTests::adjustmentManagementMetadataRoundTrips()
{
    AdjustmentData adjustment;
    adjustment.reset(AdjustmentType::Contrast);
    auto parameters = std::get<ContrastParameters>(adjustment.parameters);
    parameters.contrast = 22.0;
    adjustment.parameters = parameters;
    adjustment.normalise();

    QString id;
    QString error;
    QVERIFY2(AdjustmentPresetStore::createUserPreset(
                 QStringLiteral("Portrait Contrast"), adjustment,
                 QStringLiteral("Portrait"),
                 {QStringLiteral("skin"), QStringLiteral("studio")},
                 &id, &error), qPrintable(error));
    QVERIFY(id.startsWith(QStringLiteral("user.adjustment.")));

    QVector<AdjustmentPreset> available =
        AdjustmentPresetStore::presets(adjustment.type);
    auto findById = [&available, &id] {
        return std::find_if(available.cbegin(), available.cend(),
            [&id](const AdjustmentPreset &preset) {
                return preset.metadata.id == id;
            });
    };
    auto created = findById();
    QVERIFY(created != available.cend());
    QCOMPARE(created->metadata.category, QStringLiteral("Portrait"));
    QCOMPARE(created->metadata.tags,
             QStringList({QStringLiteral("skin"), QStringLiteral("studio")}));

    QVERIFY2(AdjustmentPresetStore::setFavourite(*created, true, &error),
             qPrintable(error));
    QVERIFY2(AdjustmentPresetStore::recordUse(*created, &error),
             qPrintable(error));
    available = AdjustmentPresetStore::presets(adjustment.type);
    created = findById();
    QVERIFY(created != available.cend());
    QVERIFY(created->metadata.favourite);
    QCOMPARE(created->metadata.useCount, quint64(1));

    QVERIFY2(AdjustmentPresetStore::updateMetadata(
                 *created, QStringLiteral("Editorial"),
                 {QStringLiteral("portrait"), QStringLiteral("print")},
                 &error), qPrintable(error));
    available = AdjustmentPresetStore::presets(adjustment.type);
    created = findById();
    QVERIFY(created != available.cend());
    QCOMPARE(created->metadata.category, QStringLiteral("Editorial"));
    QCOMPARE(created->metadata.tags,
             QStringList({QStringLiteral("portrait"), QStringLiteral("print")}));

    QVERIFY2(AdjustmentPresetStore::renameUserPreset(
                 *created, QStringLiteral("Editorial Contrast"), &error),
             qPrintable(error));
    available = AdjustmentPresetStore::presets(adjustment.type);
    created = findById();
    QVERIFY(created != available.cend());
    QCOMPARE(created->name, QStringLiteral("Editorial Contrast"));
    QCOMPARE(created->metadata.id, id);

    QVERIFY2(AdjustmentPresetStore::duplicateUserPreset(
                 *created, QStringLiteral("Editorial Contrast Copy"), &error),
             qPrintable(error));
    available = AdjustmentPresetStore::presets(adjustment.type);
    const auto duplicate = std::find_if(
        available.cbegin(), available.cend(),
        [](const AdjustmentPreset &preset) {
            return preset.name == QStringLiteral("Editorial Contrast Copy");
        });
    QVERIFY(duplicate != available.cend());
    QVERIFY(duplicate->metadata.id != id);
    QCOMPARE(duplicate->metadata.category, QStringLiteral("Editorial"));
    QCOMPARE(duplicate->metadata.tags,
             QStringList({QStringLiteral("portrait"), QStringLiteral("print")}));

    const QString tooLongCategory(
        PresetStore::MaximumCategoryLength + 1, QLatin1Char('x'));
    QVERIFY(!AdjustmentPresetStore::updateMetadata(
        *duplicate, tooLongCategory, {}, &error));
    QVERIFY(!error.isEmpty());

    const QString tooLongTag(
        PresetStore::MaximumTagLength + 1, QLatin1Char('t'));
    QVERIFY(!AdjustmentPresetStore::updateMetadata(
        *duplicate, QStringLiteral("Editorial"), {tooLongTag}, &error));
    QVERIFY(!error.isEmpty());
}

void PresetTests::vectorManagementMetadataRoundTrips()
{
    VectorAppearance appearance;
    appearance.fill.enabled = true;
    appearance.fill.colour = QColor(QStringLiteral("#224466"));
    appearance.stroke.enabled = true;
    appearance.stroke.colour = QColor(QStringLiteral("#ffcc88"));
    appearance.stroke.width = 4.5;
    appearance.normalise();

    QString id;
    QString error;
    QVERIFY2(VectorAppearancePresetStore::createUserPreset(
                 QStringLiteral("Diagram Accent"), appearance,
                 QStringLiteral("Diagrams"),
                 {QStringLiteral("arrow"), QStringLiteral("accent")},
                 &id, &error), qPrintable(error));
    QVERIFY(id.startsWith(QStringLiteral("user.vector-appearance.")));

    QVector<VectorAppearancePreset> available =
        VectorAppearancePresetStore::presets();
    auto findById = [&available, &id] {
        return std::find_if(available.cbegin(), available.cend(),
            [&id](const VectorAppearancePreset &preset) {
                return preset.metadata.id == id;
            });
    };
    auto created = findById();
    QVERIFY(created != available.cend());
    QCOMPARE(created->metadata.category, QStringLiteral("Diagrams"));
    QCOMPARE(created->metadata.tags,
             QStringList({QStringLiteral("arrow"), QStringLiteral("accent")}));

    QVERIFY2(VectorAppearancePresetStore::updateMetadata(
                 *created, QStringLiteral("Annotation"),
                 {QStringLiteral("callout")}, &error), qPrintable(error));
    available = VectorAppearancePresetStore::presets();
    created = findById();
    QVERIFY(created != available.cend());
    QCOMPARE(created->metadata.category, QStringLiteral("Annotation"));
    QCOMPARE(created->metadata.tags,
             QStringList({QStringLiteral("callout")}));

    QVERIFY2(VectorAppearancePresetStore::duplicateUserPreset(
                 *created, QStringLiteral("Diagram Accent Copy"), &error),
             qPrintable(error));
    available = VectorAppearancePresetStore::presets();
    const auto duplicate = std::find_if(
        available.cbegin(), available.cend(),
        [](const VectorAppearancePreset &preset) {
            return preset.name == QStringLiteral("Diagram Accent Copy");
        });
    QVERIFY(duplicate != available.cend());
    QVERIFY(duplicate->metadata.id != id);
    QCOMPARE(duplicate->metadata.category, QStringLiteral("Annotation"));
    QCOMPARE(duplicate->metadata.tags,
             QStringList({QStringLiteral("callout")}));
}

QTEST_APPLESS_MAIN(PresetTests)

#include "test_presets.moc"
