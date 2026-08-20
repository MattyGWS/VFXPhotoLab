#pragma once

#include "Adjustment.h"

#include <QByteArray>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QUuid>

namespace vfx {

struct SvgImportResult {
    QSize canvasSize;
    QVector<LayerNode> layers;
    QStringList warnings;
    int importedLayerCount = 0;
    int skippedElementCount = 0;
};

struct SvgExportResult {
    QStringList warnings;
    int exportedLayerCount = 0;
    int skippedLayerCount = 0;
};

class SvgWorkflow final {
public:
    static constexpr qsizetype MaximumFileBytes = 32 * 1024 * 1024;
    static constexpr int MaximumElementCount = 100000;
    static constexpr int MaximumNestingDepth = 256;
    static constexpr int MaximumLayerTreeNodeCount = 20000;
    static constexpr int MaximumEditableLayerCount = 10000;
    static constexpr int MaximumVectorObjectCount = 20000;
    static constexpr int MaximumPathNodeCount = 500000;
    static constexpr qsizetype MaximumTextCharacterCount = 4 * 1024 * 1024;

    static bool importFile(const QString &filePath,
                           SvgImportResult *result,
                           QString *errorMessage = nullptr);
    static bool importData(const QByteArray &data,
                           const QString &sourceName,
                           SvgImportResult *result,
                           QString *errorMessage = nullptr);

    // An empty selectedRootIds set exports every supported layer. Otherwise,
    // selected roots are detached with their accumulated world transform and
    // exported as independent top-level SVG groups.
    static bool exportFile(const QString &filePath,
                           const QSize &canvasSize,
                           const QVector<LayerNode> &layers,
                           const QSet<QUuid> &selectedRootIds,
                           SvgExportResult *result,
                           QString *errorMessage = nullptr);
    static QByteArray exportData(const QSize &canvasSize,
                                 const QVector<LayerNode> &layers,
                                 const QSet<QUuid> &selectedRootIds,
                                 SvgExportResult *result,
                                 QString *errorMessage = nullptr);
};

} // namespace vfx
