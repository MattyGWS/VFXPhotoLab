#include "BlueNoise64.h"
#include "ColourManagement.h"
#include "ColourResourceAudit.h"
#include "DisplayColourManagement.h"
#include "ColourConversion.h"
#include "DocumentSession.h"
#include "HistogramService.h"
#include "ImageProfileImport.h"
#include "ImageExport.h"
#include "OcioIntegration.h"
#include "ClipboardOperations.h"
#include "ImageProcessor.h"
#include "ManagedAdjustmentGpuLut.h"
#include "PhotoDocument.h"
#include "SessionCache.h"
#include "gpu/RenderBackend.h"
#include "gpu/TiledCanvasEngine.h"

#include <QCryptographicHash>
#include <QFile>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cmath>

using namespace vfx;

namespace {

bool exactImagesEqual(const QImage &left, const QImage &right)
{
    if (left.isNull() || right.isNull()) {
        return left.isNull() == right.isNull();
    }
    if (left.size() != right.size()
        || left.format() != right.format()
        || left.colorSpace() != right.colorSpace()
        || left.depth() != right.depth()) {
        return false;
    }
    const qsizetype activeRowBytes = static_cast<qsizetype>(left.width())
        * static_cast<qsizetype>(left.depth() / 8);
    for (int y = 0; y < left.height(); ++y) {
        if (std::memcmp(left.constScanLine(y),
                        right.constScanLine(y),
                        static_cast<std::size_t>(activeRowBytes)) != 0) {
            return false;
        }
    }
    return true;
}

QImage patternedImage(const QSize size, const QColorSpace &colourSpace)
{
    QImage image(size, QImage::Format_RGBA8888);
    image.setColorSpace(colourSpace);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[x * 4 + 0] = static_cast<uchar>((x * 37 + y * 11 + 19) & 0xff);
            row[x * 4 + 1] = static_cast<uchar>((x * 13 + y * 29 + 71) & 0xff);
            row[x * 4 + 2] = static_cast<uchar>((x * 53 + y * 7 + 3) & 0xff);
            row[x * 4 + 3] = static_cast<uchar>((x * 17 + y * 31 + 101) & 0xff);
        }
    }
    return image;
}

std::array<float, 3> sampleDisplayLut(const QVector<qfloat16> &values,
                                      const int edge,
                                      const std::array<float, 3> input)
{
    const int highest = edge - 1;
    std::array<int, 3> lower {};
    std::array<int, 3> upper {};
    std::array<float, 3> amount {};
    for (int component = 0; component < 3; ++component) {
        const float scaled = std::clamp(input[component], 0.0f, 1.0f)
            * static_cast<float>(highest);
        lower[component] = static_cast<int>(std::floor(scaled));
        upper[component] = std::min(lower[component] + 1, highest);
        amount[component] = scaled - static_cast<float>(lower[component]);
    }
    const auto fetch = [&](const int red, const int green, const int blue) {
        const qsizetype texel = qsizetype(red)
            + qsizetype(blue) * edge
            + qsizetype(green) * edge * edge;
        const qsizetype offset = texel * 4;
        return std::array<float, 3> {
            static_cast<float>(values[offset]),
            static_cast<float>(values[offset + 1]),
            static_cast<float>(values[offset + 2])};
    };
    const auto mix = [](const std::array<float, 3> &left,
                        const std::array<float, 3> &right,
                        const float t) {
        return std::array<float, 3> {
            left[0] + (right[0] - left[0]) * t,
            left[1] + (right[1] - left[1]) * t,
            left[2] + (right[2] - left[2]) * t};
    };
    const auto c000 = fetch(lower[0], lower[1], lower[2]);
    const auto c100 = fetch(upper[0], lower[1], lower[2]);
    const auto c010 = fetch(lower[0], upper[1], lower[2]);
    const auto c110 = fetch(upper[0], upper[1], lower[2]);
    const auto c001 = fetch(lower[0], lower[1], upper[2]);
    const auto c101 = fetch(upper[0], lower[1], upper[2]);
    const auto c011 = fetch(lower[0], upper[1], upper[2]);
    const auto c111 = fetch(upper[0], upper[1], upper[2]);
    const auto z0 = mix(mix(c000, c100, amount[0]),
                        mix(c010, c110, amount[0]), amount[1]);
    const auto z1 = mix(mix(c001, c101, amount[0]),
                        mix(c011, c111, amount[0]), amount[1]);
    return mix(z0, z1, amount[2]);
}

void appendU16Be(QByteArray *bytes, const quint16 value)
{
    bytes->append(static_cast<char>((value >> 8) & 0xff));
    bytes->append(static_cast<char>(value & 0xff));
}

void appendU16Le(QByteArray *bytes, const quint16 value)
{
    bytes->append(static_cast<char>(value & 0xff));
    bytes->append(static_cast<char>((value >> 8) & 0xff));
}

void appendU32Le(QByteArray *bytes, const quint32 value)
{
    bytes->append(static_cast<char>(value & 0xff));
    bytes->append(static_cast<char>((value >> 8) & 0xff));
    bytes->append(static_cast<char>((value >> 16) & 0xff));
    bytes->append(static_cast<char>((value >> 24) & 0xff));
}

} // namespace

class ColourManagementTests final : public QObject {
    Q_OBJECT

private slots:
    void descriptorsAndStateRoundTripDeterministically();
    void untaggedPolicyChangesInterpretationWithoutChangingPixels();
    void taggedPngImportPreservesProfileMetadata();
    void jpegAndTiffContainerProfilesCanBeRecovered();
    void advertisedInvalidProfileUsesUntaggedPolicy();
    void schemaOneColourStateMigratesToExplicitInputMetadata();
    void untaggedImportMetadataPersistsAcrossProjectAndColdStorage();
    void transformServiceCachesQtTransforms();
    void projectVersionFifteenPersistsExplicitColourState();
    void versionFourteenMigrationPreservesExactLegacyRender();
    void sessionSnapshotPreservesColourStateAcrossColdStorage();
    void histogramAndTileKeysIncludeColourStateRevision();
    void renderBackendRejectsObsoleteColourStateRevision();
    void assignProfileRetagsWithoutChangingComponentValues();
    void convertProfilePreservesAlphaAndRoundTripsWithinIntegerTolerance();
    void convertProfileIncludesSemanticColoursButNotMasks();
    void convertProfileSupportsRgba64AndCancellation();
    void externalIccProfileLoaderValidatesAndFingerprints();
    void profileOperationsPersistAcrossProjectAndColdStorage();
    void adjustmentDomainsAreExplicitAndStable();
    void managedExposureUsesLinearWorkingDomain();
    void managedLinearSrgbExposureDoesNotDecodeTwice();
    void managedProcessingPreservesLegacyOutputWhenRequested();
    void managedEightAndSixteenBitExposureRemainConsistent();
    void managedWideGamutAdjustmentPreservesAlphaAndWorkingProfile();
    void ocioSchemaFourPersistsAndOlderSchemasMigratePresentationDisabled();
    void bundledAcesConfigurationCanBeInspected();
    void ocioWorkingSpacesExposeValidAdjustmentProxies();
    void ocioCpuRoundTripPreservesAlpha();
    void ocioExportCanTargetNonWorkingConfigSpace();
    void ocioDisplayViewIsDistinctAndDeterministic();
    void changedOcioFingerprintRequiresExplicitRelink();
    void ocioConfigurationPersistsAcrossProjectAndColdStorage();
    void presentationMetadataUpdateLeavesProcessingStateUntouched();
    void presentationSettingsRemainIsolatedAcrossDocuments();
    void displayTransformIsDeterministicAndPresentationOnly();
    void displayGpuLatticeIsDeterministicAndMatchesCpuReference();
    void managedAdjustmentGpuLatticesAreDeterministic();
    void exportCapabilitiesAreExplicit();
    void blueNoiseQuantisationIsDeterministicAndAlphaSafe();
    void colourManagedExportIgnoresPresentationState();
    void formatSpecificExportHandlingIsExplicit();
    void exportCancellationStopsQuantisation();
    void sixteenBitPngExportRetainsPrecisionAndProfile();
    void taggedJpegAndSixteenBitTiffRoundTripWhenAvailable();
    void outputDefaultsPersistWithoutProcessingRevision();
    void externalIccResourceWarningsDoNotDirtyProjects();
    void changedExternalIccRequiresExplicitRelink();
    void missingOcioResourceIsReportedWithoutSubstitution();
    void coldResidencyReauditsColourResources();
    void metadataOnlyColourUpdatesAreIdempotent();
    void malformedProjectSourceBase64IsRejected();
    void softProofSettingsInvalidateDisplayFingerprint();
    void monitorDiscoveryFallsBackDeterministically();
};

void ColourManagementTests::descriptorsAndStateRoundTripDeterministically()
{
    const ColourSpaceDescriptor srgb = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    const ColourSpaceDescriptor linear = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgbLinear));
    QVERIFY(srgb.isValid());
    QVERIFY(linear.isValid());
    QVERIFY(srgb.stableFingerprint() != linear.stableFingerprint());

    DocumentColourState state = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::DisplayP3));
    state.displayTransform.kind = DisplayTransformKind::SystemIcc;
    state.output.profile = srgb;
    state.revision = 42;
    QString error;
    QVERIFY2(state.isSafe(&error), qPrintable(error));

    const QJsonObject json = state.toJson();
    const auto restored = DocumentColourState::fromJson(json, &error);
    QVERIFY2(restored.has_value(), qPrintable(error));
    QCOMPARE(restored->revision, quint64(42));
    QVERIFY(restored->semanticallyEquals(state));
    QCOMPARE(restored->stableFingerprint(), state.stableFingerprint());

    QJsonObject damaged = json;
    damaged.insert(QStringLiteral("fingerprint"), QStringLiteral("00"));
    QVERIFY(!DocumentColourState::fromJson(damaged, &error).has_value());
    QVERIFY(error.contains(QStringLiteral("integrity"), Qt::CaseInsensitive));

    const QByteArray externalBytes = QColorSpace(QColorSpace::SRgb).iccProfile();
    const QByteArray externalFingerprint = QCryptographicHash::hash(
        externalBytes, QCryptographicHash::Sha256);
    ColourSpaceDescriptor external = ColourSpaceDescriptor::externalIcc(
        QStringLiteral("/tmp/reference.icc"), externalFingerprint,
        externalBytes, QStringLiteral("Reference"));
    QVERIFY(external.isValid());
    external.iccProfile = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    QVERIFY(!external.isValid());
}

void ColourManagementTests::untaggedPolicyChangesInterpretationWithoutChangingPixels()
{
    const QImage source = patternedImage(QSize(11, 9), QColorSpace());
    const QByteArray originalBytes(
        reinterpret_cast<const char *>(source.constBits()), source.sizeInBytes());

    QImage unresolved = source;
    ImageColourImportInfo unresolvedInfo;
    unresolvedInfo.sourceStatus = InputProfileStatus::Untagged;
    applyUntaggedImagePolicy(&unresolved, &unresolvedInfo,
                             UntaggedImagePolicy::Ask);
    QVERIFY(!unresolved.colorSpace().isValid());
    QVERIFY(!unresolvedInfo.policyWasApplied);

    QImage assumed = source;
    ImageColourImportInfo assumedInfo;
    assumedInfo.sourceStatus = InputProfileStatus::Untagged;
    applyUntaggedImagePolicy(&assumed, &assumedInfo,
                             UntaggedImagePolicy::AssumeSRgb);
    QCOMPARE(assumed.colorSpace(), QColorSpace(QColorSpace::SRgb));
    QVERIFY(assumedInfo.policyWasApplied);
    QVERIFY(assumedInfo.appliedPolicy == UntaggedImagePolicy::AssumeSRgb);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(assumed.constBits()),
                        assumed.sizeInBytes()), originalBytes);

    QImage untagged = source;
    ImageColourImportInfo untaggedInfo;
    untaggedInfo.sourceStatus = InputProfileStatus::Untagged;
    applyUntaggedImagePolicy(&untagged, &untaggedInfo,
                             UntaggedImagePolicy::LeaveUntagged);
    QVERIFY(!untagged.colorSpace().isValid());
    QVERIFY(untaggedInfo.policyWasApplied);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(untagged.constBits()),
                        untagged.sizeInBytes()), originalBytes);

    ClipboardPayload payload;
    payload.imageKind = ClipboardImageKind::Rgba;
    payload.sourceKind = ClipboardSourceKind::ExternalImage;
    payload.image = source;
    payload.documentBounds = QRect(QPoint(), source.size());
    payload.sourceDocumentSize = source.size();
    const QImage explicitUntagged = clipboardPayloadAsNewDocumentRaster(
        payload, QColorSpace());
    QVERIFY(!explicitUntagged.isNull());
    QVERIFY(!explicitUntagged.colorSpace().isValid());
}

void ColourManagementTests::taggedPngImportPreservesProfileMetadata()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("display-p3.png"));
    const QColorSpace displayP3(QColorSpace::DisplayP3);
    QImage source = patternedImage(QSize(13, 7), displayP3);
    QVERIFY(source.save(path, "PNG"));

    QString error;
    ImageFileReadResult read = PhotoDocument::readImageFileDetailed(path, &error);
    QVERIFY2(read.isValid(), qPrintable(error));
    QVERIFY(read.image.colorSpace().isValid());
    QVERIFY(read.colourInfo.sourceStatus == InputProfileStatus::EmbeddedValid);
    QVERIFY(!read.colourInfo.originalProfileFingerprint.isEmpty());
    QVERIFY(!read.colourInfo.policyWasApplied);

    PhotoDocument document;
    document.setSourceImage(read.image, path, read.colourInfo);
    QVERIFY(document.colourState().inputProfileStatus
            == InputProfileStatus::EmbeddedValid);
    QVERIFY(!document.colourState().inputProfile.iccProfile.isEmpty());
    QCOMPARE(document.colourState().originalInputProfileFingerprint,
             read.colourInfo.originalProfileFingerprint);
    QVERIFY(!document.colourState().untaggedPolicyApplied);

    const QString projectPath = directory.filePath(QStringLiteral("profile.vfxphoto"));
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));
    PhotoDocument restored;
    QVERIFY2(restored.loadProject(projectPath, &error), qPrintable(error));
    QVERIFY(restored.colourState().inputProfileStatus
            == InputProfileStatus::EmbeddedValid);
    QCOMPARE(restored.colourState().originalInputProfileFingerprint,
             read.colourInfo.originalProfileFingerprint);
}

