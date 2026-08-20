#pragma once

#include "TransformInterpolation.h"

#include <QAbstractScrollArea>
#include <QColor>
#include <QHash>
#include <QElapsedTimer>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QPainterPath>
#include <QRectF>
#include <QRegion>
#include <QTimer>
#include <QTransform>
#include <QSize>
#include <QString>
#include <QVector>
#include <QPolygonF>
#include <QLineF>
#include <QSet>

#include "CropTypes.h"

#include <functional>
#include <optional>
#include <memory>

class QEvent;
class QPainter;

namespace vfx {

class CanvasRuler;
class DisplayColourTransform;

enum class CanvasPaintMode {
    None,
    Brush,
    Eraser,
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
    Gradient
};

enum class CanvasTransformMode {
    None,
    Move,
    ScaleTopLeft,
    ScaleTop,
    ScaleTopRight,
    ScaleRight,
    ScaleBottomRight,
    ScaleBottom,
    ScaleBottomLeft,
    ScaleLeft,
    Rotate,
    Pivot,
    SkewTop,
    SkewRight,
    SkewBottom,
    SkewLeft,
    ControlTopLeft,
    ControlTopRight,
    ControlBottomRight,
    ControlBottomLeft
};

enum class CanvasTransformInteractionMode {
    FreeTransform,
    Scale,
    Rotate,
    Skew,
    Distort,
    Perspective
};

enum class CanvasSelectionLassoMode {
    None,
    Freehand,
    Polygonal
};

enum class CanvasMarqueePreviewMode {
    Rectangle,
    RoundedRectangle,
    Ellipse,
    Line,
    Polygon,
    Star,
    Arrow
};

struct CanvasVectorPathNode {
    QPointF anchor;
    QPointF inHandle;
    QPointF outHandle;
    bool inHandleActive = false;
    bool outHandleActive = false;
    QPointF cornerHandle;
    bool cornerHandleActive = false;
};

enum class CanvasVectorEndpointRole {
    Continue,
    Close,
    Join,
    Active
};

struct CanvasVectorEndpointMarker {
    QPointF position;
    CanvasVectorEndpointRole role = CanvasVectorEndpointRole::Continue;
    bool hovered = false;
};

enum class CanvasVectorHoverPart {
    None,
    Anchor,
    InHandle,
    OutHandle,
    CornerHandle,
    Segment
};

struct CanvasVectorHover {
    CanvasVectorHoverPart part = CanvasVectorHoverPart::None;
    int nodeIndex = -1;
    int segmentIndex = -1;
    QPainterPath segmentPath;

    bool isValid() const { return part != CanvasVectorHoverPart::None; }
};

// Presentation-only geometry for the selected Vignette adjustment. The values
// mirror the persisted renderer contract, but this structure never enters
// project, preset, clipboard or residency data.
struct CanvasVignetteOverlay {
    double size = 100.0;
    double midpoint = 50.0;
    double roundness = 0.0;
    double feather = 50.0;
    double centreX = 0.0;
    double centreY = 0.0;
    double rotation = 0.0;

    bool operator==(const CanvasVignetteOverlay &) const = default;
};

class ImageCanvas final : public QAbstractScrollArea {
    Q_OBJECT

public:
    struct PresentationTileUpdate {
        QRect basePreviewRect;
        QImage image;
        int level = 0;
    };

    explicit ImageCanvas(QWidget *parent = nullptr);

    void setImage(const QImage &image, const QSize &documentSize = {});
    void setDisplayColourTransform(
        std::shared_ptr<const DisplayColourTransform> transform);
    QByteArray displayColourTransformFingerprint() const;
    void beginTiledPresentation(const QImage &fallbackImage,
                                const QSize &documentSize,
                                quint64 generation,
                                bool preserveTransientTiles = false);
    void beginTiledPresentationRequest(quint64 generation, quint64 requestSerial);
    void clearTransientPresentationTiles();
    bool updatePresentationTile(const QRect &basePreviewRect,
                                const QImage &tileImage,
                                int level,
                                quint64 generation,
                                quint64 requestSerial);
    bool updatePresentationTiles(const QVector<PresentationTileUpdate> &tiles,
                                 quint64 generation,
                                 quint64 requestSerial);
    QRect visiblePreviewRegion() const;
    void clearImage();
    bool hasImage() const;

