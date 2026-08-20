#include "DocumentSession.h"

#include "SessionCache.h"
#include "ImageProcessor.h"
#include "SmartLayerTileCache.h"

#include <QFileInfo>
#include <QSet>


namespace vfx {

namespace {
std::atomic<quint64> g_renderSerial {1};

void collectSmartCacheIdentities(const QVector<LayerNode> &layers, QSet<QUuid> *ids)
{
    if (!ids) return;
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Smart) ids->insert(layer.id);
        collectSmartCacheIdentities(layer.children, ids);
    }
}

void collectLayerIds(const QVector<LayerNode> &layers, QSet<QUuid> *ids)
{
    if (!ids) return;
    for (const LayerNode &layer : layers) {
        if (!layer.id.isNull()) ids->insert(layer.id);
        collectLayerIds(layer.children, ids);
    }
}
}

quint64 DocumentSession::nextRenderSerial()
{
    return g_renderSerial.fetch_add(1, std::memory_order_relaxed);
}

DocumentSession::DocumentSession()
    : m_undoStack(std::make_unique<QUndoStack>())
    , m_rasterHistoryStats(std::make_shared<RasterHistoryStats>())
    , m_maskHistoryStats(std::make_shared<RasterHistoryStats>())
    , m_channelHistoryStats(std::make_shared<RasterHistoryStats>())
    , m_structuralHistoryStats(std::make_shared<RasterHistoryStats>())
    , m_selectionHistoryStats(std::make_shared<RasterHistoryStats>())
{
    // Sparse paint commands are already byte-bounded by compressed tile
    // payloads. Keep the established deterministic per-stack command cap as a
    // second guard; DocumentResidencyManager additionally enforces the shared
    // workspace byte budget across resident pixels and retained history.
    m_undoStack->setUndoLimit(100);
}

DocumentSession::~DocumentSession() = default;

const QUuid &DocumentSession::id() const
{
    return m_id;
}

quint64 DocumentSession::renderSerial() const
{
    return m_renderSerial;
}

void DocumentSession::advanceRenderSerial()
{
    m_renderSerial = nextRenderSerial();
}

PhotoDocument &DocumentSession::document()
{
    return m_document;
}

const PhotoDocument &DocumentSession::document() const
{
    return m_document;
}

QUndoStack *DocumentSession::undoStack()
{
    return m_undoStack.get();
}

const QUndoStack *DocumentSession::undoStack() const
{
    return m_undoStack.get();
}

bool &DocumentSession::applyingUndoRedo()
{
    return m_applyingUndoRedo;
}

bool DocumentSession::applyingUndoRedo() const
{
    return m_applyingUndoRedo;
}

bool &DocumentSession::baselineRequiresSave()
{
    return m_baselineRequiresSave;
}

bool DocumentSession::baselineRequiresSave() const
{
    return m_baselineRequiresSave;
}

QUuid &DocumentSession::editTargetLayerId()
{
    return m_editTargetLayerId;
}

const QUuid &DocumentSession::editTargetLayerId() const
{
    return m_editTargetLayerId;
}

LayerEditTarget &DocumentSession::editTarget()
{
    return m_editTarget;
}

LayerEditTarget DocumentSession::editTarget() const
{
    return m_editTarget;
}

ChannelView &DocumentSession::channelView()
{
    return m_channelView;
}

ChannelView DocumentSession::channelView() const
{
    return m_channelView;
}

bool &DocumentSession::selectionEdgesVisible()
{
    return m_selectionEdgesVisible;
}

bool DocumentSession::selectionEdgesVisible() const
{
    return m_selectionEdgesVisible;
}

CloneStampSessionState &DocumentSession::cloneStampState()
{
    return m_cloneStampState;
}

const CloneStampSessionState &DocumentSession::cloneStampState() const
{
    return m_cloneStampState;
}

CloneStampSessionState &DocumentSession::healingBrushState()
{
    return m_healingBrushState;
}

const CloneStampSessionState &DocumentSession::healingBrushState() const
{
    return m_healingBrushState;
}

CropSessionState &DocumentSession::cropState()
{
    return m_cropState;
}

const CropSessionState &DocumentSession::cropState() const
{
    return m_cropState;
}

TransformWorkflowState &DocumentSession::lastTransform()
{
    return m_lastTransform;
}

const TransformWorkflowState &DocumentSession::lastTransform() const
{
    return m_lastTransform;
}

bool DocumentSession::isSmartSourceEditor() const
{
    return m_smartSourceEditBinding.isValid();
}

const DocumentSession::SmartSourceEditBinding &DocumentSession::smartSourceEditBinding() const
{
    return m_smartSourceEditBinding;
}

