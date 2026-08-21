#include "ExportQueueController.h"

#include "ExportProfileStore.h"
#include "ImageProcessor.h"
#include "ImageSizeOperations.h"
#include "gpu/RenderBackend.h"

#include <QDebug>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QWaitCondition>
#include <QtConcurrent>

#include <algorithm>
#include <utility>

namespace vfx {
namespace {

constexpr int MaximumQueuedJobs = 16;
constexpr int MaximumRetainedJobs = 128;



} // namespace

bool exportQueueWorkerFinishedCompletely(
    const ExportQueueWorkerResult &worker,
    const qsizetype expectedOutputCount)
{
    if (expectedOutputCount < 1 || worker.cancelled
        || !worker.renderError.isEmpty()
        || worker.outputs.size() != expectedOutputCount) {
        return false;
    }
    return std::all_of(worker.outputs.cbegin(), worker.outputs.cend(),
        [](const ExportQueueOutputResult &result) {
            return result.completed || result.skipped;
        });
}

struct ExportQueueController::JobRecord {
    ExportQueueJobInfo info;
    ExportQueueEnqueueRequest request;
    quint64 executionGeneration = 0;
};

struct ExportQueueController::Control {
    std::atomic_bool cancelRequested {false};
    std::atomic_bool pauseRequested {false};
    QMutex mutex;
    QWaitCondition condition;

    void setPaused(const bool paused)
    {
        pauseRequested.store(paused, std::memory_order_release);
        if (!paused) {
            QMutexLocker locker(&mutex);
            condition.wakeAll();
        }
    }

    void cancel()
    {
        cancelRequested.store(true, std::memory_order_release);
        QMutexLocker locker(&mutex);
        condition.wakeAll();
    }

