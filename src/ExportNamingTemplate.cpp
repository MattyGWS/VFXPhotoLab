#include "ExportNamingTemplate.h"

#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>

namespace vfx {
namespace {

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) *errorMessage = message;
}

QString documentStem(const QString &name)
{
    const QString fileName = QFileInfo(name).fileName();
    const QString base = QFileInfo(fileName).completeBaseName().trimmed();
    return base.isEmpty() ? fileName.trimmed() : base;
}

bool portableStemIsSafe(const QString &stem, QString *errorMessage)
{
    if (stem.isEmpty()) {
        setError(errorMessage,
                 QStringLiteral("The filename template resolves to an empty name."));
        return false;
    }
    if (stem.size() > ExportNamingTemplate::MaximumResolvedStemLength) {
        setError(errorMessage,
                 QStringLiteral("The resolved filename is too long."));
        return false;
    }
    if (stem == QStringLiteral(".") || stem == QStringLiteral("..")) {
        setError(errorMessage,
                 QStringLiteral("The resolved filename is not safe."));
        return false;
    }
    if (stem.endsWith(QLatin1Char(' ')) || stem.endsWith(QLatin1Char('.'))) {
        setError(errorMessage,
                 QStringLiteral("The resolved filename may not end with a space or full stop."));
        return false;
    }
    static const QRegularExpression forbidden(
        QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])"));
    if (stem.contains(forbidden)) {
        setError(errorMessage,
                 QStringLiteral("The resolved filename contains characters that are not portable between Linux and Windows."));
        return false;
    }
    static const QRegularExpression reserved(
        QStringLiteral("^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\\..*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(stem).hasMatch()) {
        setError(errorMessage,
                 QStringLiteral("The resolved filename is reserved on Windows."));
        return false;
    }
    return true;
}

} // namespace

QStringList ExportNamingTemplate::supportedTokens()
{
    return {
        QStringLiteral("document"),
        QStringLiteral("profile"),
        QStringLiteral("format"),
        QStringLiteral("bit_depth"),
        QStringLiteral("width"),
        QStringLiteral("height"),
        QStringLiteral("working_space"),
        QStringLiteral("output_space"),
        QStringLiteral("date"),
        QStringLiteral("time")
    };
}

QString ExportNamingTemplate::tokenHelpText()
{
    return QStringLiteral(
        "{document}, {profile}, {format}, {bit_depth}, {width}, {height}, "
        "{working_space}, {output_space}, {date}, {time}. Use {{ or }} for a literal brace.");
}

bool ExportNamingTemplate::validate(const QString &nameTemplate,
                                    QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (nameTemplate.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("Enter a filename template."));
        return false;
    }
    if (nameTemplate.size() > MaximumTemplateLength) {
        setError(errorMessage, QStringLiteral("The filename template is too long."));
        return false;
    }

    const QStringList tokens = supportedTokens();
    for (qsizetype index = 0; index < nameTemplate.size();) {
        const QChar current = nameTemplate.at(index);
        if (current == QLatin1Char('{')) {
            if (index + 1 < nameTemplate.size()
                && nameTemplate.at(index + 1) == QLatin1Char('{')) {
                index += 2;
                continue;
            }
            const qsizetype close = nameTemplate.indexOf(QLatin1Char('}'), index + 1);
            if (close < 0) {
                setError(errorMessage,
                         QStringLiteral("The filename template contains an unmatched opening brace."));
                return false;
            }
            const QString token = nameTemplate.mid(index + 1, close - index - 1);
            if (!tokens.contains(token)) {
                setError(errorMessage,
                         QStringLiteral("Unknown filename token: {%1}.").arg(token));
                return false;
            }
            index = close + 1;
            continue;
        }
        if (current == QLatin1Char('}')) {
            if (index + 1 < nameTemplate.size()
                && nameTemplate.at(index + 1) == QLatin1Char('}')) {
                index += 2;
                continue;
            }
            setError(errorMessage,
                     QStringLiteral("The filename template contains an unmatched closing brace."));
            return false;
        }
        if (current.unicode() < 0x20
            || QStringLiteral("<>:\"/\\|?*").contains(current)) {
            setError(errorMessage,
                     QStringLiteral("The filename template contains a character that is not portable between Linux and Windows."));
            return false;
        }
        ++index;
    }
    return true;
}

QString ExportNamingTemplate::portableComponent(const QString &text,
                                                const QString &fallback)
{
    QString result = text.normalized(QString::NormalizationForm_KC).trimmed();
    result.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f]+)")),
                   QStringLiteral("-"));
    result.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    result.replace(QRegularExpression(QStringLiteral("-+")), QStringLiteral("-"));
    while (result.endsWith(QLatin1Char(' ')) || result.endsWith(QLatin1Char('.'))) {
        result.chop(1);
    }
    while (result.startsWith(QLatin1Char(' '))) result.remove(0, 1);
    if (result.isEmpty()) result = fallback;
    if (result.size() > 72) result = result.left(72).trimmed();
    return result;
}

QString ExportNamingTemplate::resolve(const QString &nameTemplate,
                                      const ExportNamingContext &context,
                                      QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (!validate(nameTemplate, errorMessage)) return {};

    const QDateTime timestamp = context.timestampUtc.isValid()
        ? context.timestampUtc.toUTC() : QDateTime::currentDateTimeUtc();
    const QHash<QString, QString> replacements = {
        {QStringLiteral("document"), portableComponent(documentStem(context.documentName),
                                                       QStringLiteral("document"))},
        {QStringLiteral("profile"), portableComponent(context.profileName,
                                                      QStringLiteral("custom"))},
        {QStringLiteral("format"), portableComponent(context.formatSuffix.toLower(),
                                                     QStringLiteral("image"))},
        {QStringLiteral("bit_depth"), QStringLiteral("%1bit").arg(context.bitDepth)},
        {QStringLiteral("width"), QString::number(std::max(0, context.imageSize.width()))},
        {QStringLiteral("height"), QString::number(std::max(0, context.imageSize.height()))},
        {QStringLiteral("working_space"), portableComponent(context.workingSpaceName,
                                                            QStringLiteral("working"))},
        {QStringLiteral("output_space"), portableComponent(context.outputSpaceName,
                                                           QStringLiteral("output"))},
        {QStringLiteral("date"), timestamp.date().toString(QStringLiteral("yyyy-MM-dd"))},
        {QStringLiteral("time"), timestamp.time().toString(QStringLiteral("HH-mm-ss"))}
    };

    QString resolved;
    resolved.reserve(nameTemplate.size() + 32);
    for (qsizetype index = 0; index < nameTemplate.size();) {
        const QChar current = nameTemplate.at(index);
        if (current == QLatin1Char('{') && index + 1 < nameTemplate.size()
            && nameTemplate.at(index + 1) == QLatin1Char('{')) {
            resolved += QLatin1Char('{');
            index += 2;
            continue;
        }
        if (current == QLatin1Char('}') && index + 1 < nameTemplate.size()
            && nameTemplate.at(index + 1) == QLatin1Char('}')) {
            resolved += QLatin1Char('}');
            index += 2;
            continue;
        }
        if (current == QLatin1Char('{')) {
            const qsizetype close = nameTemplate.indexOf(QLatin1Char('}'), index + 1);
            const QString token = nameTemplate.mid(index + 1, close - index - 1);
            resolved += replacements.value(token);
            index = close + 1;
            continue;
        }
        resolved += current;
        ++index;
    }
    resolved = resolved.trimmed();
    if (!portableStemIsSafe(resolved, errorMessage)) return {};
    return resolved;
}

} // namespace vfx
