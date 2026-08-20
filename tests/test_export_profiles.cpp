#include "ExportNamingTemplate.h"
#include "ExportProfileStore.h"
#include "ImageExport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <QtTest/QtTest>

using namespace vfx;

class ExportProfileTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void namingTokensResolveDeterministically();
    void namingValidationRejectsUnsafeOrUnknownNames();
    void profilePayloadRoundTrips();
    void builtInProfilesAreValidAndStable();
    void userProfileCrudAndExchangeRetainStableIdentity();
    void forcedFlattenWorksForAlphaCapableFormat();
};

void ExportProfileTests::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("VFXPhotoLabTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ExportProfiles"));
    cleanup();
}

void ExportProfileTests::cleanup()
{
    const QString root = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    if (!root.isEmpty()) QDir(root).removeRecursively();
}

void ExportProfileTests::namingTokensResolveDeterministically()
{
    ExportNamingContext context;
    context.documentName = QStringLiteral("Studio Portrait.vfxphoto");
    context.profileName = QStringLiteral("Web PNG — sRGB 8-bit");
    context.formatSuffix = QStringLiteral("png");
    context.bitDepth = 8;
    context.imageSize = QSize(4032, 2268);
    context.workingSpaceName = QStringLiteral("Display P3");
    context.outputSpaceName = QStringLiteral("sRGB");
    context.timestampUtc = QDateTime(
        QDate(2026, 8, 3), QTime(22, 41, 7), Qt::UTC);

    QString error;
    const QString resolved = ExportNamingTemplate::resolve(
        QStringLiteral("{document}-{profile}-{width}x{height}-{bit_depth}-{date}-{time}-{{proof}}"),
        context, &error);
    QVERIFY2(!resolved.isEmpty(), qPrintable(error));
    QCOMPARE(resolved,
             QStringLiteral("Studio Portrait-Web PNG — sRGB 8-bit-4032x2268-8bit-2026-08-03-22-41-07-{proof}"));
}

void ExportProfileTests::namingValidationRejectsUnsafeOrUnknownNames()
{
    QString error;
    QVERIFY(!ExportNamingTemplate::validate(
        QStringLiteral("{document}-{unknown}"), &error));
    QVERIFY(error.contains(QStringLiteral("Unknown")));
    QVERIFY(!ExportNamingTemplate::validate(
        QStringLiteral("{document"), &error));
    QVERIFY(!ExportNamingTemplate::validate(
        QStringLiteral("../escape"), &error));

    ExportNamingContext context;
    context.documentName = QStringLiteral("Image.png");
    context.profileName = QStringLiteral("Custom");
    context.formatSuffix = QStringLiteral("png");
    context.bitDepth = 8;
    context.imageSize = QSize(1, 1);
    context.timestampUtc = QDateTime(
        QDate(2026, 8, 3), QTime(1, 2, 3), Qt::UTC);
    QVERIFY(ExportNamingTemplate::resolve(
        QStringLiteral("CON"), context, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(ExportNamingTemplate::resolve(
        QStringLiteral("../escape"), context, &error).isEmpty());
}

void ExportProfileTests::profilePayloadRoundTrips()
{
    ExportProfileData input;
    input.formatSuffix = QStringLiteral("tiff");
    input.bitDepth = ImageExportBitDepth::Sixteen;
    input.dither = ImageExportDither::None;
    input.alphaMode = ImageExportAlphaMode::FlattenToMatte;
    input.convertToOutputProfile = true;
    input.output.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::DisplayP3));
    input.output.renderingIntent = ColourRenderingIntent::Perceptual;
    input.output.blackPointCompensation = false;
    input.output.embedProfile = true;
    input.quality = 87;
    input.matteColour = QColor(QStringLiteral("#18324f"));
    input.namingTemplate = QStringLiteral("{document}-{output_space}-{bit_depth}");

    QString error;
    const QJsonObject json = input.toJson(&error);
    QVERIFY2(!json.isEmpty(), qPrintable(error));
    ExportProfileData decoded;
    QVERIFY2(ExportProfileData::fromJson(json, &decoded, &error),
             qPrintable(error));
    QVERIFY(decoded == input);

    QJsonObject damaged = json;
    damaged.insert(QStringLiteral("bitDepth"), QStringLiteral("32"));
    QVERIFY(!ExportProfileData::fromJson(damaged, &decoded, &error));
    damaged = json;
    damaged.insert(QStringLiteral("convertToOutputProfile"), QStringLiteral("true"));
    QVERIFY(!ExportProfileData::fromJson(damaged, &decoded, &error));
    damaged = json;
    damaged.insert(QStringLiteral("namingTemplate"), QStringLiteral("../escape"));
    QVERIFY(!ExportProfileData::fromJson(damaged, &decoded, &error));
}

void ExportProfileTests::builtInProfilesAreValidAndStable()
{
    const QVector<ExportProfile> first = ExportProfileStore::builtInProfiles();
    const QVector<ExportProfile> second = ExportProfileStore::builtInProfiles();
    QVERIFY(first.size() >= 5);
    QCOMPARE(first.size(), second.size());
    for (int index = 0; index < first.size(); ++index) {
        QString error;
        QVERIFY(first.at(index).builtIn);
        QVERIFY2(first.at(index).data.isValid(&error), qPrintable(error));
        QCOMPARE(first.at(index).metadata.id, second.at(index).metadata.id);
        QVERIFY(first.at(index).metadata.id.startsWith(
            QStringLiteral("builtin.export-profile.")));
    }
}