void ColourManagementTests::jpegAndTiffContainerProfilesCanBeRecovered()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray profile = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    QVERIFY(!profile.isEmpty());
    QVERIFY(profile.size() < 65000);

    QByteArray jpeg;
    jpeg.append(char(0xff));
    jpeg.append(char(0xd8));
    jpeg.append(char(0xff));
    jpeg.append(char(0xe2));
    const QByteArray signature("ICC_PROFILE\0", 12);
    const quint16 jpegSegmentLength = static_cast<quint16>(
        2 + signature.size() + 2 + profile.size());
    appendU16Be(&jpeg, jpegSegmentLength);
    jpeg += signature;
    jpeg.append(char(1));
    jpeg.append(char(1));
    jpeg += profile;
    jpeg.append(char(0xff));
    jpeg.append(char(0xd9));
    const QString jpegPath = directory.filePath(QStringLiteral("profile.jpg"));
    QFile jpegFile(jpegPath);
    QVERIFY(jpegFile.open(QIODevice::WriteOnly));
    QCOMPARE(jpegFile.write(jpeg), static_cast<qint64>(jpeg.size()));
    jpegFile.close();

    QImage jpegPixels = patternedImage(QSize(4, 3), QColorSpace());
    const ImageColourImportInfo jpegInfo = inspectImageColourProfile(
        jpegPath, &jpegPixels);
    QVERIFY(jpegInfo.sourceStatus == InputProfileStatus::EmbeddedValid);
    QCOMPARE(jpegInfo.originalIccProfile, profile);
    QVERIFY(jpegPixels.colorSpace().isValid());

    QByteArray tiff;
    tiff.append("II", 2);
    appendU16Le(&tiff, 42);
    appendU32Le(&tiff, 8);
    appendU16Le(&tiff, 1);
    appendU16Le(&tiff, 34675); // ICCProfile
    appendU16Le(&tiff, 7);     // UNDEFINED
    appendU32Le(&tiff, static_cast<quint32>(profile.size()));
    constexpr quint32 ProfileOffset = 8 + 2 + 12 + 4;
    appendU32Le(&tiff, ProfileOffset);
    appendU32Le(&tiff, 0);
    QCOMPARE(tiff.size(), static_cast<qsizetype>(ProfileOffset));
    tiff += profile;
    const QString tiffPath = directory.filePath(QStringLiteral("profile.tiff"));
    QFile tiffFile(tiffPath);
    QVERIFY(tiffFile.open(QIODevice::WriteOnly));
    QCOMPARE(tiffFile.write(tiff), static_cast<qint64>(tiff.size()));
    tiffFile.close();

    QImage tiffPixels = patternedImage(QSize(4, 3), QColorSpace());
    const ImageColourImportInfo tiffInfo = inspectImageColourProfile(
        tiffPath, &tiffPixels);
    QVERIFY(tiffInfo.sourceStatus == InputProfileStatus::EmbeddedValid);
    QCOMPARE(tiffInfo.originalIccProfile, profile);
    QVERIFY(tiffPixels.colorSpace().isValid());
}

void ColourManagementTests::advertisedInvalidProfileUsesUntaggedPolicy()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("advertised-invalid.png"));
    QByteArray bytes;
    bytes.append("\x89PNG\r\n\x1a\n", 8);
    bytes.append("\0\0\0\x0b", 4);
    bytes.append("iCCP", 4);
    bytes.append("bad\0\0broken", 11);
    bytes.append("\0\0\0\0", 4); // Inspector deliberately does not require CRC validation.
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), static_cast<qint64>(bytes.size()));
    file.close();

    QImage pixels = patternedImage(QSize(5, 4), QColorSpace());
    const QByteArray original(
        reinterpret_cast<const char *>(pixels.constBits()), pixels.sizeInBytes());
    ImageColourImportInfo info = inspectImageColourProfile(path, &pixels);
    QVERIFY(info.embeddedProfileAdvertised);
    QVERIFY(info.sourceStatus == InputProfileStatus::InvalidOrUnsupported);
    QVERIFY(!info.warnings.isEmpty());
    applyUntaggedImagePolicy(&pixels, &info,
                             UntaggedImagePolicy::AssumeSRgb);
    QCOMPARE(pixels.colorSpace(), QColorSpace(QColorSpace::SRgb));
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(pixels.constBits()),
                        pixels.sizeInBytes()), original);
}

void ColourManagementTests::schemaOneColourStateMigratesToExplicitInputMetadata()
{
    DocumentColourState current = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::SRgb));
    QJsonObject legacy = current.toJson();
    legacy.insert(QStringLiteral("schema"), 1);
    legacy.remove(QStringLiteral("inputProfileStatus"));
    legacy.remove(QStringLiteral("untaggedPolicy"));
    legacy.remove(QStringLiteral("untaggedPolicyApplied"));
    legacy.remove(QStringLiteral("originalInputProfileFingerprint"));
    legacy.remove(QStringLiteral("fingerprint"));

    QString error;
    const auto restored = DocumentColourState::fromJson(legacy, &error);
    QVERIFY2(restored.has_value(), qPrintable(error));
    QVERIFY(restored->inputProfileStatus == InputProfileStatus::LegacyUnknown);
    QVERIFY(!restored->untaggedPolicyApplied);
    QVERIFY(!restored->originalInputProfileFingerprint.isEmpty());
}

void ColourManagementTests::untaggedImportMetadataPersistsAcrossProjectAndColdStorage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source = patternedImage(QSize(17, 12), QColorSpace());
    const QByteArray originalBytes(
        reinterpret_cast<const char *>(source.constBits()), source.sizeInBytes());
    ImageColourImportInfo info;
    info.sourceStatus = InputProfileStatus::Untagged;
    applyUntaggedImagePolicy(&source, &info,
                             UntaggedImagePolicy::AssumeSRgb);

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("untagged.tga"), info);
    QVERIFY(document.colourState().inputProfileStatus
            == InputProfileStatus::Untagged);
    QVERIFY(document.colourState().untaggedPolicyApplied);
    QVERIFY(document.colourState().untaggedPolicy
            == UntaggedImagePolicy::AssumeSRgb);
    QCOMPARE(document.sourceImage().colorSpace(),
             QColorSpace(QColorSpace::SRgb));
    QCOMPARE(QByteArray(
                 reinterpret_cast<const char *>(document.sourceImage().constBits()),
                 document.sourceImage().sizeInBytes()),
             originalBytes);

    QString error;
    const QString projectPath = directory.filePath(
        QStringLiteral("assumed-srgb.vfxphoto"));
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));
    PhotoDocument projectRestored;
    QVERIFY2(projectRestored.loadProject(projectPath, &error), qPrintable(error));
    QVERIFY(projectRestored.colourState().inputProfileStatus
            == InputProfileStatus::Untagged);
    QVERIFY(projectRestored.colourState().untaggedPolicyApplied);
    QVERIFY(projectRestored.colourState().untaggedPolicy
            == UntaggedImagePolicy::AssumeSRgb);
    QCOMPARE(projectRestored.sourceImage().colorSpace(),
             QColorSpace(QColorSpace::SRgb));

    SessionCacheStore store(directory.path());
    QVERIFY(store.isAvailable());
    DocumentSession sourceSession;
    sourceSession.document().setSourceImage(
        source, QStringLiteral("untagged.tga"), info);
    sourceSession.refreshSummary();
    QString snapshotPath;
    qint64 snapshotBytes = 0;
    QVERIFY2(store.writeSnapshot(sourceSession,
                                 &snapshotPath,
                                 &snapshotBytes,
                                 &error),
             qPrintable(error));
    QVERIFY(snapshotBytes > 0);

    DocumentSession snapshotRestored;
    QVERIFY2(store.restoreSnapshot(snapshotPath,
                                   &snapshotRestored,
                                   &error),
             qPrintable(error));
    const DocumentColourState &restoredState =
        snapshotRestored.document().colourState();
    QVERIFY(restoredState.inputProfileStatus == InputProfileStatus::Untagged);
    QVERIFY(restoredState.untaggedPolicyApplied);
    QVERIFY(restoredState.untaggedPolicy
            == UntaggedImagePolicy::AssumeSRgb);
    QVERIFY(restoredState.semanticallyEquals(
        sourceSession.document().colourState()));
}

void ColourManagementTests::transformServiceCachesQtTransforms()
{
    ColourTransformService &service = ColourTransformService::instance();
    service.clear();

    ColourTransformRequest request;
    request.source = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    request.destination = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgbLinear));
    request.purpose = ColourTransformPurpose::InputToWorking;

    QVERIFY(service.qtTransform(request).has_value());
    ColourTransformService::CacheStats stats = service.cacheStats();
    QCOMPARE(stats.entries, 1);
    QCOMPARE(stats.misses, quint64(1));
    QCOMPARE(stats.hits, quint64(0));

    QVERIFY(service.qtTransform(request).has_value());
    stats = service.cacheStats();
    QCOMPARE(stats.entries, 1);
    QCOMPARE(stats.misses, quint64(1));
    QCOMPARE(stats.hits, quint64(1));

    request.source = ColourSpaceDescriptor::untagged();
    QVERIFY(!service.qtTransform(request).has_value());
    QCOMPARE(service.cacheStats().misses, quint64(2));
}

void ColourManagementTests::projectVersionFifteenPersistsExplicitColourState()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    NewDocumentSettings settings;
    settings.pixelSize = QSize(17, 13);
    settings.colourSpace = QColorSpace(QColorSpace::DisplayP3);
    PhotoDocument document;
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(document.colourState().processingCompatibility
            == ColourProcessingCompatibility::ManagedV1);
    QVERIFY(document.colourState().presentationColourManagementEnabled);
    QVERIFY(document.colourState().displayTransform.kind
            == DisplayTransformKind::SystemIcc);

    const QString path = directory.filePath(QStringLiteral("managed-v15.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    QVERIFY(json.isObject());
    QCOMPARE(json.object().value(QStringLiteral("version")).toInt(),
             PhotoDocument::ProjectFormatVersion);
    QVERIFY(json.object().value(QStringLiteral("colourManagement")).isObject());
    QCOMPARE(json.object().value(QStringLiteral("colourManagement"))
                 .toObject().value(QStringLiteral("schema")).toInt(),
             DocumentColourState::JsonSchemaVersion);

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(path, &error), qPrintable(error));
    QVERIFY(restored.colourState().semanticallyEquals(document.colourState()));
    QCOMPARE(restored.colourStateRevision(), document.colourStateRevision());
    QCOMPARE(restored.sourceImage().colorSpace(), QColorSpace(QColorSpace::DisplayP3));
    QVERIFY(!restored.isModified());
}

void ColourManagementTests::versionFourteenMigrationPreservesExactLegacyRender()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    PhotoDocument original;
    const QImage source = patternedImage(QSize(19, 15), QColorSpace(QColorSpace::SRgb));
    original.setSourceImage(source, QStringLiteral("legacy-source.png"));
    const QUuid exposureId = original.addAdjustment(AdjustmentType::Exposure);
    QVERIFY(!exposureId.isNull());
    QVERIFY(original.updateLayer(exposureId, [](LayerNode &layer) {
        layer.exposure = 0.625;
        layer.opacity = 0.83;
    }));
    const QUuid saturationId = original.addAdjustment(AdjustmentType::Saturation);
    QVERIFY(!saturationId.isNull());
    QVERIFY(original.updateLayer(saturationId, [](LayerNode &layer) {
        layer.saturation = -0.27;
    }));

    const QImage baseline = ImageProcessor::renderPreservingHiddenRgb(
        original.sourceImage(), original.layers(), nullptr,
        original.sourceImage().size());
    QVERIFY(!baseline.isNull());

    QString error;
    const QString path = directory.filePath(QStringLiteral("legacy-v14.vfxphoto"));
    QVERIFY2(original.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    file.close();
    QVERIFY(json.isObject());
    QJsonObject root = json.object();
    root.insert(QStringLiteral("version"), 14);
    root.remove(QStringLiteral("colourManagement"));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    QCOMPARE(file.write(bytes), static_cast<qint64>(bytes.size()));
    file.close();

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(path, &error), qPrintable(error));
    QVERIFY(restored.colourState().processingCompatibility
            == ColourProcessingCompatibility::LegacyV1);
    QVERIFY(restored.colourState().displayTransform.kind
            == DisplayTransformKind::Disabled);
    QVERIFY(!restored.isModified());

    const QImage migratedRender = ImageProcessor::renderPreservingHiddenRgb(
        restored.sourceImage(), restored.layers(), nullptr,
        restored.sourceImage().size());
    QVERIFY(exactImagesEqual(migratedRender, baseline));

    const QString migratedPath = directory.filePath(QStringLiteral("migrated-v15.vfxphoto"));
    QVERIFY2(restored.saveProject(migratedPath, &error), qPrintable(error));
    QFile migratedFile(migratedPath);
    QVERIFY(migratedFile.open(QIODevice::ReadOnly));
    const QJsonObject migratedRoot = QJsonDocument::fromJson(
        migratedFile.readAll()).object();
    QCOMPARE(migratedRoot.value(QStringLiteral("version")).toInt(),
             PhotoDocument::ProjectFormatVersion);
    const auto persisted = DocumentColourState::fromJson(
        migratedRoot.value(QStringLiteral("colourManagement")).toObject(), &error);
    QVERIFY2(persisted.has_value(), qPrintable(error));
    QVERIFY(persisted->processingCompatibility
            == ColourProcessingCompatibility::LegacyV1);
}

void ColourManagementTests::sessionSnapshotPreservesColourStateAcrossColdStorage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SessionCacheStore store(directory.path());
    QVERIFY(store.isAvailable());

    DocumentSession sourceSession;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(23, 17);
    settings.bitDepth = 16;
    settings.colourSpace = QColorSpace(QColorSpace::SRgbLinear);
    QString error;
    QVERIFY2(sourceSession.document().createNewDocument(settings, &error), qPrintable(error));
    sourceSession.refreshSummary();

    QString snapshotPath;
    qint64 snapshotBytes = 0;
    QVERIFY2(store.writeSnapshot(sourceSession,
                                 &snapshotPath,
                                 &snapshotBytes,
                                 &error),
             qPrintable(error));
    QVERIFY(snapshotBytes > 0);

    DocumentSession restoredSession;
    QVERIFY2(store.restoreSnapshot(snapshotPath, &restoredSession, &error), qPrintable(error));
    QVERIFY(restoredSession.document().colourState().semanticallyEquals(
        sourceSession.document().colourState()));
    QCOMPARE(restoredSession.document().colourStateRevision(),
             sourceSession.document().colourStateRevision());
    QCOMPARE(restoredSession.document().sourceImage().colorSpace(),
             QColorSpace(QColorSpace::SRgbLinear));
}

