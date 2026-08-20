#pragma once

#include "PhotoDocument.h"
#include "CropTypes.h"
#include "RasterHistory.h"

#include <QHash>
#include <QIcon>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QUndoStack>
#include <QUuid>
#include <QVector>

#include <atomic>
#include <memory>

namespace vfx {

class SessionCacheStore;


enum class SessionResidency {
    Hot,
    Warm,
    Cold
};

enum class ChannelView {
    Composite,
    Grey,
    Red,
    Green,
    Blue,
    Alpha,
    Mask
};

enum class LayerEditTarget {
    Pixels,
    Grey,
    Red,
    Green,
    Blue,
    Alpha,
    Mask
};


enum class TransformWorkflowKind : quint8 {
    LayerTransform,
    OrthogonalDocument
};

struct TransformWorkflowState {
    bool valid = false;
    TransformWorkflowKind kind = TransformWorkflowKind::LayerTransform;
    QTransform documentTransform;
    QRectF sourceBounds;
    QPointF initialPivotDocument;
    QPointF finalPivotDocument;
    int mode = 0;
    int interpolation = 1;
    bool selectedPixels = false;
    int orthogonalDocumentOperation = -1;
};

struct CloneStampSessionState {
    bool sourceValid = false;
    bool compositeSource = false;
    QPointF sourceAnchorDocument;
    QUuid sourceLayerId;
    LayerEditTarget sourceTarget = LayerEditTarget::Pixels;
    bool alignedMode = true;
    bool alignedOffsetValid = false;
    QPointF alignedOffsetDocument;
};

// Owns all durable state that belongs to one open document rather than to the
// application window. The workspace may keep many instances open while one
// shared canvas and renderer bind to the active session. Inactive sessions can
// remain warm or move to private cold storage without multiplying render-cache
// budgets.
class DocumentSession final {
public:
    struct DocumentState {
        QImage canvasImage;
        SelectionMask::Snapshot selection;
        QVector<LayerNode> layers;
        SmartSourceRegistry smartSources;
        QVector<double> horizontalGuides;
        QVector<double> verticalGuides;
        double resolutionX = 72.0;
        double resolutionY = 72.0;
        DocumentColourState colourState;
        QUuid editTargetLayerId;
        LayerEditTarget editTarget = LayerEditTarget::Pixels;
        ChannelView channelView = ChannelView::Composite;
        CropSessionState cropState;
    };

    struct SmartSourceEditBinding {
        QUuid ownerSessionId;
        QUuid sourceId;
        QString sourceName;
        QHash<QUuid, quint64> baselineSourceRevisions;

        bool isValid() const
        {
            return !ownerSessionId.isNull() && !sourceId.isNull();
        }
    };

    struct ViewState {
        double zoom = 1.0;
        QPoint scrollPosition;
        bool fitToView = true;
        bool valid = false;
    };

    struct Summary {
        QString displayName;
        QString sourcePath;
        QString projectPath;
        QSize pixelSize;
        int bitDepth = 8;
        DocumentColourModel colourModel = DocumentColourModel::Rgb;
        bool blankDocument = false;
        bool modified = false;
    };

    DocumentSession();
    ~DocumentSession();

    DocumentSession(const DocumentSession &) = delete;
    DocumentSession &operator=(const DocumentSession &) = delete;
    DocumentSession(DocumentSession &&) = delete;
    DocumentSession &operator=(DocumentSession &&) = delete;

    const QUuid &id() const;
    quint64 renderSerial() const;
    void advanceRenderSerial();

    PhotoDocument &document();
    const PhotoDocument &document() const;

    QUndoStack *undoStack();
    const QUndoStack *undoStack() const;

    bool &applyingUndoRedo();
    bool applyingUndoRedo() const;
    bool &baselineRequiresSave();
    bool baselineRequiresSave() const;

    QUuid &editTargetLayerId();
    const QUuid &editTargetLayerId() const;
    LayerEditTarget &editTarget();
    LayerEditTarget editTarget() const;
    ChannelView &channelView();
    ChannelView channelView() const;
    bool &selectionEdgesVisible();
    bool selectionEdgesVisible() const;
    CloneStampSessionState &cloneStampState();
    const CloneStampSessionState &cloneStampState() const;
    CloneStampSessionState &healingBrushState();
    const CloneStampSessionState &healingBrushState() const;
    CropSessionState &cropState();
    const CropSessionState &cropState() const;
    TransformWorkflowState &lastTransform();
    const TransformWorkflowState &lastTransform() const;

