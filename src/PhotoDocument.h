#pragma once

#include "Adjustment.h"
#include "ColourManagement.h"
#include "SelectionMask.h"

#include <QByteArray>
#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QLineF>
#include <QRectF>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QSize>
#include <QUuid>
#include <QTransform>
#include <QVector>

#include <functional>

namespace vfx {

class SessionSnapshotCodec;

enum class DocumentColourModel {
    Rgb,
    Grayscale
};

struct ImageFileReadResult {
    QImage image;
    ImageColourImportInfo colourInfo;

    bool isValid() const { return !image.isNull(); }
};

struct NewDocumentSettings {
    QString name = QStringLiteral("Untitled Photo");
    QSize pixelSize {1920, 1080};
    int bitDepth = 8;
    DocumentColourModel colourModel = DocumentColourModel::Rgb;
    QColorSpace colourSpace = QColorSpace(QColorSpace::SRgb);
    QColor backgroundColour = QColor(Qt::white);
    double resolutionX = 72.0;
    double resolutionY = 72.0;
};

class PhotoDocument final {
public:
    static constexpr int ProjectFormatVersion = 27;
    static constexpr int PreviewMaxDimension = 2048;

    bool hasImage() const;
    bool isModified() const;
    void setModified(bool modified = true);

    const QImage &sourceImage() const;
    const QImage &previewSource() const;
    const QString &sourcePath() const;
    const QString &projectPath() const;
    const QUuid &documentIdentity() const;
    const QStringList &loadWarnings() const;
    const QStringList &colourResourceWarnings() const;
    const QStringList &linkedSourceWarnings() const;
    QString displayName() const;
    const QString &documentName() const;
    DocumentColourModel colourModel() const;
    bool isBlankDocument() const;
    double resolutionX() const;
    double resolutionY() const;
    QString colourProfileName() const;
    const DocumentColourState &colourState() const;
    quint64 colourStateRevision() const;
    QByteArray colourStateFingerprint() const;
    qint64 estimatedResidentBytes() const;