void ColourManagementTests::histogramAndTileKeysIncludeColourStateRevision()
{
    HistogramRequest request;
    request.documentSessionId = QUuid::createUuid();
    request.adjustmentLayerId = QUuid::createUuid();
    request.documentRevision = 7;
    request.colourStateRevision = 1;
    request.source = patternedImage(QSize(16, 16), QColorSpace(QColorSpace::SRgb));
    request.documentSize = request.source.size();
    const QString firstKey = request.cacheKey();
    request.colourStateRevision = 2;
    QVERIFY(firstKey != request.cacheKey());
    request.colourStateRevision = 1;
    request.processingCompatibility = ColourProcessingCompatibility::ManagedV1;
    QVERIFY(firstKey != request.cacheKey());
    request.processingCompatibility = ColourProcessingCompatibility::LegacyV1;

    LayerNode base;
    base.type = LayerType::BaseImage;
    QVector<LayerNode> layers {base};
    TiledCanvasEngine engine;
    const QUuid sessionId = QUuid::createUuid();
    QVERIFY(!engine.renderRegion(request.source,
                                 layers,
                                 request.source.rect(),
                                 request.source.size(),
                                 false,
                                 0,
                                 nullptr,
                                 nullptr,
                                 sessionId,
                                 11).isNull());
    const TileCache::Stats afterFirst = engine.cacheStats();
    QVERIFY(afterFirst.misses >= 1);

    QVERIFY(!engine.renderRegion(request.source,
                                 layers,
                                 request.source.rect(),
                                 request.source.size(),
                                 false,
                                 0,
                                 nullptr,
                                 nullptr,
                                 sessionId,
                                 11).isNull());
    const TileCache::Stats afterSecond = engine.cacheStats();
    QVERIFY(afterSecond.hits > afterFirst.hits);

    QVERIFY(!engine.renderRegion(request.source,
                                 layers,
                                 request.source.rect(),
                                 request.source.size(),
                                 false,
                                 0,
                                 nullptr,
                                 nullptr,
                                 sessionId,
                                 11,
                                 ColourProcessingCompatibility::ManagedV1).isNull());
    const TileCache::Stats afterCompatibilityChange = engine.cacheStats();
    QVERIFY(afterCompatibilityChange.misses > afterSecond.misses);

    QVERIFY(!engine.renderRegion(request.source,
                                 layers,
                                 request.source.rect(),
                                 request.source.size(),
                                 false,
                                 0,
                                 nullptr,
                                 nullptr,
                                 sessionId,
                                 12,
                                 ColourProcessingCompatibility::ManagedV1).isNull());
    const TileCache::Stats afterColourChange = engine.cacheStats();
    QVERIFY(afterColourChange.misses > afterCompatibilityChange.misses);
}

void ColourManagementTests::renderBackendRejectsObsoleteColourStateRevision()
{
    RenderBackend &backend = RenderBackend::instance();
    backend.resetDocumentState();

    const QUuid sessionId = QUuid::createUuid();
    const RenderSessionContext first {sessionId, 9, 17,
                                      ColourProcessingCompatibility::LegacyV1};
    const RenderSessionContext changedCompatibility {
        sessionId, 9, 17, ColourProcessingCompatibility::ManagedV1};
    const RenderSessionContext changedRevision {
        sessionId, 9, 18, ColourProcessingCompatibility::ManagedV1};
    backend.activateSession(first);
    QVERIFY(backend.isSessionCurrent(first));
    QVERIFY(!backend.isSessionCurrent(changedCompatibility));
    QVERIFY(!backend.isSessionCurrent(changedRevision));

    backend.resetSessionState(changedCompatibility);
    QVERIFY(!backend.isSessionCurrent(first));
    QVERIFY(backend.isSessionCurrent(changedCompatibility));

    backend.resetSessionState(changedRevision);
    QVERIFY(!backend.isSessionCurrent(changedCompatibility));
    QVERIFY(backend.isSessionCurrent(changedRevision));
    backend.resetDocumentState();
}


void ColourManagementTests::assignProfileRetagsWithoutChangingComponentValues()
{
    PhotoDocument document;
    const QImage source = patternedImage(
        QSize(17, 11), QColorSpace(QColorSpace::SRgb));
    document.setSourceImage(source, QStringLiteral("tagged.png"));

    const QByteArray beforeBytes(
        reinterpret_cast<const char *>(document.sourceImage().constBits()),
        document.sourceImage().sizeInBytes());
    const QVector<LayerNode> beforeLayers = document.layers();
    const InputProfileStatus originalInputStatus =
        document.colourState().inputProfileStatus;
    const QByteArray originalInputFingerprint =
        document.colourState().originalInputProfileFingerprint;
    const ColourSpaceDescriptor displayP3 = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::DisplayP3));

    PreparedColourProfileResult prepared;
    QString error;
    QVERIFY2(prepareAssignedDocumentProfile(
                 document, displayP3, &prepared, &error),
             qPrintable(error));
    QVERIFY(prepared.operation == DocumentProfileOperation::Assign);
    QCOMPARE(prepared.canvasImage.colorSpace(), QColorSpace(QColorSpace::DisplayP3));
    QCOMPARE(QByteArray(
                 reinterpret_cast<const char *>(prepared.canvasImage.constBits()),
                 prepared.canvasImage.sizeInBytes()),
             beforeBytes);
    QCOMPARE(prepared.layers.size(), beforeLayers.size());
    for (int index = 0; index < prepared.layers.size(); ++index) {
        const QImage &before = beforeLayers.at(index).rasterImage;
        const QImage &after = prepared.layers.at(index).rasterImage;
        if (before.isNull()) continue;
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(after.constBits()),
                            after.sizeInBytes()),
                 QByteArray(reinterpret_cast<const char *>(before.constBits()),
                            before.sizeInBytes()));
        QCOMPARE(after.colorSpace(), QColorSpace(QColorSpace::DisplayP3));
    }
    QVERIFY(prepared.colourState.processingCompatibility
            == ColourProcessingCompatibility::ManagedV1);
    QCOMPARE(prepared.colourState.workingSpace.stableFingerprint(),
             displayP3.stableFingerprint());
    QVERIFY(prepared.colourState.inputProfileStatus == originalInputStatus);
    QCOMPARE(prepared.colourState.originalInputProfileFingerprint,
             originalInputFingerprint);
}

void ColourManagementTests::convertProfilePreservesAlphaAndRoundTripsWithinIntegerTolerance()
{
    QImage source(QSize(9, 7), QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4 + 0] = static_cast<uchar>(17 + x * 19);
            row[x * 4 + 1] = static_cast<uchar>(31 + y * 23);
            row[x * 4 + 2] = static_cast<uchar>(211 - x * 7 - y * 5);
            row[x * 4 + 3] = static_cast<uchar>((x + y) % 3 == 0
                                                ? 0 : 39 + x * 13);
        }
    }
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("alpha-safe.png"));

    const ColourSpaceDescriptor linear = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgbLinear));
    ColourTransformService::instance().clear();
    PreparedColourProfileResult toLinear;
    QString error;
    QVERIFY2(prepareConvertedDocumentProfile(
                 document, linear, &toLinear, nullptr, &error),
             qPrintable(error));
    QCOMPARE(toLinear.canvasImage.colorSpace(),
             QColorSpace(QColorSpace::SRgbLinear));
    const ColourTransformService::CacheStats transformStats =
        ColourTransformService::instance().cacheStats();
    QCOMPARE(transformStats.entries, qsizetype(1));
    QCOMPARE(transformStats.misses, quint64(1));

    bool anyRgbChanged = false;
    bool transparentRgbChanged = false;
    for (int y = 0; y < source.height(); ++y) {
        const uchar *before = source.constScanLine(y);
        const uchar *after = toLinear.canvasImage.constScanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            QCOMPARE(after[x * 4 + 3], before[x * 4 + 3]);
            const bool rgbChanged = after[x * 4 + 0] != before[x * 4 + 0]
                || after[x * 4 + 1] != before[x * 4 + 1]
                || after[x * 4 + 2] != before[x * 4 + 2];
            anyRgbChanged = anyRgbChanged || rgbChanged;
            if (before[x * 4 + 3] == 0) {
                transparentRgbChanged = transparentRgbChanged || rgbChanged;
            }
        }
    }
    QVERIFY(anyRgbChanged);
    QVERIFY(transparentRgbChanged);

    PhotoDocument linearDocument = document;
    QVERIFY2(linearDocument.replaceStructuralState(
                 toLinear.canvasImage,
                 toLinear.layers,
                 document.selectionMask().snapshot(),
                 document.horizontalGuides(),
                 document.verticalGuides(),
                 document.resolutionX(),
                 document.resolutionY(),
                 toLinear.colourState,
                 &error),
             qPrintable(error));
    const ColourSpaceDescriptor srgb = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    PreparedColourProfileResult roundTrip;
    QVERIFY2(prepareConvertedDocumentProfile(
                 linearDocument, srgb, &roundTrip, nullptr, &error),
             qPrintable(error));
    for (int y = 0; y < source.height(); ++y) {
        const uchar *before = source.constScanLine(y);
        const uchar *after = roundTrip.canvasImage.constScanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            QCOMPARE(after[x * 4 + 3], before[x * 4 + 3]);
            for (int channel = 0; channel < 3; ++channel) {
                QVERIFY(std::abs(int(after[x * 4 + channel])
                                 - int(before[x * 4 + channel])) <= 4);
            }
        }
    }
}

void ColourManagementTests::convertProfileIncludesSemanticColoursButNotMasks()
{
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(32, 24);
    settings.colourSpace = QColorSpace(QColorSpace::SRgb);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));

    const QUuid vectorId = document.addVectorShape(
        VectorShapeType::Rectangle,
        QRectF(2.0, 2.0, 12.0, 9.0),
        QColor(189, 73, 41, 127));
    QVERIFY(!vectorId.isNull());
    QVERIFY(document.updateLayer(vectorId, [](LayerNode &layer) {
        layer.vectorData.objects[0].stroke.enabled = true;
        layer.vectorData.objects[0].stroke.colour = QColor(35, 166, 219, 91);
        layer.vectorData.objects[0].stroke.opacity = 0.75;
    }));

    TextLayerData text;
    text.text = QStringLiteral("Colour");
    text.colour = QColor(201, 99, 33, 77);
    const QUuid textId = document.addTextLayer(text);
    QVERIFY(!textId.isNull());

    const QUuid gradientId = document.addAdjustment(AdjustmentType::GradientMap);
    QVERIFY(!gradientId.isNull());
    QVERIFY(document.updateLayer(gradientId, [](LayerNode &layer) {
        GradientMapParameters gradient;
        gradient.stops = {{0.0, QColor(22, 44, 188, 63)},
                          {1.0, QColor(240, 190, 35, 129)}};
        layer.setGradientMapParameters(gradient);
    }));

    const QUuid baseId = document.baseLayerId();
    QVERIFY(document.addMask(baseId));
    const QImage maskBefore = document.layerById(baseId).maskImage;
    QVERIFY(!maskBefore.isNull());

    const QVector<LayerNode> beforeLayers = document.layers();
    const ColourSpaceDescriptor displayP3 = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::DisplayP3));
    PreparedColourProfileResult prepared;
    QVERIFY2(prepareConvertedDocumentProfile(
                 document, displayP3, &prepared, nullptr, &error),
             qPrintable(error));
    QVERIFY(prepared.processedSemanticColours >= 5);

    const auto find = [](const QVector<LayerNode> &layers,
                         const QUuid &id,
                         const auto &self) -> const LayerNode * {
        for (const LayerNode &layer : layers) {
            if (layer.id == id) return &layer;
            if (const LayerNode *nested = self(layer.children, id, self)) {
                return nested;
            }
        }
        return nullptr;
    };
    const LayerNode *beforeVector = find(beforeLayers, vectorId, find);
    const LayerNode *afterVector = find(prepared.layers, vectorId, find);
    const LayerNode *beforeText = find(beforeLayers, textId, find);
    const LayerNode *afterText = find(prepared.layers, textId, find);
    const LayerNode *beforeGradient = find(beforeLayers, gradientId, find);
    const LayerNode *afterGradient = find(prepared.layers, gradientId, find);
    const LayerNode *afterBase = find(prepared.layers, baseId, find);
    QVERIFY(beforeVector && afterVector && beforeText && afterText
            && beforeGradient && afterGradient && afterBase);

    QCOMPARE(afterVector->vectorData.objects[0].fill.colour.alpha(),
             beforeVector->vectorData.objects[0].fill.colour.alpha());
    QVERIFY(afterVector->vectorData.objects[0].fill.colour
            != beforeVector->vectorData.objects[0].fill.colour);
    QCOMPARE(afterVector->vectorData.objects[0].stroke.colour.alpha(),
             beforeVector->vectorData.objects[0].stroke.colour.alpha());
    QCOMPARE(afterText->textData.colour.alpha(),
             beforeText->textData.colour.alpha());
    QVERIFY(afterText->textData.colour != beforeText->textData.colour);

    const auto beforeStops = std::get<GradientMapParameters>(
        beforeGradient->effectiveAdjustmentData().parameters).stops;
    const auto afterStops = std::get<GradientMapParameters>(
        afterGradient->effectiveAdjustmentData().parameters).stops;
    QCOMPARE(afterStops.size(), beforeStops.size());
    QVERIFY(afterStops[0].colour != beforeStops[0].colour);
    QCOMPARE(afterStops[0].colour.alpha(), beforeStops[0].colour.alpha());
    QVERIFY(exactImagesEqual(afterBase->maskImage, maskBefore));
}


