#pragma once

#include "DisplayColourManagement.h"

#include "ClipboardOperations.h"
#include "CloneStamp.h"
#include "HealingBrush.h"
#include "HistogramService.h"
#include "LevelsEditorWidget.h"
#include "CurvesEditorWidget.h"
#include "GradientMapEditorWidget.h"
#include "GradientOperations.h"
#include "SpotHealing.h"
#include "PatchTool.h"
#include "ToneBrush.h"
#include "SmudgeBrush.h"
#include "DocumentSession.h"
#include "ImageCanvas.h"
#include "DocumentTransformOperations.h"
#include "TransformInterpolation.h"
#include "SessionCache.h"
#include "SelectionHistory.h"
#include "SelectionTransformOperations.h"
#include "gpu/RenderBackend.h"

#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QImage>
#include <QIcon>
#include <QHash>
#include <QLineF>
#include <QMainWindow>
#include <QPainterPath>
#include <QPoint>
#include <QPointF>
#include <QQueue>
#include <QRect>
#include <QRectF>
#include <QSet>
#include <QTransform>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QAction;
class QActionGroup;
class QCloseEvent;
class QEvent;
class QObject;
class QComboBox;
class QCheckBox;
class QDockWidget;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QKeyEvent;
class QListWidget;
class QMenu;
class QPushButton;
class QScrollArea;
class QShortcut;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTabBar;
class QTabWidget;
class QTextEdit;
class QToolBar;
class QToolButton;
class QUndoGroup;
class QUndoStack;
class QTreeWidgetItem;
class QVBoxLayout;
class QWidget;

namespace vfx {

class ExportQueueController;
class ExportQueueDock;
class SliderSpinBox;

enum class CanvasFitMode;
struct AutomaticTrimRequest;

class ColourWheelWidget;
class DocumentStripWidget;
class LayerTreeWidget;
struct PreparedColourProfileResult;

enum class TransformScope {
    WholeLayers,
    SelectedPixels
};

enum class TransformMode {
    FreeTransform,
    Scale,
    Rotate,
    Skew,
    Distort,
    Perspective
};

enum class CloneStampSourceMode {
    CurrentTarget,
    Composite
};

enum class PatchToolMode {
    Source,
    Destination
};

enum class EditorTool {
    Move,
    RectangleSelect,
    EllipseSelect,
    FreehandLasso,
    PolygonalLasso,
    Brush,
    Eraser,
    Crop,
    CloneStamp,
    HealingBrush,
    SpotHealing,
    Patch,
    Dodge,
    Burn,
    Sponge,
    Blur,
    Sharpen,
    Smudge,
    Fill,
    Gradient,
    Eyedropper,
    Shape,
    Text,
    Pen,
    DirectSelection,
    Corner
};

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    void openFile(const QString &filePath);

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    using DocumentState = DocumentSession::DocumentState;

    enum class PathHitPart {
        None,
        Anchor,
        InHandle,
        OutHandle,
        CornerHandle,
        Segment
    };

    struct TransformPreviewCache {
        QUuid documentSessionId;
        quint64 documentSessionSerial = 0;
        quint64 renderRevision = 0;
        quint64 requestSerial = 0;
        ChannelView channelView = ChannelView::Composite;
        QVector<QUuid> layerIds;
        QRectF initialBounds;
        QVector<QRectF> snapBounds;
        QVector<QPointF> snapTargets;
        QVector<QPointF> snapSources;
        QVector<LayerNode> livePreviewBaseLayers;
        QImage backgroundPresentation;
        QImage foregroundPresentation;
        QRectF foregroundPresentationBounds;
        QTransform foregroundPresentationTransform;
        bool usesLiveVectorPreview = false;
        bool valid = false;
    };

    struct PathHit {
        PathHitPart part = PathHitPart::None;
        int nodeIndex = -1;
        int segmentIndex = -1;
        QUuid nodeId;
        QUuid segmentStartNodeId;
        QUuid segmentEndNodeId;
        double segmentT = 0.0;
        double screenDistance = 1.0e30;
        bool isValid() const { return part != PathHitPart::None; }
        bool matches(const PathHit &other) const {
            return part == other.part && nodeIndex == other.nodeIndex
                && segmentIndex == other.segmentIndex
                && nodeId == other.nodeId
                && segmentStartNodeId == other.segmentStartNodeId
                && segmentEndNodeId == other.segmentEndNodeId;
        }
    };

    struct VectorObjectHit {
        QUuid layerId;
        QUuid objectId;
        bool isValid() const { return !layerId.isNull() && !objectId.isNull(); }
    };

    struct PathEndpointHit {
        QUuid layerId;
        QUuid objectId;
        int nodeIndex = -1;
        QPointF documentPosition;
        double screenDistance = 1.0e30;
        bool isValid() const {
            return !layerId.isNull() && !objectId.isNull() && nodeIndex >= 0;
        }
        bool matches(const PathEndpointHit &other) const {
            return layerId == other.layerId && objectId == other.objectId
                && nodeIndex == other.nodeIndex;
        }
    };

    struct PaintCommitResult {
        QUuid documentSessionId;
        quint64 documentSessionSerial = 0;
        quint64 colourStateRevision = 0;
        QImage image;
        RasterTileDeltaSet rasterHistoryDelta;
        MaskTileDeltaSet maskHistoryDelta;
        ChannelTileDeltaSet channelHistoryDelta;
        bool targetMask = false;
        int targetChannel = -1;
        bool usedGpu = false;
        bool cloneStamp = false;
        bool healingBrush = false;
        bool spotHealing = false;
        bool patchTool = false;
        ToneBrushOperation toneBrushOperation = ToneBrushOperation::None;
        bool patchMovesSelection = false;
        bool cancelled = false;
        SelectionMask::Snapshot patchSelectionAfter;
        QString error;
    };

    struct SelectionTransformTarget {
        QUuid layerId;
        LayerEditTarget editTarget = LayerEditTarget::Pixels;
        QImage sourceImage;
        QImage clearedImage;
        ClipboardPayload payload;
        QSize targetExtent;
        QTransform targetToDocument;
    };

    struct PreviewTileResult {
        QRect basePreviewRect;
        QImage image;
        int level = 0;
    };

    struct PreviewTileBatch {
        QUuid documentSessionId;
        quint64 documentSessionSerial = 0;
        quint64 colourStateRevision = 0;
        ColourProcessingCompatibility processingCompatibility =
            ColourProcessingCompatibility::LegacyV1;
        quint64 generation = 0;
        quint64 requestSerial = 0;
        int level = 0;
        QUuid channelLayerId;
        quint64 channelLayerRevision = 0;
        bool atomicPublication = false;
        bool interactivePublication = false;
        QVector<PreviewTileResult> tiles;
        TiledCanvasEngine::RenderInfo renderInfo;
        bool hasRenderInfo = false;
    };

    struct DocumentThumbnailResult {
        QUuid documentSessionId;
        quint64 documentSessionSerial = 0;
        quint64 documentRevision = 0;
        quint64 colourStateRevision = 0;
        QImage thumbnail;
    };

    struct GradientPreviewFrame {
        quint64 gestureSerial = 0;
        quint64 generation = 0;
        QImage composite;
        QImage maskOverlay;
        bool cancelled = false;
        QString error;
    };

    struct PreviewTileRequest {
        QUuid documentSessionId;
        quint64 documentSessionSerial = 0;
        quint64 colourStateRevision = 0;
        ColourProcessingCompatibility processingCompatibility =
            ColourProcessingCompatibility::LegacyV1;
        quint64 generation = 0;
        quint64 requestSerial = 0;
        int level = 0;
        QImage levelSource;
        QVector<LayerNode> layers;
        QSize documentSize;
        QSize basePreviewSize;
        QVector<QRect> levelTileRects;
        ChannelView channelView = ChannelView::Composite;
        QUuid channelLayerId;
        quint64 channelLayerRevision = 0;
        bool atomicPublication = false;
        bool interactivePublication = false;
        bool interactiveWholeRegion = false;
        std::shared_ptr<std::atomic_bool> cancelRequested;
    };

