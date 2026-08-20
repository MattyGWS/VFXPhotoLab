#pragma once

#include "Adjustment.h"

#include <QString>
#include <QUuid>
#include <QVector>

namespace vfx {

class PhotoDocument;

enum class LayerMergeKind {
    Raster,
    Vector
};

struct LayerMergePlan {
    LayerMergeKind kind = LayerMergeKind::Raster;
    QVector<QUuid> layerIds; // Top-to-bottom sibling order.
    QUuid parentId;
    int firstIndex = -1;
    int lastIndex = -1;

    bool isValid() const
    {
        return layerIds.size() >= 2
            && firstIndex >= 0
            && lastIndex >= firstIndex
            && lastIndex - firstIndex + 1 == layerIds.size();
    }
};

class LayerMergeOperations final {
public:
    static LayerMergePlan analyse(const PhotoDocument &document,
                                  const QVector<QUuid> &selectedRootLayerIds,
                                  QString *errorMessage = nullptr);

    static bool buildMergedLayer(const PhotoDocument &document,
                                 const LayerMergePlan &plan,
                                 LayerNode *mergedLayer,
                                 QString *errorMessage = nullptr);

    static bool replacePlannedRange(const QVector<LayerNode> &sourceLayers,
                                    const LayerMergePlan &plan,
                                    const LayerNode &mergedLayer,
                                    QVector<LayerNode> *resultLayers,
                                    QString *errorMessage = nullptr);
};

} // namespace vfx