    double zoom() const;
    bool isFitToView() const;
    void setZoom(double zoom, const QPointF &anchor = {});
    void zoomOutToPreviousStop();
    void zoomInToNextStop();
    QPoint scrollPosition() const;
    void setScrollPosition(const QPoint &position);
    void fitToView();
    void actualPixels();
    void setToolCursor(Qt::CursorShape cursor);
    QPointF mapDocumentToViewport(const QPointF &documentPosition) const;
    QRectF mapDocumentRectToViewport(const QRectF &documentRect) const;
    QTransform documentToViewportMapping() const;
    void setTextToolHitTestingEnabled(bool enabled);
    void setVectorPathEditingEnabled(bool enabled);
    bool vectorPathEditingEnabled() const;
    void setVectorPathOverlay(const QVector<CanvasVectorPathNode> &nodes,
                              bool closed,
                              const QSet<int> &selectedNodes = {},
                              int activeHandleNode = -1,
                              const QPainterPath &displayPath = {},
                              bool cornerEditing = false,
                              const CanvasVectorHover &hover = {});
    void setVectorPathEndpointMarkers(
        const QVector<CanvasVectorEndpointMarker> &markers);
    void setVectorNodeMarquee(const QRectF &documentBounds, bool visible);
    void clearVectorPathOverlay();
    void setTextEditingActive(bool active);
    bool textEditingActive() const;
    void setLeftDragPans(bool enabled);
    void setColourSamplingEnabled(bool enabled);
    void setPaintMode(CanvasPaintMode mode);
    CanvasPaintMode paintMode() const;
    void cancelPaintGesture();
    void setBrushDiameter(double documentPixels);
    void setCloneSourceMarker(const QPointF &documentPosition, bool visible);
    void clearCloneSourceMarker();
    void setGradientOverlay(const QPointF &startDocument,
                            const QPointF &endDocument,
                            bool visible = true);
    void clearGradientOverlay();

    void setVignetteOverlay(const CanvasVignetteOverlay &overlay,
                            bool visible = true);
    void clearVignetteOverlay();
    bool vignetteOverlayVisible() const;
#ifndef Q_MOC_RUN
    void setVignetteOverlayCallbacks(
        std::function<void()> interactionStarted,
        std::function<void(double, double, double, double, double)> changed,
        std::function<void()> interactionFinished);
#endif

    // Preview-space tile updates. The same region contract is consumed by the
    // WGSL compositor; the current CPU fallback remains useful on unsupported
    // hardware and never requires a full-frame rebuild while the pointer moves.
    void beginLiveCompositePreview();
    void setLiveCompositePreviewImage(const QImage &renderedImage);
    void updateLiveCompositeRegion(const QRect &previewRegion,
                                   const QImage &renderedRegion);
    void clearLiveStrokePreview();
    void commitLiveCompositePreview();

    // Presentation-only layer-mask overlay. The supplied image is interpreted
    // as mask coverage in preview space: black is transparent and white is a
    // 50%-opaque red. It never mutates document pixels or rendered tiles.
    void setMaskOverlay(const QImage &maskImage);
    void setMaskOverlayPreviewImage(const QImage &maskImage);
    void updateMaskOverlayRegion(const QRect &previewRegion,
                                 const QImage &maskRegion);
    void clearMaskOverlay();
    bool hasMaskOverlay() const;