void ExportProfileTests::userProfileCrudAndExchangeRetainStableIdentity()
{
    ExportProfileData data = ExportProfileStore::builtInProfiles().first().data;
    data.namingTemplate = QStringLiteral("{document}-delivery");
    QString id;
    QString error;
    QVERIFY2(ExportProfileStore::createUserProfile(
                 QStringLiteral("Studio Delivery"), data,
                 QStringLiteral("Studio"),
                 {QStringLiteral("client"), QStringLiteral("delivery")},
                 &id, &error), qPrintable(error));
    QVERIFY(id.startsWith(QStringLiteral("user.export-profile.")));

    QVector<ExportProfile> profiles = ExportProfileStore::profiles();
    auto findId = [&profiles, &id] {
        return std::find_if(profiles.cbegin(), profiles.cend(),
            [&id](const ExportProfile &profile) {
                return profile.metadata.id == id;
            });
    };
    auto profile = findId();
    QVERIFY(profile != profiles.cend());
    QCOMPARE(profile->metadata.category, QStringLiteral("Studio"));

    QVERIFY2(ExportProfileStore::renameUserProfile(
                 *profile, QStringLiteral("Studio Web Delivery"), &error),
             qPrintable(error));
    profiles = ExportProfileStore::profiles();
    profile = findId();
    QVERIFY(profile != profiles.cend());
    QCOMPARE(profile->metadata.id, id);
    QCOMPARE(profile->name, QStringLiteral("Studio Web Delivery"));

    ExportProfileData updated = profile->data;
    updated.quality = 81;
    updated.namingTemplate = QStringLiteral("{document}-{date}-client");
    QVERIFY2(ExportProfileStore::updateUserProfile(
                 *profile, updated, &error), qPrintable(error));
    profiles = ExportProfileStore::profiles();
    profile = findId();
    QVERIFY(profile != profiles.cend());
    QCOMPARE(profile->data.quality, 81);

    QVERIFY2(ExportProfileStore::updateMetadata(
                 *profile, QStringLiteral("Client Delivery"),
                 {QStringLiteral("approved"), QStringLiteral("web")}, &error),
             qPrintable(error));
    profiles = ExportProfileStore::profiles();
    profile = findId();
    QVERIFY(profile != profiles.cend());
    QCOMPARE(profile->metadata.category, QStringLiteral("Client Delivery"));
    QVERIFY(profile->metadata.tags.contains(QStringLiteral("approved")));

    QVERIFY2(ExportProfileStore::setFavourite(*profile, true, &error),
             qPrintable(error));
    QVERIFY2(ExportProfileStore::recordUse(*profile, &error), qPrintable(error));
    profiles = ExportProfileStore::profiles();
    profile = findId();
    QVERIFY(profile != profiles.cend());
    QVERIFY(profile->metadata.favourite);
    QVERIFY(profile->metadata.useCount >= 1);

    QString duplicateId;
    QVERIFY2(ExportProfileStore::duplicateUserProfile(
                 *profile, QStringLiteral("Studio Web Delivery Copy"),
                 &duplicateId, &error), qPrintable(error));
    QVERIFY(!duplicateId.isEmpty());
    QVERIFY(duplicateId != id);
    profiles = ExportProfileStore::profiles();
    const auto duplicate = std::find_if(
        profiles.cbegin(), profiles.cend(),
        [&duplicateId](const ExportProfile &entry) {
            return entry.metadata.id == duplicateId;
        });
    QVERIFY(duplicate != profiles.cend());
    QVERIFY2(ExportProfileStore::removeUserProfile(*duplicate, &error),
             qPrintable(error));
    profiles = ExportProfileStore::profiles();
    profile = findId();
    QVERIFY(profile != profiles.cend());

    QTemporaryDir exchange;
    QVERIFY(exchange.isValid());
    const QString path = exchange.filePath(
        QStringLiteral("studio.vfxpreset.json"));
    QVERIFY2(ExportProfileStore::exportProfileFile(*profile, path, &error),
             qPrintable(error));
    QVERIFY2(ExportProfileStore::removeUserProfile(*profile, &error),
             qPrintable(error));
    QString importedId;
    QVERIFY2(ExportProfileStore::importProfileFile(
                 path, &importedId, &error), qPrintable(error));
    QCOMPARE(importedId, id);
    profiles = ExportProfileStore::profiles();
    profile = findId();
    QVERIFY(profile != profiles.cend());
    QVERIFY(profile->data == updated);
}

void ExportProfileTests::forcedFlattenWorksForAlphaCapableFormat()
{
    DocumentColourState state = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::SRgb));
    state.output.profile = state.workingSpace;

    QImage source(QSize(1, 1), QImage::Format_RGBA64);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    auto *pixel = reinterpret_cast<QRgba64 *>(source.scanLine(0));
    pixel[0] = QRgba64::fromRgba64(65535, 0, 0, 0);

    ImageExportRequest request;
    request.filePath = QStringLiteral("flatten.png");
    request.bitDepth = ImageExportBitDepth::Eight;
    request.dither = ImageExportDither::None;
    request.alphaMode = ImageExportAlphaMode::FlattenToMatte;
    request.convertToOutputProfile = false;
    request.output = state.output;
    request.matteColour = QColor(0, 0, 255);

    PreparedImageExport prepared;
    QString error;
    QVERIFY2(prepareImageExport(source, state, request, &prepared,
                                nullptr, &error), qPrintable(error));
    QVERIFY(prepared.flattenedTransparency);
    QCOMPARE(prepared.image.format(), QImage::Format_RGBA8888);
    const uchar *output = prepared.image.constScanLine(0);
    QCOMPARE(output[0], quint8(0));
    QCOMPARE(output[1], quint8(0));
    QCOMPARE(output[2], quint8(255));
    QCOMPARE(output[3], quint8(255));
}

QTEST_MAIN(ExportProfileTests)
#include "test_export_profiles.moc"
