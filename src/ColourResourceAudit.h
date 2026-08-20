#pragma once

#include "ColourManagement.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace vfx {

enum class ColourResourceIssueSeverity : quint8 {
    Advisory,
    Warning,
    Blocking
};

struct ColourResourceIssue {
    ColourResourceIssueSeverity severity = ColourResourceIssueSeverity::Warning;
    QString role;
    QString message;
};

struct ColourResourceAuditReport {
    QVector<ColourResourceIssue> issues;

    bool isHealthy() const;
    bool hasBlockingIssues() const;
    QString summary() const;
    QStringList messages() const;
};

// Audits only external dependencies and saved references. It never mutates the
// document state, relinks a profile/configuration or substitutes a different
// resource. Embedded ICC bytes remain authoritative when their original file
// is missing or changed.
ColourResourceAuditReport auditDocumentColourResources(
    const DocumentColourState &state);

} // namespace vfx