    // Presentation-only treatment mask used by deferred retouch tools such as
    // Spot Healing. Coverage is shown as 50%-opaque red and never enters the
    // document, compositor, thumbnails, history, clipboard or project data.
    void beginTreatmentOverlay(const QSize &previewSize);
    void updateTreatmentOverlayRegion(const QRect &previewRegion,
                                      const QImage &coverageRegion);
    void clearTreatmentOverlay();
    bool hasTreatmentOverlay() const;

    // Presentation-only 50%-coverage selection contour. The coverage image is
    // preview-sized; full-resolution selection tiles remain owned by the
    // document. Animation only repaints the cached path.
    void setSelectionDisplay(const QImage &coverageImage,
                             bool active,
                             bool edgesVisible = true);
    void setSelectionEdgesVisible(bool visible);
    void setSelectionPreviewDisplay(const QImage &coverageImage);
    void clearSelectionPreviewDisplay();
    bool selectionEdgesVisible() const;
    void clearSelectionDisplay();

    void setSelectionMarqueeEnabled(bool enabled);
    void setSelectionMarqueeFixedOneToOne(bool enabled);
    void setSelectionMarqueeFromCentre(bool enabled);
    void setSelectionMarqueeEllipse(bool ellipse);
    void setSelectionMarqueeCornerRadius(double radius);
    void setSelectionMarqueePreviewMode(CanvasMarqueePreviewMode mode,
                                        int polygonSides = 5,
                                        double starInnerRatio = 0.5,
                                        double rotationDegrees = -90.0,
                                        double arrowHeadLengthRatio = 0.35,
                                        double arrowShaftWidthRatio = 0.35);
    CanvasMarqueePreviewMode selectionMarqueePreviewMode() const;
    QLineF selectionMarqueeLine() const;
    void setSelectionMarqueeDeselectOnClick(bool enabled);
    void setSelectionMarqueeFinishOnClick(bool enabled);
    void setSelectionMarqueeClipToImage(bool enabled);
    void setSelectionMarqueeGeometryModifiersEnabled(bool enabled);
    void setSelectionMarqueePixelSnappingEnabled(bool enabled);
    void cancelSelectionMarqueeGesture();
    void setSelectionLassoMode(CanvasSelectionLassoMode mode);
    void cancelSelectionLassoGesture();

    void setTransformDragEnabled(bool enabled);
    void setTransformSnappingEnabled(bool enabled);
    bool transformSnappingEnabled() const;
    void setTransformSnapDistance(double screenPixels);
    double transformSnapDistance() const;
    void setTransformSnapBounds(const QVector<QRectF> &bounds);
    void setTransformSnapPoints(const QVector<QPointF> &targetPoints,
                                const QVector<QPointF> &sourcePoints = {});
    void setTransformInteractionMode(CanvasTransformInteractionMode mode);
    CanvasTransformInteractionMode transformInteractionMode() const;
    void setTransformInterpolation(TransformInterpolation interpolation);
    TransformInterpolation transformInterpolation() const;
    void setTransformPivot(const QPointF &documentPosition);
    QPointF transformPivot() const;
    void setTransformSessionTransform(const QTransform &documentTransform);
    QTransform transformSessionTransform() const;
    void setTransformPendingChanges(bool pending);
    bool transformPendingChanges() const;
    void cancelTransformGesture();
    void setTransformSelectionBounds(const QRectF &documentBounds);
    void clearTransformSelection();
    void beginTransformPreview(
        const QImage &background,
        const QImage &foreground,
        const QRectF &documentBounds,
        const QTransform &foregroundBaseTransform = QTransform(),
        const QRectF &foregroundDocumentBounds = QRectF());
    void setTransformPreviewForeground(
        const QImage &foreground,
        bool alreadyTransformed,
        const QRectF &foregroundDocumentBounds = QRectF());
    void updateTransformPreview(const QTransform &documentTransform);
    void commitTransformPreview();
    void clearTransformPreview();

