#include "ExportQueueController.h"
#include "ExportQueueCore.h"

#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

using namespace vfx;

class ExportQueueTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void cleanupTestCase();
    void stateNamesAreStable();
    void terminalStatesAreRecognised();
    void cancellationAndRemovalRulesAreBounded();
    void transitionsRejectTerminalResurrection();
    void pauseResumeTransitionsAreValid();
    void progressIsClampedAndRounded();
    void identifiersArePortableAndBounded();
    void invalidSnapshotsAreRejected();
    void pausedQueueKeepsAndCancelsPendingJobs();
    void unfinishedQueueIsBounded();
    void terminalHistoryIsBounded();
    void recoverySnapshotRoundTrips();
    void recoverySnapshotPreserves16BitHiddenRgb();
    void recoveredJobsRequireExplicitResume();
    void recoveredAskCollisionsRequireFreshConfirmation();
    void cleanShutdownCanPreserveJobs();
    void malformedRecoveryIsQuarantined();
    void wrongTypedRecoveryIsQuarantined();
    void resolvedPayloadMustMatchRecoverablePlan();
    void workerCompletionClassificationIsStrict();
    void deepLayerTreesAreRejectedWithoutRecursion();
    void progressIncludesConfiguredSkipOutputs();
    void recoveryResolutionMatchesQueuedSnapshot();
    void skipPolicyRechecksFilesystemAtExecution();

private:
    static ExportQueueEnqueueRequest validRequest(int index = 1);
    QTemporaryDir m_recoveryRoot;
};


void ExportQueueTests::initTestCase()
{
    QVERIFY(m_recoveryRoot.isValid());
    qputenv("VFXPHOTOLAB_EXPORT_QUEUE_RECOVERY_ROOT",
            m_recoveryRoot.path().toUtf8());
}

void ExportQueueTests::cleanup()
{
    QDir directory(ExportQueuePersistence::storageDirectory());
    if (directory.exists()) directory.removeRecursively();
}

void ExportQueueTests::cleanupTestCase()
{
    qunsetenv("VFXPHOTOLAB_EXPORT_QUEUE_RECOVERY_ROOT");
}

void ExportQueueTests::stateNamesAreStable()
{
    QCOMPARE(exportQueueJobStateName(ExportQueueJobState::Pending),
             QStringLiteral("Pending"));
    QCOMPARE(exportQueueJobStateName(ExportQueueJobState::Running),
             QStringLiteral("Running"));
    QCOMPARE(exportQueueJobStateName(ExportQueueJobState::Paused),
             QStringLiteral("Paused"));
    QCOMPARE(exportQueueJobStateName(ExportQueueJobState::Recovered),
             QStringLiteral("Recovered"));
    QCOMPARE(exportQueueJobStateName(ExportQueueJobState::CompletedWithIssues),
             QStringLiteral("Completed with issues"));
}

void ExportQueueTests::terminalStatesAreRecognised()
{
    QVERIFY(!exportQueueJobStateIsTerminal(ExportQueueJobState::Pending));
    QVERIFY(!exportQueueJobStateIsTerminal(ExportQueueJobState::Running));
    QVERIFY(!exportQueueJobStateIsTerminal(ExportQueueJobState::Paused));
    QVERIFY(!exportQueueJobStateIsTerminal(ExportQueueJobState::Recovered));
    QVERIFY(exportQueueJobStateIsTerminal(ExportQueueJobState::Completed));
    QVERIFY(exportQueueJobStateIsTerminal(
        ExportQueueJobState::CompletedWithIssues));
    QVERIFY(exportQueueJobStateIsTerminal(ExportQueueJobState::Failed));
    QVERIFY(exportQueueJobStateIsTerminal(ExportQueueJobState::Cancelled));
}