    bool waitWhilePaused()
    {
        if (cancelRequested.load(std::memory_order_acquire)) return false;
        QMutexLocker locker(&mutex);
        while (pauseRequested.load(std::memory_order_acquire)
               && !cancelRequested.load(std::memory_order_acquire)) {
            condition.wait(&mutex, 250);
        }
        return !cancelRequested.load(std::memory_order_acquire);
    }
};

ExportQueueController::ExportQueueController(QObject *parent)
    : QObject(parent)
{
}

ExportQueueController::~ExportQueueController()
{
    shutdownAndWait();
}

bool ExportQueueController::enqueue(const ExportQueueEnqueueRequest &request,
                                    QString *jobId,
                                    QString *errorMessage)
{
    if (jobId) jobId->clear();
    if (errorMessage) errorMessage->clear();
    if (m_shuttingDown) {
        if (errorMessage) *errorMessage = QStringLiteral("The export queue is shutting down.");
        return false;
    }
    int unfinished = 0;
    for (const auto &record : std::as_const(m_jobs)) {
        if (record && !exportQueueJobStateIsTerminal(record->info.state)) {
            ++unfinished;
        }
    }
    if (unfinished >= MaximumQueuedJobs) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The export queue may contain at most %1 unfinished jobs.")
                .arg(MaximumQueuedJobs);
        }
        return false;
    }
    if (request.source.isNull() || request.source.size().isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("The queued document image is invalid.");
        return false;
    }
    if (request.source.format() != QImage::Format_RGBA8888
        && request.source.format() != QImage::Format_RGBA64) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The queued document snapshot must use straight RGBA8 or RGBA16 pixels so in-session and recovered exports remain identical.");
        }
        return false;
    }
    if (request.plan.documentSize != request.source.size()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The queued document size does not match its captured source image.");
        }
        return false;
    }
    if (request.processingCompatibility
        != request.colourState.processingCompatibility) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The queued processing contract does not match its captured colour state.");
        }
        return false;
    }
    QString colourStateError;
    if (!request.colourState.isSafe(&colourStateError)) {
        if (errorMessage) {
            *errorMessage = colourStateError.isEmpty()
                ? QStringLiteral("The queued colour state is invalid.")
                : colourStateError;
        }
        return false;
    }
    if (request.outputs.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("The queued job has no resolved outputs.");
        return false;
    }
    QString resolvedError;
    if (!validateResolvedProductionExportOutputs(
            request.plan, request.colourState, request.outputs, &resolvedError)) {
        if (errorMessage) {
            *errorMessage = resolvedError.isEmpty()
                ? QStringLiteral("The queued resolved outputs are inconsistent with their production plan.")
                : resolvedError;
        }
        return false;
    }
    // SkipExisting is a preflight observation, not a permanent execution
    // decision. Queue every validated output and recheck the filesystem in the
    // worker so a file deleted while the job waits is exported normally.

    auto record = std::make_shared<JobRecord>();
    record->request = request;
    for (int attempt = 0; attempt < 16 && record->info.id.isEmpty(); ++attempt) {
        const QString candidate = QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toLower();
        if (!recordById(candidate)
            && !QFileInfo::exists(ExportQueuePersistence::jobPath(candidate))) {
            record->info.id = candidate;
        }
    }
    if (record->info.id.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "A unique recoverable export queue identifier could not be allocated.");
        }
        return false;
    }
    record->info.title = request.title.trimmed();
    if (record->info.title.isEmpty()) {
        record->info.title = request.documentName.trimmed();
    }
    if (record->info.title.isEmpty()) {
        record->info.title = QStringLiteral("Production export");
    }
    record->info.documentName = request.documentName.trimmed();
    record->info.outputDirectory = request.plan.outputDirectory;
    record->info.createdUtc = QDateTime::currentDateTimeUtc();
    record->info.state = ExportQueueJobState::Pending;
    record->info.progressValue = 0;
    record->info.progressMaximum = request.outputs.size() + 1;
    record->info.totalOutputs = request.outputs.size();
    record->info.statusText = QStringLiteral("Waiting in queue");
    QString persistenceError;
    if (!ExportQueuePersistence::writeJob(record->info.id,
                                          record->info.createdUtc,
                                          record->request,
                                          &persistenceError)) {
        if (errorMessage) {
            *errorMessage = persistenceError.isEmpty()
                ? QStringLiteral("The export job could not be made recoverable.")
                : persistenceError;
        }
        return false;
    }

    // Do not evict terminal history until the new job is fully validated and
    // durably persisted. A rejected enqueue must be side-effect free.
    while (m_jobs.size() >= MaximumRetainedJobs) {
        const auto oldestTerminal = std::find_if(
            m_jobs.begin(), m_jobs.end(),
            [](const std::shared_ptr<JobRecord> &existing) {
                return !existing
                    || exportQueueJobStateIsTerminal(existing->info.state);
            });
        if (oldestTerminal == m_jobs.end()) break;
        m_jobs.erase(oldestTerminal);
    }
    m_jobs.push_back(record);
    if (jobId) *jobId = record->info.id;
    emit jobsChanged();
    emit queueStateChanged();
    QTimer::singleShot(0, this, &ExportQueueController::startNextIfPossible);
    return true;
}

int ExportQueueController::restoreRecoverableJobs(QStringList *warnings)
{
    if (m_shuttingDown || hasActiveJob()) return 0;
    const QVector<RecoverableExportQueueJob> recovered =
        ExportQueuePersistence::loadJobs(warnings);
    int restoredCount = 0;
    for (const RecoverableExportQueueJob &saved : recovered) {
        if (m_jobs.size() >= MaximumRetainedJobs
            || recordById(saved.id)
            || saved.request.source.isNull()
            || saved.request.plan.documentSize != saved.request.source.size()) {
            continue;
        }
        auto record = std::make_shared<JobRecord>();
        record->request = saved.request;
        record->info.id = saved.id;
        record->info.title = saved.request.title.trimmed();
        if (record->info.title.isEmpty()) {
            record->info.title = saved.request.documentName.trimmed();
        }
        if (record->info.title.isEmpty()) {
            record->info.title = QStringLiteral("Recovered production export");
        }
        record->info.documentName = saved.request.documentName.trimmed();
        record->info.outputDirectory = saved.request.plan.outputDirectory;
        record->info.createdUtc = saved.createdUtc;
        record->info.state = ExportQueueJobState::Recovered;
        record->info.progressValue = 0;
        record->info.progressMaximum = std::max(
            1, static_cast<int>(saved.request.plan.outputs.size()) + 1);
        record->info.totalOutputs = saved.request.plan.outputs.size();
        record->info.statusText = QStringLiteral(
            "Recovered safely — review collisions before resuming");
        record->info.details.push_back(QStringLiteral(
            "This job was restored from a recoverable snapshot and has not written any new files in this session."));
        m_jobs.push_back(record);
        ++restoredCount;
    }
    if (restoredCount > 0) {
        emit jobsChanged();
        emit queueStateChanged();
    }
    return restoredCount;
}