    void setCropEnabled(bool enabled);
    bool cropEnabled() const;
    void setCropFrame(const QRectF &documentBounds);
    QRectF cropFrame() const;
    void setCropConstraint(CropMode mode, double aspectRatio, const QSize &fixedSize);
    void setCropOverlay(CropOverlay overlay, int orientation = 0);
    void setCropDimOpacity(double opacity);
    void setCropSnappingEnabled(bool enabled);
    void setCropPreviewAngle(double degrees);
    void setCropStraightenSampling(bool enabled);
    void setCropSnapBounds(const QVector<QRectF> &bounds);
    void resetCropInteraction();

    void setRulersVisible(bool visible);
    bool rulersVisible() const;
    void setGuidesVisible(bool visible);
    bool guidesVisible() const;
    void setGuideSnappingEnabled(bool enabled);
    bool guideSnappingEnabled() const;
    void setGuides(const QVector<double> &horizontal, const QVector<double> &vertical);
    const QVector<double> &horizontalGuides() const;
    const QVector<double> &verticalGuides() const;
    void clearGuides();
    void setSnapBounds(const QVector<QRectF> &bounds);

signals:
    void zoomChanged(double zoom);
    void presentationViewportChanged(const QRect &visiblePreviewRegion,
                                     double zoom,
                                     bool settled);
    void fileDropped(const QString &filePath);
    void guidesChanged(const QVector<double> &horizontal, const QVector<double> &vertical);
    void guidesVisibilityChanged(bool visible);
    void colourSampled(const QColor &colour, const QPointF &documentPosition);
    void paintStrokeStarted(const QPointF &documentPosition);
    void paintStrokeContinued(const QPointF &from, const QPointF &to);
    void paintStrokeFinished();
    void cloneSourceSampleRequested(const QPointF &documentPosition);
    void cancelLongRunningEditRequested();
    void selectionMarqueeStarted(const QRectF &documentBounds,
                                 Qt::KeyboardModifiers startModifiers);
    void selectionMarqueeChanged(const QRectF &documentBounds);
    void selectionMarqueeFinished(const QRectF &documentBounds);
    void selectionMarqueeCancelled();
    void selectionLassoStarted(const QPainterPath &documentPath,
                               Qt::KeyboardModifiers startModifiers);
    void selectionLassoChanged(const QPainterPath &documentPath);
    void selectionLassoFinished(const QPainterPath &documentPath);
    void selectionLassoCancelled();
    void selectionDeselectRequested();
    void cropFrameChanged(const QRectF &documentBounds);
    void cropFrameFinished(const QRectF &documentBounds);
    void cropApplyRequested();
    void cropCancelRequested();
    void cropOverlayChanged(CropOverlay overlay, int orientation);
    void cropStraightenLineFinished(const QLineF &documentLine,
                                    Qt::KeyboardModifiers modifiers);
    void transformDragStarted(CanvasTransformMode mode,
                              const QPointF &documentPosition,
                              Qt::KeyboardModifiers startModifiers);
    void transformDragChanged(const QTransform &documentTransform);
    void transformDragFinished(const QTransform &documentTransform);
    void transformPivotChanged(const QPointF &documentPosition);
    void transformControlPointSelected(int index);
    void transformContextMenuRequested(const QPoint &globalPosition);
    void transformApplyRequested();
    void textToolPressed(const QPointF &documentPosition,
                         Qt::KeyboardModifiers modifiers);
    void vectorPathPointerPressed(const QPointF &documentPosition,
                                  Qt::KeyboardModifiers modifiers);
    void vectorPathPointerMoved(const QPointF &documentPosition,
                                Qt::MouseButtons buttons,
                                Qt::KeyboardModifiers modifiers);
    void vectorPathPointerReleased(const QPointF &documentPosition,
                                   Qt::KeyboardModifiers modifiers);
    void vectorPathPointerDoubleClicked(const QPointF &documentPosition,
                                        Qt::KeyboardModifiers modifiers);
    void vectorPathPointerLeft();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    friend class CanvasRuler;