    const QVector<LayerNode> &layers() const;
    const SmartSourceRegistry &smartSources() const;
    bool registerSmartSource(const SmartSourceDescriptor &descriptor, QString *errorMessage = nullptr);
    bool replaceSmartSource(const SmartSourceDescriptor &descriptor, QString *errorMessage = nullptr);
    bool removeSmartSource(const QUuid &id, QString *errorMessage = nullptr);
    bool bumpSmartSourceRevision(const QUuid &id, quint64 *newRevision = nullptr);
    // 0.14.0f per-instance Live Filter stack operations. These deliberately
    // mutate only Smart Layer instance state; the authoritative Smart Source
    // and sibling instances remain unchanged.
    QUuid addLiveFilter(const QUuid &smartLayerId, AdjustmentType type,
                        QString *errorMessage = nullptr);
    bool removeLiveFilter(const QUuid &smartLayerId, const QUuid &filterId);
    bool setLiveFilterEnabled(const QUuid &smartLayerId, const QUuid &filterId, bool enabled);
    bool updateLiveFilter(const QUuid &smartLayerId, const QUuid &filterId,
                          const AdjustmentData &adjustment);
    bool moveLiveFilter(const QUuid &smartLayerId, const QUuid &filterId, int destinationIndex);
    bool addLiveFilterMask(const QUuid &smartLayerId, const QUuid &filterId);
    bool removeLiveFilterMask(const QUuid &smartLayerId, const QUuid &filterId);
    bool setLiveFilterMaskEnabled(const QUuid &smartLayerId, const QUuid &filterId, bool enabled);
    bool setLiveFilterMaskInverted(const QUuid &smartLayerId, const QUuid &filterId, bool inverted);
    bool updateLiveFilterMask(const QUuid &smartLayerId, const QUuid &filterId,
                              const QImage &mask, const QSize &referenceSize,
                              const QPointF &referenceOrigin);
    // Per-layer Layer Effect stack operations. Effect definitions are
    // separate from Live Filters and may be owned by Raster/Vector/Text/Smart
    // layers. Until a renderer exists for an effect type it is created disabled
    // and cannot be enabled, so authored state never claims a silent no-op.
    QUuid addLayerEffect(const QUuid &layerId, LayerEffectType type,
                         QString *errorMessage = nullptr);
    bool removeLayerEffect(const QUuid &layerId, const QUuid &effectId);
    bool setLayerEffectEnabled(const QUuid &layerId, const QUuid &effectId,
                               bool enabled, QString *errorMessage = nullptr);
    bool updateLayerEffect(const QUuid &layerId, const QUuid &effectId,
                           const LayerEffect &updated, QString *errorMessage = nullptr);
    bool moveLayerEffect(const QUuid &layerId, const QUuid &effectId, int destinationIndex);
    bool canConvertLayersToSmart(const QVector<QUuid> &ids,
                                 QString *errorMessage = nullptr) const;
    QUuid convertLayersToEmbeddedSmart(const QVector<QUuid> &ids,
                                       QString *errorMessage = nullptr);
    bool embeddedSmartSourceLayers(const QUuid &sourceId,
                                   QVector<LayerNode> *layers,
                                   QSize *canvasSize = nullptr,
                                   DocumentColourState *colourState = nullptr,
                                   QString *errorMessage = nullptr) const;
    // 0.14.0l explicit cross-document Smart Source workflow. Linked sources
    // retain their own authoritative .vfxphoto colour/pixel state; this document
    // stores only identity/path/fingerprint plus a derived presentation cache.
    QUuid createLinkedSmartLayer(const QString &projectPath,
                                 const QUuid &selection = {},
                                 QString *errorMessage = nullptr);
    bool replaceLinkedSmartSource(const QUuid &smartLayerId,
                                  const QString &projectPath,
                                  QString *errorMessage = nullptr);
    bool relinkLinkedSmartSource(const QUuid &smartLayerId,
                                 const QString &projectPath,
                                 QString *errorMessage = nullptr);
    bool embedLinkedSmartSource(const QUuid &smartLayerId,
                                QString *errorMessage = nullptr);
    bool refreshLinkedSmartSources(QHash<QUuid, quint64> *changedRevisions = nullptr,
                                   QStringList *warnings = nullptr,
                                   QString *errorMessage = nullptr,
                                   const QUuid &triggerDocumentId = {},
                                   const QString &triggerProjectPath = {});
    // Runtime dependency query used by 0.14.0m open-document propagation. The
    // identity closure is populated during linked-source resolution; the path
    // fallback keeps direct links targetable during migration/relink edges.
    bool dependsOnLinkedDocument(const QUuid &documentId,
                                 const QString &projectPath = {}) const;
    QString resolvedLinkedSmartSourcePath(const QUuid &smartLayerId) const;
    // Materialise an embedded Smart Source as a normal editable PhotoDocument.
    // The returned document owns an editing branch of the source registry so
    // nested Smart Layers remain fully functional while the source is open.
    bool createEditableSmartSourceDocument(
        const QUuid &sourceId,
        PhotoDocument *editableDocument,
        QHash<QUuid, quint64> *baselineSourceRevisions = nullptr,
        QString *errorMessage = nullptr) const;
    // Atomically commit a source-editor branch back into this owning document.
    // Only Smart Sources whose revisions changed relative to the editor's open
    // baseline are adopted; stale owner revisions are rejected rather than
    // overwritten. The edited source and transitive dependants are then
    // selectively invalidated and re-presented.
    bool commitEditableSmartSourceDocument(
        const QUuid &sourceId,
        const PhotoDocument &editedDocument,
        const QHash<QUuid, quint64> &baselineSourceRevisions,
        QHash<QUuid, quint64> *changedRevisions = nullptr,
        QString *errorMessage = nullptr);
    // Refresh the source-registry branch of an already open source editor
    // after its owner accepts a Save. Editable layers remain untouched; only
    // source revisions/presentations and the lightweight observed revisions on
    // nested Smart Layer instances are synchronised.
    bool adoptSmartSourceRegistrySnapshot(const SmartSourceRegistry &registry,
                                          QString *errorMessage = nullptr,
                                          const QString &registryOwnerProjectPath = {});
    int layerCount() const;
    int adjustmentCount() const;
    QUuid baseLayerId() const;
    LayerNode layerById(const QUuid &id) const;
    bool containsLayer(const QUuid &id) const;
    QPointF layerWorldOffset(const QUuid &id) const;
    QTransform layerWorldTransform(const QUuid &id) const;
    QTransform layerParentWorldTransform(const QUuid &id) const;