void ExportQueueTests::cancellationAndRemovalRulesAreBounded()
{
    QVERIFY(exportQueueJobStateCanCancel(ExportQueueJobState::Pending));
    QVERIFY(exportQueueJobStateCanCancel(ExportQueueJobState::Running));
    QVERIFY(exportQueueJobStateCanCancel(ExportQueueJobState::Paused));
    QVERIFY(exportQueueJobStateCanCancel(ExportQueueJobState::Recovered));
    QVERIFY(!exportQueueJobStateCanCancel(ExportQueueJobState::Completed));
    QVERIFY(!exportQueueJobStateCanRemove(ExportQueueJobState::Running));
    QVERIFY(exportQueueJobStateCanRemove(ExportQueueJobState::Completed));
    QVERIFY(exportQueueJobStateCanRemove(ExportQueueJobState::Failed));
    QVERIFY(exportQueueJobStateCanRemove(ExportQueueJobState::Cancelled));
}

void ExportQueueTests::transitionsRejectTerminalResurrection()
{
    QVERIFY(exportQueueJobStateCanTransition(
        ExportQueueJobState::Pending, ExportQueueJobState::Running));
    QVERIFY(exportQueueJobStateCanTransition(
        ExportQueueJobState::Running, ExportQueueJobState::Completed));
    QVERIFY(!exportQueueJobStateCanTransition(
        ExportQueueJobState::Completed, ExportQueueJobState::Running));
    QVERIFY(!exportQueueJobStateCanTransition(
        ExportQueueJobState::Failed, ExportQueueJobState::Pending));
    QVERIFY(exportQueueJobStateCanTransition(
        ExportQueueJobState::Recovered, ExportQueueJobState::Pending));
}

void ExportQueueTests::pauseResumeTransitionsAreValid()
{
    QVERIFY(exportQueueJobStateCanTransition(
        ExportQueueJobState::Running, ExportQueueJobState::Paused));
    QVERIFY(exportQueueJobStateCanTransition(
        ExportQueueJobState::Paused, ExportQueueJobState::Running));
    QVERIFY(exportQueueJobStateCanTransition(
        ExportQueueJobState::Paused, ExportQueueJobState::Cancelled));
    QVERIFY(!exportQueueJobStateCanTransition(
        ExportQueueJobState::Pending, ExportQueueJobState::Paused));
}

void ExportQueueTests::progressIsClampedAndRounded()
{
    QCOMPARE(exportQueueProgressPercent(0, 10), 0);
    QCOMPARE(exportQueueProgressPercent(1, 3), 33);
    QCOMPARE(exportQueueProgressPercent(2, 3), 67);
    QCOMPARE(exportQueueProgressPercent(10, 10), 100);
    QCOMPARE(exportQueueProgressPercent(15, 10), 100);
    QCOMPARE(exportQueueProgressPercent(-5, 10), 0);
    QCOMPARE(exportQueueProgressPercent(1, 0), 0);
}

void ExportQueueTests::identifiersArePortableAndBounded()
{
    QVERIFY(validExportQueueJobIdentifier(
        QStringLiteral("42e7a8dc-ff36-4c68-a2af-b8eff2bcbce1")));
    QVERIFY(validExportQueueJobIdentifier(QStringLiteral("job-123")));
    QVERIFY(!validExportQueueJobIdentifier(QStringLiteral("Job-123")));
    QVERIFY(!validExportQueueJobIdentifier(QStringLiteral(" job-123")));
    QVERIFY(!validExportQueueJobIdentifier(QStringLiteral("job_123")));
    QVERIFY(!validExportQueueJobIdentifier(QString(65, QLatin1Char('a'))));
}