    struct GuideHit {
        Qt::Orientation orientation = Qt::Horizontal;
        int index = -1;
        bool isValid() const { return index >= 0; }
    };

    QRectF imageRect() const;
    void updateScrollBars();
    void updateRulerGeometry();
    void updateRulers();
    bool panGestureActive(const QMouseEvent *event) const;
    bool sampleColourAt(const QPointF &viewportPosition);
    std::optional<QPointF> documentPositionAt(const QPointF &viewportPosition) const;
    QPointF documentPositionAtUnclamped(const QPointF &viewportPosition) const;
    QPointF documentEdgePositionAtUnclamped(
        const QPointF &viewportPosition) const;
    QRectF currentSelectionMarqueeBounds() const;
    QLineF currentSelectionMarqueeLine() const;
    bool appendFreehandLassoPoint(const QPointF &documentPosition,
                                  const QPointF &viewportPosition,
                                  bool force = false);
    QPainterPath currentSelectionLassoPath(bool includeLivePoint,
                                           bool closePath) const;
    QRect selectionLassoViewportBounds() const;
    void finishSelectionLassoGesture();
    QPointF previewPositionForDocument(const QPointF &documentPosition) const;
    QRect viewportRectForPreviewRegion(const QRect &previewRegion) const;
    void notifyPresentationViewportChanged(bool settled);
    void trimPresentationTiles();
    static QString presentationTileKey(const QRect &basePreviewRect, int level);
    static QImage colouriseMaskOverlay(const QImage &maskImage);
    static QPainterPath selectionBoundaryPath(const QImage &coverageImage);

    double documentScale(Qt::Orientation orientation) const;
    double documentCoordinate(Qt::Orientation guideOrientation,
                              double viewportPosition) const;
    double viewportCoordinate(Qt::Orientation guideOrientation,
                              double documentPosition) const;
    double snapGuidePosition(Qt::Orientation orientation,
                             double position,
                             bool *snapped = nullptr) const;
    GuideHit guideAt(const QPointF &position, double tolerance = 7.0) const;
    void removeGuide(const GuideHit &hit);

    void beginGuideFromRuler(Qt::Orientation guideOrientation, double viewportPosition);
    void updateGuideFromRuler(double viewportPosition);
    void finishGuideFromRuler(bool commit);
    void beginExistingGuideDrag(const GuideHit &hit);
    void updateExistingGuideDrag(const QPointF &position);
    void finishExistingGuideDrag(const QPointF &position);
    void cancelGuideDrag();
    void commitGuidesChanged();

    enum class CropDragMode {
        None,
        Create,
        Move,
        Left,
        Top,
        Right,
        Bottom,
        TopLeft,
        TopRight,
        BottomRight,
        BottomLeft
    };
    CropDragMode cropModeAt(const QPointF &viewportPosition) const;
    QRectF cropFrameFromPointer(const QPointF &documentPosition,
                                Qt::KeyboardModifiers modifiers) const;
    QRectF constrainedCropFrame(const QRectF &candidate,
                                CropDragMode mode,
                                Qt::KeyboardModifiers modifiers) const;
    QRectF snappedCropFrame(const QRectF &candidate, CropDragMode mode) const;
    QVector<double> cropSnapTargets(Qt::Orientation orientation) const;
    void updateCropCursor(const QPointF &viewportPosition);
    void paintCropOverlay(QPainter &painter) const;

    enum class VignetteDragMode {
        None,
        Centre,
        Size,
        Midpoint,
        Rotation
    };
    QPointF vignetteCentreDocument() const;
    QSizeF vignetteEffectiveHalfAxes(double size) const;
    double vignetteStartDistance() const;
    double vignetteEndDistance() const;
    QPainterPath vignettePath(double distance) const;
    QPointF vignetteHandleDocumentPosition(VignetteDragMode mode) const;
    VignetteDragMode vignetteModeAt(const QPointF &viewportPosition) const;
    double vignetteDistanceAt(const QPointF &documentPosition, double size) const;
    void updateVignetteFromPointer(const QPointF &documentPosition);
    void updateVignetteCursor(const QPointF &viewportPosition);
    void paintVignetteOverlay(QPainter &painter) const;