bool ExportQueueController::resumeRecoveredJob(
    const QString &jobId,
    const bool confirmExistingFiles,
    QStringList *collisionWarnings,
    QString *errorMessage)
{
    if (collisionWarnings) collisionWarnings->clear();
    if (errorMessage) errorMessage->clear();
    const auto record = recordById(jobId);
    if (!record || record->info.state != ExportQueueJobState::Recovered) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The selected job is not awaiting recovery.");
        }
        return false;
    }
    QVector<ResolvedProductionExportOutput> outputs;
    QStringList warnings;
    QString resolutionError;
    if (!resolveProductionExportPlan(record->request.plan,
                                     record->request.colourState,
                                     &outputs, &warnings,
                                     &resolutionError)) {
        record->info.statusText = QStringLiteral(
            "Recovery validation failed — the job was retained for review");
        record->info.details = {
            resolutionError.isEmpty()
                ? QStringLiteral("The recovered job no longer passes validation.")
                : resolutionError
        };
        emit jobsChanged();
        emit queueStateChanged();
        if (errorMessage) *errorMessage = record->info.details.constFirst();
        return false;
    }
    if (record->request.plan.collisionPolicy
            == ProductionExportCollisionPolicy::AskBeforeStart
        && !warnings.isEmpty() && !confirmExistingFiles) {
        if (collisionWarnings) *collisionWarnings = warnings;
        return false;
    }
    record->request.outputs = outputs;
    record->info.state = ExportQueueJobState::Pending;
    record->info.progressValue = 0;
    record->info.progressMaximum = outputs.size() + 1;
    record->info.totalOutputs = outputs.size();
    record->info.completedOutputs = 0;
    record->info.skippedOutputs = 0;
    record->info.failedOutputs = 0;
    record->info.warningCount = 0;
    record->info.startedUtc = {};
    record->info.finishedUtc = {};
    record->info.statusText = QStringLiteral("Recovered job is ready in the queue");
    record->info.details.clear();
    emit jobsChanged();
    emit queueStateChanged();
    QTimer::singleShot(0, this, &ExportQueueController::startNextIfPossible);
    return true;
}

void ExportQueueController::preserveForRestartAndWait()
{
    if (m_shuttingDown) return;
    m_preserveUnfinishedOnShutdown = true;
    m_shuttingDown = true;
    if (m_activeControl) m_activeControl->cancel();

    ExportQueueWorkerResult finishedWorker;
    bool haveFinishedWorker = false;
    qsizetype expectedActiveOutputs = -1;
    if (const auto active = recordById(m_activeJobId)) {
        expectedActiveOutputs = active->request.outputs.size();
    }
    if (m_watcher) {
        m_watcher->future().waitForFinished();
        finishedWorker = m_watcher->result();
        haveFinishedWorker = true;
        delete m_watcher;
        m_watcher = nullptr;
    }

    // A job can finish its final atomic write just before the shutdown cancel
    // reaches the worker. Treat a fully processed result as terminal now, so a
    // successful job is never restored and executed a second time next launch.
    if (haveFinishedWorker
        && exportQueueWorkerFinishedCompletely(
            finishedWorker, expectedActiveOutputs)) {
        m_preserveUnfinishedOnShutdown = false;
        finishActiveJob(finishedWorker);
        m_preserveUnfinishedOnShutdown = true;
    } else {
        m_activeControl.reset();
        m_activeJobId.clear();
    }

    for (const auto &record : std::as_const(m_jobs)) {
        if (!record || exportQueueJobStateIsTerminal(record->info.state)) continue;
        record->info.state = ExportQueueJobState::Recovered;
        record->info.statusText = QStringLiteral("Preserved for the next launch");
        record->info.startedUtc = {};
        record->info.finishedUtc = {};
        record->info.progressValue = 0;
        ++record->executionGeneration;
        record->request.outputs.clear();
    }
    emit jobsChanged();
    emit queueStateChanged();
    emit becameIdle();
}

void ExportQueueController::resumeAfterAbortedShutdown()
{
    if (!m_preserveUnfinishedOnShutdown) return;
    m_shuttingDown = false;
    m_preserveUnfinishedOnShutdown = false;
    emit jobsChanged();
    emit queueStateChanged();
}