void ColourManagementTests::convertProfileSupportsRgba64AndCancellation()
{
    QImage source(QSize(5, 4), QImage::Format_RGBA64);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < source.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>(1000 + x * 7000),
                static_cast<quint16>(32000 + y * 4000),
                static_cast<quint16>(61000 - x * 3000 - y * 1500),
                static_cast<quint16>((x + y) % 2 == 0 ? 0 : 41000));
        }
    }
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("sixteen-bit.png"));
    const ColourSpaceDescriptor displayP3 = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::DisplayP3));

    PreparedColourProfileResult prepared;
    QString error;
    QVERIFY2(prepareConvertedDocumentProfile(
                 document, displayP3, &prepared, nullptr, &error),
             qPrintable(error));
    QCOMPARE(prepared.canvasImage.format(), QImage::Format_RGBA64);
    QCOMPARE(prepared.canvasImage.colorSpace(), QColorSpace(QColorSpace::DisplayP3));
    for (int y = 0; y < source.height(); ++y) {
        const auto *before = reinterpret_cast<const QRgba64 *>(source.constScanLine(y));
        const auto *after = reinterpret_cast<const QRgba64 *>(
            prepared.canvasImage.constScanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            QCOMPARE(after[x].alpha(), before[x].alpha());
        }
    }

    std::atomic_bool cancelled(true);
    PreparedColourProfileResult cancelledResult;
    QVERIFY(!prepareConvertedDocumentProfile(
        document, displayP3, &cancelledResult, &cancelled, &error));
    QVERIFY(cancelledResult.cancelled);
}


void ColourManagementTests::profileOperationsPersistAcrossProjectAndColdStorage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(21, 15);
    settings.bitDepth = 16;
    settings.colourSpace = QColorSpace(QColorSpace::SRgb);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));

    const ColourSpaceDescriptor displayP3 = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::DisplayP3));
    PreparedColourProfileResult prepared;
    QVERIFY2(prepareConvertedDocumentProfile(
                 document, displayP3, &prepared, nullptr, &error),
             qPrintable(error));
    QVERIFY2(document.replaceStructuralState(
                 prepared.canvasImage,
                 prepared.layers,
                 document.selectionMask().snapshot(),
                 document.horizontalGuides(),
                 document.verticalGuides(),
                 document.resolutionX(),
                 document.resolutionY(),
                 prepared.colourState,
                 &error),
             qPrintable(error));
    QCOMPARE(document.sourceImage().format(), QImage::Format_RGBA64);
    QCOMPARE(document.sourceImage().colorSpace(),
             QColorSpace(QColorSpace::DisplayP3));
    QVERIFY(document.colourState().processingCompatibility
            == ColourProcessingCompatibility::ManagedV1);

    const QString projectPath = directory.filePath(
        QStringLiteral("converted-display-p3.vfxphoto"));
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));
    PhotoDocument projectRestored;
    QVERIFY2(projectRestored.loadProject(projectPath, &error), qPrintable(error));
    QVERIFY(projectRestored.colourState().semanticallyEquals(
        document.colourState()));
    QCOMPARE(projectRestored.sourceImage().format(), QImage::Format_RGBA64);
    QCOMPARE(projectRestored.sourceImage().colorSpace(),
             QColorSpace(QColorSpace::DisplayP3));
    for (const LayerNode &layer : projectRestored.layers()) {
        if (!layer.rasterImage.isNull()) {
            QCOMPARE(layer.rasterImage.colorSpace(),
                     QColorSpace(QColorSpace::DisplayP3));
        }
    }

    SessionCacheStore store(directory.path());
    QVERIFY(store.isAvailable());
    DocumentSession sourceSession;
    sourceSession.document() = document;
    sourceSession.refreshSummary();
    QString snapshotPath;
    qint64 snapshotBytes = 0;
    QVERIFY2(store.writeSnapshot(sourceSession,
                                 &snapshotPath,
                                 &snapshotBytes,
                                 &error),
             qPrintable(error));
    QVERIFY(snapshotBytes > 0);

    DocumentSession snapshotRestored;
    QVERIFY2(store.restoreSnapshot(snapshotPath,
                                   &snapshotRestored,
                                   &error),
             qPrintable(error));
    QVERIFY(snapshotRestored.document().colourState().semanticallyEquals(
        document.colourState()));
    QCOMPARE(snapshotRestored.document().sourceImage().format(),
             QImage::Format_RGBA64);
    QCOMPARE(snapshotRestored.document().sourceImage().colorSpace(),
             QColorSpace(QColorSpace::DisplayP3));
}

void ColourManagementTests::externalIccProfileLoaderValidatesAndFingerprints()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString validPath = directory.filePath(QStringLiteral("display-p3.icc"));
    const QByteArray profile = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    QVERIFY(!profile.isEmpty());
    QFile valid(validPath);
    QVERIFY(valid.open(QIODevice::WriteOnly));
    QCOMPARE(valid.write(profile), static_cast<qint64>(profile.size()));
    valid.close();

    ColourSpaceDescriptor descriptor;
    QString error;
    QVERIFY2(loadExternalIccProfile(validPath, &descriptor, &error),
             qPrintable(error));
    QVERIFY(descriptor.kind == ColourSpaceKind::ExternalIcc);
    QVERIFY(descriptor.toQColorSpace().isValid());
    QCOMPARE(descriptor.externalFingerprint.size(), 32);
    QCOMPARE(descriptor.iccProfile, profile);

    const QString invalidPath = directory.filePath(QStringLiteral("invalid.icc"));
    QFile invalid(invalidPath);
    QVERIFY(invalid.open(QIODevice::WriteOnly));
    QCOMPARE(invalid.write("not an icc profile"), qint64(18));
    invalid.close();
    QVERIFY(!loadExternalIccProfile(invalidPath, &descriptor, &error));
    QVERIFY(!error.isEmpty());
}


void ColourManagementTests::adjustmentDomainsAreExplicitAndStable()
{
    AdjustmentData data;
    data.reset(AdjustmentType::Exposure);
    QCOMPARE(adjustmentProcessingDomain(data),
             AdjustmentProcessingDomain::LinearWorking);
    QVERIFY(adjustmentRequiresManagedDomainTransform(data));

    data.reset(AdjustmentType::Levels);
    QCOMPARE(adjustmentProcessingDomain(data),
             AdjustmentProcessingDomain::EncodedWorking);
    QVERIFY(!adjustmentRequiresManagedDomainTransform(data));

    data.reset(AdjustmentType::ChannelMixer);
    QCOMPARE(adjustmentProcessingDomain(data),
             AdjustmentProcessingDomain::RawComponents);
    QVERIFY(!adjustmentRequiresManagedDomainTransform(data));

    data.reset(AdjustmentType::Lut);
    QCOMPARE(adjustmentProcessingDomain(data),
             AdjustmentProcessingDomain::LutContract);
    QVERIFY(!adjustmentRequiresManagedDomainTransform(data));

    LayerNode managedLayer;
    managedLayer.type = LayerType::Adjustment;
    managedLayer.setExposure(1.0);
    QVERIFY(layerTreeRequiresManagedDomainTransform({managedLayer}));
    managedLayer.visible = false;
    QVERIFY(!layerTreeRequiresManagedDomainTransform({managedLayer}));
    managedLayer.visible = true;
    managedLayer.resetAdjustmentParameters(AdjustmentType::Levels);
    QVERIFY(!layerTreeRequiresManagedDomainTransform({managedLayer}));

    data.reset(AdjustmentType::Threshold);
    ThresholdParameters threshold = std::get<ThresholdParameters>(data.parameters);
    threshold.source = ThresholdSource::Luminance;
    data.parameters = threshold;
    QCOMPARE(adjustmentProcessingDomain(data),
             AdjustmentProcessingDomain::EncodedSrgb);
    threshold.source = ThresholdSource::Red;
    data.parameters = threshold;
    QCOMPARE(adjustmentProcessingDomain(data),
             AdjustmentProcessingDomain::RawComponents);
}

void ColourManagementTests::managedExposureUsesLinearWorkingDomain()
{
    QImage source(1, 1, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.fill(QColor(128, 128, 128, 173));

    LayerNode base;
    base.type = LayerType::BaseImage;
    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.setExposure(1.0);

    const QImage legacy = ImageProcessor::render(
        source, {exposure, base}, nullptr, source.size(),
        ColourProcessingCompatibility::LegacyV1);
    const QImage managed = ImageProcessor::render(
        source, {exposure, base}, nullptr, source.size(),
        ColourProcessingCompatibility::ManagedV1);
    QVERIFY(!legacy.isNull());
    QVERIFY(!managed.isNull());
    QCOMPARE(managed.colorSpace(), source.colorSpace());
    QCOMPARE(managed.pixelColor(0, 0).alpha(), 173);
    // Both contracts implement the same mathematical +1 EV result here; the
    // managed path differs by making the linear working-domain conversion
    // explicit instead of embedding it in the legacy operator.
    QVERIFY(std::abs(legacy.pixelColor(0, 0).red()
                     - managed.pixelColor(0, 0).red()) <= 2);
    QVERIFY(managed.pixelColor(0, 0).red() >= 170);
    QVERIFY(managed.pixelColor(0, 0).red() <= 190);
}


void ColourManagementTests::managedLinearSrgbExposureDoesNotDecodeTwice()
{
    QImage source(1, 1, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
    source.fill(QColor(64, 64, 64, 149));

    LayerNode base;
    base.type = LayerType::BaseImage;
    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.setExposure(1.0);

    const QImage managed = ImageProcessor::render(
        source, {exposure, base}, nullptr, source.size(),
        ColourProcessingCompatibility::ManagedV1);
    QVERIFY(!managed.isNull());
    QCOMPARE(managed.colorSpace(), source.colorSpace());
    QCOMPARE(managed.pixelColor(0, 0).alpha(), 149);
    // 0.25 linear with +1 EV becomes approximately 0.5 linear. A second
    // sRGB decode would produce a substantially darker result.
    QVERIFY(managed.pixelColor(0, 0).red() >= 126);
    QVERIFY(managed.pixelColor(0, 0).red() <= 130);
}

void ColourManagementTests::managedProcessingPreservesLegacyOutputWhenRequested()
{
    QImage source = patternedImage(QSize(17, 11), QColorSpace(QColorSpace::DisplayP3));
    LayerNode base;
    base.type = LayerType::BaseImage;
    LayerNode saturation;
    saturation.type = LayerType::Adjustment;
    SaturationParameters parameters;
    parameters.saturation = 35.0;
    saturation.setSaturationParameters(parameters);

    const QImage historical = ImageProcessor::render(source, {saturation, base});
    const QImage explicitLegacy = ImageProcessor::render(
        source, {saturation, base}, nullptr, source.size(),
        ColourProcessingCompatibility::LegacyV1);
    QVERIFY(exactImagesEqual(historical, explicitLegacy));

    const QImage managed = ImageProcessor::render(
        source, {saturation, base}, nullptr, source.size(),
        ColourProcessingCompatibility::ManagedV1);
    QVERIFY(!managed.isNull());
    QCOMPARE(managed.colorSpace(), source.colorSpace());
    QVERIFY(!exactImagesEqual(managed, explicitLegacy));
}

void ColourManagementTests::managedEightAndSixteenBitExposureRemainConsistent()
{
    QImage source8(1, 1, QImage::Format_RGBA8888);
    source8.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source8.fill(QColor(96, 137, 183, 211));

    QImage source16(1, 1, QImage::Format_RGBA64);
    source16.setColorSpace(source8.colorSpace());
    source16.fill(QColor::fromRgbF(96.0 / 255.0,
                                   137.0 / 255.0,
                                   183.0 / 255.0,
                                   211.0 / 255.0));

    LayerNode base;
    base.type = LayerType::BaseImage;
    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    ExposureParameters parameters;
    parameters.exposure = 0.75;
    exposure.setExposureParameters(parameters);

    const QImage result8 = ImageProcessor::render(
        source8, {exposure, base}, nullptr, source8.size(),
        ColourProcessingCompatibility::ManagedV1).convertToFormat(QImage::Format_RGBA8888);
    const QImage result16 = ImageProcessor::render(
        source16, {exposure, base}, nullptr, source16.size(),
        ColourProcessingCompatibility::ManagedV1).convertToFormat(QImage::Format_RGBA64);
    QVERIFY(!result8.isNull());
    QVERIFY(!result16.isNull());
    const QColor eight = result8.pixelColor(0, 0);
    const QColor sixteen = result16.pixelColor(0, 0);
    QVERIFY(std::abs(eight.redF() - sixteen.redF()) <= 3.0 / 255.0);
    QVERIFY(std::abs(eight.greenF() - sixteen.greenF()) <= 3.0 / 255.0);
    QVERIFY(std::abs(eight.blueF() - sixteen.blueF()) <= 3.0 / 255.0);
    QVERIFY(std::abs(eight.alphaF() - sixteen.alphaF()) <= 1.0 / 255.0);
}

void ColourManagementTests::managedWideGamutAdjustmentPreservesAlphaAndWorkingProfile()
{
    QImage source = patternedImage(QSize(9, 7), QColorSpace(QColorSpace::DisplayP3));
    LayerNode base;
    base.type = LayerType::BaseImage;
    LayerNode whiteBalance;
    whiteBalance.type = LayerType::Adjustment;
    WhiteBalanceParameters parameters;
    parameters.temperature = 28.0;
    parameters.tint = -13.0;
    whiteBalance.setWhiteBalanceParameters(parameters);

    const QImage result = ImageProcessor::render(
        source, {whiteBalance, base}, nullptr, source.size(),
        ColourProcessingCompatibility::ManagedV1).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!result.isNull());
    QCOMPARE(result.colorSpace(), source.colorSpace());
    const QImage original = source.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < result.height(); ++y) {
        const uchar *before = original.constScanLine(y);
        const uchar *after = result.constScanLine(y);
        for (int x = 0; x < result.width(); ++x) {
            QCOMPARE(after[x * 4 + 3], before[x * 4 + 3]);
        }
    }
}


