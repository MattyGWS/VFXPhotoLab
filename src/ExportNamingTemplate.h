#pragma once

#include <QDateTime>
#include <QSize>
#include <QString>
#include <QStringList>

namespace vfx {

struct ExportNamingContext {
    QString documentName;
    QString profileName;
    QString formatSuffix;
    int bitDepth = 8;
    QSize imageSize;
    QString workingSpaceName;
    QString outputSpaceName;
    QDateTime timestampUtc;
};

class ExportNamingTemplate final {
public:
    static constexpr int MaximumTemplateLength = 256;
    static constexpr int MaximumResolvedStemLength = 180;

    static QStringList supportedTokens();
    static QString tokenHelpText();

    static bool validate(const QString &nameTemplate,
                         QString *errorMessage = nullptr);
    static QString resolve(const QString &nameTemplate,
                           const ExportNamingContext &context,
                           QString *errorMessage = nullptr);
    static QString portableComponent(const QString &text,
                                     const QString &fallback = QStringLiteral("output"));
};

} // namespace vfx