QVector<ExportQueueJobInfo> ExportQueueController::jobs() const
{
    QVector<ExportQueueJobInfo> result;
    result.reserve(m_jobs.size());
    for (const auto &record : m_jobs) {
        if (record) result.push_back(record->info);
    }
    return result;
}

ExportQueueJobInfo ExportQueueController::job(const QString &jobId) const
{
    const auto record = recordById(jobId);
    return record ? record->info : ExportQueueJobInfo {};
}

bool ExportQueueController::hasUnfinishedJobs() const
{
    return std::any_of(m_jobs.cbegin(), m_jobs.cend(),
        [](const std::shared_ptr<JobRecord> &record) {
            return record && !exportQueueJobStateIsTerminal(record->info.state);
        });
}

bool ExportQueueController::hasActiveJob() const
{
    return !m_activeJobId.isEmpty() && m_watcher;
}

bool ExportQueueController::isPaused() const
{
    return m_paused;
}

QString ExportQueueController::activeJobId() const
{
    return m_activeJobId;
}

bool ExportQueueController::setPaused(const bool paused)
{
    if (m_paused == paused) return true;
    m_paused = paused;
    if (m_activeControl) {
        m_activeControl->setPaused(paused);
        const auto active = recordById(m_activeJobId);
        if (active && !exportQueueJobStateIsTerminal(active->info.state)) {
            if (paused && active->info.state == ExportQueueJobState::Running) {
                active->info.statusText = QStringLiteral(
                    "Pausing after the current export operation…");
            } else if (!paused && active->info.state == ExportQueueJobState::Paused) {
                active->info.state = ExportQueueJobState::Running;
                active->info.statusText = QStringLiteral("Resuming export…");
            }
        }
    }
    emit jobsChanged();
    emit queueStateChanged();
    if (!paused) QTimer::singleShot(0, this, &ExportQueueController::startNextIfPossible);
    return true;
}

bool ExportQueueController::cancelJob(const QString &jobId)
{
    const auto record = recordById(jobId);
    if (!record || !exportQueueJobStateCanCancel(record->info.state)) return false;
    if (jobId == m_activeJobId && m_activeControl) {
        record->info.statusText = QStringLiteral("Cancelling export…");
        m_activeControl->cancel();
    } else {
        record->info.state = ExportQueueJobState::Cancelled;
        record->info.statusText = QStringLiteral("Cancelled before starting");
        record->info.finishedUtc = QDateTime::currentDateTimeUtc();
        releaseSnapshot(record);
    }
    emit jobsChanged();
    emit queueStateChanged();
    return true;
}

void ExportQueueController::cancelAll()
{
    for (const auto &record : std::as_const(m_jobs)) {
        if (!record || !exportQueueJobStateCanCancel(record->info.state)) continue;
        if (record->info.id == m_activeJobId && m_activeControl) {
            record->info.statusText = QStringLiteral("Cancelling export…");
            m_activeControl->cancel();
        } else {
            record->info.state = ExportQueueJobState::Cancelled;
            record->info.statusText = QStringLiteral("Cancelled before starting");
            record->info.finishedUtc = QDateTime::currentDateTimeUtc();
            releaseSnapshot(record);
        }
    }
    emit jobsChanged();
    emit queueStateChanged();
    if (!hasActiveJob()) emit becameIdle();
}

bool ExportQueueController::removeJob(const QString &jobId)
{
    const auto found = std::find_if(m_jobs.begin(), m_jobs.end(),
        [&jobId](const std::shared_ptr<JobRecord> &record) {
            return record && record->info.id == jobId;
        });
    if (found == m_jobs.end() || !exportQueueJobStateCanRemove((*found)->info.state)) {
        return false;
    }
    m_jobs.erase(found);
    emit jobsChanged();
    emit queueStateChanged();
    return true;
}

int ExportQueueController::clearFinished()
{
    const int before = m_jobs.size();
    m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
        [](const std::shared_ptr<JobRecord> &record) {
            return !record || exportQueueJobStateIsTerminal(record->info.state);
        }), m_jobs.end());
    const int removed = before - m_jobs.size();
    if (removed > 0) {
        emit jobsChanged();
        emit queueStateChanged();
    }
    return removed;
}

