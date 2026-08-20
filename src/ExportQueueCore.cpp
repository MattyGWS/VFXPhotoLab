#include "ExportQueueCore.h"

#include <algorithm>

namespace vfx {

QString exportQueueJobStateName(const ExportQueueJobState state)
{
    switch (state) {
    case ExportQueueJobState::Pending: return QStringLiteral("Pending");
    case ExportQueueJobState::Running: return QStringLiteral("Running");
    case ExportQueueJobState::Paused: return QStringLiteral("Paused");
    case ExportQueueJobState::Recovered: return QStringLiteral("Recovered");
    case ExportQueueJobState::Completed: return QStringLiteral("Completed");
    case ExportQueueJobState::CompletedWithIssues:
        return QStringLiteral("Completed with issues");
    case ExportQueueJobState::Failed: return QStringLiteral("Failed");
    case ExportQueueJobState::Cancelled: return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Unknown");
}

bool exportQueueJobStateIsTerminal(const ExportQueueJobState state)
{
    return state == ExportQueueJobState::Completed
        || state == ExportQueueJobState::CompletedWithIssues
        || state == ExportQueueJobState::Failed
        || state == ExportQueueJobState::Cancelled;
}

bool exportQueueJobStateCanCancel(const ExportQueueJobState state)
{
    return state == ExportQueueJobState::Pending
        || state == ExportQueueJobState::Running
        || state == ExportQueueJobState::Paused
        || state == ExportQueueJobState::Recovered;
}

bool exportQueueJobStateCanRemove(const ExportQueueJobState state)
{
    return exportQueueJobStateIsTerminal(state);
}

bool exportQueueJobStateCanTransition(const ExportQueueJobState from,
                                      const ExportQueueJobState to)
{
    if (from == to) return true;
    if (exportQueueJobStateIsTerminal(from)) return false;
    switch (from) {
    case ExportQueueJobState::Pending:
        return to == ExportQueueJobState::Running
            || to == ExportQueueJobState::Cancelled
            || to == ExportQueueJobState::Failed;
    case ExportQueueJobState::Running:
        return to == ExportQueueJobState::Paused
            || to == ExportQueueJobState::Completed
            || to == ExportQueueJobState::CompletedWithIssues
            || to == ExportQueueJobState::Failed
            || to == ExportQueueJobState::Cancelled;
    case ExportQueueJobState::Paused:
        return to == ExportQueueJobState::Running
            || to == ExportQueueJobState::Completed
            || to == ExportQueueJobState::CompletedWithIssues
            || to == ExportQueueJobState::Failed
            || to == ExportQueueJobState::Cancelled;
    case ExportQueueJobState::Recovered:
        return to == ExportQueueJobState::Pending
            || to == ExportQueueJobState::Cancelled
            || to == ExportQueueJobState::Failed;
    case ExportQueueJobState::Completed:
    case ExportQueueJobState::CompletedWithIssues:
    case ExportQueueJobState::Failed:
    case ExportQueueJobState::Cancelled:
        return false;
    }
    return false;
}

int exportQueueProgressPercent(const int value, const int maximum)
{
    if (maximum <= 0) return 0;
    const qint64 boundedValue = std::clamp<qint64>(value, 0, maximum);
    return static_cast<int>((boundedValue * 100 + maximum / 2) / maximum);
}

bool validExportQueueJobIdentifier(const QString &identifier)
{
    if (identifier.isEmpty() || identifier.size() > 64
        || identifier != identifier.trimmed()) {
        return false;
    }
    for (const QChar character : identifier) {
        const ushort value = character.unicode();
        const bool lowerAsciiAlphaNumeric =
            (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9');
        if (!lowerAsciiAlphaNumeric && character != QLatin1Char('-')) {
            return false;
        }
    }
    return true;
}

} // namespace vfx