DocumentSession::SmartSourceEditBinding &DocumentSession::smartSourceEditBinding()
{
    return m_smartSourceEditBinding;
}

void DocumentSession::clearSmartSourceEditBinding()
{
    m_smartSourceEditBinding = {};
}

bool &DocumentSession::propertyUndoActive()
{
    return m_propertyUndoActive;
}

bool DocumentSession::propertyUndoActive() const
{
    return m_propertyUndoActive;
}

QString &DocumentSession::propertyUndoText()
{
    return m_propertyUndoText;
}

DocumentSession::DocumentState &DocumentSession::propertyUndoBefore()
{
    return m_propertyUndoBefore;
}

QVector<QUuid> &DocumentSession::propertyUndoSelection()
{
    return m_propertyUndoSelection;
}

QHash<QString, QIcon> &DocumentSession::thumbnailCache()
{
    return m_thumbnailCache;
}

const QHash<QString, QIcon> &DocumentSession::thumbnailCache() const
{
    return m_thumbnailCache;
}

QImage &DocumentSession::workspaceThumbnail()
{
    return m_workspaceThumbnail;
}

const QImage &DocumentSession::workspaceThumbnail() const
{
    return m_workspaceThumbnail;
}

const std::shared_ptr<RasterHistoryStats> &DocumentSession::rasterHistoryStats() const
{
    return m_rasterHistoryStats;
}

const std::shared_ptr<RasterHistoryStats> &DocumentSession::maskHistoryStats() const
{
    return m_maskHistoryStats;
}

const std::shared_ptr<RasterHistoryStats> &DocumentSession::channelHistoryStats() const
{
    return m_channelHistoryStats;
}

const std::shared_ptr<RasterHistoryStats> &DocumentSession::structuralHistoryStats() const
{
    return m_structuralHistoryStats;
}

const std::shared_ptr<RasterHistoryStats> &DocumentSession::selectionHistoryStats() const
{
    return m_selectionHistoryStats;
}

QVector<QUuid> &DocumentSession::selectedLayerIds()
{
    return m_selectedLayerIds;
}

const QVector<QUuid> &DocumentSession::selectedLayerIds() const
{
    return m_selectedLayerIds;
}

DocumentSession::ViewState &DocumentSession::viewState()
{
    return m_viewState;
}

const DocumentSession::ViewState &DocumentSession::viewState() const
{
    return m_viewState;
}


SessionResidency DocumentSession::residency() const
{
    return m_residency;
}

bool DocumentSession::isDocumentResident() const
{
    return m_residency != SessionResidency::Cold && m_document.hasImage();
}

qint64 DocumentSession::estimatedResidentBytes() const
{
    return isDocumentResident() ? m_document.estimatedResidentBytes() : 0;
}

qint64 DocumentSession::estimatedHistoryBytes() const
{
    return m_rasterHistoryStats->storedBytes
        + m_maskHistoryStats->storedBytes
        + m_channelHistoryStats->storedBytes
        + m_structuralHistoryStats->storedBytes
        + m_selectionHistoryStats->storedBytes;
}

const DocumentSession::Summary &DocumentSession::summary() const
{
    return m_summary;
}

void DocumentSession::refreshSummary()
{
    if (!m_document.hasImage()) {
        return;
    }
    m_summary.displayName = m_document.displayName();
    m_summary.sourcePath = m_document.sourcePath();
    m_summary.projectPath = m_document.projectPath();
    m_summary.pixelSize = m_document.sourceImage().size();
    m_summary.bitDepth = m_document.sourceImage().depth() > 32 ? 16 : 8;
    m_summary.colourModel = m_document.colourModel();
    m_summary.blankDocument = m_document.isBlankDocument();
    m_summary.modified = m_document.isModified();
}

const QString &DocumentSession::backingSnapshotPath() const
{
    return m_backingSnapshotPath;
}

qint64 DocumentSession::backingSnapshotBytes() const
{
    return m_backingSnapshotBytes;
}

bool DocumentSession::historyWasDiscardedForColdStorage() const
{
    return m_historyDiscardedForColdStorage;
}