    DocumentSession &session();
    const DocumentSession &session() const;
    PhotoDocument &document();
    const PhotoDocument &document() const;
    QUndoStack *undoStack();
    const QUndoStack *undoStack() const;
    RenderSessionContext renderSessionContext() const;
    bool setActiveSession(DocumentSession *nextSession);
    void connectActiveSessionSignals();
    DocumentSession *sessionById(const QUuid &sessionId) const;
    DocumentSession *existingSmartSourceEditor(const QUuid &ownerSessionId,
                                               const QUuid &sourceId) const;
    DocumentSession *createDocumentSession();
    bool activateDocumentSession(DocumentSession *nextSession);
    void initialiseActiveDocument(PhotoDocument &&candidate,
                                  bool baselineRequiresSave,
                                  const QUuid &preferredLayerId = {});
    void captureActiveSessionUiState();
    void resetPreviewControllerForSessionSwitch();
    void restoreActiveSessionUi();
    bool closeSession(DocumentSession *targetSession, bool promptToSave = true);
    void closeOtherDocuments(const QUuid &sessionId);
    void closeAllDocuments();
    void activateNextDocument(int direction);
    bool maybeSaveChanges(DocumentSession *targetSession);
    void updateDocumentStripEntry(DocumentSession *targetSession);
    void refreshDocumentStrip();
    void scheduleDocumentThumbnail();
    void startDocumentThumbnailRender();
    void documentThumbnailFinished();

    void buildActions();
    void buildMenusAndToolOptionsBar();
    void buildCentralArea();
    void buildDocumentStrip();
    void buildToolsToolbar();
    void buildColorDock();
    void buildInspectorDock();
    void buildChannelsDock();
    void buildLayersDock();
    void buildExportQueueDock();
    void arrangeRightDocks();
    void applyTheme(const QString &themeId);
    void refreshThemeDependentUi();
    void restoreStoredWorkspaceLayout();
    void saveWorkspaceLayout();
    void resetWorkspaceLayout();

    void newDocument();
    void newDocumentFromClipboard();
    void openImageDialog();
    void openProjectDialog();
    void openPath(const QString &filePath);
    void openImagePath(const QString &filePath);
    void openProjectPath(const QString &filePath);
    void showColourManagementPreferences();
    void showDocumentColourInformation();
    void showDisplayColourManagement();
    void refreshDisplayColourManagement(bool force = false);
    bool applyPresentationColourState(const DocumentColourState &updated,
                                      QString *errorMessage = nullptr);
    void configureOpenColorIO();
    void assignDocumentProfile();
    void convertDocumentProfile();
    std::optional<ColourSpaceDescriptor> chooseDocumentProfile(
        const QString &title,
        const QString &description);
    void commitPreparedColourProfileResult(
        const PreparedColourProfileResult &prepared,
        const DocumentState &before,
        const QVector<QUuid> &layerSelection,
        const QString &historyText);
    bool resolveImportedImageColour(QImage *image,
                                    ImageColourImportInfo *colourInfo,
                                    const QString &sourceLabel);
    bool resolveClipboardColourPolicy(ClipboardPayload *payload,
                                      ImageColourImportInfo *colourInfo,
                                      const QString &sourceLabel);
    void showImageProfileWarnings(const QString &sourceLabel,
                                  const QStringList &warnings);
    void saveProject();
    bool saveProjectAs();
    void exportImage();
    void productionExport();
    void closeDocument();
    void changeImageSize();
    void transformWholeDocument(OrthogonalDocumentTransform operation);
    void transformAgain();
    void repeatTransform();
    void transformAndDuplicate();
    void copyTransformValues();
    void pasteTransformValues();
    void changeCanvasSize();
    void revealAllCanvas();
    void fitCanvasToSelection();
    void fitCanvasToSelectedLayers();
    void trimTransparentPixels();
    void trimByCornerColour();
    void beginCanvasFitOperation(CanvasFitMode mode,
                                 const QVector<QUuid> &layerIds,
                                 const QString &historyText,
                                 const QString &progressText,
                                 const QString &completionText);
    void beginAutomaticTrimOperation(const AutomaticTrimRequest &request,
                                     const QString &historyText,
                                     const QString &progressText,
                                     const QString &completionText);

    void selectAllPixels();
    void deselectPixels();
    void deselectFromCanvasClick();
    void invertSelection();
    void loadLayerAlphaSelection(SelectionCombineMode mode);
    void loadLayerMaskSelection(SelectionCombineMode mode);
    void loadLayerCoverageSelection(const QUuid &layerId,
                                    bool maskSource,
                                    SelectionCombineMode mode);
    bool layerCoverageSource(const QUuid &layerId,
                             bool maskSource,
                             QRect *documentRect,
                             QImage *coverage,
                             QString *sourceName = nullptr) const;
    void featherSelection();
    void expandSelection();
    void contractSelection();
    void smoothSelection();
    void refineSelection();
    void commitProcessedSelection(const QString &commandName,
                                  const SelectionMask::Snapshot &before,
                                  const SelectionMask::Snapshot &after);
    void setSelectionEdgesVisible(bool visible);
    void refreshSelectionDisplay();
    void beginSelectionMarquee(const QRectF &documentBounds,
                               Qt::KeyboardModifiers startModifiers);
    void updateSelectionMarquee(const QRectF &documentBounds);
    void renderSelectionMarqueePreview();
    void finishSelectionMarquee(const QRectF &documentBounds);
    void cancelSelectionMarquee();
    void beginSelectionLasso(const QPainterPath &documentPath,
                             Qt::KeyboardModifiers startModifiers);
    void updateSelectionLasso(const QPainterPath &documentPath);
    void finishSelectionLasso(const QPainterPath &documentPath);
    void cancelSelectionLasso();
    void beginSelectionPathGesture(const QPainterPath &documentPath,
                                   Qt::KeyboardModifiers startModifiers,
                                   const QString &commandName);
    void updateSelectionPathGesture(const QPainterPath &documentPath);
    void finishSelectionPathGesture(const QPainterPath &documentPath);
    void cancelSelectionPathGesture(const QString &message);
    QPainterPath selectionMarqueePath(const QRectF &documentBounds) const;
    SelectionShape activeSelectionShape() const;
    SelectionCombineMode resolvedSelectionCombineMode(
        Qt::KeyboardModifiers startModifiers) const;
    QString selectionCombineModeName(SelectionCombineMode mode) const;
    void recordSelectionChange(const QString &text,
                               const SelectionMask::Snapshot &before,
                               const SelectionMask::Snapshot &after,
                               const QRect &affectedRect = {});
    void applySelectionHistoryChange(const SelectionTileDeltaSet &deltaSet,
                                     bool targetAfter);
    void applyPatchSelectionHistoryChange(
        const SelectionMask::Snapshot &snapshot);

    void addAdjustment(AdjustmentType type);
    void addRasterLayer();
    void addGroup();
    void addMaskToSelectedLayer();
    void createOrReplaceMaskFromSelection();
    void removeMaskFromSelectedLayer();
    void copyActiveTarget();
    void copyMerged();
    void cutActiveTarget();
    void pasteClipboard();
    std::optional<ClipboardPayload> buildClipboardPayload(bool merged,
                                                          QString *errorMessage = nullptr);
    bool activeDirectClipboardTarget(QUuid *layerId,
                                     LayerEditTarget *target) const;
    bool setSystemClipboardPayload(const ClipboardPayload &payload,
                                   QString *errorMessage = nullptr);
    std::optional<ClipboardPayload> systemClipboardPayload(
        QString *errorMessage = nullptr) const;
    bool pasteClipboardAsRasterLayer(const ClipboardPayload &payload);
    bool pasteClipboardIntoActiveDirectTarget(const ClipboardPayload &payload);
    bool clearActiveTargetThroughSnapshot(const SelectionMask::Snapshot &selectionSnapshot,
                                          const QString &commandPrefix,
                                          QString *resultMessage = nullptr);
    bool routeClipboardActionToFocusedEditor(const char *operation);
    void clearSelectedContents();
    void deleteOrClearFocusedTarget();
    void setSelectedMasksEnabled(bool enabled);
    void setSelectedMasksInverted(bool inverted);
    void setLayerEditTarget(const QUuid &layerId, LayerEditTarget target);
    void showLayerContextMenu(const QPoint &position);
    void convertSelectedLayersToSmart();
    void placeLinkedSmartLayer();
    void replaceSelectedSmartLayerSource();
    void relinkSelectedSmartLayerSource();
    void embedSelectedLinkedSmartSource();
    void editSelectedSmartLayerContents();
    bool saveSmartSourceEditor(DocumentSession *sourceEditor);
    void refreshOpenLinkedDependents(const QUuid &savedDocumentId,
                                     const QString &savedProjectPath);
    bool refreshLinkedSmartSourcesForOutput(QString *errorMessage = nullptr);
    void mergeSelectedLayers();
    void duplicateSelectedLayers();
    void removeSelectedLayer();
    void moveSelectedLayerUp();
    void moveSelectedLayerDown();
    void layerTreeStructureChanged();
    void layersDropRequested(const QVector<QUuid> &ids,
                             const QUuid &destinationParentId,
                             int destinationIndex);