    bool isSmartSourceEditor() const;
    const SmartSourceEditBinding &smartSourceEditBinding() const;
    SmartSourceEditBinding &smartSourceEditBinding();
    void clearSmartSourceEditBinding();

    bool &propertyUndoActive();
    bool propertyUndoActive() const;
    QString &propertyUndoText();
    DocumentState &propertyUndoBefore();
    QVector<QUuid> &propertyUndoSelection();

    QHash<QString, QIcon> &thumbnailCache();
    const QHash<QString, QIcon> &thumbnailCache() const;
    QImage &workspaceThumbnail();
    const QImage &workspaceThumbnail() const;

    const std::shared_ptr<RasterHistoryStats> &rasterHistoryStats() const;
    const std::shared_ptr<RasterHistoryStats> &maskHistoryStats() const;
    const std::shared_ptr<RasterHistoryStats> &channelHistoryStats() const;
    const std::shared_ptr<RasterHistoryStats> &structuralHistoryStats() const;
    const std::shared_ptr<RasterHistoryStats> &selectionHistoryStats() const;

    QVector<QUuid> &selectedLayerIds();
    const QVector<QUuid> &selectedLayerIds() const;
    ViewState &viewState();
    const ViewState &viewState() const;

    SessionResidency residency() const;
    bool isDocumentResident() const;
    qint64 estimatedResidentBytes() const;
    qint64 estimatedHistoryBytes() const;
    const Summary &summary() const;
    void refreshSummary();

    const QString &backingSnapshotPath() const;
    qint64 backingSnapshotBytes() const;
    bool historyWasDiscardedForColdStorage() const;

    bool evictToDisk(SessionCacheStore &store, QString *errorMessage = nullptr);
    bool restoreFromDisk(const SessionCacheStore &store, QString *errorMessage = nullptr);
    void discardUndoHistoryForMemoryPressure();
    void discardBackingSnapshot(SessionCacheStore &store);

    // Clears state that must never leak when this stable session object is
    // reused for the welcome-page placeholder or its first loaded document.
    // Normal workspace switching preserves each session instead of calling this.
    void resetDocumentLocalState();

private:
    friend class SessionSnapshotCodec;
    friend class DocumentResidencyManager;

    static quint64 nextRenderSerial();

    QUuid m_id = QUuid::createUuid();
    quint64 m_renderSerial = nextRenderSerial();
    PhotoDocument m_document;
    std::unique_ptr<QUndoStack> m_undoStack;

    bool m_applyingUndoRedo = false;
    bool m_baselineRequiresSave = false;
    QUuid m_editTargetLayerId;
    LayerEditTarget m_editTarget = LayerEditTarget::Pixels;
    ChannelView m_channelView = ChannelView::Composite;
    bool m_selectionEdgesVisible = true;
    CloneStampSessionState m_cloneStampState;
    CloneStampSessionState m_healingBrushState;
    CropSessionState m_cropState;
    TransformWorkflowState m_lastTransform;
    SmartSourceEditBinding m_smartSourceEditBinding;

    bool m_propertyUndoActive = false;
    QString m_propertyUndoText;
    DocumentState m_propertyUndoBefore;
    QVector<QUuid> m_propertyUndoSelection;

    QHash<QString, QIcon> m_thumbnailCache;
    QImage m_workspaceThumbnail;
    std::shared_ptr<RasterHistoryStats> m_rasterHistoryStats;
    std::shared_ptr<RasterHistoryStats> m_maskHistoryStats;
    std::shared_ptr<RasterHistoryStats> m_channelHistoryStats;
    std::shared_ptr<RasterHistoryStats> m_structuralHistoryStats;
    std::shared_ptr<RasterHistoryStats> m_selectionHistoryStats;
    QVector<QUuid> m_selectedLayerIds;
    ViewState m_viewState;

    SessionResidency m_residency = SessionResidency::Hot;
    Summary m_summary;
    QString m_backingSnapshotPath;
    qint64 m_backingSnapshotBytes = 0;
    bool m_historyDiscardedForColdStorage = false;
};

} // namespace vfx