bool DocumentSession::evictToDisk(SessionCacheStore &store, QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (m_residency == SessionResidency::Cold) {
        return true;
    }
    if (!m_document.hasImage()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("There is no resident document to move to disk.");
        }
        return false;
    }

    refreshSummary();
    QString snapshotPath;
    qint64 snapshotBytes = 0;
    if (!store.writeSnapshot(*this, &snapshotPath, &snapshotBytes, errorMessage)) {
        return false;
    }

    // Keep Undo/Redo resident when possible. Sparse paint history is already
    // compressed, while structural commands account for the image buffers they
    // retain. The residency manager purges history only as a second, hard-memory
    // pressure step after document pixels have been moved to disk. The existing
    // clean baseline must remain unchanged so Undo can still return to the saved
    // state after this session is restored.

    QSet<QUuid> smartCacheIdentities;
    for (const SmartSourceDescriptor &source : m_document.smartSources().descriptors()) {
        smartCacheIdentities.insert(source.id);
    }
    collectSmartCacheIdentities(m_document.layers(), &smartCacheIdentities);
    SmartLayerTileCache::instance().invalidateSources(smartCacheIdentities);
    QSet<QUuid> layerEffectCacheIdentities;
    collectLayerIds(m_document.layers(), &layerEffectCacheIdentities);
    for (const SmartSourceDescriptor &source : m_document.smartSources().descriptors()) {
        if (!source.hasEmbeddedDocument()) continue;
        QVector<LayerNode> embeddedLayers;
        if (m_document.embeddedSmartSourceLayers(source.id, &embeddedLayers)) {
            collectLayerIds(embeddedLayers, &layerEffectCacheIdentities);
        }
    }
    ImageProcessor::invalidateLayerEffectCaches(layerEffectCacheIdentities);

    m_document.clear();
    m_thumbnailCache.clear();
    m_propertyUndoActive = false;
    m_propertyUndoText.clear();
    m_propertyUndoBefore = {};
    m_propertyUndoSelection.clear();
    m_backingSnapshotPath = snapshotPath;
    m_backingSnapshotBytes = snapshotBytes;
    m_residency = SessionResidency::Cold;
    advanceRenderSerial();
    return true;
}

bool DocumentSession::restoreFromDisk(const SessionCacheStore &store, QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (m_residency != SessionResidency::Cold) {
        return m_document.hasImage();
    }
    if (m_backingSnapshotPath.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The document has no private backing snapshot.");
        }
        return false;
    }

    const QString path = m_backingSnapshotPath;
    const qint64 bytes = m_backingSnapshotBytes;
    if (!store.restoreSnapshot(path, this, errorMessage)) {
        return false;
    }
    m_backingSnapshotPath = path;
    m_backingSnapshotBytes = bytes > 0 ? bytes : QFileInfo(path).size();
    m_residency = SessionResidency::Warm;
    advanceRenderSerial();
    return true;
}

void DocumentSession::discardUndoHistoryForMemoryPressure()
{
    if (m_undoStack->count() <= 0) {
        return;
    }
    m_applyingUndoRedo = true;
    m_baselineRequiresSave = m_baselineRequiresSave || m_summary.modified
        || m_document.isModified();
    m_undoStack->clear();
    m_undoStack->setClean();
    *m_rasterHistoryStats = {};
    *m_maskHistoryStats = {};
    *m_channelHistoryStats = {};
    *m_structuralHistoryStats = {};
    *m_selectionHistoryStats = {};
    m_historyDiscardedForColdStorage = true;
    m_applyingUndoRedo = false;
}

void DocumentSession::discardBackingSnapshot(SessionCacheStore &store)
{
    store.removeSnapshot(m_backingSnapshotPath);
    m_backingSnapshotPath.clear();
    m_backingSnapshotBytes = 0;
    m_historyDiscardedForColdStorage = false;
}

void DocumentSession::resetDocumentLocalState()
{
    clearSmartSourceEditBinding();
    // Suppress cleanChanged side effects while the old document is being
    // detached from this session. The MainWindow connection intentionally
    // ignores stack changes made as part of a replacement transaction.
    m_applyingUndoRedo = true;
    m_baselineRequiresSave = false;
    m_editTargetLayerId = {};
    m_editTarget = LayerEditTarget::Pixels;
    m_channelView = ChannelView::Composite;
    m_selectionEdgesVisible = true;
    m_cloneStampState = {};
    m_healingBrushState = {};
    m_cropState = {};
    m_lastTransform = {};
    m_propertyUndoActive = false;
    m_propertyUndoText.clear();
    m_propertyUndoBefore = {};
    m_propertyUndoSelection.clear();
    m_thumbnailCache.clear();
    m_workspaceThumbnail = {};
    m_selectedLayerIds.clear();
    m_viewState = {};
    m_residency = SessionResidency::Hot;
    m_summary = {};
    m_backingSnapshotPath.clear();
    m_backingSnapshotBytes = 0;
    m_historyDiscardedForColdStorage = false;

    m_undoStack->clear();
    m_undoStack->setClean();
    *m_rasterHistoryStats = {};
    *m_maskHistoryStats = {};
    *m_channelHistoryStats = {};
    *m_structuralHistoryStats = {};
    *m_selectionHistoryStats = {};
    advanceRenderSerial();
    m_applyingUndoRedo = false;
}

} // namespace vfx