    const SelectionMask &selectionMask() const;
    SelectionMask &selectionMask();

    const QVector<double> &horizontalGuides() const;
    const QVector<double> &verticalGuides() const;
    void setGuides(const QVector<double> &horizontal, const QVector<double> &vertical);

    void clear();
    bool createNewDocument(const NewDocumentSettings &settings,
                           QString *errorMessage = nullptr);
    void setSourceImage(const QImage &image, const QString &sourcePath = {});
    void setSourceImage(const QImage &image,
                        const QString &sourcePath,
                        const ImageColourImportInfo &colourInfo);
    bool replaceCanvasImage(const QImage &image);
    // Atomically replace the complete document-space structural state used by
    // Canvas Size, Reveal/Fit/Trim and structural Undo/Redo. Every candidate
    // is validated and prepared before any live member changes, so a damaged
    // selection, duplicate layer ID, invalid guide or failed canvas conversion
    // can never leave a partially replaced document.
    bool replaceStructuralState(
        const QImage &canvasImage,
        const QVector<LayerNode> &layers,
        const SelectionMask::Snapshot &selection,
        const QVector<double> &horizontalGuides,
        const QVector<double> &verticalGuides,
        QString *errorMessage = nullptr);
    bool replaceStructuralState(
        const QImage &canvasImage,
        const QVector<LayerNode> &layers,
        const SelectionMask::Snapshot &selection,
        const QVector<double> &horizontalGuides,
        const QVector<double> &verticalGuides,
        double resolutionX,
        double resolutionY,
        QString *errorMessage = nullptr);
    bool replaceStructuralState(
        const QImage &canvasImage,
        const QVector<LayerNode> &layers,
        const SelectionMask::Snapshot &selection,
        const QVector<double> &horizontalGuides,
        const QVector<double> &verticalGuides,
        double resolutionX,
        double resolutionY,
        const DocumentColourState &colourState,
        QString *errorMessage = nullptr);
    bool replaceStructuralState(
        const QImage &canvasImage,
        const QVector<LayerNode> &layers,
        const SmartSourceRegistry &smartSources,
        const SelectionMask::Snapshot &selection,
        const QVector<double> &horizontalGuides,
        const QVector<double> &verticalGuides,
        double resolutionX,
        double resolutionY,
        const DocumentColourState &colourState,
        QString *errorMessage = nullptr);

    // Replace only presentation metadata. This path deliberately leaves the
    // authoritative canvas, layer tree, selection, processing revision and raw
    // render caches unchanged.
    bool replacePresentationColourState(
        const DocumentColourState &colourState,
        QString *errorMessage = nullptr);

    // Replace output/export metadata without touching document pixels, the
    // processing revision, Undo history or render caches.
    bool replaceOutputColourSettings(
        const OutputColourSettings &settings,
        QString *errorMessage = nullptr);