void ColourManagementTests::ocioSchemaFourPersistsAndOlderSchemasMigratePresentationDisabled()
{
    DocumentColourState state = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::SRgb));
    OcioConfigReference reference;
    reference.source = OcioConfigSource::BuiltIn;
    reference.identifier = QStringLiteral("ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5");
    reference.displayName = QStringLiteral("ACES CG Config 2.0");
    reference.version = QStringLiteral("2.5 / OCIO 2.5.2");
    reference.fingerprint = QCryptographicHash::hash(
        QByteArrayLiteral("test-ocio-config"), QCryptographicHash::Sha256);
    reference.iccBridgeSpace = QStringLiteral("sRGB - Texture");
    state.ocioConfig = reference;
    state.revision = 9;

    QString error;
    QVERIFY2(state.isSafe(&error), qPrintable(error));
    const QJsonObject encoded = state.toJson();
    QCOMPARE(encoded.value(QStringLiteral("schema")).toInt(), 4);
    QVERIFY(encoded.value(QStringLiteral("presentationColourManagementEnabled")).toBool());
    const auto restored = DocumentColourState::fromJson(encoded, &error);
    QVERIFY2(restored.has_value(), qPrintable(error));
    QVERIFY(restored->ocioConfig == reference);
    QVERIFY(restored->presentationColourManagementEnabled);
    QCOMPARE(restored->revision, quint64(9));

    QJsonObject schemaThree = encoded;
    schemaThree.insert(QStringLiteral("schema"), 3);
    schemaThree.remove(QStringLiteral("presentationColourManagementEnabled"));
    schemaThree.remove(QStringLiteral("fingerprint"));
    const auto migratedThree = DocumentColourState::fromJson(schemaThree, &error);
    QVERIFY2(migratedThree.has_value(), qPrintable(error));
    QVERIFY(!migratedThree->presentationColourManagementEnabled);
    QVERIFY(migratedThree->displayTransform.kind == DisplayTransformKind::SystemIcc);

    QJsonObject schemaTwo = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::SRgb)).toJson();
    schemaTwo.insert(QStringLiteral("schema"), 2);
    schemaTwo.remove(QStringLiteral("ocioConfig"));
    schemaTwo.remove(QStringLiteral("fingerprint"));
    const auto migrated = DocumentColourState::fromJson(schemaTwo, &error);
    QVERIFY2(migrated.has_value(), qPrintable(error));
    QVERIFY(!migrated->ocioConfig.isConfigured());
    QVERIFY(migrated->workingSpace.kind != ColourSpaceKind::Ocio);
    QVERIFY(!migrated->presentationColourManagementEnabled);
}

void ColourManagementTests::bundledAcesConfigurationCanBeInspected()
{
    if (!ocioSupportCompiled()) {
        QSKIP("OpenColorIO was not compiled into this build.");
    }
    const auto choices = ocioBuiltInConfigChoices();
    QVERIFY(choices.size() >= 2);
    OcioConfigInspection inspection;
    QString error;
    QVERIFY2(inspectOcioConfiguration(OcioConfigSource::BuiltIn,
                                      choices.first().uri,
                                      &inspection, &error),
             qPrintable(error));
    QVERIFY(inspection.available);
    QCOMPARE(inspection.reference.source, OcioConfigSource::BuiltIn);
    QCOMPARE(inspection.reference.fingerprint.size(), 32);
    QVERIFY(!inspection.defaultWorkingSpace.isEmpty());
    QVERIFY(!inspection.defaultDisplay.isEmpty());
    QVERIFY(!inspection.defaultView.isEmpty());
    const auto hasSpace = [&](const QString &name) {
        return std::any_of(inspection.colourSpaces.cbegin(),
                           inspection.colourSpaces.cend(),
                           [&](const OcioColourSpaceInfo &space) {
                               return space.name.compare(name, Qt::CaseInsensitive) == 0;
                           });
    };
    QVERIFY(hasSpace(QStringLiteral("ACEScg")));
    QVERIFY(hasSpace(QStringLiteral("ACES2065-1")));
    QVERIFY(!ocioWorkingColourSpaces(inspection).isEmpty());
    const auto exportSpaces = ocioExportColourSpaces(inspection);
    QVERIFY(exportSpaces.size() >= ocioWorkingColourSpaces(inspection).size());
    QVERIFY(std::none_of(exportSpaces.cbegin(), exportSpaces.cend(),
                         [](const ColourSpaceDescriptor &space) {
                             return !space.isValid() || space.isUntagged();
                         }));
    QVERIFY2(validateOcioDisplayView(inspection.reference,
                                     inspection.defaultWorkingSpace,
                                     inspection.defaultDisplay,
                                     inspection.defaultView,
                                     QString(), &error),
             qPrintable(error));
}

void ColourManagementTests::ocioWorkingSpacesExposeValidAdjustmentProxies()
{
    const QByteArray fingerprint = QCryptographicHash::hash(
        QByteArrayLiteral("proxy-config"), QCryptographicHash::Sha256);
    const ColourSpaceDescriptor acescg = ColourSpaceDescriptor::ocio(
        QStringLiteral("ocio://proxy"), fingerprint,
        QStringLiteral("ACEScg"), QStringLiteral("ACEScg"));
    const QColorSpace acescgProxy = ocioQtWorkingSpaceProxy(acescg);
    QVERIFY(acescgProxy.isValid());
    QCOMPARE(acescgProxy.transferFunction(),
             QColorSpace::TransferFunction::Linear);

    const ColourSpaceDescriptor linear709 = ColourSpaceDescriptor::ocio(
        QStringLiteral("ocio://proxy"), fingerprint,
        QStringLiteral("Linear Rec.709 (sRGB)"),
        QStringLiteral("Linear Rec.709 (sRGB)"));
    QCOMPARE(ocioQtWorkingSpaceProxy(linear709),
             QColorSpace(QColorSpace::SRgbLinear));

    const ColourSpaceDescriptor unsupported = ColourSpaceDescriptor::ocio(
        QStringLiteral("ocio://proxy"), fingerprint,
        QStringLiteral("Camera Log Encoding"),
        QStringLiteral("Camera Log Encoding"));
    QVERIFY(!ocioQtWorkingSpaceProxy(unsupported).isValid());
}

void ColourManagementTests::ocioCpuRoundTripPreservesAlpha()
{
    if (!ocioSupportCompiled()) {
        QSKIP("OpenColorIO was not compiled into this build.");
    }
    OcioConfigInspection inspection;
    QString error;
    QVERIFY2(inspectOcioConfiguration(OcioConfigSource::BuiltIn,
                                      ocioBuiltInConfigChoices().first().uri,
                                      &inspection, &error),
             qPrintable(error));
    const ColourSpaceDescriptor srgb = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    const ColourSpaceDescriptor acescg = ColourSpaceDescriptor::ocio(
        inspection.reference.identifier,
        inspection.reference.fingerprint,
        QStringLiteral("ACEScg"),
        QStringLiteral("ACEScg"));
    auto toAces = createOcioCpuTransform(inspection.reference, srgb, acescg, &error);
    QVERIFY2(toAces && toAces->isValid(), qPrintable(error));
    auto toSrgb = createOcioCpuTransform(inspection.reference, acescg, srgb, &error);
    QVERIFY2(toSrgb && toSrgb->isValid(), qPrintable(error));

    QImage source(4, 2, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    const QColor samples[] = {
        QColor(32, 64, 96, 0), QColor(96, 128, 160, 51),
        QColor(128, 128, 128, 127), QColor(200, 180, 140, 255),
        QColor(48, 80, 112, 13), QColor(110, 150, 190, 89),
        QColor(170, 150, 130, 201), QColor(224, 208, 192, 254)
    };
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, samples[y * source.width() + x]);
        }
    }
    QImage converted = source;
    QVERIFY2(applyOcioCpuTransform(&converted, *toAces, nullptr, &error),
             qPrintable(error));
    QCOMPARE(converted.colorSpace(), ocioQtWorkingSpaceProxy(acescg));
    QVERIFY2(applyOcioCpuTransform(&converted, *toSrgb, nullptr, &error),
             qPrintable(error));
    QCOMPARE(converted.colorSpace(), QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            const QColor before = source.pixelColor(x, y);
            const QColor after = converted.pixelColor(x, y);
            QCOMPARE(after.alpha(), before.alpha());
            QVERIFY(std::abs(after.red() - before.red()) <= 3);
            QVERIFY(std::abs(after.green() - before.green()) <= 3);
            QVERIFY(std::abs(after.blue() - before.blue()) <= 3);
        }
    }
}

void ColourManagementTests::ocioExportCanTargetNonWorkingConfigSpace()
{
    if (!ocioSupportCompiled()) {
        QSKIP("OpenColorIO was not compiled into this build.");
    }
    OcioConfigInspection inspection;
    QString error;
    QVERIFY2(inspectOcioConfiguration(OcioConfigSource::BuiltIn,
                                      ocioBuiltInConfigChoices().first().uri,
                                      &inspection, &error),
             qPrintable(error));
    const ColourSpaceDescriptor acescg = ColourSpaceDescriptor::ocio(
        inspection.reference.identifier,
        inspection.reference.fingerprint,
        QStringLiteral("ACEScg"),
        QStringLiteral("ACEScg"));
    const QVector<ColourSpaceDescriptor> destinations =
        ocioExportColourSpaces(inspection);
    const auto found = std::find_if(
        destinations.cbegin(), destinations.cend(),
        [&](const ColourSpaceDescriptor &space) {
            return space.ocioSpace.compare(acescg.ocioSpace,
                                           Qt::CaseInsensitive) != 0
                && !ocioQtWorkingSpaceProxy(space).isValid();
        });
    if (found == destinations.cend()) {
        QSKIP("The bundled configuration exposes no non-proxy export space.");
    }

    QImage image(QSize(7, 3), QImage::Format_RGBA64);
    image.setColorSpace(ocioQtWorkingSpaceProxy(acescg));
    for (int y = 0; y < image.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>(7000 + x * 4000),
                static_cast<quint16>(9000 + y * 8000),
                static_cast<quint16>(11000 + (x + y) * 3000),
                static_cast<quint16>((x * 9000 + y * 3000) & 0xffff));
        }
    }
    QVector<quint16> alpha;
    alpha.reserve(image.width() * image.height());
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgba64 *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) alpha.push_back(row[x].alpha());
    }

    QVERIFY2(transformImageColourSpace(
                 &image, inspection.reference, acescg, *found,
                 ColourTransformPurpose::WorkingToOutput,
                 ColourRenderingIntent::RelativeColorimetric, true,
                 nullptr, &error),
             qPrintable(error));
    QVERIFY(!image.colorSpace().isValid());
    int index = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgba64 *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            QCOMPARE(row[x].alpha(), alpha.at(index++));
        }
    }
}

void ColourManagementTests::ocioDisplayViewIsDistinctAndDeterministic()
{
    if (!ocioSupportCompiled()) {
        QSKIP("OpenColorIO was not compiled into this build.");
    }
    const auto choices = ocioBuiltInConfigChoices();
    QVERIFY(!choices.isEmpty());
    OcioConfigInspection inspection;
    QString error;
    QVERIFY2(inspectOcioConfiguration(OcioConfigSource::BuiltIn,
                                      choices.first().uri,
                                      &inspection, &error),
             qPrintable(error));
    QVERIFY(inspection.available);
    QVERIFY(!inspection.defaultDisplay.isEmpty());
    QVERIFY(!inspection.defaultView.isEmpty());

    const ColourSpaceDescriptor source = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    const auto display = createOcioDisplayTransform(
        inspection.reference, source,
        inspection.defaultDisplay, inspection.defaultView, {}, &error);
    QVERIFY2(display, qPrintable(error));
    QVERIFY(display->isValid());

    const QImage sourceImage = patternedImage(
        QSize(11, 7), QColorSpace(QColorSpace::SRgb));
    QImage first = sourceImage;
    QImage second = sourceImage;
    QVERIFY2(applyOcioDisplayTransform(&first, *display, nullptr, &error),
             qPrintable(error));
    QVERIFY2(applyOcioDisplayTransform(&second, *display, nullptr, &error),
             qPrintable(error));
    QVERIFY(exactImagesEqual(first, second));
    for (int y = 0; y < sourceImage.height(); ++y) {
        for (int x = 0; x < sourceImage.width(); ++x) {
            QCOMPARE(first.pixelColor(x, y).alpha(),
                     sourceImage.pixelColor(x, y).alpha());
        }
    }

    DocumentColourState state = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::SRgb));
    state.ocioConfig = inspection.reference;
    state.presentationColourManagementEnabled = true;
    state.displayTransform.kind = DisplayTransformKind::OcioView;
    state.displayTransform.profile = ColourSpaceDescriptor::untagged();
    state.displayTransform.ocioConfigId = inspection.reference.identifier;
    state.displayTransform.ocioConfigFingerprint = inspection.reference.fingerprint;
    state.displayTransform.ocioDisplay = inspection.defaultDisplay;
    state.displayTransform.ocioView = inspection.defaultView;
    MonitorProfileInfo monitor;
    monitor.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    const auto presentation = createDisplayColourTransform(state, monitor, &error);
    QVERIFY2(presentation, qPrintable(error));
    const auto gpuLut = presentation->gpuLutData(&error);
    QVERIFY2(gpuLut, qPrintable(error));
    QVERIFY(gpuLut->isValid());
    QCOMPARE(gpuLut->edgeSize, 65);
    QVERIFY(!gpuLut->fingerprint.isEmpty());
    QVERIFY(gpuLut->referenceMaximumDifference >= 0);
    QVERIFY(gpuLut->referenceMaximumDifference <= 4);
}

void ColourManagementTests::changedOcioFingerprintRequiresExplicitRelink()
{
    if (!ocioSupportCompiled()) {
        QSKIP("OpenColorIO was not compiled into this build.");
    }
    OcioConfigInspection inspection;
    QString error;
    QVERIFY2(inspectOcioConfiguration(OcioConfigSource::BuiltIn,
                                      ocioBuiltInConfigChoices().first().uri,
                                      &inspection, &error),
             qPrintable(error));
    OcioConfigReference changed = inspection.reference;
    changed.fingerprint[0] = static_cast<char>(changed.fingerprint.at(0) ^ 0x5a);
    OcioConfigInspection resolved;
    QVERIFY2(resolveOcioConfiguration(changed, &resolved, &error), qPrintable(error));
    QVERIFY(!resolved.fingerprintMatchesSavedReference);
    QVERIFY(!resolved.warning.isEmpty());

    const ColourSpaceDescriptor source = ColourSpaceDescriptor::ocio(
        changed.identifier, changed.fingerprint, QStringLiteral("ACEScg"));
    const ColourSpaceDescriptor target = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    auto transform = createOcioCpuTransform(changed, source, target, &error);
    QVERIFY(!transform);
    QVERIFY(error.contains(QStringLiteral("changed"), Qt::CaseInsensitive)
            || error.contains(QStringLiteral("fingerprint"), Qt::CaseInsensitive));
}