    DocumentState captureDocumentState() const;
    DocumentState captureDocumentState(DocumentSession *targetSession) const;
    void applyDocumentState(const DocumentState &state,
                            const QVector<QUuid> &selection = {});
    void applyDocumentStateToSession(DocumentSession *targetSession,
                                     const DocumentState &state,
                                     const QVector<QUuid> &selection = {});
    void recordDocumentChangeForSession(DocumentSession *targetSession,
                                        const QString &text,
                                        const DocumentState &before,
                                        const DocumentState &after,
                                        const QVector<QUuid> &beforeSelection,
                                        const QVector<QUuid> &afterSelection);
    void recordDocumentChange(const QString &text,
                              const DocumentState &before,
                              const DocumentState &after,
                              const QVector<QUuid> &beforeSelection = {},
                              const QVector<QUuid> &afterSelection = {});
    void beginPropertyUndo(const QString &text);
    void finishPropertyUndo();

    void refreshDocumentUi(const QUuid &preferredLayerId = {});
    void refreshLayerTree(const QUuid &preferredLayerId = {});
    void appendLayerTreeItems(const QVector<LayerNode> &layers, QTreeWidgetItem *parent);
    void setLayerTreeItemThumbnails(QTreeWidgetItem *item,
                                    const LayerNode &layer,
                                    bool useCache = true);
    void refreshLayerTargetIndicators();
    QVector<LayerNode> layerTreeFromWidget() const;
    QVector<LayerNode> childLayersFromItem(const QTreeWidgetItem *parent) const;
    void refreshLayerPropertyControls();
    void rebuildInspectorPanel();
    void addAdjustmentPresetControls(const LayerNode &layer);
    void showAdjustmentPresetManager(const LayerNode &layer);
    void populateLiveFilterMenu(QMenu *menu, const QUuid &smartLayerId);
    void addLiveFilterToSmartLayer(const QUuid &smartLayerId, AdjustmentType type);
    void removeLiveFilterFromSmartLayer(const QUuid &smartLayerId, const QUuid &filterId);
    void moveLiveFilterInSmartLayer(const QUuid &smartLayerId, const QUuid &filterId, int delta);
    void setLiveFilterMaskFromSelection(const QUuid &smartLayerId, const QUuid &filterId,
                                        bool requireSelection);
    void loadLiveFilterMaskSelection(const QUuid &smartLayerId, const QUuid &filterId);
    void selectLiveFilterTreeItem(const QUuid &smartLayerId, const QUuid &filterId);
    void populateLayerEffectMenu(QMenu *menu, const QUuid &layerId);
    void addLayerEffectToLayer(const QUuid &layerId, LayerEffectType type);
    void removeLayerEffectFromLayer(const QUuid &layerId, const QUuid &effectId);
    void moveLayerEffectInLayer(const QUuid &layerId, const QUuid &effectId, int delta);
    void selectLayerEffectTreeItem(const QUuid &layerId, const QUuid &effectId);
    void updateInspectorAdjustment(const QUuid &id,
                                   const std::function<void(LayerNode &)> &update);
    void updateActionStates();
    bool documentMutationBusy() const;
    void resetUndoHistory();
    void beginNewDocumentSession();
    void updateWindowTitle();
    void updateStatusText();
    QUuid selectedLiveFilterId() const;
    QUuid selectedLiveFilterOwnerId() const;
    QUuid selectedLayerEffectId() const;
    QUuid selectedLayerEffectOwnerId() const;
    QUuid selectedLayerId() const;
    QVector<QUuid> selectedLayerIds() const;
    QVector<QUuid> selectedRootLayerIds() const;
    // Layer Effects and Live Filters are presentation-only sub-items. Canvas
    // tools and layer-level commands resolve them to their owning LayerNode,
    // while structural tree commands continue using selectedRootLayerIds().
    QVector<QUuid> selectedOwningRootLayerIds() const;
    void restoreLayerSelection(const QVector<QUuid> &ids);
    QString selectedEditTargetName() const;
    void updateSelectedLayers(const std::function<void(LayerNode &)> &update,
                              bool refreshTree = false,
                              bool rebuildInspector = true);