void ExportQueueController::shutdownAndWait()
{
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    cancelAll();
    if (m_watcher) {
        m_watcher->future().waitForFinished();
        delete m_watcher;
        m_watcher = nullptr;
    }
    m_activeControl.reset();
    m_activeJobId.clear();
    for (const auto &record : std::as_const(m_jobs)) releaseSnapshot(record);
}

std::shared_ptr<ExportQueueController::JobRecord>
ExportQueueController::recordById(const QString &jobId) const
{
    const auto found = std::find_if(m_jobs.cbegin(), m_jobs.cend(),
        [&jobId](const std::shared_ptr<JobRecord> &record) {
            return record && record->info.id == jobId;
        });
    return found == m_jobs.cend() ? nullptr : *found;
}

void ExportQueueController::startNextIfPossible()
{
    if (m_shuttingDown || m_watcher || !m_activeJobId.isEmpty()) return;
    const auto found = std::find_if(m_jobs.cbegin(), m_jobs.cend(),
        [](const std::shared_ptr<JobRecord> &record) {
            return record && record->info.state == ExportQueueJobState::Pending;
        });
    if (found == m_jobs.cend()) {
        emit becameIdle();
        return;
    }
    if (m_paused) return;
    startJob(*found);
}

void ExportQueueController::startJob(const std::shared_ptr<JobRecord> &record)
{
    if (!record || m_watcher || m_shuttingDown) return;
    m_activeJobId = record->info.id;
    record->info.state = ExportQueueJobState::Running;
    record->info.startedUtc = QDateTime::currentDateTimeUtc();
    record->info.statusText = QStringLiteral("Rendering the document snapshot…");
    record->info.progressValue = 0;
    m_activeControl = std::make_shared<Control>();
    m_activeControl->setPaused(m_paused);
    emit jobsChanged();
    emit queueStateChanged();

    const ExportQueueEnqueueRequest request = record->request;
    const QString jobId = record->info.id;
    const quint64 executionGeneration = ++record->executionGeneration;
    const std::shared_ptr<Control> control = m_activeControl;
    // shutdownAndWait() keeps the controller alive until this worker exits.
    // Queuing against the controller itself also lets QObject discard any
    // still-pending progress events during normal child teardown.
    ExportQueueController *const receiver = this;

    m_watcher = new QFutureWatcher<ExportQueueWorkerResult>(this);
    connect(m_watcher, &QFutureWatcher<ExportQueueWorkerResult>::finished,
            this, [this] {
        if (!m_watcher) return;
        const ExportQueueWorkerResult worker = m_watcher->result();
        m_watcher->deleteLater();
        m_watcher = nullptr;
        finishActiveJob(worker);
    });

    m_watcher->setFuture(QtConcurrent::run(
        [request, jobId, executionGeneration, control, receiver] {
            ExportQueueWorkerResult worker;
            worker.outputs.reserve(request.outputs.size());
            const int maximum = request.outputs.size() + 1;
            int progressValue = 0;

            const auto publish = [receiver, &jobId, executionGeneration, maximum](
                                     const int value,
                                     const QString &text,
                                     const ExportQueueJobState state) {
                if (!receiver) return;
                QMetaObject::invokeMethod(receiver,
                    [receiver, jobId, executionGeneration, value, maximum,
                     text, state] {
                        receiver->updateJobProgress(jobId, executionGeneration,
                                                    value, maximum, text, state);
                    }, Qt::QueuedConnection);
            };
            const auto checkpoint = [&] (const QString &pausedText,
                                         const QString &resumeText) {
                if (control->cancelRequested.load(std::memory_order_acquire)) {
                    return false;
                }
                if (!control->pauseRequested.load(std::memory_order_acquire)) {
                    return true;
                }
                publish(progressValue, pausedText, ExportQueueJobState::Paused);
                if (!control->waitWhilePaused()) return false;
                publish(progressValue, resumeText, ExportQueueJobState::Running);
                return true;
            };

            if (!checkpoint(QStringLiteral("Paused before rendering"),
                            QStringLiteral("Rendering the document snapshot…"))) {
                worker.cancelled = true;
                return worker;
            }
            QImage rendered = ImageProcessor::renderPreservingHiddenRgb(
                request.source, request.layers, &control->cancelRequested,
                request.source.size(), request.processingCompatibility);
            if (control->cancelRequested.load(std::memory_order_acquire)) {
                worker.cancelled = true;
                return worker;
            }
            if (rendered.isNull()) {
                worker.renderError = QStringLiteral(
                    "The full-resolution production render failed.");
                return worker;
            }
            if (rendered.size() != request.source.size()) {
                worker.renderError = QStringLiteral(
                    "The production render returned an unexpected image size.");
                return worker;
            }
            progressValue = 1;
            publish(progressValue, QStringLiteral("Preparing production outputs…"),
                    ExportQueueJobState::Running);

            QStringList executionReservedPaths;
            executionReservedPaths.reserve(request.outputs.size());
            for (const ResolvedProductionExportOutput &output : request.outputs) {
                executionReservedPaths.push_back(output.request.filePath);
            }

            for (const ResolvedProductionExportOutput &output : request.outputs) {
                ExportQueueOutputResult result;
                result.path = output.request.filePath;
                result.profileId = output.profileId;
                // `skipExisting` records the dialog-time observation only. The
                // file may have been deleted while this job waited, so every
                // output reaches the execution-time collision check below.
                if (!checkpoint(QStringLiteral("Paused between outputs"),
                                QStringLiteral("Preparing the next output…"))) {
                    worker.cancelled = true;
                    break;
                }

                ImageExportRequest executionRequest = output.request;
                const bool existsNow = QFileInfo::exists(executionRequest.filePath);
                if (request.plan.collisionPolicy
                        == ProductionExportCollisionPolicy::SkipExisting
                    && existsNow) {
                    result.skipped = true;
                    worker.outputs.push_back(std::move(result));
                    ++progressValue;
                    publish(progressValue, QStringLiteral("Skipped existing output"),
                            ExportQueueJobState::Running);
                    continue;
                }
                if (request.plan.collisionPolicy
                        == ProductionExportCollisionPolicy::AskBeforeStart
                    && existsNow && !output.existedAtPreflight) {
                    result.error = QStringLiteral(
                        "The output file appeared after collision confirmation and was not replaced.");
                    worker.outputs.push_back(std::move(result));
                    ++progressValue;
                    publish(progressValue, QStringLiteral("Output failed collision recheck"),
                            ExportQueueJobState::Running);
                    continue;
                }
                if (request.plan.collisionPolicy
                        == ProductionExportCollisionPolicy::AutoRename
                    && existsNow) {
                    const QString uniquePath = uniqueProductionExportOutputPath(
                        executionRequest.filePath, executionReservedPaths);
                    if (uniquePath.isEmpty()) {
                        result.error = QStringLiteral(
                            "A unique output filename could not be reserved at execution time.");
                        worker.outputs.push_back(std::move(result));
                        ++progressValue;
                        publish(progressValue, QStringLiteral("Output auto-rename failed"),
                                ExportQueueJobState::Running);
                        continue;
                    }
                    executionRequest.filePath = uniquePath;
                    result.path = uniquePath;
                    executionReservedPaths.push_back(uniquePath);
                }

                publish(progressValue,
                        QStringLiteral("Creating %1 — %2 × %3…")
                            .arg(QFileInfo(executionRequest.filePath).fileName())
                            .arg(output.outputSize.width())
                            .arg(output.outputSize.height()),
                        ExportQueueJobState::Running);

                QImage outputSurface;
                if (!output.resizeRequired) {
                    outputSurface = rendered;
                } else {
                    if (output.resampleMethod == ImageResampleMethod::NearestNeighbour
                        || output.resampleMethod == ImageResampleMethod::Bilinear) {
                        outputSurface = RenderBackend::instance().resampleImageTiled(
                            rendered, output.outputSize, output.resampleMethod,
                            &control->cancelRequested, nullptr);
                        if (!outputSurface.isNull()
                            && outputSurface.size() != output.outputSize) {
                            outputSurface = QImage();
                        }
                    }
                    if (outputSurface.isNull()
                        && !control->cancelRequested.load(std::memory_order_acquire)) {
                        outputSurface = resampleStraightRgbaCpuReference(
                            rendered, output.outputSize, output.resampleMethod,
                            &control->cancelRequested);
                    }
                    if (!outputSurface.isNull()
                        && outputSurface.size() != output.outputSize) {
                        outputSurface = QImage();
                    }
                    if (outputSurface.isNull()) {
                        if (control->cancelRequested.load(std::memory_order_acquire)) {
                            worker.cancelled = true;
                            break;
                        }
                        result.error = QStringLiteral(
                            "The requested output resize failed.");
                        worker.outputs.push_back(std::move(result));
                        ++progressValue;
                        publish(progressValue, QStringLiteral("Output resize failed"),
                                ExportQueueJobState::Running);
                        continue;
                    }
                }

                PreparedImageExport prepared;
                if (!prepareImageExport(outputSurface, request.colourState,
                                        executionRequest, &prepared,
                                        &control->cancelRequested,
                                        &result.error)) {
                    if (control->cancelRequested.load(std::memory_order_acquire)) {
                        worker.cancelled = true;
                        break;
                    }
                    worker.outputs.push_back(std::move(result));
                    ++progressValue;
                    publish(progressValue, QStringLiteral("Output preparation failed"),
                            ExportQueueJobState::Running);
                    continue;
                }

                const bool existsBeforeWrite = QFileInfo::exists(
                    executionRequest.filePath);
                if (request.plan.collisionPolicy
                        == ProductionExportCollisionPolicy::SkipExisting
                    && existsBeforeWrite) {
                    result.skipped = true;
                    worker.outputs.push_back(std::move(result));
                    ++progressValue;
                    publish(progressValue, QStringLiteral("Skipped existing output"),
                            ExportQueueJobState::Running);
                    continue;
                }
                if (request.plan.collisionPolicy
                        == ProductionExportCollisionPolicy::AskBeforeStart
                    && existsBeforeWrite && !output.existedAtPreflight) {
                    result.error = QStringLiteral(
                        "The output file appeared after collision confirmation and was not replaced.");
                    worker.outputs.push_back(std::move(result));
                    ++progressValue;
                    publish(progressValue, QStringLiteral("Output failed collision recheck"),
                            ExportQueueJobState::Running);
                    continue;
                }
                if (request.plan.collisionPolicy
                        == ProductionExportCollisionPolicy::AutoRename
                    && existsBeforeWrite) {
                    const QString uniquePath = uniqueProductionExportOutputPath(
                        executionRequest.filePath, executionReservedPaths);
                    if (uniquePath.isEmpty()) {
                        result.error = QStringLiteral(
                            "A unique output filename could not be reserved before writing.");
                        worker.outputs.push_back(std::move(result));
                        ++progressValue;
                        publish(progressValue, QStringLiteral("Output auto-rename failed"),
                                ExportQueueJobState::Running);
                        continue;
                    }
                    executionRequest.filePath = uniquePath;
                    result.path = uniquePath;
                    executionReservedPaths.push_back(uniquePath);
                }

                if (!writePreparedImageExport(executionRequest.filePath,
                                              prepared,
                                              executionRequest.quality,
                                              &result.error)) {
                    worker.outputs.push_back(std::move(result));
                    ++progressValue;
                    publish(progressValue, QStringLiteral("Output write failed"),
                            ExportQueueJobState::Running);
                    continue;
                }
                result.warnings = prepared.warnings;
                result.completed = true;
                worker.outputs.push_back(std::move(result));
                ++progressValue;
                publish(progressValue,
                        QStringLiteral("Completed %1")
                            .arg(QFileInfo(executionRequest.filePath).fileName()),
                        ExportQueueJobState::Running);
            }
            return worker;
        }));
}

