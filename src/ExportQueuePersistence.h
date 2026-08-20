#pragma once

#include "PhotoDocument.h"
#include "ProductionExport.h"

#include <QDateTime>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

namespace vfx {

struct ExportQueueEnqueueRequest {
    QString title;
    QString documentName;
    QImage source;
    QVector<LayerNode> layers;
    DocumentColourState colourState;
    ColourProcessingCompatibility processingCompatibility =
        ColourProcessingCompatibility::ManagedV1;
    ProductionExportPlan plan;
    QVector<ResolvedProductionExportOutput> outputs;
};

struct RecoverableExportQueueJob {
    QString id;
    QDateTime createdUtc;
    ExportQueueEnqueueRequest request;
};

class ExportQueuePersistence final {
public:
    static constexpr int SchemaVersion = 1;
    static constexpr int MaximumRecoverableJobs = 16;

    static QString storageDirectory();
    static QString jobPath(const QString &jobId);

    static bool writeJob(const QString &jobId,
                         const QDateTime &createdUtc,
                         const ExportQueueEnqueueRequest &request,
                         QString *errorMessage = nullptr);
    static bool removeJob(const QString &jobId,
                          QString *errorMessage = nullptr);
    static QVector<RecoverableExportQueueJob> loadJobs(
        QStringList *warnings = nullptr);
};

} // namespace vfx
