#include "ColourResourceAudit.h"

#include "OcioIntegration.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace vfx {
namespace {

constexpr qint64 MaximumAuditedIccBytes = 16LL * 1024LL * 1024LL;

void addIssue(ColourResourceAuditReport *report,
              const ColourResourceIssueSeverity severity,
              const QString &role,
              const QString &message)
{
    if (!report || message.trimmed().isEmpty()) return;
    const auto duplicate = std::find_if(
        report->issues.cbegin(), report->issues.cend(),
        [&](const ColourResourceIssue &issue) {
            return issue.severity == severity
                && issue.role == role
                && issue.message == message;
        });
    if (duplicate == report->issues.cend()) {
        report->issues.push_back({severity, role, message});
    }
}

QString descriptorName(const ColourSpaceDescriptor &descriptor)
{
    if (!descriptor.displayName.trimmed().isEmpty()) {
        return descriptor.displayName.trimmed();
    }
    if (!descriptor.ocioSpace.trimmed().isEmpty()) {
        return descriptor.ocioSpace.trimmed();
    }
    return QStringLiteral("Unnamed colour space");
}

bool hasEmbeddedIccFallback(const ColourSpaceDescriptor &descriptor)
{
    return !descriptor.iccProfile.isEmpty()
        && descriptor.externalFingerprint.size() == 32
        && QCryptographicHash::hash(descriptor.iccProfile,
                                    QCryptographicHash::Sha256)
            == descriptor.externalFingerprint
        && QColorSpace::fromIccProfile(descriptor.iccProfile).isValid();
}

void auditExternalIcc(const QString &role,
                      const ColourSpaceDescriptor &descriptor,
                      ColourResourceAuditReport *report)
{
    if (descriptor.kind != ColourSpaceKind::ExternalIcc) return;

    const QString path = descriptor.externalPath.trimmed();
    const bool embeddedFallback = hasEmbeddedIccFallback(descriptor);
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists() || !info.isFile()) {
        addIssue(
            report,
            embeddedFallback ? ColourResourceIssueSeverity::Warning
                             : ColourResourceIssueSeverity::Blocking,
            role,
            embeddedFallback
                ? QStringLiteral("%1 ICC profile '%2' is no longer available at '%3'. The embedded profile copy is retained and will be used; choose the profile again only to relink its source file.")
                      .arg(role, descriptorName(descriptor), path)
                : QStringLiteral("%1 ICC profile '%2' is unavailable at '%3' and no embedded profile copy exists. The related transform cannot be reproduced until the profile is relinked.")
                      .arg(role, descriptorName(descriptor), path));
        return;
    }
    if (info.size() <= 0 || info.size() > MaximumAuditedIccBytes) {
        addIssue(
            report,
            embeddedFallback ? ColourResourceIssueSeverity::Warning
                             : ColourResourceIssueSeverity::Blocking,
            role,
            embeddedFallback
                ? QStringLiteral("%1 ICC source '%2' is empty or exceeds the 16 MiB safety limit. The embedded profile copy remains authoritative.")
                      .arg(role, path)
                : QStringLiteral("%1 ICC source '%2' is empty or exceeds the 16 MiB safety limit and cannot be used.")
                      .arg(role, path));
        return;
    }

    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        addIssue(
            report,
            embeddedFallback ? ColourResourceIssueSeverity::Warning
                             : ColourResourceIssueSeverity::Blocking,
            role,
            embeddedFallback
                ? QStringLiteral("%1 ICC source '%2' could not be read. The embedded profile copy remains authoritative.")
                      .arg(role, path)
                : QStringLiteral("%1 ICC source '%2' could not be read and no embedded copy is available.")
                      .arg(role, path));
        return;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() != info.size()) {
        addIssue(
            report,
            embeddedFallback ? ColourResourceIssueSeverity::Warning
                             : ColourResourceIssueSeverity::Blocking,
            role,
            embeddedFallback
                ? QStringLiteral("%1 ICC source '%2' could not be read completely. The embedded profile copy remains authoritative.")
                      .arg(role, path)
                : QStringLiteral("%1 ICC source '%2' could not be read completely.")
                      .arg(role, path));
        return;
    }

    const QByteArray currentFingerprint = QCryptographicHash::hash(
        bytes, QCryptographicHash::Sha256);
    if (!descriptor.externalFingerprint.isEmpty()
        && currentFingerprint != descriptor.externalFingerprint) {
        addIssue(
            report,
            embeddedFallback ? ColourResourceIssueSeverity::Warning
                             : ColourResourceIssueSeverity::Blocking,
            role,
            embeddedFallback
                ? QStringLiteral("%1 ICC source '%2' has changed since it was saved. The document continues using its embedded fingerprint-matched copy; explicit relinking is required to adopt the changed file.")
                      .arg(role, path)
                : QStringLiteral("%1 ICC source '%2' has changed and no embedded fingerprint-matched copy is available. Explicit relinking is required.")
                      .arg(role, path));
        return;
    }

    const QColorSpace onDisk = QColorSpace::fromIccProfile(bytes);
    if (!onDisk.isValid()) {
        addIssue(
            report,
            embeddedFallback ? ColourResourceIssueSeverity::Warning
                             : ColourResourceIssueSeverity::Blocking,
            role,
            embeddedFallback
                ? QStringLiteral("%1 ICC source '%2' is no longer a supported profile. The embedded profile copy remains authoritative.")
                      .arg(role, path)
                : QStringLiteral("%1 ICC source '%2' is not a supported profile.")
                      .arg(role, path));
    }
}