ExportQueueEnqueueRequest ExportQueueTests::validRequest(const int index)
{
    ExportQueueEnqueueRequest request;
    request.title = QStringLiteral("Test export %1").arg(index);
    request.documentName = QStringLiteral("Queue Test");
    request.source = QImage(2, 2, QImage::Format_RGBA8888);
    request.source.fill(qRgba(10, 20, 30, 255));
    request.source.setPixelColor(0, 0, QColor(10, 20, 30, 0));
    request.source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    request.colourState = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::SRgb));
    request.processingCompatibility =
        request.colourState.processingCompatibility;
    request.plan.documentSize = request.source.size();
    request.plan.outputDirectory = QDir::tempPath();
    request.plan.timestampUtc = QDateTime::fromSecsSinceEpoch(
        1700000000, Qt::UTC);

    ProductionExportOutput configured;
    configured.id = QStringLiteral("output-%1").arg(index);
    configured.profileId = QStringLiteral("builtin-export-profile-test");
    configured.profileName = QStringLiteral("Test PNG");
    configured.profile.formatSuffix = QStringLiteral("png");
    configured.profile.convertToOutputProfile = false;
    configured.profile.output.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    configured.profile.namingTemplate = QStringLiteral("queue-test-{document}");
    configured.namingTemplate = QStringLiteral("queue-test-{document}");
    request.plan.documentName = QStringLiteral("queue-%1").arg(index);
    request.plan.workingSpaceName = QStringLiteral("sRGB");
    request.plan.outputs.push_back(configured);

    ResolvedProductionExportOutput output;
    output.profileId = configured.profileId;
    output.profileName = configured.profileName;
    output.id = configured.id;
    output.outputSize = request.source.size();
    output.request.filePath = QDir(request.plan.outputDirectory).filePath(
        QStringLiteral("queue-test-queue-%1.png").arg(index));
    output.request.quality = configured.profile.quality;
    output.request.bitDepth = configured.profile.bitDepth;
    output.request.dither = configured.profile.dither;
    output.request.alphaMode = configured.profile.alphaMode;
    output.request.convertToOutputProfile =
        configured.profile.convertToOutputProfile;
    output.request.output = configured.profile.output;
    output.request.matteColour = configured.profile.matteColour;
    request.outputs.push_back(output);
    return request;
}

void ExportQueueTests::invalidSnapshotsAreRejected()
{
    ExportQueueController controller;
    controller.setPaused(true);
    QString error;

    ExportQueueEnqueueRequest request = validRequest();
    request.source = QImage();
    QVERIFY(!controller.enqueue(request, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("invalid"), Qt::CaseInsensitive));

    request = validRequest();
    request.source = request.source.convertToFormat(QImage::Format_ARGB32);
    QVERIFY(!controller.enqueue(request, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("straight RGBA"),
                           Qt::CaseInsensitive));

    request = validRequest();
    request.plan.documentSize = QSize(3, 2);
    QVERIFY(!controller.enqueue(request, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("does not match"), Qt::CaseInsensitive));

    request = validRequest();
    request.outputs.clear();
    QVERIFY(!controller.enqueue(request, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("no resolved outputs"),
                           Qt::CaseInsensitive));

    request = validRequest();
    request.processingCompatibility = ColourProcessingCompatibility::LegacyV1;
    QVERIFY(!controller.enqueue(request, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("processing contract"),
                           Qt::CaseInsensitive));

    request = validRequest();
    LayerNode first;
    LayerNode duplicate;
    duplicate.id = first.id;
    request.layers = {first, duplicate};
    QVERIFY(!controller.enqueue(request, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("duplicate identifiers"),
                           Qt::CaseInsensitive));
}

