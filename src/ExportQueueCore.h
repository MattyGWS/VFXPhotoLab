#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace vfx {

enum class ExportQueueJobState : quint8 {
    Pending,
    Running,
    Paused,
    Recovered,
    Completed,
    CompletedWithIssues,
    Failed,
    Cancelled
};

struct ExportQueueJobInfo {
    QString id;
    QString title;
    QString documentName;
    QString outputDirectory;
    QDateTime createdUtc;
    QDateTime startedUtc;
    QDateTime finishedUtc;
    ExportQueueJobState state = ExportQueueJobState::Pending;
    int progressValue = 0;
    int progressMaximum = 1;
    int totalOutputs = 0;
    int completedOutputs = 0;
    int skippedOutputs = 0;
    int failedOutputs = 0;
    int warningCount = 0;
    QString statusText;
    QStringList details;
};

QString exportQueueJobStateName(ExportQueueJobState state);
bool exportQueueJobStateIsTerminal(ExportQueueJobState state);
bool exportQueueJobStateCanCancel(ExportQueueJobState state);
bool exportQueueJobStateCanRemove(ExportQueueJobState state);
bool exportQueueJobStateCanTransition(ExportQueueJobState from,
                                      ExportQueueJobState to);
int exportQueueProgressPercent(int value, int maximum);
bool validExportQueueJobIdentifier(const QString &identifier);

} // namespace vfx
