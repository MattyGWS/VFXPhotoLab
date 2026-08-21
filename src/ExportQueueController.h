#pragma once

#include "ExportQueueCore.h"
#include "ExportQueuePersistence.h"

#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QVector>

#include <atomic>
#include <memory>

namespace vfx {

struct ExportQueueOutputResult {
    QString path;
    QString profileId;
    QString error;
    QStringList warnings;
    bool completed = false;
    bool skipped = false;
};

struct ExportQueueWorkerResult {
    QVector<ExportQueueOutputResult> outputs;
    QString renderError;
    bool cancelled = false;
};

// Returns true only when a worker has processed every expected output and no
// retryable failure occurred. Warnings and deliberate skips are complete
// outcomes; cancellation, render failure, missing results and per-output
// failures require recovery or retry.
bool exportQueueWorkerFinishedCompletely(
    const ExportQueueWorkerResult &worker,
    qsizetype expectedOutputCount);

// Execution-time collision predicate shared by the worker and regression
// tests. Skip Existing is intentionally re-evaluated immediately before an
// output is written; the preflight observation is not authoritative after a
// queued job has waited or been recovered.
bool exportQueueShouldSkipExistingOutput(
    ProductionExportCollisionPolicy policy,
    const QString &filePath);

class ExportQueueController final : public QObject {
    Q_OBJECT

public:
    explicit ExportQueueController(QObject *parent = nullptr);
    ~ExportQueueController() override;

    bool enqueue(const ExportQueueEnqueueRequest &request,
                 QString *jobId = nullptr,
                 QString *errorMessage = nullptr);
    int restoreRecoverableJobs(QStringList *warnings = nullptr);
    bool resumeRecoveredJob(const QString &jobId,
                            bool confirmExistingFiles,
                            QStringList *collisionWarnings = nullptr,
                            QString *errorMessage = nullptr);
    void preserveForRestartAndWait();
    void resumeAfterAbortedShutdown();

    QVector<ExportQueueJobInfo> jobs() const;
    ExportQueueJobInfo job(const QString &jobId) const;
    bool hasUnfinishedJobs() const;
    bool hasActiveJob() const;
    bool isPaused() const;
    QString activeJobId() const;

    bool setPaused(bool paused);
    bool cancelJob(const QString &jobId);
    void cancelAll();
    bool removeJob(const QString &jobId);
    int clearFinished();
    void shutdownAndWait();

signals:
    void jobsChanged();
    void queueStateChanged();
    void messageAvailable(const QString &message, int timeoutMs);
    void becameIdle();

private:
    struct JobRecord;
    struct Control;

    std::shared_ptr<JobRecord> recordById(const QString &jobId) const;
    void startNextIfPossible();
    void startJob(const std::shared_ptr<JobRecord> &record);
    void updateJobProgress(const QString &jobId,
                           quint64 executionGeneration,
                           int value,
                           int maximum,
                           const QString &statusText,
                           ExportQueueJobState state);
    void finishActiveJob(const ExportQueueWorkerResult &worker);
    void releaseSnapshot(const std::shared_ptr<JobRecord> &record);

    QVector<std::shared_ptr<JobRecord>> m_jobs;
    QString m_activeJobId;
    bool m_paused = false;
    bool m_shuttingDown = false;
    bool m_preserveUnfinishedOnShutdown = false;
    std::shared_ptr<Control> m_activeControl;
    QFutureWatcher<ExportQueueWorkerResult> *m_watcher = nullptr;
};

} // namespace vfx