    void setActiveTool(EditorTool tool);
    void setSnappingEnabled(bool enabled);
    void createVectorShape(const QRectF &documentBounds);
    void createTextLayer(const QRectF &documentBounds);
    QUuid topTextLayerAt(const QPointF &documentPosition) const;
    void handleTextToolPressed(const QPointF &documentPosition,
                               Qt::KeyboardModifiers modifiers);
    void beginCanvasTextEditing(const QUuid &layerId,
                                const QPointF &documentPosition = {},
                                bool selectAll = false);
    void commitCanvasTextEditing();
    void cancelCanvasTextEditing();
    void updateCanvasTextEditorGeometry();
    void updateCanvasTextEditorStyle();
    bool buildTextBoxTransformCandidate(const QTransform &documentTransform,
                                        LayerNode *candidate) const;
    void applyTextBoxTransform(const QTransform &documentTransform);
    void setActiveVectorShapeType(VectorShapeType type);
    bool selectedPathLayer(QUuid *layerId = nullptr,
                           LayerNode *layer = nullptr,
                           VectorShape *shape = nullptr) const;
    bool activateCompoundPathContourAt(const QPointF &documentPosition);
    VectorObjectHit topVectorPathObjectAt(const QPointF &documentPosition) const;
    VectorObjectHit topVectorObjectAt(const QPointF &documentPosition) const;
    bool activateVectorObject(const VectorObjectHit &hit);
    bool convertVectorLayerToPath(const QUuid &layerId);
    void convertSelectedVectorShapesToPaths();
    void expandSelectedVectorStrokes();
    QVector<QUuid> selectedVectorLayerIds() const;
    std::optional<VectorAppearance> selectedVectorAppearance() const;
    std::optional<VectorAppearance> clipboardVectorAppearance(
        QString *errorMessage = nullptr) const;
    bool applyVectorAppearanceToSelection(const VectorAppearance &appearance,
                                          const QString &historyText);
    void syncVectorAppearanceDefaults(const VectorAppearance &appearance);
    void swapVectorFillAndStroke();
    void resetVectorFillAndStroke();
    void copyVectorAppearance();
    void pasteVectorAppearance();
    std::optional<VectorAppearance> currentVectorAppearanceForPreset() const;
    void showVectorAppearancePresets();
    QToolButton *createVectorAppearanceQuickActions(QWidget *parent);
    void syncVectorPathOverlay();
    void syncVignetteOverlay();
    QVector<PathEndpointHit> visibleOpenPathEndpoints() const;
    PathEndpointHit vectorPathEndpointHitAt(
        const QPointF &documentPosition) const;
    void updatePenEndpointHover(const QPointF &documentPosition);
    void updateVectorPathHover(const QPointF &documentPosition);
    void clearVectorPathHover();
    bool beginPenContinuation(const PathEndpointHit &endpoint);
    bool joinPenPathToEndpoint(const PathEndpointHit &endpoint);
    bool cancelCurrentPenSegment();
    int penActiveEndpointIndex(const VectorShape &shape) const;
    int penCloseEndpointIndex(const VectorShape &shape) const;
    PathHit vectorPathHitAt(const QPointF &documentPosition,
                            bool includeSegments = true) const;
    QPointF snapVectorPathPoint(const QPointF &documentPosition,
                                int draggedNode = -1) const;
    void rebuildVectorPathSnapTargets(int draggedNode = -1);
    void clearVectorPathSnapTargets();
    void beginVectorNodeMarquee(const QPointF &documentPosition,
                                Qt::KeyboardModifiers modifiers);
    void updateVectorNodeMarquee(const QPointF &documentPosition);
    void finishVectorNodeMarquee(const QPointF &documentPosition);
    bool interactivePreviewEditActive() const;
    bool detailSensitiveSpatialPreviewActive() const;
    void handleVectorPathPointerPressed(const QPointF &documentPosition,
                                        Qt::KeyboardModifiers modifiers);
    void handleVectorPathPointerMoved(const QPointF &documentPosition,
                                      Qt::MouseButtons buttons,
                                      Qt::KeyboardModifiers modifiers);
    void handleVectorPathPointerReleased(const QPointF &documentPosition,
                                         Qt::KeyboardModifiers modifiers);
    void handleVectorPathPointerDoubleClicked(const QPointF &documentPosition,
                                              Qt::KeyboardModifiers modifiers);
    bool handleVectorNodeKeyPress(QKeyEvent *event);
    bool selectAllPathNodes();
    void clearPathNodeSelection();
    void nudgeSelectedPathNodes(const QPointF &documentDelta);
    void finishPenPath();
    void deleteSelectedPathNodes();
    void makeSelectedPathNodesSharp();
    void setSelectedPathNodeMode(VectorNodeMode mode);
    void setSelectedPathClosed(bool closed, bool applyPenAppearance = false);
    void setSelectedPathCornerRadius(double radius, bool recordUndo = true);
    void setSelectedPathCornerStyle(VectorCornerStyle style);
    void clearSelectedPathCorners();
    void bakeSelectedPathCorners();
    void rebuildToolOptionsBar();
    void disposeToolOptionsBarActions(bool immediate = false);
    void ensureCropState();
    void syncCropCanvas();
    void syncCropOptionControls();
    void updateCropSnapBounds();
    void cropFromSelection();
    void cancelPendingCrop();
    void applyPendingCrop();
    QString toolName(EditorTool tool) const;
    QString toolDescription(EditorTool tool) const;
    void sampleCloneSource(const QPointF &documentPosition);
    bool resolveCurrentCloneTarget(QUuid *layerId, LayerEditTarget *target) const;
    void refreshCloneSourceMarker();
    void clearActiveCloneSource();
    void beginPaintStroke(const QPointF &documentPosition);
    void performFill(const QPointF &documentPosition);
    void beginGradientGesture(const QPointF &documentPosition);
    void updateGradientGesture(const QPointF &documentPosition);
    void finishGradientGesture();
    void cancelGradientGesture();
    void requestGradientPreview();
    void startGradientPreviewRender();
    void gradientPreviewFinished();
    void continuePaintStroke(const QPointF &from, const QPointF &to);
    void finishPaintStroke();
    bool ensurePaintTarget();
    void paintRasterSegment(const QPointF &from, const QPointF &to);
    QRect updatePatchPreview(const QPointF &documentPosition);
    SelectionMask::Snapshot translatedSelectionSnapshot(
        const SelectionMask::Snapshot &source,
        const QPointF &offsetDocument) const;
    QRect paintPreviewSegment(const QPointF &from, const QPointF &to);
    void updateLivePaintLayerCopy();
    void releaseLivePaintLayerCopy();
    void refreshLiveMaskThumbnail();
    void commitPaintStroke();
    void paintCommitFinished();
    void applyRasterHistoryChange(const QUuid &layerId,
                                  const RasterTileDeltaSet &deltaSet,
                                  bool targetAfter,
                                  bool createdLayer,
                                  const LayerNode &createdLayerTemplate,
                                  const QUuid &createdParentId,
                                  int createdIndex,
                                  const QVector<QUuid> &selection);
    void applyMaskHistoryChange(const QUuid &layerId,
                                const MaskTileDeltaSet &deltaSet,
                                bool targetAfter,
                                ChannelView presentationView,
                                const QVector<QUuid> &selection);
    void applyChannelHistoryChange(const QUuid &layerId,
                                   const ChannelTileDeltaSet &deltaSet,
                                   bool targetAfter,
                                   LayerEditTarget target,
                                   const QVector<QUuid> &selection);
    void applyGreyHistoryChange(const QUuid &layerId,
                                const RasterTileDeltaSet &deltaSet,
                                bool targetAfter,
                                const QVector<QUuid> &selection);
    void resetPaintTransaction();
    void rollbackCreatedPaintLayer();
    void updateTransformSelectionOverlay();
    void scheduleTransformPreviewPrewarm();
    void startTransformPreviewPrewarm();
    void transformPreviewPrewarmFinished();
    void invalidateTransformPreviewCache();
    bool transformPreviewCacheMatches(const QVector<QUuid> &layerIds) const;
    void rebuildMovedTransformPreviewCache(const QTransform &documentTransform,
                                           const QVector<QUuid> &layerIds,
                                           const QRectF &newBounds);
    void updateAutomaticTransformScope();
    bool selectedPixelTransformAvailable(QString *reason = nullptr) const;
    bool beginSelectedPixelTransform(CanvasTransformMode mode);
    void applySelectedPixelTransform(const QTransform &documentTransform);
    void applyDocumentAndSelectionState(const DocumentState &state,
                                        const SelectionMask::Snapshot &pixelSelection,
                                        const QVector<QUuid> &layerSelection);
    void resetTransformTransaction();
    void beginLayerTransform(CanvasTransformMode mode,
                             const QPointF &documentPosition,
                             Qt::KeyboardModifiers startModifiers);
    void updateLayerTransform(const QTransform &documentTransform);
    void finishLayerTransform(const QTransform &documentTransform);
    void applyTransformSession();
    void cancelTransformSession();
    bool ensureTransformSession();
    void setTransformMode(TransformMode mode);
    void setTransformInterpolation(TransformInterpolation interpolation);
    TransformWorkflowState currentTransformWorkflowState() const;
    bool beginTransformFromWorkflowState(const TransformWorkflowState &workflow,
                                         bool duplicate,
                                         bool applyImmediately);
    QTransform adaptedTransformForCurrentBounds(
        const TransformWorkflowState &workflow,
        const QRectF &currentBounds,
        QPointF *finalPivot) const;
    void rememberAppliedTransform(const QTransform &documentTransform);
    void setTransformSessionTransform(const QTransform &documentTransform,
                                      const QPointF &pivotDocument);
    QImage renderLayerTransformSurface(const QVector<LayerNode> &layers,
                                       const QRectF &documentRegion = {});
    void updateLiveVectorTransformPreview(const QTransform &documentTransform);
    bool settleTransformBeforeInspectorEdit();
    void updateTransformNumericControls();
    void applyTransformTranslation(double targetX, double targetY, bool updateX);
    void applyTransformSize(double targetWidth, double targetHeight, bool updateWidth);
    void applyTransformAngle(double targetDegrees);
    void applyTransformControlPoint(double value, bool updateX);
    void selectTransformControlPoint(int index);
    QPolygonF transformControlQuad() const;
    bool setTransformControlQuad(const QPolygonF &quad);
    void flipTransform(bool horizontal);
    void rotateTransform90(bool clockwise);
    void resetPendingTransform();
    void showTransformContextMenu(const QPoint &globalPosition);

    void loadStoredColours();
    void setEditingPrimaryColour(bool primary);
    QColor activeColour() const;
    void setActiveColour(const QColor &colour, bool persist = true);
    void persistActiveColour();
    void choosePrimaryColour();
    void chooseSecondaryColour();
    void swapColours();
    void resetColours();
    void updateColourPanel();
    void updateColourControls();
    void loadStoredSwatches();
    void saveStoredSwatches() const;
    void saveCurrentColourToSwatch(int preferredIndex = -1);
    void updateSavedSwatches();
    void activateEyedropperFromColourPanel();

    void setChannelView(ChannelView view);
    void syncCanvasGuidesFromDocument();
    QImage imageForChannelView(const QImage &image) const;
    QImage selectedMaskValueImage(const QImage &maskOverride = QImage()) const;
    QImage selectedMaskValueRegion(const QImage &maskOverride,
                                   const QRect &previewRegion) const;
    QImage selectedMaskChannelImage(const QImage &maskOverride = QImage()) const;
    QImage selectedMaskChannelRegion(const QImage &maskOverride,
                                     const QRect &previewRegion) const;
    void syncMaskOverlayPresentation();
    void updateChannelModelUi();
    void updateMaskChannelAvailability();
    QString channelViewName() const;