void ColourManagementTests::ocioConfigurationPersistsAcrossProjectAndColdStorage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(12, 10);
    settings.bitDepth = 8;
    settings.colourSpace = QColorSpace(QColorSpace::SRgb);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));

    DocumentColourState state = document.colourState();
    state.ocioConfig.source = OcioConfigSource::BuiltIn;
    state.ocioConfig.identifier = QStringLiteral(
        "ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5");
    state.ocioConfig.displayName = QStringLiteral("ACES CG Config 2.0");
    state.ocioConfig.version = QStringLiteral("2.5 / OCIO 2.5.2");
    state.ocioConfig.fingerprint = QCryptographicHash::hash(
        QByteArrayLiteral("persistent-config"), QCryptographicHash::Sha256);
    state.ocioConfig.iccBridgeSpace = QStringLiteral("sRGB - Texture");
    state.displayTransform.kind = DisplayTransformKind::OcioView;
    state.displayTransform.profile = ColourSpaceDescriptor::untagged();
    state.displayTransform.ocioConfigId = state.ocioConfig.identifier;
    state.displayTransform.ocioConfigFingerprint = state.ocioConfig.fingerprint;
    state.displayTransform.ocioDisplay = QStringLiteral("sRGB");
    state.displayTransform.ocioView = QStringLiteral("ACES 2.0 - SDR 100 nits");
    state.presentationColourManagementEnabled = true;
    state.proofing.enabled = true;
    state.proofing.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::DisplayP3));
    state.proofing.renderingIntent = ColourRenderingIntent::Perceptual;
    state.proofing.blackPointCompensation = false;
    state.proofing.gamutWarning = true;
    ++state.revision;
    QVERIFY2(state.isSafe(&error), qPrintable(error));

    QVERIFY2(document.replaceStructuralState(
                 document.sourceImage(), document.layers(),
                 document.selectionMask().snapshot(),
                 document.horizontalGuides(), document.verticalGuides(),
                 document.resolutionX(), document.resolutionY(), state, &error),
             qPrintable(error));

    const QString projectPath = directory.filePath(QStringLiteral("ocio.vfxphoto"));
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));
    PhotoDocument restored;
    QVERIFY2(restored.loadProject(projectPath, &error), qPrintable(error));
    QVERIFY(restored.colourState().semanticallyEquals(state));
    QCOMPARE(restored.colourState().displayTransform.ocioView,
             state.displayTransform.ocioView);
    QVERIFY(restored.colourState().presentationColourManagementEnabled);
    QVERIFY(restored.colourState().proofing.enabled);
    QVERIFY(restored.colourState().proofing.gamutWarning);

    SessionCacheStore store(directory.path());
    QVERIFY(store.isAvailable());
    DocumentSession sourceSession;
    sourceSession.document() = document;
    sourceSession.refreshSummary();
    QString snapshotPath;
    qint64 snapshotBytes = 0;
    QVERIFY2(store.writeSnapshot(sourceSession, &snapshotPath,
                                 &snapshotBytes, &error), qPrintable(error));
    DocumentSession coldRestored;
    QVERIFY2(store.restoreSnapshot(snapshotPath, &coldRestored, &error),
             qPrintable(error));
    QVERIFY(coldRestored.document().colourState().semanticallyEquals(state));
}


void ColourManagementTests::presentationMetadataUpdateLeavesProcessingStateUntouched()
{
    NewDocumentSettings settings;
    settings.pixelSize = QSize(23, 17);
    settings.colourSpace = QColorSpace(QColorSpace::DisplayP3);
    PhotoDocument document;
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    document.setModified(false);

    const quint64 revision = document.colourStateRevision();
    const qint64 sourceKey = document.sourceImage().cacheKey();
    const QUuid baseId = document.baseLayerId();
    QVERIFY(!baseId.isNull());
    const qint64 layerKey = document.layerById(baseId).rasterImage.cacheKey();
    const QImage sourcePixels = document.sourceImage();

    DocumentColourState updated = document.colourState();
    updated.presentationColourManagementEnabled = false;
    updated.proofing.enabled = true;
    updated.proofing.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    updated.proofing.gamutWarning = true;
    updated.revision += 100;

    QVERIFY2(document.replacePresentationColourState(updated, &error),
             qPrintable(error));
    QCOMPARE(document.colourStateRevision(), revision);
    QCOMPARE(document.sourceImage().cacheKey(), sourceKey);
    QCOMPARE(document.layerById(baseId).rasterImage.cacheKey(), layerKey);
    QVERIFY(exactImagesEqual(document.sourceImage(), sourcePixels));
    QVERIFY(!document.colourState().presentationColourManagementEnabled);
    QVERIFY(document.colourState().proofing.enabled);
    QVERIFY(document.colourState().proofing.gamutWarning);
    QVERIFY(document.isModified());
}

void ColourManagementTests::presentationSettingsRemainIsolatedAcrossDocuments()
{
    NewDocumentSettings settings;
    settings.pixelSize = QSize(13, 11);
    settings.colourSpace = QColorSpace(QColorSpace::SRgb);
    PhotoDocument first;
    PhotoDocument second;
    QString error;
    QVERIFY2(first.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY2(second.createNewDocument(settings, &error), qPrintable(error));

    const quint64 firstRevision = first.colourStateRevision();
    const quint64 secondRevision = second.colourStateRevision();
    DocumentColourState firstPresentation = first.colourState();
    firstPresentation.proofing.enabled = true;
    firstPresentation.proofing.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::DisplayP3));
    firstPresentation.proofing.renderingIntent = ColourRenderingIntent::Perceptual;
    firstPresentation.proofing.gamutWarning = true;
    QVERIFY2(first.replacePresentationColourState(firstPresentation, &error),
             qPrintable(error));

    QVERIFY(first.colourState().proofing.enabled);
    QVERIFY(first.colourState().proofing.gamutWarning);
    QVERIFY(!second.colourState().proofing.enabled);
    QVERIFY(!second.colourState().proofing.gamutWarning);
    QCOMPARE(first.colourStateRevision(), firstRevision);
    QCOMPARE(second.colourStateRevision(), secondRevision);
    QVERIFY(!first.colourState().semanticallyEquals(second.colourState()));
}

void ColourManagementTests::displayTransformIsDeterministicAndPresentationOnly()
{
    DocumentColourState state = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::DisplayP3));
    state.presentationColourManagementEnabled = true;
    state.displayTransform.kind = DisplayTransformKind::SystemIcc;

    MonitorProfileInfo monitor;
    monitor.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    monitor.screenName = QStringLiteral("Deterministic test screen");
    monitor.sourceLabel = QStringLiteral("Test profile");
    monitor.source = MonitorProfileSource::ManualOverride;
    monitor.detected = true;
    monitor.reliable = true;
    monitor.fallback = false;

    QString error;
    const auto transform = createDisplayColourTransform(state, monitor, &error);
    QVERIFY2(transform, qPrintable(error));
    QVERIFY(!transform->isIdentity());
    QVERIFY(transform->status().active);

    const QImage source = patternedImage(QSize(19, 13),
                                         QColorSpace(QColorSpace::DisplayP3));
    const QImage untouched = source;
    QImage first = source;
    QImage second = source;
    QVERIFY2(transform->apply(&first, nullptr, &error), qPrintable(error));
    QVERIFY2(transform->apply(&second, nullptr, &error), qPrintable(error));
    QVERIFY(exactImagesEqual(first, second));
    QVERIFY(exactImagesEqual(source, untouched));
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            QCOMPARE(first.pixelColor(x, y).alpha(), source.pixelColor(x, y).alpha());
        }
    }

    state.presentationColourManagementEnabled = false;
    const auto identity = createDisplayColourTransform(state, monitor, &error);
    QVERIFY2(identity, qPrintable(error));
    QVERIFY(identity->isIdentity());
    QImage identityResult = source;
    QVERIFY(identity->apply(&identityResult, nullptr, &error));
    QVERIFY(exactImagesEqual(identityResult, source));
}

void ColourManagementTests::displayGpuLatticeIsDeterministicAndMatchesCpuReference()
{
    DocumentColourState state = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::DisplayP3));
    state.presentationColourManagementEnabled = true;
    state.displayTransform.kind = DisplayTransformKind::SystemIcc;

    MonitorProfileInfo monitor;
    monitor.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    monitor.screenName = QStringLiteral("GPU lattice test screen");
    monitor.sourceLabel = QStringLiteral("sRGB test monitor");
    monitor.source = MonitorProfileSource::ManualOverride;
    monitor.detected = true;
    monitor.reliable = true;
    monitor.fallback = false;

    QString error;
    const auto transform = createDisplayColourTransform(state, monitor, &error);
    QVERIFY2(transform, qPrintable(error));
    const auto first = transform->gpuLutData(&error);
    QVERIFY2(first, qPrintable(error));
    const auto second = transform->gpuLutData(&error);
    QVERIFY2(second, qPrintable(error));
    QVERIFY(first.get() == second.get());
    const auto duplicateTransform = createDisplayColourTransform(
        state, monitor, &error);
    QVERIFY2(duplicateTransform, qPrintable(error));
    const auto sharedAcrossTransforms = duplicateTransform->gpuLutData(&error);
    QVERIFY2(sharedAcrossTransforms, qPrintable(error));
    QVERIFY(first.get() == sharedAcrossTransforms.get());
    QCOMPARE(first->edgeSize, 65);
    QVERIFY(first->isValid());
    QVERIFY(!first->fingerprint.isEmpty());
    QCOMPARE(first->forwardRgba16f.size(), first->texelCount() * 4);
    QVERIFY(!first->gamutWarning);
    QVERIFY(first->gamutRoundTripRgba16f.isEmpty());
    QVERIFY(first->referenceMaximumDifference >= 0);
    QVERIFY(first->referenceMaximumDifference <= 4);

    const QImage source = patternedImage(QSize(23, 17),
                                         QColorSpace(QColorSpace::DisplayP3));
    QImage cpu = source;
    QVERIFY2(transform->apply(&cpu, nullptr, &error), qPrintable(error));
    const QImage cpu8 = cpu.convertToFormat(QImage::Format_RGBA8888);
    int maximumDifference = 0;
    for (int y = 0; y < source.height(); ++y) {
        const uchar *inputRow = source.constScanLine(y);
        const uchar *cpuRow = cpu8.constScanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            const int offset = x * 4;
            const auto mapped = sampleDisplayLut(
                first->forwardRgba16f, first->edgeSize,
                {inputRow[offset] / 255.0f,
                 inputRow[offset + 1] / 255.0f,
                 inputRow[offset + 2] / 255.0f});
            for (int component = 0; component < 3; ++component) {
                const int quantised = std::clamp(
                    static_cast<int>(std::lround(mapped[component] * 255.0f)),
                    0, 255);
                maximumDifference = std::max(
                    maximumDifference,
                    std::abs(quantised - int(cpuRow[offset + component])));
            }
            QCOMPARE(cpuRow[offset + 3], inputRow[offset + 3]);
        }
    }
    QVERIFY2(maximumDifference <= 4,
             qPrintable(QStringLiteral("CPU/lattice maximum difference was %1")
                            .arg(maximumDifference)));

    state.proofing.enabled = true;
    state.proofing.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    state.proofing.renderingIntent = ColourRenderingIntent::RelativeColorimetric;
    state.proofing.blackPointCompensation = true;
    state.proofing.gamutWarning = true;
    const auto proofed = createDisplayColourTransform(state, monitor, &error);
    QVERIFY2(proofed, qPrintable(error));
    const auto proofLut = proofed->gpuLutData(&error);
    QVERIFY2(proofLut, qPrintable(error));
    QVERIFY(proofLut->isValid());
    QVERIFY(proofLut->gamutWarning);
    QVERIFY(proofLut->referenceMaximumDifference >= 0);
    QVERIFY(proofLut->referenceMaximumDifference <= 4);
    QCOMPARE(proofLut->gamutRoundTripRgba16f.size(),
             proofLut->texelCount() * 4);
    QVERIFY(proofLut->fingerprint != first->fingerprint);
}

void ColourManagementTests::managedAdjustmentGpuLatticesAreDeterministic()
{
    const QColorSpace displayP3(QColorSpace::DisplayP3);
    QString error;
    const auto linearFirst = createManagedAdjustmentGpuLut(
        displayP3, AdjustmentProcessingDomain::LinearWorking, &error);
    QVERIFY2(linearFirst, qPrintable(error));
    const auto linearSecond = createManagedAdjustmentGpuLut(
        displayP3, AdjustmentProcessingDomain::LinearWorking, &error);
    QVERIFY2(linearSecond, qPrintable(error));
    QVERIFY(linearFirst.get() == linearSecond.get());
    QVERIFY(linearFirst->isValid());
    QCOMPARE(linearFirst->edgeSize, 65);
    QCOMPARE(linearFirst->domain, AdjustmentProcessingDomain::LinearWorking);
    QCOMPARE(linearFirst->workingToDomainRgba16f.size(),
             linearFirst->texelCount() * 4);
    QCOMPARE(linearFirst->domainToWorkingRgba16f.size(),
             linearFirst->texelCount() * 4);
    QVERIFY(linearFirst->referenceMaximumDifference >= 0);
    QVERIFY(linearFirst->referenceMaximumDifference <= 4);

    const auto encodedFirst = createManagedAdjustmentGpuLut(
        displayP3, AdjustmentProcessingDomain::EncodedSrgb, &error);
    QVERIFY2(encodedFirst, qPrintable(error));
    const auto encodedSecond = createManagedAdjustmentGpuLut(
        displayP3, AdjustmentProcessingDomain::EncodedSrgb, &error);
    QVERIFY2(encodedSecond, qPrintable(error));
    QVERIFY(encodedFirst.get() == encodedSecond.get());
    QVERIFY(encodedFirst->isValid());
    QCOMPARE(encodedFirst->edgeSize, 65);
    QCOMPARE(encodedFirst->domain, AdjustmentProcessingDomain::EncodedSrgb);
    QVERIFY(encodedFirst->fingerprint != linearFirst->fingerprint);
    QVERIFY(encodedFirst->referenceMaximumDifference >= 0);
    QVERIFY(encodedFirst->referenceMaximumDifference <= 4);

    const auto identity = createManagedAdjustmentGpuLut(
        QColorSpace(QColorSpace::SRgb),
        AdjustmentProcessingDomain::EncodedSrgb, &error);
    QVERIFY2(identity, qPrintable(error));
    QVERIFY(identity->isValid());
    QVERIFY(identity->referenceMaximumDifference >= 0);
    QVERIFY(identity->referenceMaximumDifference <= 4);
}