void ExportQueueTests::pausedQueueKeepsAndCancelsPendingJobs()
{
    ExportQueueController controller;
    controller.setPaused(true);
    QString jobId;
    QString error;
    QVERIFY(controller.enqueue(validRequest(), &jobId, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(validExportQueueJobIdentifier(jobId));
    QVERIFY(controller.hasUnfinishedJobs());
    QVERIFY(!controller.hasActiveJob());
    QCOMPARE(controller.job(jobId).state, ExportQueueJobState::Pending);

    QVERIFY(controller.cancelJob(jobId));
    QCOMPARE(controller.job(jobId).state, ExportQueueJobState::Cancelled);
    QVERIFY(!controller.hasUnfinishedJobs());
    QVERIFY(controller.removeJob(jobId));
    QVERIFY(controller.jobs().isEmpty());
}

void ExportQueueTests::unfinishedQueueIsBounded()
{
    ExportQueueController controller;
    controller.setPaused(true);
    QString error;
    for (int i = 0; i < 16; ++i) {
        QVERIFY2(controller.enqueue(validRequest(i + 1), nullptr, &error),
                 qPrintable(error));
    }
    QVERIFY(!controller.enqueue(validRequest(17), nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("at most 16"), Qt::CaseInsensitive));
    QCOMPARE(controller.jobs().size(), 16);
    controller.cancelAll();
    QCOMPARE(controller.clearFinished(), 16);
}

void ExportQueueTests::terminalHistoryIsBounded()
{
    ExportQueueController controller;
    controller.setPaused(true);
    QString latestId;
    QString error;
    for (int i = 0; i < 140; ++i) {
        QVERIFY2(controller.enqueue(validRequest(i + 1), &latestId, &error),
                 qPrintable(error));
        QVERIFY(controller.cancelJob(latestId));
    }
    QCOMPARE(controller.jobs().size(), 128);
    QCOMPARE(controller.job(latestId).state, ExportQueueJobState::Cancelled);

    ExportQueueEnqueueRequest invalid = validRequest(141);
    invalid.outputs[0].request.quality = 7;
    QVERIFY(!controller.enqueue(invalid, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("profile"), Qt::CaseInsensitive));
    QCOMPARE(controller.jobs().size(), 128);
}


void ExportQueueTests::recoverySnapshotRoundTrips()
{
    ExportQueueEnqueueRequest request = validRequest(201);

    LayerNode featheredVector;
    featheredVector.id = QUuid::createUuid();
    featheredVector.type = LayerType::Vector;
    featheredVector.name = QStringLiteral("Queued Feather Vector");
    VectorShape vectorShape;
    vectorShape.type = VectorShapeType::Rectangle;
    vectorShape.bounds = QRectF(-1.0, 0.25, 2.5, 1.5);
    vectorShape.fill.enabled = true;
    vectorShape.fill.colour = QColor(210, 70, 35, 190);
    vectorShape.stroke.enabled = true;
    vectorShape.stroke.colour = QColor(25, 90, 220, 230);
    vectorShape.stroke.width = 0.5;
    vectorShape.normalise();
    featheredVector.vectorData.objects = {vectorShape};
    featheredVector.vectorData.featherRadius = 12.75;
    featheredVector.vectorData.normalise();
    QVERIFY(featheredVector.vectorData.isSafe());
    request.layers = {featheredVector};

    ProductionExportOutput disabledDraft;
    disabledDraft.id = QStringLiteral("disabled-draft");
    disabledDraft.enabled = false;
    request.plan.outputs.push_back(disabledDraft);
    const QString id = QStringLiteral("11111111-2222-3333-4444-555555555555");
    const QDateTime created = QDateTime::fromSecsSinceEpoch(1700000100, Qt::UTC);
    QString error;
    QVERIFY2(ExportQueuePersistence::writeJob(id, created, request, &error),
             qPrintable(error));
    QVERIFY(QFileInfo::exists(ExportQueuePersistence::jobPath(id)));

    QStringList warnings;
    const QVector<RecoverableExportQueueJob> jobs =
        ExportQueuePersistence::loadJobs(&warnings);
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
    QCOMPARE(jobs.size(), 1);
    QCOMPARE(jobs.constFirst().id, id);
    QCOMPARE(jobs.constFirst().createdUtc, created);
    QCOMPARE(jobs.constFirst().request.source.size(), request.source.size());
    QCOMPARE(jobs.constFirst().request.source.pixelColor(0, 0),
             request.source.pixelColor(0, 0));
    QCOMPARE(jobs.constFirst().request.layers.size(), 1);
    const LayerNode restoredVector = jobs.constFirst().request.layers.constFirst();
    QCOMPARE(restoredVector.type, LayerType::Vector);
    QCOMPARE(restoredVector.name, featheredVector.name);
    QVERIFY(restoredVector.vectorData == featheredVector.vectorData);
    QCOMPARE(restoredVector.vectorData.featherRadius, 12.75);
    QCOMPARE(jobs.constFirst().request.plan.outputs.size(), 1);
    QVERIFY(jobs.constFirst().request.plan.outputs.constFirst().enabled);
    QVERIFY(ExportQueuePersistence::removeJob(id, &error));
}

void ExportQueueTests::recoverySnapshotPreserves16BitHiddenRgb()
{
    ExportQueueEnqueueRequest request = validRequest(204);
    request.source = QImage(2, 2, QImage::Format_RGBA64);
    request.source.fill(QColor::fromRgba64(1000, 2000, 3000, 65535));
    request.source.setPixelColor(0, 0,
        QColor::fromRgba64(1234, 2345, 3456, 0));
    request.source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    request.plan.documentSize = request.source.size();
    request.outputs[0].outputSize = request.source.size();

    const QString id = QStringLiteral("22222222-3333-4444-5555-666666666666");
    QString error;
    QVERIFY2(ExportQueuePersistence::writeJob(
                 id, QDateTime::fromSecsSinceEpoch(1700000200, Qt::UTC),
                 request, &error),
             qPrintable(error));
    QStringList warnings;
    const QVector<RecoverableExportQueueJob> jobs =
        ExportQueuePersistence::loadJobs(&warnings);
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
    QCOMPARE(jobs.size(), 1);
    QCOMPARE(jobs.constFirst().request.source.format(), QImage::Format_RGBA64);
    const QRgba64 pixel = reinterpret_cast<const QRgba64 *>(
        jobs.constFirst().request.source.constScanLine(0))[0];
    QCOMPARE(pixel.red(), quint16(1234));
    QCOMPARE(pixel.green(), quint16(2345));
    QCOMPARE(pixel.blue(), quint16(3456));
    QCOMPARE(pixel.alpha(), quint16(0));
}

void ExportQueueTests::recoveredJobsRequireExplicitResume()
{
    const ExportQueueEnqueueRequest request = validRequest(202);
    const QString id = QStringLiteral("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    QString error;
    QVERIFY2(ExportQueuePersistence::writeJob(
                 id, QDateTime::currentDateTimeUtc(), request, &error),
             qPrintable(error));

    ExportQueueController controller;
    QStringList warnings;
    QCOMPARE(controller.restoreRecoverableJobs(&warnings), 1);
    QVERIFY(warnings.isEmpty());
    QTest::qWait(20);
    QCOMPARE(controller.job(id).state, ExportQueueJobState::Recovered);
    QVERIFY(!controller.hasActiveJob());

    controller.setPaused(true);
    QStringList collisions;
    QVERIFY2(controller.resumeRecoveredJob(id, false, &collisions, &error),
             qPrintable(error));
    QCOMPARE(controller.job(id).state, ExportQueueJobState::Pending);
    QVERIFY(controller.cancelJob(id));
    QVERIFY(!QFileInfo::exists(ExportQueuePersistence::jobPath(id)));
}


void ExportQueueTests::recoveredAskCollisionsRequireFreshConfirmation()
{
    QTemporaryDir outputDirectory;
    QVERIFY(outputDirectory.isValid());
    ExportQueueEnqueueRequest request = validRequest(205);
    request.plan.outputDirectory = outputDirectory.path();
    request.plan.collisionPolicy = ProductionExportCollisionPolicy::AskBeforeStart;
    const QString existingPath = QDir(outputDirectory.path()).filePath(
        QStringLiteral("queue-test-queue-205.png"));
    QFile existing(existingPath);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write("existing"), qint64(8));
    existing.close();

    const QString id = QStringLiteral("33333333-4444-5555-6666-777777777777");
    QString error;
    QVERIFY2(ExportQueuePersistence::writeJob(
                 id, QDateTime::currentDateTimeUtc(), request, &error),
             qPrintable(error));
    ExportQueueController controller;
    controller.setPaused(true);
    QStringList warnings;
    QCOMPARE(controller.restoreRecoverableJobs(&warnings), 1);
    QVERIFY(warnings.isEmpty());

    QStringList collisions;
    QVERIFY(!controller.resumeRecoveredJob(id, false, &collisions, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(!collisions.isEmpty());
    QCOMPARE(controller.job(id).state, ExportQueueJobState::Recovered);
    QVERIFY2(controller.resumeRecoveredJob(id, true, &collisions, &error),
             qPrintable(error));
    QCOMPARE(controller.job(id).state, ExportQueueJobState::Pending);
    QVERIFY(controller.cancelJob(id));
}

void ExportQueueTests::cleanShutdownCanPreserveJobs()
{
    QString id;
    QString error;
    {
        ExportQueueController controller;
        controller.setPaused(true);
        QVERIFY2(controller.enqueue(validRequest(203), &id, &error),
                 qPrintable(error));
        controller.preserveForRestartAndWait();
        QCOMPARE(controller.job(id).state, ExportQueueJobState::Recovered);
        QVERIFY(QFileInfo::exists(ExportQueuePersistence::jobPath(id)));
    }
    ExportQueueController restored;
    QStringList warnings;
    QCOMPARE(restored.restoreRecoverableJobs(&warnings), 1);
    QVERIFY(warnings.isEmpty());
    QCOMPARE(restored.job(id).state, ExportQueueJobState::Recovered);
    QVERIFY(restored.cancelJob(id));
}

void ExportQueueTests::malformedRecoveryIsQuarantined()
{
    QDir directory;
    QVERIFY(directory.mkpath(ExportQueuePersistence::storageDirectory()));
    const QString id = QStringLiteral("bbbbbbbb-cccc-dddd-eeee-ffffffffffff");
    QFile file(ExportQueuePersistence::jobPath(id));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("{not-json"), qint64(9));
    file.close();

    QStringList warnings;
    QVERIFY(ExportQueuePersistence::loadJobs(&warnings).isEmpty());
    QCOMPARE(warnings.size(), 1);
    QVERIFY(!QFileInfo::exists(ExportQueuePersistence::jobPath(id)));
    const QFileInfoList quarantined = QDir(
        ExportQueuePersistence::storageDirectory()).entryInfoList(
            {QStringLiteral("*.invalid*")}, QDir::Files);
    QCOMPARE(quarantined.size(), 1);
}

void ExportQueueTests::wrongTypedRecoveryIsQuarantined()
{
    const QString id = QStringLiteral("44444444-5555-6666-7777-888888888888");
    QString error;
    QVERIFY2(ExportQueuePersistence::writeJob(
                 id, QDateTime::currentDateTimeUtc(), validRequest(206), &error),
             qPrintable(error));
    QFile file(ExportQueuePersistence::jobPath(id));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QJsonObject root = document.object();
    QJsonObject request = root.value(QStringLiteral("request")).toObject();
    QJsonObject plan = request.value(QStringLiteral("plan")).toObject();
    QJsonArray outputs = plan.value(QStringLiteral("outputs")).toArray();
    QJsonObject output = outputs.at(0).toObject();
    QJsonObject resize = output.value(QStringLiteral("resize")).toObject();
    resize.insert(QStringLiteral("preserveAspect"), QStringLiteral("yes"));
    output.insert(QStringLiteral("resize"), resize);
    outputs[0] = output;
    plan.insert(QStringLiteral("outputs"), outputs);
    request.insert(QStringLiteral("plan"), plan);
    root.insert(QStringLiteral("request"), request);

    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray malformed = QJsonDocument(root).toJson(QJsonDocument::Compact);
    QCOMPARE(file.write(malformed), qint64(malformed.size()));
    file.close();

    QStringList warnings;
    QVERIFY(ExportQueuePersistence::loadJobs(&warnings).isEmpty());
    QCOMPARE(warnings.size(), 1);
    QVERIFY(warnings.constFirst().contains(
        QStringLiteral("resize"), Qt::CaseInsensitive));
    QVERIFY(!QFileInfo::exists(ExportQueuePersistence::jobPath(id)));
}


void ExportQueueTests::resolvedPayloadMustMatchRecoverablePlan()
{
    ExportQueueController controller;
    controller.setPaused(true);
    QString error;

    ExportQueueEnqueueRequest request = validRequest(301);
    request.outputs[0].request.filePath = QDir(request.plan.outputDirectory).filePath(
        QStringLiteral("different-name.png"));
    QVERIFY(!controller.enqueue(request, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("filename template"), Qt::CaseInsensitive));
    QVERIFY(controller.jobs().isEmpty());

    request = validRequest(302);
    request.outputs[0].profileId = QStringLiteral("another-profile");
    QVERIFY(!controller.enqueue(request, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("production plan"), Qt::CaseInsensitive));
    QVERIFY(controller.jobs().isEmpty());

    request = validRequest(303);
    request.outputs[0].request.filePath = QDir::home().filePath(
        QStringLiteral("queue-test-queue-303.png"));
    QVERIFY(!controller.enqueue(request, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("selected directory"), Qt::CaseInsensitive));
    QVERIFY(controller.jobs().isEmpty());
}

void ExportQueueTests::workerCompletionClassificationIsStrict()
{
    ExportQueueWorkerResult worker;
    ExportQueueOutputResult completed;
    completed.completed = true;
    ExportQueueOutputResult skipped;
    skipped.skipped = true;
    worker.outputs = {completed, skipped};
    QVERIFY(exportQueueWorkerFinishedCompletely(worker, 2));

    worker.outputs[1].skipped = false;
    worker.outputs[1].error = QStringLiteral("write failed");
    QVERIFY(!exportQueueWorkerFinishedCompletely(worker, 2));
    worker.outputs[1] = skipped;
    worker.cancelled = true;
    QVERIFY(!exportQueueWorkerFinishedCompletely(worker, 2));
    worker.cancelled = false;
    worker.renderError = QStringLiteral("render failed");
    QVERIFY(!exportQueueWorkerFinishedCompletely(worker, 2));
    worker.renderError.clear();
    QVERIFY(!exportQueueWorkerFinishedCompletely(worker, 1));
    QVERIFY(!exportQueueWorkerFinishedCompletely(ExportQueueWorkerResult {}, 0));
    QVERIFY(!exportQueueWorkerFinishedCompletely(worker, -1));
}

void ExportQueueTests::deepLayerTreesAreRejectedWithoutRecursion()
{
    ExportQueueEnqueueRequest request = validRequest(304);
    LayerNode root;
    LayerNode *cursor = &root;
    for (int depth = 1; depth <= LayerNode::MaximumTreeDepth; ++depth) {
        cursor->children.push_back(LayerNode {});
        cursor = &cursor->children.last();
    }
    request.layers = {root};

    const QString id = QStringLiteral("55555555-6666-7777-8888-999999999999");
    QString error;
    QVERIFY(!ExportQueuePersistence::writeJob(
        id, QDateTime::currentDateTimeUtc(), request, &error));
    QVERIFY(error.contains(QStringLiteral("hierarchy"), Qt::CaseInsensitive)
            || error.contains(QStringLiteral("safety"), Qt::CaseInsensitive));
    QVERIFY(!QFileInfo::exists(ExportQueuePersistence::jobPath(id)));
}


void ExportQueueTests::progressIncludesConfiguredSkipOutputs()
{
    ExportQueueEnqueueRequest request = validRequest(305);
    request.plan.collisionPolicy = ProductionExportCollisionPolicy::SkipExisting;
    request.outputs[0].skipExisting = true;
    request.outputs[0].existedAtPreflight = true;

    ProductionExportOutput second = request.plan.outputs.constFirst();
    second.id = QStringLiteral("output-305-second");
    second.namingTemplate = QStringLiteral("queue-test-{document}-second");
    request.plan.outputs.push_back(second);

    ResolvedProductionExportOutput resolvedSecond = request.outputs.constFirst();
    resolvedSecond.id = second.id;
    resolvedSecond.skipExisting = false;
    resolvedSecond.existedAtPreflight = false;
    resolvedSecond.request.filePath = QDir(request.plan.outputDirectory).filePath(
        QStringLiteral("queue-test-queue-305-second.png"));
    request.outputs.push_back(resolvedSecond);

    ExportQueueController controller;
    controller.setPaused(true);
    QString id;
    QString error;
    QVERIFY2(controller.enqueue(request, &id, &error), qPrintable(error));
    QCOMPARE(controller.job(id).progressMaximum, 3);
    QCOMPARE(controller.job(id).totalOutputs, 2);
    QVERIFY(controller.cancelJob(id));
}

void ExportQueueTests::recoveryResolutionMatchesQueuedSnapshot()
{
    ExportQueueEnqueueRequest request = validRequest(306);
    const QString id = QStringLiteral("66666666-7777-8888-9999-aaaaaaaaaaaa");
    const QDateTime created = QDateTime::fromSecsSinceEpoch(1700000200, Qt::UTC);
    QString error;
    QVERIFY2(ExportQueuePersistence::writeJob(id, created, request, &error),
             qPrintable(error));

    QStringList warnings;
    const QVector<RecoverableExportQueueJob> jobs =
        ExportQueuePersistence::loadJobs(&warnings);
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
    QCOMPARE(jobs.size(), 1);

    QVector<ResolvedProductionExportOutput> reResolved;
    QVERIFY2(resolveProductionExportPlan(
                 jobs.constFirst().request.plan,
                 jobs.constFirst().request.colourState,
                 &reResolved, nullptr, &error), qPrintable(error));
    QCOMPARE(reResolved.size(), jobs.constFirst().request.outputs.size());
    QVERIFY2(validateResolvedProductionExportOutputs(
                 jobs.constFirst().request.plan,
                 jobs.constFirst().request.colourState,
                 jobs.constFirst().request.outputs,
                 &error), qPrintable(error));
    QCOMPARE(reResolved.constFirst().id,
             jobs.constFirst().request.outputs.constFirst().id);
    QCOMPARE(reResolved.constFirst().request.filePath,
             jobs.constFirst().request.outputs.constFirst().request.filePath);
    QCOMPARE(reResolved.constFirst().request.quality,
             jobs.constFirst().request.outputs.constFirst().request.quality);
    QVERIFY(ExportQueuePersistence::removeJob(id, &error));
}

void ExportQueueTests::skipPolicyRechecksFilesystemAtExecution()
{
    QTemporaryDir outputDirectory;
    QVERIFY(outputDirectory.isValid());
    ExportQueueEnqueueRequest request = validRequest(307);
    request.plan.outputDirectory = outputDirectory.path();
    request.plan.collisionPolicy = ProductionExportCollisionPolicy::SkipExisting;
    request.outputs[0].request.filePath = QDir(outputDirectory.path()).filePath(
        QStringLiteral("queue-test-queue-307.png"));
    request.outputs[0].skipExisting = true;
    request.outputs[0].existedAtPreflight = true;

    QFile preflightFile(request.outputs.constFirst().request.filePath);
    QVERIFY(preflightFile.open(QIODevice::WriteOnly));
    preflightFile.write("preflight collision");
    preflightFile.close();

    ExportQueueController controller;
    controller.setPaused(true);
    QString id;
    QString error;
    QVERIFY2(controller.enqueue(request, &id, &error), qPrintable(error));
    QVERIFY(QFile::remove(preflightFile.fileName()));
    QVERIFY(controller.setPaused(false));

    QTRY_VERIFY_WITH_TIMEOUT(
        exportQueueJobStateIsTerminal(controller.job(id).state), 10000);
    const ExportQueueJobInfo info = controller.job(id);
    QCOMPARE(info.state, ExportQueueJobState::Completed);
    QCOMPARE(info.completedOutputs, 1);
    QCOMPARE(info.skippedOutputs, 0);
    QCOMPARE(info.progressValue, info.progressMaximum);
    QVERIFY(QFileInfo::exists(preflightFile.fileName()));
}

QTEST_APPLESS_MAIN(ExportQueueTests)
#include "test_export_queue.moc"