    void schedulePreviewRender();
    void startPreviewRender();
    void previewRenderFinished();
    void completeSettledPreviewBookkeeping();
    void previewViewportChanged(const QRect &visiblePreviewRegion,
                                double zoom,
                                bool settled);
    void rebuildPreviewTilePlan(const QRect &visiblePreviewRegion,
                                double zoom,
                                bool settled,
                                bool authoritativeOnly = false);
    void startNextPreviewTileBatch();
    void cancelPreviewTileWork();

    void updateLayer(const QUuid &id, const std::function<void(LayerNode &)> &update);
    void requestAdjustmentHistogram(const QUuid &layerId);
    void beginLevelsEyedropper(LevelsEyedropperRole role);
    void beginCurvesEyedropper(const QUuid &layerId);
    void beginHueSaturationEyedropper(const QUuid &layerId,
                                      HueSaturationRange range);
    void beginWhiteBalanceEyedropper(const QUuid &layerId);
    void clearOneShotAdjustmentEyedroppers();
    void addDoubleControl(const QString &label,
                          double minimum,
                          double maximum,
                          double step,
                          double value,
                          int decimals,
                          const std::function<void(double)> &setter);
    void clearInspectorLayout();

    void openSvgDialog();
    void importSvgDialog();
    void exportSvgDocument();
    void exportSelectedSvg();
    bool openSvgPath(const QString &filePath);
    bool importSvgPath(const QString &filePath);
    bool exportSvgTo(const QString &filePath, const QSet<QUuid> &selectedRootIds);
    void showSvgWarnings(const QString &title, const QStringList &warnings,
                         int skippedCount = 0);

    bool maybeSaveChanges();
    bool writeProjectTo(const QString &filePath);
    QString imageOpenFilter() const;
    QString imageExportFilter() const;
    void showAboutDialog();

    std::vector<std::unique_ptr<DocumentSession>> m_sessions;
    DocumentSession *m_activeSession = nullptr;
    bool m_shuttingDown = false;
    QUndoGroup *m_undoGroup = nullptr;
    QSet<QUuid> m_registeredSessionIds;
    std::unique_ptr<DocumentResidencyManager> m_residencyManager;

    QStackedWidget *m_centralStack = nullptr;
    QWidget *m_welcomePage = nullptr;
    QPushButton *m_newFromClipboardButton = nullptr;
    ImageCanvas *m_canvas = nullptr;
    DocumentStripWidget *m_documentStrip = nullptr;
    QUuid m_presentedMaskOverlayLayerId;
    qint64 m_presentedMaskOverlayCacheKey = 0;
    QSize m_presentedMaskOverlayPreviewSize;
    QTransform m_presentedMaskOverlayWorldTransform;
    bool m_presentedMaskOverlayInverted = false;
    bool m_vignetteOverlayControlsVisible = true;
    bool m_vignetteOverlayPropertyEditActive = false;

    QMenu *m_viewMenu = nullptr;
    QActionGroup *m_themeActionGroup = nullptr;
    QToolBar *m_toolOptionsBar = nullptr;
    QToolBar *m_toolsToolbar = nullptr;
    QActionGroup *m_toolActionGroup = nullptr;
    QAction *m_eyedropperToolAction = nullptr;
    QAction *m_rectangleShapeToolAction = nullptr;
    QAction *m_roundedRectangleShapeToolAction = nullptr;
    QAction *m_ellipseShapeToolAction = nullptr;
    QAction *m_lineShapeToolAction = nullptr;
    QAction *m_polygonShapeToolAction = nullptr;
    QAction *m_starShapeToolAction = nullptr;
    QAction *m_arrowShapeToolAction = nullptr;
    QAction *m_penToolAction = nullptr;
    QAction *m_directSelectionToolAction = nullptr;
    QAction *m_cornerToolAction = nullptr;
    QAction *m_rectangleSelectToolAction = nullptr;
    QAction *m_ellipseSelectToolAction = nullptr;
    QAction *m_freehandLassoToolAction = nullptr;
    QAction *m_polygonalLassoToolAction = nullptr;
    QAction *m_cropToolAction = nullptr;
    QAction *m_cloneStampToolAction = nullptr;
    QAction *m_healingBrushToolAction = nullptr;
    QAction *m_spotHealingToolAction = nullptr;
    QAction *m_patchToolAction = nullptr;
    QAction *m_dodgeToolAction = nullptr;
    QAction *m_burnToolAction = nullptr;
    QAction *m_spongeToolAction = nullptr;
    QAction *m_blurToolAction = nullptr;
    QAction *m_sharpenToolAction = nullptr;
    QAction *m_smudgeToolAction = nullptr;
    QAction *m_fillToolAction = nullptr;
    QAction *m_gradientToolAction = nullptr;
    QToolButton *m_selectionToolButton = nullptr;
    QToolButton *m_shapeToolButton = nullptr;
    QToolButton *m_pathToolButton = nullptr;
    QToolButton *m_retouchToolButton = nullptr;
    QToolButton *m_toneToolButton = nullptr;
    QToolButton *m_detailToolButton = nullptr;
    QToolButton *m_fillGradientToolButton = nullptr;

    QComboBox *m_cropModeCombo = nullptr;
    QComboBox *m_cropPresetCombo = nullptr;
    QSpinBox *m_cropXSpin = nullptr;
    QSpinBox *m_cropYSpin = nullptr;
    QSpinBox *m_cropWidthSpin = nullptr;
    QSpinBox *m_cropHeightSpin = nullptr;
    QComboBox *m_cropOverlayCombo = nullptr;
    SliderSpinBox *m_cropDimSpin = nullptr;
    QCheckBox *m_cropDeleteCheck = nullptr;
    QToolButton *m_cropStraightenButton = nullptr;
    SliderSpinBox *m_cropAngleSpin = nullptr;
    bool m_syncingCropControls = false;

    QDockWidget *m_colourDock = nullptr;
    QDockWidget *m_inspectorDock = nullptr;
    QDockWidget *m_channelsDock = nullptr;
    QDockWidget *m_layersDock = nullptr;
    ExportQueueController *m_exportQueueController = nullptr;
    ExportQueueDock *m_exportQueueDock = nullptr;

    QPushButton *m_primarySwatch = nullptr;
    QPushButton *m_secondarySwatch = nullptr;
    QToolButton *m_colourSwapButton = nullptr;
    QToolButton *m_colourDefaultButton = nullptr;
    QTabBar *m_colourTabs = nullptr;
    ColourWheelWidget *m_colourWheel = nullptr;
    QLineEdit *m_colourHexEdit = nullptr;
    SliderSpinBox *m_redSpin = nullptr;
    SliderSpinBox *m_greenSpin = nullptr;
    SliderSpinBox *m_blueSpin = nullptr;
    SliderSpinBox *m_alphaSpin = nullptr;
    SliderSpinBox *m_hsvAlphaSpin = nullptr;
    SliderSpinBox *m_hueSpin = nullptr;
    SliderSpinBox *m_saturationSpin = nullptr;
    SliderSpinBox *m_valueSpin = nullptr;
    QPushButton *m_colourEyedropperButton = nullptr;
    QVector<QPushButton *> m_savedColourButtons;
    QVector<QColor> m_savedColours;
    int m_selectedSavedColour = -1;
    bool m_editingPrimaryColour = true;
    bool m_updatingColourControls = false;

    QScrollArea *m_inspectorScrollArea = nullptr;
    QWidget *m_inspectorContainer = nullptr;
    QVBoxLayout *m_inspectorLayout = nullptr;
    HistogramService *m_histogramService = nullptr;
    LevelsEditorWidget *m_levelsEditor = nullptr;
    CurvesEditorWidget *m_curvesEditor = nullptr;
    QUuid m_levelsEditorLayerId;
    QUuid m_curvesEditorLayerId;
    quint64 m_levelsHistogramSerial = 0;
    QTimer m_levelsHistogramRefreshTimer;
    LevelsEyedropperRole m_levelsEyedropperRole = LevelsEyedropperRole::None;
    EditorTool m_toolBeforeLevelsEyedropper = EditorTool::Move;
    QUuid m_curvesEyedropperLayerId;
    EditorTool m_toolBeforeCurvesEyedropper = EditorTool::Move;
    QUuid m_hueSaturationEyedropperLayerId;
    HueSaturationRange m_hueSaturationEyedropperRange = HueSaturationRange::Reds;
    HueSaturationRange m_hueSaturationInspectorRange = HueSaturationRange::Reds;
    EditorTool m_toolBeforeHueSaturationEyedropper = EditorTool::Move;
    QUuid m_whiteBalanceEyedropperLayerId;
    EditorTool m_toolBeforeWhiteBalanceEyedropper = EditorTool::Move;