void ColourManagementTests::exportCapabilitiesAreExplicit()
{
    const ImageExportCapabilities png = imageExportCapabilitiesForPath(
        QStringLiteral("output.png"));
    QVERIFY(png.valid);
    QVERIFY(png.supportsAlpha);
    QVERIFY(png.supportsSixteenBit);
    QVERIFY(png.supportsIccProfile);

    const ImageExportCapabilities jpeg = imageExportCapabilitiesForPath(
        QStringLiteral("output.JPEG"));
    QVERIFY(jpeg.valid);
    QVERIFY(!jpeg.supportsAlpha);
    QVERIFY(!jpeg.supportsSixteenBit);
    QVERIFY(jpeg.supportsIccProfile);
    QVERIFY(jpeg.supportsQuality);

    const ImageExportCapabilities tga = imageExportCapabilitiesForPath(
        QStringLiteral("output.tga"));
    QVERIFY(tga.valid);
    QVERIFY(tga.supportsAlpha);
    QVERIFY(!tga.supportsSixteenBit);
    QVERIFY(!tga.supportsIccProfile);

    const ImageExportCapabilities tiff = imageExportCapabilitiesForPath(
        QStringLiteral("output.tif"));
    QVERIFY(tiff.valid);
    QVERIFY(tiff.supportsAlpha);
    QVERIFY(tiff.supportsSixteenBit);
    QVERIFY(tiff.supportsIccProfile);

    const ImageExportCapabilities webp = imageExportCapabilitiesForPath(
        QStringLiteral("output.webp"));
    QVERIFY(webp.valid);
    QVERIFY(webp.supportsAlpha);
    QVERIFY(!webp.supportsSixteenBit);
    QVERIFY(!webp.supportsIccProfile);
    QVERIFY(webp.supportsQuality);

    const ImageExportCapabilities bmp = imageExportCapabilitiesForPath(
        QStringLiteral("output.bmp"));
    QVERIFY(bmp.valid);
    QVERIFY(!bmp.supportsAlpha);
    QVERIFY(!bmp.supportsSixteenBit);
    QVERIFY(!bmp.supportsIccProfile);

    QVERIFY(!imageExportCapabilitiesForPath(
        QStringLiteral("output.unknown")).valid);
}

void ColourManagementTests::blueNoiseQuantisationIsDeterministicAndAlphaSafe()
{
    std::array<bool, 4096> seenRanks {};
    for (const std::uint16_t rank : BlueNoiseRanks64) {
        const std::size_t index = static_cast<std::size_t>(rank);
        QVERIFY(index < seenRanks.size());
        QVERIFY(!seenRanks[index]);
        seenRanks[index] = true;
    }
    QVERIFY(std::all_of(seenRanks.cbegin(), seenRanks.cend(),
                        [](const bool seen) { return seen; }));

    QImage source(QSize(64, 64), QImage::Format_RGBA64);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < source.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const quint16 value = static_cast<quint16>(
                (static_cast<quint32>(x + y * source.width()) * 65535u)
                / static_cast<quint32>(source.width() * source.height() - 1));
            const quint16 alpha = static_cast<quint16>(
                (static_cast<quint32>(x) * 65535u)
                / static_cast<quint32>(source.width() - 1));
            row[x] = QRgba64::fromRgba64(value, value, value, alpha);
        }
    }

    const QImage first = quantizeExportToEightBit(
        source, ImageExportDither::BlueNoise64, 12345u);
    const QImage second = quantizeExportToEightBit(
        source, ImageExportDither::BlueNoise64, 12345u);
    const QImage nearest = quantizeExportToEightBit(
        source, ImageExportDither::None, 12345u);
    QVERIFY(exactImagesEqual(first, second));
    QVERIFY(!exactImagesEqual(first, nearest));
    QCOMPARE(first.format(), QImage::Format_RGBA8888);
    QCOMPARE(first.colorSpace(), source.colorSpace());

    for (int y = 0; y < first.height(); ++y) {
        const auto *src = reinterpret_cast<const QRgba64 *>(source.constScanLine(y));
        const uchar *dst = first.constScanLine(y);
        for (int x = 0; x < first.width(); ++x) {
            const int offset = x * 4;
            const quint8 expectedAlpha = static_cast<quint8>(
                (static_cast<quint32>(src[x].alpha()) * 255u + 32767u)
                / 65535u);
            QCOMPARE(dst[offset + 3], expectedAlpha);
            QCOMPARE(dst[offset + 0], dst[offset + 1]);
            QCOMPARE(dst[offset + 1], dst[offset + 2]);
        }
    }
    QCOMPARE(first.constScanLine(0)[0], quint8(0));
    // At x=0 Alpha is exactly zero for every row. The RGB reference must
    // nevertheless remain present and be quantised rather than replaced.
    const uchar *hidden = first.constScanLine(17);
    QVERIFY(hidden[0] > 0);
    QCOMPARE(hidden[3], quint8(0));
    const uchar *last = first.constScanLine(first.height() - 1)
        + (first.width() - 1) * 4;
    QCOMPARE(last[0], quint8(255));
}

void ColourManagementTests::colourManagedExportIgnoresPresentationState()
{
    DocumentColourState state = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::DisplayP3));
    state.output.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    state.output.embedProfile = true;

    QImage source(QSize(19, 11), QImage::Format_RGBA64);
    source.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    for (int y = 0; y < source.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>((x * 4093 + y * 251) & 0xffff),
                static_cast<quint16>((x * 877 + y * 4099) & 0xffff),
                static_cast<quint16>((x * 1999 + y * 313) & 0xffff),
                static_cast<quint16>((x * 3571 + y * 911) & 0xffff));
        }
    }

    ImageExportRequest request;
    request.filePath = QStringLiteral("reference.png");
    request.bitDepth = ImageExportBitDepth::Eight;
    request.dither = ImageExportDither::BlueNoise64;
    request.output = state.output;
    request.convertToOutputProfile = true;

    PreparedImageExport ordinary;
    QString error;
    QVERIFY2(prepareImageExport(source, state, request, &ordinary,
                                nullptr, &error), qPrintable(error));
    QVERIFY(ordinary.convertedToOutputProfile);
    QVERIFY(ordinary.profileEmbedded);
    QVERIFY(ordinary.dithered);
    QCOMPARE(ordinary.image.format(), QImage::Format_RGBA8888);
    QCOMPARE(ordinary.image.colorSpace(), QColorSpace(QColorSpace::SRgb));

    DocumentColourState presentationChanged = state;
    presentationChanged.presentationColourManagementEnabled = true;
    presentationChanged.displayTransform.kind = DisplayTransformKind::IccProfile;
    presentationChanged.displayTransform.profile =
        ColourSpaceDescriptor::fromQColorSpace(QColorSpace(QColorSpace::AdobeRgb));
    presentationChanged.proofing.enabled = true;
    presentationChanged.proofing.profile =
        ColourSpaceDescriptor::fromQColorSpace(QColorSpace(QColorSpace::ProPhotoRgb));
    presentationChanged.proofing.gamutWarning = true;

    PreparedImageExport proofed;
    QVERIFY2(prepareImageExport(source, presentationChanged, request, &proofed,
                                nullptr, &error), qPrintable(error));
    QVERIFY(exactImagesEqual(ordinary.image, proofed.image));
    QCOMPARE(ordinary.outputProfile.stableFingerprint(),
             proofed.outputProfile.stableFingerprint());

    for (int y = 0; y < source.height(); ++y) {
        const auto *src = reinterpret_cast<const QRgba64 *>(source.constScanLine(y));
        const uchar *dst = ordinary.image.constScanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            const quint8 expectedAlpha = static_cast<quint8>(
                (static_cast<quint32>(src[x].alpha()) * 255u + 32767u)
                / 65535u);
            QCOMPARE(dst[x * 4 + 3], expectedAlpha);
        }
    }
}

void ColourManagementTests::formatSpecificExportHandlingIsExplicit()
{
    DocumentColourState state = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::SRgb));
    state.output.profile = state.workingSpace;
    state.output.embedProfile = true;

    QImage source(QSize(2, 1), QImage::Format_RGBA64);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    auto *pixels = reinterpret_cast<QRgba64 *>(source.scanLine(0));
    pixels[0] = QRgba64::fromRgba64(65535, 0, 0, 0);
    pixels[1] = QRgba64::fromRgba64(65535, 0, 0, 32768);

    ImageExportRequest bmp;
    bmp.filePath = QStringLiteral("flatten.bmp");
    bmp.bitDepth = ImageExportBitDepth::Eight;
    bmp.dither = ImageExportDither::None;
    bmp.convertToOutputProfile = false;
    bmp.output = state.output;
    bmp.matteColour = QColor(0, 0, 255);

    PreparedImageExport flattened;
    QString error;
    QVERIFY2(prepareImageExport(source, state, bmp, &flattened,
                                nullptr, &error), qPrintable(error));
    QVERIFY(flattened.flattenedTransparency);
    QVERIFY(!flattened.profileEmbedded);
    QCOMPARE(flattened.image.format(), QImage::Format_RGB888);
    QVERIFY(!flattened.image.colorSpace().isValid());
    QVERIFY(!flattened.warnings.isEmpty());
    const uchar *flat = flattened.image.constScanLine(0);
    QCOMPARE(flat[0], quint8(0));
    QCOMPARE(flat[1], quint8(0));
    QCOMPARE(flat[2], quint8(255));
    QVERIFY(flat[3] >= 127 && flat[3] <= 129);
    QCOMPARE(flat[4], quint8(0));
    QVERIFY(flat[5] >= 127 && flat[5] <= 128);

    ImageExportRequest tga = bmp;
    tga.filePath = QStringLiteral("tagged.tga");
    tga.matteColour = Qt::white;
    PreparedImageExport untaggedTga;
    QVERIFY2(prepareImageExport(source, state, tga, &untaggedTga,
                                nullptr, &error), qPrintable(error));
    QVERIFY(!untaggedTga.flattenedTransparency);
    QVERIFY(!untaggedTga.profileEmbedded);
    QVERIFY(!untaggedTga.image.colorSpace().isValid());
    QVERIFY(!untaggedTga.warnings.isEmpty());

    DocumentColourState untagged = state;
    untagged.workingSpace = ColourSpaceDescriptor::untagged();
    untagged.output.profile = ColourSpaceDescriptor::untagged();
    QImage untaggedPixels = source;
    untaggedPixels.setColorSpace(QColorSpace());
    ImageExportRequest keep;
    keep.filePath = QStringLiteral("untagged.png");
    keep.bitDepth = ImageExportBitDepth::Eight;
    keep.dither = ImageExportDither::None;
    keep.convertToOutputProfile = false;
    keep.output = untagged.output;
    keep.output.embedProfile = true;
    PreparedImageExport untaggedPng;
    QVERIFY2(prepareImageExport(untaggedPixels, untagged, keep,
                                &untaggedPng, nullptr, &error), qPrintable(error));
    QVERIFY(!untaggedPng.profileEmbedded);
    QVERIFY(!untaggedPng.image.colorSpace().isValid());
    QVERIFY(!untaggedPng.warnings.isEmpty());
}

void ColourManagementTests::exportCancellationStopsQuantisation()
{
    QImage source(QSize(64, 64), QImage::Format_RGBA64);
    source.fill(QColor(40, 80, 120, 200));
    std::atomic_bool cancelled {true};
    QVERIFY(quantizeExportToEightBit(source,
                                     ImageExportDither::BlueNoise64,
                                     7u,
                                     &cancelled).isNull());
}

void ColourManagementTests::sixteenBitPngExportRetainsPrecisionAndProfile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("sixteen-bit.png"));

    DocumentColourState state = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::SRgb));
    state.output.profile = state.workingSpace;
    state.output.embedProfile = true;

    QImage source(QSize(23, 7), QImage::Format_RGBA64);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < source.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>(x * 2003 + y * 17),
                static_cast<quint16>(x * 1091 + y * 301),
                static_cast<quint16>(x * 503 + y * 4093),
                static_cast<quint16>(65535 - x * 997));
        }
    }

    ImageExportRequest request;
    request.filePath = path;
    request.bitDepth = ImageExportBitDepth::Sixteen;
    request.dither = ImageExportDither::None;
    request.convertToOutputProfile = true;
    request.output = state.output;

    PreparedImageExport prepared;
    QString error;
    QVERIFY2(prepareImageExport(source, state, request, &prepared,
                                nullptr, &error), qPrintable(error));
    QCOMPARE(prepared.image.format(), QImage::Format_RGBA64);
    QVERIFY(prepared.profileEmbedded);
    QVERIFY2(writePreparedImageExport(path, prepared, 95, &error),
             qPrintable(error));

    QImageReader reader(path);
    const QImage restored = reader.read();
    QVERIFY2(!restored.isNull(), qPrintable(reader.errorString()));
    QVERIFY(restored.depth() > 32);
    QVERIFY(restored.colorSpace().isValid());
    QCOMPARE(restored.colorSpace(), QColorSpace(QColorSpace::SRgb));
    const QImage restored64 = restored.convertToFormat(QImage::Format_RGBA64);
    const auto *originalRow = reinterpret_cast<const QRgba64 *>(
        source.constScanLine(3));
    const auto *restoredRow = reinterpret_cast<const QRgba64 *>(
        restored64.constScanLine(3));
    QCOMPARE(restoredRow[9].red(), originalRow[9].red());
    QCOMPARE(restoredRow[9].green(), originalRow[9].green());
    QCOMPARE(restoredRow[9].blue(), originalRow[9].blue());
    QCOMPARE(restoredRow[9].alpha(), originalRow[9].alpha());
}

void ColourManagementTests::taggedJpegAndSixteenBitTiffRoundTripWhenAvailable()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    DocumentColourState state = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::DisplayP3));
    state.output.profile = state.workingSpace;
    state.output.embedProfile = true;

    QImage source(QSize(29, 9), QImage::Format_RGBA64);
    source.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    for (int y = 0; y < source.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>(2000 + x * 1703 + y * 41),
                static_cast<quint16>(4000 + x * 911 + y * 1709),
                static_cast<quint16>(6000 + x * 503 + y * 2027),
                static_cast<quint16>(10000 + x * 1501));
        }
    }

    const auto exercise = [&](const QString &fileName,
                              const ImageExportBitDepth depth,
                              const bool expectAlpha,
                              const bool expectSixteenBit) {
        const QString path = directory.filePath(fileName);
        const ImageExportCapabilities capabilities =
            imageExportCapabilitiesForPath(path);
        if (!imageExportWriterAvailable(capabilities)) return;

        ImageExportRequest request;
        request.filePath = path;
        request.bitDepth = depth;
        request.dither = ImageExportDither::None;
        request.convertToOutputProfile = true;
        request.output = state.output;
        request.matteColour = QColor(Qt::white);

        PreparedImageExport prepared;
        QString error;
        QVERIFY2(prepareImageExport(source, state, request, &prepared,
                                    nullptr, &error), qPrintable(error));
        QVERIFY(prepared.profileEmbedded);
        QCOMPARE(prepared.flattenedTransparency, !expectAlpha);
        QVERIFY2(writePreparedImageExport(path, prepared, 96, &error),
                 qPrintable(error));

        QImageReader reader(path);
        const QImage restored = reader.read();
        QVERIFY2(!restored.isNull(), qPrintable(reader.errorString()));
        QVERIFY(restored.colorSpace().isValid());
        QCOMPARE(restored.colorSpace(), QColorSpace(QColorSpace::DisplayP3));
        QCOMPARE(restored.hasAlphaChannel(), expectAlpha);
        if (expectSixteenBit) QVERIFY(restored.depth() > 32);
    };

    exercise(QStringLiteral("tagged.jpg"), ImageExportBitDepth::Eight,
             false, false);
    exercise(QStringLiteral("tagged-16.tiff"), ImageExportBitDepth::Sixteen,
             true, true);
}