bool stateNeedsOcioRuntime(const DocumentColourState &state)
{
    const auto isOcio = [](const ColourSpaceDescriptor &descriptor) {
        return descriptor.kind == ColourSpaceKind::Ocio;
    };
    return isOcio(state.inputProfile)
        || isOcio(state.workingSpace)
        || isOcio(state.proofing.profile)
        || isOcio(state.output.profile)
        || state.displayTransform.kind == DisplayTransformKind::OcioView;
}

void auditOcioDescriptor(const QString &role,
                         const ColourSpaceDescriptor &descriptor,
                         const QSet<QString> &availableSpaces,
                         ColourResourceAuditReport *report)
{
    if (descriptor.kind != ColourSpaceKind::Ocio) return;
    if (!availableSpaces.contains(descriptor.ocioSpace)) {
        addIssue(
            report,
            ColourResourceIssueSeverity::Blocking,
            role,
            QStringLiteral("%1 OCIO colour space '%2' is not present in the saved fingerprint-matched configuration. No replacement space has been substituted.")
                .arg(role, descriptor.ocioSpace));
    }
}

void auditOcio(const DocumentColourState &state,
               ColourResourceAuditReport *report)
{
    if (!state.ocioConfig.isConfigured()) return;

    const bool runtimeRequired = stateNeedsOcioRuntime(state);
    OcioConfigInspection inspection;
    QString error;
    if (!resolveOcioConfiguration(state.ocioConfig, &inspection, &error)) {
        addIssue(
            report,
            runtimeRequired ? ColourResourceIssueSeverity::Blocking
                            : ColourResourceIssueSeverity::Warning,
            QStringLiteral("OpenColorIO"),
            QStringLiteral("The saved OCIO configuration '%1' is unavailable: %2 No alternate configuration has been substituted.")
                .arg(state.ocioConfig.displayName.trimmed().isEmpty()
                         ? state.ocioConfig.identifier
                         : state.ocioConfig.displayName,
                     error.trimmed().isEmpty()
                         ? QStringLiteral("the resource could not be resolved.")
                         : error));
        return;
    }
    if (!inspection.fingerprintMatchesSavedReference) {
        addIssue(
            report,
            runtimeRequired ? ColourResourceIssueSeverity::Blocking
                            : ColourResourceIssueSeverity::Warning,
            QStringLiteral("OpenColorIO"),
            QStringLiteral("The resolved OCIO configuration differs from the fingerprint saved with this document. Explicit relinking is required; no transform has been substituted."));
        return;
    }

    QSet<QString> spaces;
    for (const OcioColourSpaceInfo &space : inspection.colourSpaces) {
        spaces.insert(space.name);
    }
    auditOcioDescriptor(QStringLiteral("Input profile"), state.inputProfile,
                        spaces, report);
    auditOcioDescriptor(QStringLiteral("Working space"), state.workingSpace,
                        spaces, report);
    if (state.proofing.enabled || state.proofing.profile.kind == ColourSpaceKind::Ocio) {
        auditOcioDescriptor(QStringLiteral("Proof profile"), state.proofing.profile,
                            spaces, report);
    }
    auditOcioDescriptor(QStringLiteral("Output profile"), state.output.profile,
                        spaces, report);

    if (state.displayTransform.kind != DisplayTransformKind::OcioView) return;

    const auto displayIt = std::find_if(
        inspection.displays.cbegin(), inspection.displays.cend(),
        [&](const OcioDisplayInfo &display) {
            return display.name == state.displayTransform.ocioDisplay;
        });
    if (displayIt == inspection.displays.cend()) {
        addIssue(
            report,
            ColourResourceIssueSeverity::Blocking,
            QStringLiteral("Display/View"),
            QStringLiteral("OCIO display '%1' is not present in the saved fingerprint-matched configuration. No replacement display has been substituted.")
                .arg(state.displayTransform.ocioDisplay));
        return;
    }
    if (!displayIt->views.contains(state.displayTransform.ocioView)) {
        addIssue(
            report,
            ColourResourceIssueSeverity::Blocking,
            QStringLiteral("Display/View"),
            QStringLiteral("OCIO view '%1' is not available for display '%2'. No replacement view has been substituted.")
                .arg(state.displayTransform.ocioView,
                     state.displayTransform.ocioDisplay));
    }
    if (!state.displayTransform.ocioLook.trimmed().isEmpty()
        && !inspection.looks.contains(state.displayTransform.ocioLook)) {
        addIssue(
            report,
            ColourResourceIssueSeverity::Blocking,
            QStringLiteral("Display/View"),
            QStringLiteral("OCIO look '%1' is not present in the saved fingerprint-matched configuration. No replacement look has been substituted.")
                .arg(state.displayTransform.ocioLook));
    }
}

} // namespace