    QListWidget *m_channelList = nullptr;

    LayerTreeWidget *m_layerTree = nullptr;
    QComboBox *m_blendModeCombo = nullptr;
    SliderSpinBox *m_layerOpacityControl = nullptr;
    QToolButton *m_addLayerButton = nullptr;
    QToolButton *m_addGroupButton = nullptr;
    QToolButton *m_addAdjustmentButton = nullptr;
    QToolButton *m_addMaskButton = nullptr;
    QToolButton *m_duplicateLayerButton = nullptr;
    QToolButton *m_removeLayerButton = nullptr;
    QToolButton *m_moveLayerUpButton = nullptr;
    QToolButton *m_moveLayerDownButton = nullptr;
    bool m_rebuildingLayerTree = false;
    bool m_updatingLayerProperties = false;

    QLabel *m_documentStatus = nullptr;
    QWidget *m_zoomControls = nullptr;
    QToolButton *m_snapStatusButton = nullptr;
    QToolButton *m_zoomOutButton = nullptr;
    QLabel *m_zoomStatus = nullptr;
    QToolButton *m_zoomInButton = nullptr;
    QToolButton *m_actualPixelsButton = nullptr;
    QToolButton *m_fitViewButton = nullptr;
    MonitorProfileInfo m_activeMonitorProfile;
    DisplayColourTransformStatus m_activeDisplayTransformStatus;
    QString m_displayColourManagementError;
    QString m_monitorProfileDiscoveryKey;

    QAction *m_undoAction = nullptr;
    QAction *m_newAction = nullptr;
    QAction *m_newFromClipboardAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_openImageAction = nullptr;
    QAction *m_openProjectAction = nullptr;
    QAction *m_colourManagementPreferencesAction = nullptr;
    QAction *m_documentColourInfoAction = nullptr;
    QAction *m_displayColourManagementAction = nullptr;
    QAction *m_openColorIOAction = nullptr;
    QAction *m_assignProfileAction = nullptr;
    QAction *m_convertProfileAction = nullptr;
    QAction *m_openSvgAction = nullptr;
    QAction *m_importSvgAction = nullptr;
    QAction *m_exportSvgAction = nullptr;
    QAction *m_exportSelectedSvgAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_saveAsAction = nullptr;
    QAction *m_exportAction = nullptr;
    QAction *m_productionExportAction = nullptr;
    QAction *m_closeAction = nullptr;
    QAction *m_closeOthersAction = nullptr;
    QAction *m_closeAllAction = nullptr;
    QAction *m_nextDocumentAction = nullptr;
    QAction *m_previousDocumentAction = nullptr;
    QAction *m_fitAction = nullptr;
    QAction *m_actualPixelsAction = nullptr;
    QAction *m_imageSizeAction = nullptr;
    QAction *m_canvasSizeAction = nullptr;
    QAction *m_flipImageHorizontalAction = nullptr;
    QAction *m_flipImageVerticalAction = nullptr;
    QAction *m_rotateImageClockwiseAction = nullptr;
    QAction *m_rotateImageCounterClockwiseAction = nullptr;
    QAction *m_rotateImage180Action = nullptr;
    QAction *m_transformAgainAction = nullptr;
    QAction *m_repeatTransformAction = nullptr;
    QAction *m_transformAndDuplicateAction = nullptr;
    QAction *m_copyTransformValuesAction = nullptr;
    QAction *m_pasteTransformValuesAction = nullptr;
    QAction *m_revealAllAction = nullptr;
    QAction *m_fitCanvasToSelectionAction = nullptr;
    QAction *m_fitCanvasToSelectedLayersAction = nullptr;
    QAction *m_trimTransparentPixelsAction = nullptr;
    QAction *m_trimCornerColourAction = nullptr;
    QAction *m_showRulersAction = nullptr;
    QAction *m_showGuidesAction = nullptr;
    QAction *m_snapGuidesAction = nullptr;
    QAction *m_clearGuidesAction = nullptr;
    QAction *m_resetWorkspaceAction = nullptr;
    QAction *m_selectAllAction = nullptr;
    QAction *m_deselectAction = nullptr;
    QAction *m_invertSelectionAction = nullptr;
    QAction *m_showSelectionEdgesAction = nullptr;
    QMenu *m_loadLayerAlphaSelectionMenu = nullptr;
    QMenu *m_loadLayerMaskSelectionMenu = nullptr;
    QAction *m_featherSelectionAction = nullptr;
    QAction *m_expandSelectionAction = nullptr;
    QAction *m_contractSelectionAction = nullptr;
    QAction *m_smoothSelectionAction = nullptr;
    QAction *m_refineSelectionAction = nullptr;
    QAction *m_addRasterLayerAction = nullptr;
    QAction *m_addGroupAction = nullptr;
    QAction *m_addMaskAction = nullptr;
    QAction *m_maskFromSelectionAction = nullptr;
    QAction *m_removeMaskAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_copyMergedAction = nullptr;
    QAction *m_cutAction = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_clearContentsAction = nullptr;
    QAction *m_enableMaskAction = nullptr;
    QAction *m_invertMaskAction = nullptr;
    QAction *m_duplicateLayerAction = nullptr;
    QAction *m_convertToSmartLayerAction = nullptr;
    QAction *m_placeLinkedSmartLayerAction = nullptr;
    QAction *m_replaceSmartLayerSourceAction = nullptr;
    QAction *m_relinkSmartLayerSourceAction = nullptr;
    QAction *m_embedLinkedSmartSourceAction = nullptr;
    QAction *m_editSmartLayerContentsAction = nullptr;
    QAction *m_mergeLayersAction = nullptr;
    QAction *m_convertShapeToPathAction = nullptr;
    QAction *m_expandStrokeAction = nullptr;
    QAction *m_swapVectorFillStrokeAction = nullptr;
    QAction *m_resetVectorFillStrokeAction = nullptr;
    QAction *m_copyVectorAppearanceAction = nullptr;
    QAction *m_pasteVectorAppearanceAction = nullptr;
    QAction *m_vectorAppearancePresetsAction = nullptr;
    QAction *m_removeLayerAction = nullptr;
    QAction *m_moveLayerUpAction = nullptr;
    QAction *m_moveLayerDownAction = nullptr;

    EditorTool m_activeTool = EditorTool::Move;
    QColor m_primaryColour = QColor(Qt::black);
    VectorShapeType m_activeVectorShapeType = VectorShapeType::Rectangle;
    double m_vectorCornerRadius = 24.0;
    bool m_vectorFillEnabled = true;
    bool m_vectorStrokeEnabled = false;
    double m_vectorFillOpacity = 1.0;
    double m_vectorStrokeOpacity = 1.0;
    double m_vectorStrokeWidth = 2.0;
    VectorStrokeAlignment m_vectorStrokeAlignment = VectorStrokeAlignment::Centre;
    VectorStrokeCap m_vectorStrokeCap = VectorStrokeCap::Round;
    VectorStrokeJoin m_vectorStrokeJoin = VectorStrokeJoin::Miter;
    double m_vectorStrokeMiterLimit = 4.0;
    VectorStrokePattern m_vectorStrokePattern = VectorStrokePattern::Solid;
    double m_vectorStrokeDashLength = 12.0;
    double m_vectorStrokeGapLength = 8.0;
    double m_vectorStrokeDashOffset = 0.0;
    VectorArrowheadType m_vectorStartArrowhead = VectorArrowheadType::None;
    VectorArrowheadType m_vectorEndArrowhead = VectorArrowheadType::None;
    double m_vectorStartArrowScale = 1.0;
    double m_vectorEndArrowScale = 1.0;
    int m_vectorPolygonSides = 5;
    double m_vectorStarInnerRatio = 0.5;
    double m_vectorVertexRotation = -90.0;
    double m_vectorArrowHeadLengthRatio = 0.35;
    double m_vectorArrowShaftWidthRatio = 0.35;
    QSet<int> m_selectedPathNodes;
    QUuid m_pathEditingLayerId;
    QUuid m_pathEditingObjectId;
    bool m_penBuildingPath = false;
    bool m_penAppendAtStart = false;
    PathEndpointHit m_penHoveredEndpoint;
    PathHit m_pathHoveredHit;
    int m_pathDragNode = -1;
    PathHitPart m_pathDragPart = PathHitPart::None;
    QPointF m_pathPressDocument;
    VectorBezierPath m_pathGestureOriginal;
    std::optional<DocumentState> m_pathGestureBefore;
    QVector<QUuid> m_pathGestureSelection;
    bool m_pathGestureCreatedLayer = false;
    bool m_cornerGestureConvertedShape = false;
    bool m_cornerGestureChanged = false;
    VectorCornerStyle m_pathCornerStyle = VectorCornerStyle::Rounded;
    double m_pathCornerRadius = 24.0;
    QVector<double> m_pathSnapXTargets;
    QVector<double> m_pathSnapYTargets;
    bool m_pathSnapTargetsValid = false;
    bool m_pathNodeMarqueeActive = false;
    QPointF m_pathNodeMarqueeStart;
    QPointF m_pathNodeMarqueeCurrent;
    QSet<int> m_pathNodeMarqueeBaseSelection;
    QColor m_secondaryColour = QColor(Qt::white);
    QByteArray m_defaultWorkspaceState;