void ColourManagementTests::outputDefaultsPersistWithoutProcessingRevision()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    PhotoDocument document;
    document.setSourceImage(patternedImage(
        QSize(13, 9), QColorSpace(QColorSpace::SRgb)));
    const quint64 revision = document.colourStateRevision();
    const qint64 sourceKey = document.sourceImage().cacheKey();

    OutputColourSettings output = document.colourState().output;
    output.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::DisplayP3));
    output.renderingIntent = ColourRenderingIntent::Perceptual;
    output.blackPointCompensation = false;
    output.embedProfile = true;
    QString error;
    QVERIFY2(document.replaceOutputColourSettings(output, &error),
             qPrintable(error));
    QCOMPARE(document.colourStateRevision(), revision);
    QCOMPARE(document.sourceImage().cacheKey(), sourceKey);
    QVERIFY(document.colourState().output == output);

    const QString projectPath = directory.filePath(QStringLiteral("output-state.vfxphoto"));
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));
    PhotoDocument restored;
    QVERIFY2(restored.loadProject(projectPath, &error), qPrintable(error));
    QVERIFY(restored.colourState().output == output);

    SessionCacheStore store(directory.path());
    QVERIFY(store.isAvailable());
    DocumentSession sourceSession;
    sourceSession.document().setSourceImage(patternedImage(
        QSize(7, 5), QColorSpace(QColorSpace::SRgb)));
    QVERIFY2(sourceSession.document().replaceOutputColourSettings(output, &error),
             qPrintable(error));
    sourceSession.refreshSummary();
    QString snapshotPath;
    qint64 snapshotBytes = 0;
    QVERIFY2(store.writeSnapshot(sourceSession, &snapshotPath, &snapshotBytes,
                                 &error), qPrintable(error));
    DocumentSession restoredSession;
    QVERIFY2(store.restoreSnapshot(snapshotPath, &restoredSession, &error),
             qPrintable(error));
    QVERIFY(restoredSession.document().colourState().output == output);
}


void ColourManagementTests::externalIccResourceWarningsDoNotDirtyProjects()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString profilePath = directory.filePath(QStringLiteral("working.icc"));
    const QByteArray profileBytes = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    QVERIFY(!profileBytes.isEmpty());
    QFile profileFile(profilePath);
    QVERIFY(profileFile.open(QIODevice::WriteOnly));
    QCOMPARE(profileFile.write(profileBytes), static_cast<qint64>(profileBytes.size()));
    profileFile.close();

    ColourSpaceDescriptor external;
    QString error;
    QVERIFY2(loadExternalIccProfile(profilePath, &external, &error),
             qPrintable(error));

    PhotoDocument document;
    document.setSourceImage(patternedImage(
        QSize(15, 11), QColorSpace(QColorSpace::SRgb)));
    DocumentColourState state = document.colourState();
    state.workingSpace = external;
    state.output.profile = external;
    QVERIFY2(document.replaceStructuralState(
                 document.sourceImage(), document.layers(),
                 document.selectionMask().snapshot(),
                 document.horizontalGuides(), document.verticalGuides(),
                 document.resolutionX(), document.resolutionY(), state, &error),
             qPrintable(error));

    const QString projectPath = directory.filePath(QStringLiteral("resource.vfxphoto"));
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));
    QVERIFY(QFile::remove(profilePath));

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(projectPath, &error), qPrintable(error));
    QVERIFY(restored.loadWarnings().isEmpty());
    QVERIFY(!restored.colourResourceWarnings().isEmpty());
    QVERIFY(!restored.isModified());
    QCOMPARE(restored.colourState().workingSpace.iccProfile, profileBytes);
    QCOMPARE(restored.sourceImage().colorSpace(),
             QColorSpace::fromIccProfile(profileBytes));

    const ColourResourceAuditReport audit =
        auditDocumentColourResources(restored.colourState());
    QVERIFY(!audit.isHealthy());
    QVERIFY(!audit.hasBlockingIssues());
    QVERIFY(audit.messages().join(QLatin1Char('\n')).contains(
        QStringLiteral("embedded"), Qt::CaseInsensitive));
}

void ColourManagementTests::changedExternalIccRequiresExplicitRelink()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString profilePath = directory.filePath(QStringLiteral("changed.icc"));
    const QByteArray originalBytes = QColorSpace(QColorSpace::SRgb).iccProfile();
    const QByteArray changedBytes = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    QVERIFY(!originalBytes.isEmpty());
    QVERIFY(!changedBytes.isEmpty());
    QVERIFY(originalBytes != changedBytes);

    QFile profileFile(profilePath);
    QVERIFY(profileFile.open(QIODevice::WriteOnly));
    QCOMPARE(profileFile.write(originalBytes), static_cast<qint64>(originalBytes.size()));
    profileFile.close();
    ColourSpaceDescriptor external;
    QString error;
    QVERIFY2(loadExternalIccProfile(profilePath, &external, &error),
             qPrintable(error));

    QVERIFY(profileFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(profileFile.write(changedBytes), static_cast<qint64>(changedBytes.size()));
    profileFile.close();

    DocumentColourState state = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::SRgb));
    state.output.profile = external;
    const ColourResourceAuditReport audit =
        auditDocumentColourResources(state);
    QVERIFY(!audit.isHealthy());
    QVERIFY(!audit.hasBlockingIssues());
    const QString details = audit.messages().join(QLatin1Char('\n'));
    QVERIFY(details.contains(QStringLiteral("changed"), Qt::CaseInsensitive));
    QVERIFY(details.contains(QStringLiteral("relink"), Qt::CaseInsensitive));
    QCOMPARE(state.output.profile.iccProfile, originalBytes);
}

void ColourManagementTests::missingOcioResourceIsReportedWithoutSubstitution()
{
    DocumentColourState state = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::SRgb));
    const QByteArray fingerprint = QCryptographicHash::hash(
        QByteArrayLiteral("missing-config"), QCryptographicHash::Sha256);
    state.ocioConfig.source = OcioConfigSource::ExternalFile;
    state.ocioConfig.identifier = QStringLiteral("/definitely/missing/config.ocio");
    state.ocioConfig.canonicalPath = state.ocioConfig.identifier;
    state.ocioConfig.displayName = QStringLiteral("Missing test config");
    state.ocioConfig.version = QStringLiteral("test");
    state.ocioConfig.fingerprint = fingerprint;
    state.ocioConfig.iccBridgeSpace = QStringLiteral("Utility - sRGB - Texture");
    state.workingSpace = ColourSpaceDescriptor::ocio(
        state.ocioConfig.identifier, fingerprint,
        QStringLiteral("ACEScg"), QStringLiteral("ACEScg"));
    state.output.profile = state.workingSpace;
    QString safetyError;
    QVERIFY2(state.isSafe(&safetyError), qPrintable(safetyError));

    const ColourResourceAuditReport audit =
        auditDocumentColourResources(state);
    QVERIFY(!audit.isHealthy());
    QVERIFY(audit.hasBlockingIssues());
    const QString details = audit.messages().join(QLatin1Char('\n'));
    QVERIFY(details.contains(QStringLiteral("unavailable"), Qt::CaseInsensitive)
            || details.contains(QStringLiteral("could not"), Qt::CaseInsensitive));
    QVERIFY(details.contains(QStringLiteral("substituted"), Qt::CaseInsensitive));
    QCOMPARE(state.workingSpace.ocioConfigFingerprint, fingerprint);
}

void ColourManagementTests::coldResidencyReauditsColourResources()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString profilePath = directory.filePath(QStringLiteral("cold.icc"));
    const QByteArray profileBytes = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    QVERIFY(!profileBytes.isEmpty());
    QFile profileFile(profilePath);
    QVERIFY(profileFile.open(QIODevice::WriteOnly));
    QCOMPARE(profileFile.write(profileBytes), static_cast<qint64>(profileBytes.size()));
    profileFile.close();

    ColourSpaceDescriptor external;
    QString error;
    QVERIFY2(loadExternalIccProfile(profilePath, &external, &error),
             qPrintable(error));

    DocumentSession session;
    session.document().setSourceImage(patternedImage(
        QSize(9, 7), QColorSpace(QColorSpace::SRgb)));
    DocumentColourState state = session.document().colourState();
    state.output.profile = external;
    QVERIFY2(session.document().replaceStructuralState(
                 session.document().sourceImage(), session.document().layers(),
                 session.document().selectionMask().snapshot(),
                 session.document().horizontalGuides(),
                 session.document().verticalGuides(),
                 session.document().resolutionX(), session.document().resolutionY(),
                 state, &error), qPrintable(error));

    SessionCacheStore store(directory.filePath(QStringLiteral("cache")));
    QVERIFY(store.isAvailable());
    QVERIFY2(session.evictToDisk(store, &error), qPrintable(error));
    QVERIFY(QFile::remove(profilePath));
    QVERIFY2(session.restoreFromDisk(store, &error), qPrintable(error));
    QVERIFY(!session.document().colourResourceWarnings().isEmpty());
    QVERIFY(session.document().loadWarnings().isEmpty());
    QCOMPARE(session.document().colourState().output.profile.iccProfile,
             profileBytes);
}

void ColourManagementTests::metadataOnlyColourUpdatesAreIdempotent()
{
    PhotoDocument document;
    document.setSourceImage(patternedImage(
        QSize(11, 8), QColorSpace(QColorSpace::SRgb)));
    document.setModified(false);
    const quint64 revision = document.colourStateRevision();
    const qint64 imageKey = document.sourceImage().cacheKey();
    QString error;

    QVERIFY2(document.replacePresentationColourState(
                 document.colourState(), &error), qPrintable(error));
    QVERIFY(!document.isModified());
    QCOMPARE(document.colourStateRevision(), revision);
    QCOMPARE(document.sourceImage().cacheKey(), imageKey);

    QVERIFY2(document.replaceOutputColourSettings(
                 document.colourState().output, &error), qPrintable(error));
    QVERIFY(!document.isModified());
    QCOMPARE(document.colourStateRevision(), revision);
    QCOMPARE(document.sourceImage().cacheKey(), imageKey);
}

void ColourManagementTests::malformedProjectSourceBase64IsRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString projectPath = directory.filePath(
        QStringLiteral("invalid-source-base64.vfxphoto"));

    PhotoDocument document;
    document.setSourceImage(patternedImage(
        QSize(8, 6), QColorSpace(QColorSpace::SRgb)));
    QString error;
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));

    QFile file(projectPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument encoded = QJsonDocument::fromJson(file.readAll());
    file.close();
    QVERIFY(encoded.isObject());
    QJsonObject root = encoded.object();
    QJsonObject source = root.value(QStringLiteral("source")).toObject();
    QString data = source.value(QStringLiteral("data")).toString();
    QVERIFY(!data.isEmpty());
    data.insert(data.size() / 2, QLatin1Char('!'));
    source.insert(QStringLiteral("data"), data);
    root.insert(QStringLiteral("source"), source);

    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray damaged = QJsonDocument(root).toJson(QJsonDocument::Compact);
    QCOMPARE(file.write(damaged), static_cast<qint64>(damaged.size()));
    file.close();

    PhotoDocument rejected;
    QVERIFY(!rejected.loadProject(projectPath, &error));
    QVERIFY(error.contains(QStringLiteral("base64"), Qt::CaseInsensitive));
    QVERIFY(!rejected.hasImage());
}

void ColourManagementTests::softProofSettingsInvalidateDisplayFingerprint()
{
    DocumentColourState state = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::SRgb));
    state.displayTransform.kind = DisplayTransformKind::SystemIcc;
    MonitorProfileInfo monitor;
    monitor.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    monitor.screenName = QStringLiteral("Proof test screen");
    monitor.sourceLabel = QStringLiteral("sRGB test");

    QString error;
    const auto ordinary = createDisplayColourTransform(state, monitor, &error);
    QVERIFY2(ordinary, qPrintable(error));

    state.proofing.enabled = true;
    state.proofing.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::DisplayP3));
    state.proofing.renderingIntent = ColourRenderingIntent::Perceptual;
    state.proofing.blackPointCompensation = false;
    state.proofing.gamutWarning = true;
    const auto proofed = createDisplayColourTransform(state, monitor, &error);
    QVERIFY2(proofed, qPrintable(error));
    QVERIFY(proofed->status().proofingActive);
    QVERIFY(proofed->status().gamutWarningActive);
    QVERIFY(ordinary->fingerprint() != proofed->fingerprint());

    QImage source = patternedImage(QSize(17, 9), QColorSpace(QColorSpace::SRgb));
    QImage first = source;
    QImage second = source;
    QVERIFY2(proofed->apply(&first, nullptr, &error), qPrintable(error));
    QVERIFY2(proofed->apply(&second, nullptr, &error), qPrintable(error));
    QVERIFY(exactImagesEqual(first, second));
}

void ColourManagementTests::monitorDiscoveryFallsBackDeterministically()
{
    const MonitorProfileInfo first = discoverMonitorProfile(nullptr);
    const MonitorProfileInfo second = discoverMonitorProfile(nullptr);
    QVERIFY(first.fallback);
    QVERIFY(first.profile.toQColorSpace().isValid());
    QCOMPARE(first.profile.toQColorSpace(), QColorSpace(QColorSpace::SRgb));
    QCOMPARE(first.stableFingerprint(), second.stableFingerprint());
}

QTEST_APPLESS_MAIN(ColourManagementTests)

#include "test_colour_management.moc"