bool ColourResourceAuditReport::isHealthy() const
{
    return issues.isEmpty();
}

bool ColourResourceAuditReport::hasBlockingIssues() const
{
    return std::any_of(issues.cbegin(), issues.cend(),
                       [](const ColourResourceIssue &issue) {
                           return issue.severity
                               == ColourResourceIssueSeverity::Blocking;
                       });
}

QString ColourResourceAuditReport::summary() const
{
    if (issues.isEmpty()) {
        return QStringLiteral("All saved colour resources are available and fingerprint-matched.");
    }
    int blocking = 0;
    int warnings = 0;
    int advisory = 0;
    for (const ColourResourceIssue &issue : issues) {
        switch (issue.severity) {
        case ColourResourceIssueSeverity::Blocking: ++blocking; break;
        case ColourResourceIssueSeverity::Warning: ++warnings; break;
        case ColourResourceIssueSeverity::Advisory: ++advisory; break;
        }
    }
    if (blocking > 0) {
        return QStringLiteral("%1 blocking colour-resource issue(s), %2 warning(s), %3 advisory item(s).")
            .arg(blocking).arg(warnings).arg(advisory);
    }
    return QStringLiteral("%1 colour-resource warning(s), %2 advisory item(s); embedded/saved state remains unchanged.")
        .arg(warnings).arg(advisory);
}

QStringList ColourResourceAuditReport::messages() const
{
    QStringList result;
    result.reserve(issues.size());
    for (const ColourResourceIssue &issue : issues) {
        QString prefix;
        switch (issue.severity) {
        case ColourResourceIssueSeverity::Blocking:
            prefix = QStringLiteral("Blocking");
            break;
        case ColourResourceIssueSeverity::Warning:
            prefix = QStringLiteral("Warning");
            break;
        case ColourResourceIssueSeverity::Advisory:
            prefix = QStringLiteral("Advisory");
            break;
        }
        result.push_back(issue.role.trimmed().isEmpty()
            ? QStringLiteral("%1: %2").arg(prefix, issue.message)
            : QStringLiteral("%1 — %2: %3").arg(prefix, issue.role, issue.message));
    }
    return result;
}

ColourResourceAuditReport auditDocumentColourResources(
    const DocumentColourState &state)
{
    ColourResourceAuditReport report;
    auditExternalIcc(QStringLiteral("Input profile"), state.inputProfile, &report);
    auditExternalIcc(QStringLiteral("Working space"), state.workingSpace, &report);
    if (state.displayTransform.kind == DisplayTransformKind::IccProfile) {
        auditExternalIcc(QStringLiteral("Display profile"),
                         state.displayTransform.profile, &report);
    }
    if (state.proofing.enabled
        || state.proofing.profile.kind == ColourSpaceKind::ExternalIcc) {
        auditExternalIcc(QStringLiteral("Proof profile"),
                         state.proofing.profile, &report);
    }
    auditExternalIcc(QStringLiteral("Output profile"), state.output.profile,
                     &report);
    auditOcio(state, &report);
    return report;
}

} // namespace vfx