    void paintVectorPathOverlay(QPainter &painter) const;
    void updateVectorPathCursor();

    QTransform documentToViewportTransform() const;
    QPolygonF transformBoxPolygon(const QTransform &documentTransform = {}) const;
    QVector<QPointF> transformHandlePoints(const QPolygonF &viewportPolygon) const;
    CanvasTransformMode transformModeAt(const QPointF &viewportPosition) const;
    struct TransformAxisSnap {
        bool active = false;
        double target = 0.0;
        double correction = 0.0;
        double screenDistance = 0.0;
        int anchorIndex = -1;
    };

    QTransform transformFromPointer(const QPointF &documentPosition,
                                    Qt::KeyboardModifiers modifiers);
    QPolygonF transformSourceQuad() const;
    QPolygonF transformDocumentQuad(const QTransform &transform) const;
    static int transformControlPointIndex(CanvasTransformMode mode);
    static bool transformFromQuad(const QPolygonF &source,
                                  const QPolygonF &target,
                                  QTransform *result);
    TransformAxisSnap resolveTransformAxisSnap(
        Qt::Orientation orientation,
        const QVector<double> &anchors,
        bool requireWholePixelCorrection = false,
        bool requireWholePixelTarget = false);
    QVector<double> transformSnapTargets(Qt::Orientation orientation) const;
    void clearTransformSnapState();
    void updateTransformCursor(const QPointF &viewportPosition);

    void paintRuler(QPainter &painter,
                    Qt::Orientation rulerOrientation,
                    const QRect &rect) const;

    QImage displayManagedCopy(const QImage &image) const;
    void rebuildDisplayPresentation();

    struct PresentationTile {
        QRect basePreviewRect;
        QImage image;
        QImage displayImage;
        int level = 0;
        quint64 lastUseSerial = 0;
    };

