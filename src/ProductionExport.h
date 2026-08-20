#pragma once

#include "ExportProfileStore.h"
#include "ImageExport.h"
#include "ImageSizeOperations.h"

#include <QDateTime>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace vfx {

enum class ProductionExportResizeMode : quint8 {
    OriginalSize,
    ExactPixels,
    LongEdge,
    Percentage
};

enum class ProductionExportCollisionPolicy : quint8 {
    AskBeforeStart,
    Overwrite,
    SkipExisting,
    AutoRename
};

struct ProductionExportResize {
    ProductionExportResizeMode mode = ProductionExportResizeMode::OriginalSize;
    int width = 0;
    int height = 0;
    int longEdge = 0;
    double percentage = 100.0;
    bool preserveAspect = true;
    ImageResampleMethod method = ImageResampleMethod::Bicubic;

    QSize resolvedSize(const QSize &sourceSize,
                       QString *errorMessage = nullptr) const;
    bool isValid(const QSize &sourceSize,
                 QString *errorMessage = nullptr) const;
};

struct ProductionExportOutput {
    QString id;
    bool enabled = true;
    QString profileId;
    QString profileName;
    ExportProfileData profile;
    QString namingTemplate;
    ProductionExportResize resize;
};

struct ProductionExportPlan {
    QString outputDirectory;
    QString documentName;
    QSize documentSize;
    QString workingSpaceName;
    QDateTime timestampUtc;
    ProductionExportCollisionPolicy collisionPolicy =
        ProductionExportCollisionPolicy::AskBeforeStart;
    QVector<ProductionExportOutput> outputs;
};

struct ResolvedProductionExportOutput {
    QString id;
    QString profileId;
    QString profileName;
    QSize outputSize;
    ImageResampleMethod resampleMethod = ImageResampleMethod::Bicubic;
    bool resizeRequired = false;
    bool existedAtPreflight = false;
    bool skipExisting = false;
    ImageExportRequest request;
};

QString productionExportResizeModeName(ProductionExportResizeMode mode);
QString productionExportCollisionPolicyName(ProductionExportCollisionPolicy policy);
QString imageResampleMethodName(ImageResampleMethod method);

// Resolves a fresh non-existing sibling path without modifying the filesystem.
// reservedPaths should contain every path already claimed by the current job.
QString uniqueProductionExportOutputPath(
    const QString &requestedPath,
    const QStringList &reservedPaths = {});

bool resolveProductionExportPlan(
    const ProductionExportPlan &plan,
    const DocumentColourState &colourState,
    QVector<ResolvedProductionExportOutput> *resolvedOutputs,
    QStringList *warnings = nullptr,
    QString *errorMessage = nullptr);

// Verifies that an already-resolved queue payload is a faithful, safe snapshot
// of the enabled outputs in `plan`. This deliberately does not recalculate
// file collisions, so a file appearing between dialog preflight and queue
// insertion cannot rewrite the user's confirmed collision decision.
bool validateResolvedProductionExportOutputs(
    const ProductionExportPlan &plan,
    const DocumentColourState &colourState,
    const QVector<ResolvedProductionExportOutput> &resolvedOutputs,
    QString *errorMessage = nullptr);

} // namespace vfx