    QUuid addAdjustment(AdjustmentType type, const QUuid &selection = {});
    QUuid addRasterLayer(const QUuid &selection = {});
    QUuid addVectorShape(VectorShapeType type,
                         const QRectF &documentBounds,
                         const QColor &fillColour,
                         const QUuid &selection = {},
                         double cornerRadius = 16.0,
                         const QLineF &documentLine = {});
    QUuid addVectorPath(const VectorBezierPath &documentPath,
                        const QColor &strokeColour,
                        const QUuid &selection = {});
    QUuid addTextLayer(const TextLayerData &data, const QUuid &selection = {});
    QUuid insertTextLayerCopy(const LayerNode &source, const QTransform &desiredWorldTransform,
                              const QUuid &selection = {});
    QUuid insertVectorLayerCopy(const LayerNode &source,
                                const QTransform &desiredWorldTransform,
                                const QUuid &selection = {});
    QUuid addGroup(const QUuid &selection = {});
    QUuid groupLayers(const QVector<QUuid> &ids, const QString &name = QStringLiteral("Group"));
    QVector<QUuid> duplicateLayers(const QVector<QUuid> &ids,
                                   QHash<QUuid, QUuid> *idMap = nullptr);
    bool moveLayers(const QVector<QUuid> &ids,
                    const QUuid &destinationParentId,
                    int destinationIndex);
    bool addMask(const QUuid &id);
    bool removeMask(const QUuid &id);
    bool setMaskEnabled(const QUuid &id, bool enabled);
    bool setMaskInverted(const QUuid &id, bool inverted);
    bool updateLayer(const QUuid &id, const std::function<void(LayerNode &)> &update);
    // Pointer-rate semantic edits already own an explicit before-state and are
    // known to change the layer. Avoid copying and deeply comparing an entire
    // long vector path on every event; the gesture release still performs the
    // authoritative normalisation and records one atomic Undo command.
    bool updateLayerInteractive(const QUuid &id,
                                const std::function<void(LayerNode &)> &update);
    bool replaceLayer(const QUuid &id, const LayerNode &layer);
    bool layerPlacement(const QUuid &id, QUuid *parentId, int *index) const;
    bool insertLayerAt(const LayerNode &layer, const QUuid &parentId, int index);
    bool removeLayer(const QUuid &id);
    bool moveLayerUp(const QUuid &id);
    bool moveLayerDown(const QUuid &id);
    bool replaceLayerTree(const QVector<LayerNode> &layers);

    bool saveProject(const QString &filePath, QString *errorMessage = nullptr);
    bool loadProject(const QString &filePath, QString *errorMessage = nullptr);

    static ImageFileReadResult readImageFileDetailed(
        const QString &filePath,
        QString *errorMessage = nullptr);
    static QImage readImageFile(const QString &filePath, QString *errorMessage = nullptr);
    static bool writeImageFile(const QString &filePath,
                               const QImage &image,
                               int quality = 95,
                               QString *errorMessage = nullptr);

private:
    friend class SessionSnapshotCodec;
    void rebuildPreviewSource();
    void createInitialRasterLayer();
    void promoteLegacyBaseLayers();
    QUuid insertLayer(LayerNode layer, const QUuid &selection);
    bool synchronizeSmartLayerPresentations(QString *errorMessage = nullptr);

    QImage m_sourceImage;
    QImage m_previewSource;
    QString m_sourcePath;
    QString m_projectPath;
    QUuid m_documentIdentity = QUuid::createUuid();
    QString m_documentName;
    DocumentColourModel m_colourModel = DocumentColourModel::Rgb;
    bool m_blankDocument = false;
    double m_resolutionX = 72.0;
    double m_resolutionY = 72.0;
    DocumentColourState m_colourState;
    QVector<LayerNode> m_layers;
    SmartSourceRegistry m_smartSources;
    SelectionMask m_selectionMask;
    QVector<double> m_horizontalGuides;
    QVector<double> m_verticalGuides;
    QStringList m_loadWarnings;
    QStringList m_colourResourceWarnings;
    QStringList m_linkedSourceWarnings;
    bool m_modified = false;
};

} // namespace vfx