    QImage m_image;
    QImage m_displayImage;
    std::shared_ptr<const DisplayColourTransform> m_displayColourTransform;
    QHash<QString, PresentationTile> m_presentationTiles;
    QRegion m_authoritativePreviewCoverage;
    quint64 m_presentationGeneration = 0;
    quint64 m_presentationRequestSerial = 0;
    quint64 m_presentationUseSerial = 0;
    QTimer m_presentationSettleTimer;
    QSize m_documentSize;
    double m_zoom = 1.0;
    bool m_fitMode = true;
    bool m_panning = false;
    bool m_spaceHeld = false;
    bool m_leftDragPans = false;
    bool m_colourSamplingEnabled = false;
    bool m_textToolHitTestingEnabled = false;
    bool m_vectorPathEditingEnabled = false;
    QVector<CanvasVectorPathNode> m_vectorPathNodes;
    QVector<CanvasVectorEndpointMarker> m_vectorPathEndpointMarkers;
    QPainterPath m_vectorPathDisplayPath;
    bool m_vectorCornerEditing = false;
    CanvasVectorHover m_vectorPathHover;
    QSet<int> m_vectorPathSelectedNodes;
    bool m_vectorPathClosed = false;
    int m_vectorPathActiveHandleNode = -1;
    QRectF m_vectorNodeMarqueeBounds;
    bool m_vectorNodeMarqueeVisible = false;
    bool m_vectorPathPointerDown = false;
    bool m_textEditingActive = false;
    bool m_samplingColour = false;
    CanvasPaintMode m_paintMode = CanvasPaintMode::None;
    bool m_painting = false;
    QImage m_liveStrokeImage;
    QImage m_displayLiveStrokeImage;
    QImage m_maskOverlayImage;
    QImage m_treatmentOverlayImage;
    QPainterPath m_selectionBoundaryPath;
    QSize m_selectionCoverageSize;
    QPainterPath m_selectionPreviewBoundaryPath;
    QSize m_selectionPreviewCoverageSize;
    bool m_selectionPreviewActive = false;
    QTimer m_selectionAntsTimer;
    int m_selectionAntsPhase = 0;
    bool m_selectionActive = false;
    bool m_selectionEdgesVisible = true;
    bool m_selectionMarqueeEnabled = false;
    bool m_selectionMarqueeFixedOneToOne = false;
    bool m_selectionMarqueeFromCentre = false;
    bool m_selectionMarqueeEllipse = false;
    double m_selectionMarqueeCornerRadius = 0.0;
    CanvasMarqueePreviewMode m_selectionMarqueePreviewMode = CanvasMarqueePreviewMode::Rectangle;
    int m_selectionMarqueePolygonSides = 5;
    double m_selectionMarqueeStarInnerRatio = 0.5;
    double m_selectionMarqueeRotationDegrees = -90.0;
    double m_selectionMarqueeArrowHeadLengthRatio = 0.35;
    double m_selectionMarqueeArrowShaftWidthRatio = 0.35;
    bool m_selectionMarqueeDeselectOnClick = true;
    bool m_selectionMarqueeFinishOnClick = false;
    bool m_selectionMarqueeClipToImage = true;
    bool m_selectionMarqueeGeometryModifiersEnabled = false;
    bool m_selectionMarqueePixelSnappingEnabled = false;
    bool m_selectionMarqueeDragging = false;
    bool m_selectionMarqueeRepositioning = false;
    QPointF m_selectionMarqueeStart;
    QPointF m_selectionMarqueeCurrent;
    QPointF m_selectionMarqueeLastRepositionPoint;
    QPointF m_selectionMarqueePressViewportPosition;
    Qt::KeyboardModifiers m_selectionMarqueeStartModifiers = Qt::NoModifier;
    Qt::KeyboardModifiers m_selectionMarqueeCurrentModifiers = Qt::NoModifier;
    CanvasSelectionLassoMode m_selectionLassoMode = CanvasSelectionLassoMode::None;
    bool m_selectionLassoActive = false;
    bool m_selectionLassoPointerDown = false;
    QPolygonF m_selectionLassoPoints;
    QPolygonF m_selectionLassoViewportPoints;
    QPointF m_selectionLassoCurrent;
    QPointF m_selectionLassoCurrentViewport;
    QPointF m_selectionLassoPressViewportPosition;
    Qt::KeyboardModifiers m_selectionLassoStartModifiers = Qt::NoModifier;
    bool m_paintPositionValid = false;
    double m_brushDiameter = 24.0;
    QPointF m_lastPaintDocumentPosition;
    QPointF m_lastMouseViewportPosition;
    QPointF m_cloneSourceDocumentPosition;
    bool m_cloneSourceMarkerVisible = false;
    QPointF m_gradientStartDocument;
    QPointF m_gradientEndDocument;
    bool m_gradientOverlayVisible = false;
    CanvasVignetteOverlay m_vignetteOverlay;
    CanvasVignetteOverlay m_vignetteDragStartOverlay;
#ifndef Q_MOC_RUN
    std::function<void()> m_vignetteOverlayInteractionStarted;
    std::function<void(double, double, double, double, double)>
        m_vignetteOverlayChanged;
    std::function<void()> m_vignetteOverlayInteractionFinished;
#endif
    bool m_vignetteOverlayVisible = false;
    bool m_vignetteDragging = false;
    VignetteDragMode m_vignetteDragMode = VignetteDragMode::None;
    bool m_mouseOverViewport = false;
    QPoint m_lastPanPosition;
    bool m_transformDragEnabled = false;
    bool m_transformSnappingEnabled = true;
    double m_transformSnapDistance = 8.0;
    bool m_transformDragging = false;
    bool m_transformPivotDragging = false;
    CanvasTransformMode m_transformMode = CanvasTransformMode::None;
    CanvasTransformInteractionMode m_transformInteractionMode =
        CanvasTransformInteractionMode::FreeTransform;
    TransformInterpolation m_transformInterpolation = TransformInterpolation::Bilinear;
    QPointF m_transformStartDocumentPosition;
    QPointF m_transformAnchorDocumentPosition;
    QPointF m_transformStartHandleDocumentPosition;
    QPointF m_transformGestureStartPivotDocument;
    QPointF m_transformPivotDocument;
    bool m_transformPivotValid = false;
    QTransform m_transformGestureBaseTransform;
    QTransform m_transformCurrentTransform;
    QTransform m_transformForegroundBaseTransform;
    QRectF m_transformForegroundDocumentBounds;
    QPolygonF m_transformGestureBaseQuad;
    QImage m_transformBackground;
    QImage m_transformForeground;
    QImage m_transformCompositePreview;
    QImage m_displayTransformBackground;
    QImage m_displayTransformForeground;
    QImage m_displayTransformCompositePreview;
    bool m_transformForegroundAlreadyTransformed = false;
    QElapsedTimer m_transformGpuPreviewTimer;
    QRectF m_transformDocumentBounds;
    QVector<QRectF> m_transformSnapBounds;
    QVector<QPointF> m_transformSnapTargetPoints;
    QVector<QPointF> m_transformSnapSourcePoints;
    bool m_transformSnapXActive = false;
    bool m_transformSnapYActive = false;
    double m_transformSnapXTarget = 0.0;
    double m_transformSnapYTarget = 0.0;
    int m_transformSnapXAnchor = -1;
    int m_transformSnapYAnchor = -1;
    bool m_transformPreviewActive = false;
    bool m_transformPendingChanges = false;
    bool m_cropEnabled = false;
    QRectF m_cropFrame;
    CropMode m_cropConstraintMode = CropMode::Free;
    double m_cropAspectRatio = 0.0;
    QSize m_cropFixedSize;
    CropOverlay m_cropOverlay = CropOverlay::RuleOfThirds;
    int m_cropOverlayOrientation = 0;
    double m_cropDimOpacity = 0.60;
    bool m_cropSnappingEnabled = true;
    double m_cropPreviewAngle = 0.0;
    bool m_cropStraightenSampling = false;
    bool m_cropDragging = false;
    CropDragMode m_cropDragMode = CropDragMode::None;
    QRectF m_cropDragStartFrame;
    QPointF m_cropDragStartDocument;
    QPointF m_cropCreateAnchor;
    bool m_cropCreateRepositioning = false;
    QPointF m_cropCreateRepositionStartDocument;
    QRectF m_cropCreateRepositionStartFrame;
    bool m_cropStraightenDragging = false;
    QPointF m_cropStraightenStart;
    QPointF m_cropStraightenCurrent;
    QVector<QRectF> m_cropSnapBounds;
    Qt::CursorShape m_toolCursor = Qt::CrossCursor;

    CanvasRuler *m_horizontalRuler = nullptr;
    CanvasRuler *m_verticalRuler = nullptr;
    QWidget *m_rulerCorner = nullptr;
    int m_rulerThickness = 24;
    bool m_rulersVisible = false;
    bool m_guidesVisible = true;
    bool m_snapGuides = true;

    QVector<double> m_horizontalGuides;
    QVector<double> m_verticalGuides;
    QVector<QRectF> m_snapBounds;

    bool m_draggingGuide = false;
    bool m_draggingNewGuide = false;
    bool m_dragGuideSnapped = false;
    Qt::Orientation m_dragGuideOrientation = Qt::Horizontal;
    int m_dragGuideIndex = -1;
    double m_dragGuidePosition = 0.0;
};

} // namespace vfx