    int m_fillTolerance = 32;
    bool m_fillContiguous = true;
    bool m_fillUseSecondaryColour = false;
    bool m_fillPreserveTransparency = false;

    RasterGradientType m_gradientType = RasterGradientType::Linear;
    RasterGradientColourMode m_gradientColourMode =
        RasterGradientColourMode::PrimaryToSecondary;
    bool m_gradientReverse = false;
    bool m_gradientGestureActive = false;
    QPointF m_gradientStartDocument;
    QPointF m_gradientEndDocument;
    QUuid m_gradientLayerId;
    LayerEditTarget m_gradientEditTarget = LayerEditTarget::Pixels;
    FillTarget m_gradientTarget = FillTarget::RasterPixels;
    int m_gradientComponentIndex = -1;
    QSize m_gradientTargetSize;
    QTransform m_gradientTargetToDocument;
    QTransform m_gradientDocumentToTarget;
    QImage m_gradientSource;
    QImage m_gradientSelectionCoverage;
    QImage m_gradientPreviewSource;
    QImage m_gradientPreviewCoverage;
    QSize m_gradientPreviewTargetSize;
    QSize m_gradientPreviewDocumentSize;
    QImage m_gradientPreviewDocumentSource;
    QVector<LayerNode> m_gradientPreviewLayers;
    QVector<QUuid> m_gradientLayerSelection;
    QTimer m_gradientPreviewTimer;
    QFutureWatcher<GradientPreviewFrame> m_gradientPreviewWatcher;
    std::shared_ptr<std::atomic_bool> m_gradientPreviewCancelToken;
    bool m_gradientPreviewPending = false;
    quint64 m_gradientGestureSerial = 0;
    quint64 m_gradientPreviewGeneration = 0;
    quint64 m_gradientPreviewPublishedGeneration = 0;

    int m_brushSize = 24;
    int m_brushOpacity = 100;
    int m_brushHardness = 80;
    int m_eraserSize = 40;
    int m_cloneSize = 40;
    int m_healSize = 60;
    int m_spotHealSize = 60;
    int m_patchOpacity = 100;
    int m_dodgeSize = 60;
    int m_burnSize = 60;
    int m_spongeSize = 60;
    int m_dodgeExposure = 20;
    int m_burnExposure = 20;
    int m_spongeFlow = 50;
    int m_dodgeHardness = 50;
    int m_burnHardness = 50;
    int m_spongeHardness = 50;
    ToneBrushRange m_dodgeRange = ToneBrushRange::Midtones;
    ToneBrushRange m_burnRange = ToneBrushRange::Midtones;
    ToneBrushOperation m_spongeMode = ToneBrushOperation::SpongeDesaturate;
    bool m_dodgeProtectTones = true;
    bool m_burnProtectTones = true;
    bool m_spongeVibranceProtection = true;
    int m_blurSize = 60;
    int m_blurStrength = 50;
    int m_blurHardness = 50;
    int m_blurRadius = 8;
    int m_sharpenSize = 60;
    int m_sharpenStrength = 40;
    int m_sharpenHardness = 50;
    int m_sharpenRadius = 3;
    bool m_sharpenProtectHighlights = true;
    int m_cloneOpacity = 100;
    int m_healOpacity = 100;
    int m_spotHealOpacity = 100;
    int m_cloneHardness = 80;
    int m_healHardness = 65;
    int m_spotHealHardness = 65;
    CloneStampSourceMode m_cloneSourceMode = CloneStampSourceMode::CurrentTarget;
    CloneStampSourceMode m_healSourceMode = CloneStampSourceMode::CurrentTarget;
    CloneStampSourceMode m_spotHealSourceMode = CloneStampSourceMode::CurrentTarget;
    CloneStampSourceMode m_patchSourceMode = CloneStampSourceMode::CurrentTarget;
    PatchToolMode m_patchMode = PatchToolMode::Source;
    bool m_cloneAligned = true;
    bool m_healAligned = true;
    int m_smudgeSize = 48;
    int m_smudgeStrength = 50;
    int m_smudgeHardness = 50;
    bool m_smudgeFingerPainting = false;
    bool m_snappingEnabled = true;
    int m_transformSnapDistance = 8;
    TransformScope m_transformScope = TransformScope::WholeLayers;
    TransformMode m_transformModeSelection = TransformMode::FreeTransform;
    TransformInterpolation m_transformInterpolation = TransformInterpolation::Bilinear;
    bool m_transformScopeExplicit = false;
    SelectionCombineMode m_selectionCombineMode = SelectionCombineMode::Replace;
    bool m_selectionAntialias = true;
    bool m_selectionFixedOneToOne = false;
    bool m_selectionFromCentre = false;
    SelectionCombineMode m_activeMarqueeCombineMode = SelectionCombineMode::Replace;
    std::optional<SelectionMask::Snapshot> m_selectionMarqueeBefore;
    QImage m_selectionMarqueeBaseCoverage;
    QTimer m_selectionMarqueePreviewTimer;
    QPainterPath m_pendingSelectionPath;
    QString m_activeSelectionGestureName;
    int m_textSize = 36;
    QTextEdit *m_canvasTextEditor = nullptr;
    QUuid m_canvasTextEditLayerId;
    bool m_canvasTextEditCommitting = false;
    QUuid m_activePaintLayerId;
    bool m_paintStrokeActive = false;
    QVector<QLineF> m_paintSegments;
    bool m_activeStrokeErasing = false;
    bool m_activeStrokeCloning = false;
    bool m_activeStrokeHealing = false;
    bool m_activeStrokeSpotHealing = false;
    bool m_activeStrokePatch = false;
    ToneBrushOperation m_activeStrokeToneBrush = ToneBrushOperation::None;
    ToneBrushRange m_activeStrokeToneRange = ToneBrushRange::Midtones;
    bool m_activeStrokeToneProtection = true;
    double m_activeStrokeToneRadius = 1.0;
    bool m_activeStrokeSmudgeFingerPainting = false;
    bool m_activeStrokeTargetsMask = false;
    bool m_activeStrokeTargetsChannel = false;
    int m_activeStrokeChannelIndex = -1;
    double m_activeStrokeDiameter = 24.0;
    double m_activeStrokeOpacity = 1.0;
    double m_activeStrokeHardness = 0.8;
    QColor m_activeStrokeColour = QColor(Qt::black);
    QImage m_activeStrokePreviewTip;
    QPointF m_cloneStrokeSourceOffsetDocument;
    QImage m_cloneSourceImage;
    QImage m_clonePreviewSourceImage;
    QImage m_clonePreviewBaseRaster;
    QImage m_clonePreviewBaseMaskGray;
    QImage m_tonePreviewBaseRaster;
    QImage m_tonePreviewBaseMaskGray;
    ToneBrushStrokeAccumulator m_tonePreviewAccumulator;
    QImage m_smudgePreviewBaseRaster;
    QImage m_smudgePreviewBaseMaskGray;
    QImage m_smudgePreviewSelectionCoverage;
    SmudgeBrushStrokeState m_smudgePreviewState;
    QVector<LayerNode> m_cloneCompositeSourceLayers;
    QImage m_cloneCompositeSourceDocument;
    QTransform m_cloneSourceDocumentToLayer;
    CloneStampSample m_cloneSampleMode = CloneStampSample::Rgba;
    CloneStampTarget m_cloneTargetMode = CloneStampTarget::RasterPixels;
    QVector<LayerNode> m_livePaintLayers;
    QImage m_livePaintRaster;
    QImage m_livePaintMaskGray;
    QImage m_spotHealingOverlayCoverage;
    QImage m_spotHealingOverlaySelectionCoverage;
    SelectionMask::Snapshot m_patchSelectionBefore;
    QImage m_patchPreviewSelectionCoverage;
    QPointF m_patchDragStartDocument;
    QPointF m_patchDragCurrentDocument;
    QRect m_patchLastPreviewDirty;
    QElapsedTimer m_liveMaskThumbnailTimer;
    QVector<QUuid> m_paintBeforeSelection;
    bool m_paintCreatedLayer = false;
    LayerNode m_paintCreatedLayerTemplate;
    QUuid m_paintCreatedParentId;
    int m_paintCreatedIndex = -1;
    QUuid m_pendingPaintLayerId;
    bool m_pendingPaintTargetsMask = false;
    bool m_pendingPaintCloneStamp = false;
    bool m_pendingPaintHealingBrush = false;
    bool m_pendingPaintSpotHealing = false;
    bool m_pendingPaintPatchTool = false;
    ToneBrushOperation m_pendingPaintToneBrush = ToneBrushOperation::None;
    bool m_pendingPatchMovesSelection = false;
    SelectionMask::Snapshot m_pendingPatchSelectionBefore;
    int m_pendingPaintChannelIndex = -1;
    quint64 m_pendingPaintLayerRevision = 0;
    quint64 m_pendingPaintSelectionRevision = 0;
    QTransform m_pendingPaintWorldTransform;