void ExportQueueController::updateJobProgress(
    const QString &jobId,
    const quint64 executionGeneration,
    const int value,
    const int maximum,
    const QString &statusText,
    const ExportQueueJobState state)
{
    if (m_shuttingDown && m_preserveUnfinishedOnShutdown) return;
    const auto record = recordById(jobId);
    if (!record || record->executionGeneration != executionGeneration
        || record->info.state == ExportQueueJobState::Recovered
        || exportQueueJobStateIsTerminal(record->info.state)) {
        return;
    }
    if (exportQueueJobStateCanTransition(record->info.state, state)) {
        record->info.state = state;
    }
    record->info.progressMaximum = std::max(1, maximum);
    record->info.progressValue = std::clamp(value, 0, record->info.progressMaximum);
    record->info.statusText = statusText;
    emit jobsChanged();
    emit queueStateChanged();
}

void ExportQueueController::finishActiveJob(
    const ExportQueueWorkerResult &worker)
{
    const auto record = recordById(m_activeJobId);
    m_activeJobId.clear();
    m_activeControl.reset();
    if (!record) {
        emit queueStateChanged();
        QTimer::singleShot(0, this, &ExportQueueController::startNextIfPossible);
        return;
    }

    QSet<QString> usedProfileIds;
    for (const ExportQueueOutputResult &result : worker.outputs) {
        if (result.completed) {
            ++record->info.completedOutputs;
            record->info.warningCount += result.warnings.size();
            for (const QString &warning : result.warnings) {
                record->info.details.push_back(
                    QStringLiteral("%1 — %2").arg(result.path, warning));
            }
            if (!result.profileId.isEmpty()) usedProfileIds.insert(result.profileId);
        } else if (result.skipped) {
            ++record->info.skippedOutputs;
        } else {
            ++record->info.failedOutputs;
            record->info.details.push_back(QStringLiteral("%1 — %2")
                .arg(result.path,
                     result.error.isEmpty()
                         ? QStringLiteral("Unknown output failure") : result.error));
        }
    }

    if (!usedProfileIds.isEmpty()) {
        const QVector<ExportProfile> profiles = ExportProfileStore::profiles();
        for (const ExportProfile &profile : profiles) {
            if (!usedProfileIds.contains(profile.metadata.id)) continue;
            QString usageError;
            if (!ExportProfileStore::recordUse(profile, &usageError)
                && !usageError.isEmpty()) {
                qWarning().noquote()
                    << "VFX Photo Lab export queue profile usage:"
                    << usageError;
            }
        }
    }

    record->info.finishedUtc = QDateTime::currentDateTimeUtc();
    if (!worker.renderError.isEmpty()) {
        record->info.state = ExportQueueJobState::Failed;
        record->info.statusText = worker.renderError;
        record->info.details.prepend(worker.renderError);
    } else if (worker.cancelled) {
        record->info.state = ExportQueueJobState::Cancelled;
        record->info.statusText = QStringLiteral(
            "Cancelled — completed outputs were kept");
    } else if (record->info.completedOutputs == 0
               && record->info.failedOutputs > 0) {
        record->info.state = ExportQueueJobState::Failed;
        record->info.statusText = QStringLiteral("Every executable output failed");
        record->info.progressValue = record->info.progressMaximum;
    } else if (record->info.completedOutputs == 0
               && record->info.failedOutputs == 0
               && record->info.skippedOutputs > 0) {
        record->info.state = ExportQueueJobState::Completed;
        record->info.statusText = QStringLiteral(
            "Completed without writing files; every output was skipped");
        record->info.progressValue = record->info.progressMaximum;
    } else if (record->info.failedOutputs > 0
               || record->info.warningCount > 0) {
        record->info.state = ExportQueueJobState::CompletedWithIssues;
        record->info.statusText = QStringLiteral(
            "Completed with failures or warnings; completed files were kept");
        record->info.progressValue = record->info.progressMaximum;
    } else {
        record->info.state = ExportQueueJobState::Completed;
        record->info.statusText = QStringLiteral("All outputs completed");
        record->info.progressValue = record->info.progressMaximum;
    }
    releaseSnapshot(record);

    QString message;
    if (record->info.state == ExportQueueJobState::Completed) {
        message = QStringLiteral("Export queue job completed: %1").arg(record->info.title);
    } else if (record->info.state == ExportQueueJobState::Cancelled) {
        message = QStringLiteral("Export queue job cancelled: %1").arg(record->info.title);
    } else {
        message = QStringLiteral("Export queue job needs attention: %1").arg(record->info.title);
    }
    emit messageAvailable(message,
        record->info.state == ExportQueueJobState::Completed ? 5000 : 8000);
    emit jobsChanged();
    emit queueStateChanged();

    if (m_shuttingDown) {
        emit becameIdle();
        return;
    }
    QTimer::singleShot(0, this, &ExportQueueController::startNextIfPossible);
}

void ExportQueueController::releaseSnapshot(
    const std::shared_ptr<JobRecord> &record)
{
    if (!record) return;
    if (!m_preserveUnfinishedOnShutdown) {
        QString removalError;
        if (!ExportQueuePersistence::removeJob(record->info.id, &removalError)
            && !removalError.isEmpty()) {
            qWarning().noquote()
                << "VFX Photo Lab export queue recovery cleanup:"
                << removalError;
        }
    }
    record->request = ExportQueueEnqueueRequest {};
}

} // namespace vfx