    QVector<QUuid> m_transformLayerIds;
    QHash<QUuid, QTransform> m_transformStartWorldTransforms;
    QHash<QUuid, QTransform> m_transformParentWorldTransforms;
    QRectF m_transformInitialBounds;
    CanvasTransformMode m_transformMode = CanvasTransformMode::None;
    QTransform m_transformSessionTransform;
    QPointF m_transformPivotDocument;
    QPointF m_transformInitialPivotDocument;
    bool m_transformPivotValid = false;
    bool m_transformNumericSyncing = false;
    bool m_transformProportionsLinked = false;
    int m_transformControlPointIndex = 0;
    DocumentState m_transformBeforeState;
    QVector<QUuid> m_transformBeforeSelection;
    SelectionMask::Snapshot m_transformBeforePixelSelection;
    QVector<SelectionTransformTarget> m_selectionTransformTargets;
    bool m_transformUsesSelectedPixels = false;
    bool m_transformDuplicatedLayers = false;
    bool m_transformUsesLiveVectorPreview = false;
    bool m_transformUsesTextBoxResize = false;
    LayerNode m_transformTextStartLayer;
    QVector<LayerNode> m_transformLivePreviewBaseLayers;
    QImage m_transformLivePreviewOriginalForeground;
    QImage m_transformActivePreviewBackground;
    QImage m_transformActivePreviewForeground;
    QRectF m_transformActivePreviewBounds;
    QTransform m_transformActivePreviewTransform;
    QVector<QRectF> m_transformActiveSnapBounds;
    QVector<QPointF> m_transformActiveSnapTargets;
    QVector<QPointF> m_transformActiveSnapSources;
    TransformPreviewCache m_transformPreviewCache;
    QTimer m_transformPreviewPrewarmTimer;
    QFutureWatcher<TransformPreviewCache> m_transformPreviewPrewarmWatcher;
    std::shared_ptr<std::atomic_bool> m_transformPreviewPrewarmCancelToken;
    quint64 m_transformPreviewPrewarmSerial = 0;
    bool m_transformActive = false;

    QComboBox *m_transformModeCombo = nullptr;
    QComboBox *m_transformInterpolationCombo = nullptr;
    QComboBox *m_transformScopeCombo = nullptr;
    QDoubleSpinBox *m_transformXSpin = nullptr;
    QDoubleSpinBox *m_transformYSpin = nullptr;
    QDoubleSpinBox *m_transformWidthSpin = nullptr;
    QDoubleSpinBox *m_transformHeightSpin = nullptr;
    SliderSpinBox *m_transformAngleSpin = nullptr;
    QComboBox *m_transformPointCombo = nullptr;
    QDoubleSpinBox *m_transformPointXSpin = nullptr;
    QDoubleSpinBox *m_transformPointYSpin = nullptr;
    QCheckBox *m_transformLinkCheck = nullptr;
    QPushButton *m_transformApplyButton = nullptr;
    QPushButton *m_transformCancelButton = nullptr;
    QShortcut *m_transformApplyShortcut = nullptr;
    QShortcut *m_transformApplyKeypadShortcut = nullptr;
    QShortcut *m_transformCancelShortcut = nullptr;

    QTimer m_colourUiTimer;
    QTimer m_previewTimer;
    QTimer m_documentThumbnailTimer;
    QFutureWatcher<DocumentThumbnailResult> m_documentThumbnailWatcher;
    std::optional<ClipboardPayload> m_internalClipboardPayload;
    QByteArray m_internalClipboardToken;
    std::shared_ptr<std::atomic_bool> m_documentThumbnailCancelToken;
    bool m_documentThumbnailPending = false;
    QFutureWatcher<PreviewTileBatch> m_previewWatcher;
    QFutureWatcher<PaintCommitResult> m_paintCommitWatcher;
    std::shared_ptr<std::atomic_bool> m_paintCommitCancelToken;
    QQueue<PreviewTileRequest> m_previewTileQueue;
    std::shared_ptr<std::atomic_bool> m_previewCancelToken;
    QVector<QImage> m_previewMipSources;
    qint64 m_previewMipSourceKey = 0;
    QVector<LayerNode> m_previewLayerSnapshot;
    QSize m_previewDocumentSize;
    QRect m_pendingPreviewVisibleRegion;
    double m_pendingPreviewZoom = 1.0;
    bool m_configuringPreviewPresentation = false;
    bool m_navigationInteractionPending = false;
    QRect m_plannedPreviewVisibleRegion;
    double m_plannedPreviewZoom = 0.0;
    bool m_plannedPreviewSettled = false;
    bool m_plannedPreviewAuthoritative = false;
    // True while a CPU spatial interaction frame is retained across render
    // generations. Blur may use a reduced level; detail-sensitive sharpen
    // adjustments retain level 0. It is cleared only after authoritative
    // settlement or an explicit presentation reset.
    bool m_cpuSpatialInteractionPreviewActive = false;
    quint64 m_plannedPreviewGeneration = 0;
    QVector<ImageCanvas::PresentationTileUpdate> m_stagedPreviewTiles;
    int m_stagedPreviewExpectedTiles = 0;
    TiledCanvasEngine::RenderInfo m_stagedPreviewRenderInfo;
    bool m_stagedPreviewHasRenderInfo = false;
    quint64 m_stagedPreviewGeneration = 0;
    quint64 m_stagedPreviewRequestSerial = 0;
    quint64 m_previewSnapshotGeneration = 0;
    quint64 m_previewRequestSerial = 0;
    quint64 m_requestedRenderRevision = 0;
    quint64 m_activeRenderRevision = 0;
    quint64 m_lastPublishedLevelZeroGeneration = 0;
    bool m_exportInProgress = false;
    bool m_closeAfterExportQueueStops = false;
    bool m_exportQueueRecoveryPrepared = false;
    std::shared_ptr<std::atomic_bool> m_exportCancelToken;
    bool m_cropOperationInProgress = false;
    std::shared_ptr<std::atomic_bool> m_cropCancelToken;
    bool m_canvasSizeOperationInProgress = false;
    std::shared_ptr<std::atomic_bool> m_canvasSizeCancelToken;
    bool m_imageSizeOperationInProgress = false;
    std::shared_ptr<std::atomic_bool> m_imageSizeCancelToken;
    bool m_colourProfileOperationInProgress = false;
    std::shared_ptr<std::atomic_bool> m_colourProfileCancelToken;
};

} // namespace vfx
