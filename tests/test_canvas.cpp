#include "AppStyle.h"
#include "DocumentStripWidget.h"
#include "CurvesEditorWidget.h"
#include "GradientMapEditorWidget.h"
#include "SliderSpinBox.h"
#include "ImageCanvas.h"
#include "PixelSnapping.h"
#include "LayerTreeWidget.h"
#include "ShortcutUtils.h"
#include "ToolBarActionLifecycle.h"

#include <QElapsedTimer>
#include <QHeaderView>
#include <QImage>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QMouseEvent>
#include <QScrollBar>
#include <QList>
#include <QPainter>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QToolBar>
#include <QUndoCommand>
#include <QUndoGroup>
#include <QUndoStack>
#include <QVariant>
#include <QVector>
#include <QtTest/QtTest>

#include <array>
#include <cmath>
#include <memory>

using namespace vfx;

namespace {

void sendLeftDragMove(QWidget *viewport,
                      const QPointF &position,
                      const Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPoint globalPoint = viewport->mapToGlobal(position.toPoint());
    QMouseEvent moveEvent(QEvent::MouseMove,
                          position,
                          position,
                          QPointF(globalPoint),
                          Qt::NoButton,
                          Qt::LeftButton,
                          modifiers);
    QCoreApplication::sendEvent(viewport, &moveEvent);
}

void sendMiddleDragMove(QWidget *viewport, const QPointF &position)
{
    const QPoint globalPoint = viewport->mapToGlobal(position.toPoint());
    QMouseEvent moveEvent(QEvent::MouseMove,
                          position,
                          position,
                          QPointF(globalPoint),
                          Qt::NoButton,
                          Qt::MiddleButton,
                          Qt::NoModifier);
    QCoreApplication::sendEvent(viewport, &moveEvent);
}

} // namespace

class CanvasTests final : public QObject {
    Q_OBJECT

private slots:
    void rebuiltToolBarDestroysOwnedActionsAndPreservesSharedActions();
    void undoStackShutdownBarrierDetachesUiBeforeDestruction();
    void stalePresentationTilesAreRejected();
    void retainedInteractionMipDoesNotFlashBackingBetweenGenerations();
    void atomicPresentationBatchRejectsWholeBatch();
    void visibleRegionTracksZoom();
    void zoomButtonsUsePredictableStops();
    void pixelSnappingUsesDistinctGuideAndVectorLattices();
    void viewportStateRoundTrips();
    void finerTransparentTileReplacesCoarseTile();
    void fineTilePreservesCoarseOutsideCoverage();
    void coarseTileDoesNotDegradeEditBacking();
    void committedLiveCompositeSeedsFollowingStroke();
    void reducedLiveCompositeDoesNotReplaceAuthoritativeBacking();
    void maskOverlayUsesCoverageWithoutChangingBacking();
    void selectionDisplayDrawsWithoutChangingBacking();
    void selectionMarqueeSupportsFixedCentreAndCommit();
    void shapePreviewDoesNotLeakIntoRectangleSelection();
    void selectionMarqueeGeometryModifiersFollowLiveKeys();
    void selectionMarqueeSpaceMovesAndEscapeCancels();
    void selectionMarqueePlainClicksRequestDeselect();
    void freehandLassoSamplesClosesAndCommits();
    void polygonalLassoSupportsVertexEditingAndCompletion();
    void vignetteOverlayEditsSizeBeyondCanvasBounds();
    void committedTransformMatchesLivePreview();
    void transformPreviewBaseTransformRestoresOffCanvasPixels();
    void transformPreviewBoundsRestoreBakedOffCanvasRotation();
    void pretransformedVectorForegroundCommitsWithoutDoubleTransform();
    void transformStartForwardsMouseModifiers();
    void transformMoveSnapsAndControlBypasses();
    void transformMoveUsesWholePixelLattice();
    void transformResizeSnapsDraggedEdge();
    void transformResizeUsesPixelBoundaryLattice();
    void persistentTransformGesturesCompose();
    void transformPivotMovesWithoutChangingSessionMatrix();
    void transformScaleMovesPivotWithContent();
    void transformModesRestrictHandlesWithoutResettingSession();
    void skewMovesOnlyTheSelectedEdge();
    void distortMovesOneCornerIndependently();
    void distortRejectsCrossedQuadrilateral();
    void perspectiveCouplesAdjacentCorners();
    void transformContextMenuIsAvailableInsideBox();
    void transformPendingStateChangesBoundsAccent();
    void transformDoubleClickAppliesOnlyPendingInterior();
    void viewportPublishesSettledRequest();
    void highZoomRenderingIsViewportBounded();
    void redoShortcutsAreUniqueAndPortable();
    void layerTreeCtrlClickThumbnailPreservesLayerSelection();
    void layerTreeRightClickPreservesExistingMultiSelection();
    void documentStripHandlesLargeModelAndActivation();
    void programmaticProgressCloseKeepsCompletedCanvasResultValid();
    void gradientBarEndsDragWhenMouseGrabIsLost();
    void curvesEyedropperAddsStableSamplePoint();
    void gradientStopUtilitiesPreserveOrderAndColours();
    void combinedSliderFieldScrubsAndKeepsClicksEditable();
    void combinedSliderFieldUsesStyleAlignedToolbarHeight();
    void gradientColourButtonUsesContainedIconSwatch();
    void vectorPathEditingPublishesPointerGestures();
    void vectorPathToolsAllowMiddleButtonPanning();
    void vectorHoverFeedbackUpdatesCursorAndOverlay();
};

void CanvasTests::rebuiltToolBarDestroysOwnedActionsAndPreservesSharedActions()
{
    QToolBar toolBar;
    QObject sharedOwner;
    QAction sharedAction(QStringLiteral("Shared"), &sharedOwner);
    QVector<QPointer<QWidget>> generatedWidgets;

    // Exercise the shutdown fallback by deliberately queuing many deferred
    // generations without returning to the event loop between rebuilds.
    for (int generation = 0; generation < 200; ++generation) {
        const int disposed = disposeToolBarOwnedActions(
            &toolBar, ToolBarActionDisposal::Deferred);
        QCOMPARE(disposed, generation > 0 ? 2 : 0);
        QVERIFY(!toolBar.actions().contains(&sharedAction));
        QCOMPARE(sharedAction.parent(), &sharedOwner);

        auto *label = new QLabel(QString::number(generation));
        generatedWidgets.push_back(label);
        toolBar.addWidget(label);
        toolBar.addSeparator();
        toolBar.addAction(&sharedAction);
        QCOMPARE(toolBar.actions().size(), 3);
    }

    QCOMPARE(disposeToolBarOwnedActions(
                 &toolBar, ToolBarActionDisposal::Deferred),
             2);
    QCOMPARE(destroyDeferredToolBarOwnedActions(&toolBar), 400);
    for (const QPointer<QWidget> &widget : generatedWidgets) {
        QVERIFY(widget.isNull());
    }
    QVERIFY(!toolBar.actions().contains(&sharedAction));
    QCOMPARE(sharedAction.parent(), &sharedOwner);

    // Immediate disposal is reserved for MainWindow teardown, where no control
    // signal is still on the stack.
    QPointer<QWidget> finalWidget = new QLabel(QStringLiteral("Final"));
    toolBar.addWidget(finalWidget);
    toolBar.addSeparator();
    QCOMPARE(disposeToolBarOwnedActions(
                 &toolBar, ToolBarActionDisposal::Immediate),
             2);
    QVERIFY(finalWidget.isNull());
}

void CanvasTests::undoStackShutdownBarrierDetachesUiBeforeDestruction()
{
    QObject uiReceiver;
    QUndoGroup undoGroup;
    auto stack = std::make_unique<QUndoStack>();
    undoGroup.addStack(stack.get());
    undoGroup.setActiveStack(stack.get());

    int documentUiRefreshes = 0;
    connect(stack.get(), &QUndoStack::cleanChanged, &uiReceiver,
            [&documentUiRefreshes](bool) { ++documentUiRefreshes; });
    stack->push(new QUndoCommand(QStringLiteral("Edit")));
    documentUiRefreshes = 0;

    // This is the MainWindow shutdown ordering: detach receiver callbacks and
    // QUndoGroup ownership before the session-owned stack begins destruction.
    stack->disconnect(&uiReceiver);
    undoGroup.setActiveStack(nullptr);
    undoGroup.removeStack(stack.get());
    stack.reset();

    QCOMPARE(documentUiRefreshes, 0);
    QVERIFY(undoGroup.stacks().isEmpty());
}

void CanvasTests::stalePresentationTilesAreRejected()
{
    ImageCanvas canvas;
    canvas.resize(800, 600);
    QImage fallback(512, 384, QImage::Format_RGBA8888);
    fallback.fill(QColor(20, 30, 40, 255));
    canvas.beginTiledPresentation(fallback, QSize(1024, 768), 7);
    canvas.beginTiledPresentationRequest(7, 11);

    QImage tile(128, 128, QImage::Format_RGBA8888);
    tile.fill(QColor(255, 0, 0, 255));
    QVERIFY(!canvas.updatePresentationTile(QRect(0, 0, 128, 128), tile, 1, 7, 10));
    QVERIFY(canvas.updatePresentationTile(QRect(0, 0, 128, 128), tile, 1, 7, 11));

    canvas.beginTiledPresentation(fallback, QSize(1024, 768), 8);
    canvas.beginTiledPresentationRequest(8, 12);
    QVERIFY(!canvas.updatePresentationTile(QRect(0, 0, 128, 128), tile, 0, 7, 11));
    QVERIFY(canvas.updatePresentationTile(QRect(0, 0, 128, 128), tile, 0, 8, 12));
}

void CanvasTests::retainedInteractionMipDoesNotFlashBackingBetweenGenerations()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(320, 192);
    canvas.show();

    QImage fallback(256, 128, QImage::Format_ARGB32_Premultiplied);
    fallback.fill(QColor(20, 40, 220, 255));
    canvas.beginTiledPresentation(fallback, fallback.size(), 1);
    canvas.actualPixels();
    canvas.beginTiledPresentationRequest(1, 1);

    QImage redMip(128, 64, QImage::Format_ARGB32_Premultiplied);
    redMip.fill(QColor(220, 30, 20, 255));
    QVERIFY(canvas.updatePresentationTile(fallback.rect(), redMip, 1, 1, 1));
    QCoreApplication::processEvents();

    auto centreColour = [&canvas, &fallback] {
        const QImage viewportImage = canvas.viewport()->grab().toImage();
        const int imageLeft = (viewportImage.width() - fallback.width()) / 2;
        const int imageTop = (viewportImage.height() - fallback.height()) / 2;
        return viewportImage.pixelColor(imageLeft + fallback.width() / 2,
                                        imageTop + fallback.height() / 2);
    };
    QColor colour = centreColour();
    QVERIFY(colour.red() > 180 && colour.blue() < 80);

    // Starting the next interaction generation must retain the previous complete
    // mip instead of exposing the blue sharp backing while the next frame runs.
    canvas.beginTiledPresentation(fallback, fallback.size(), 2, true);
    canvas.beginTiledPresentationRequest(2, 2);
    QCoreApplication::processEvents();
    colour = centreColour();
    QVERIFY(colour.red() > 180 && colour.blue() < 80);

    QImage greenMip(128, 64, QImage::Format_ARGB32_Premultiplied);
    greenMip.fill(QColor(20, 220, 40, 255));
    QVERIFY(canvas.updatePresentationTile(fallback.rect(), greenMip, 1, 2, 2));
    QCoreApplication::processEvents();
    colour = centreColour();
    QVERIFY(colour.green() > 180 && colour.blue() < 80);

    // The release generation retains the last interaction frame until its
    // authoritative level-0 viewport is committed atomically.
    canvas.beginTiledPresentation(fallback, fallback.size(), 3, true);
    canvas.beginTiledPresentationRequest(3, 3);
    QCoreApplication::processEvents();
    colour = centreColour();
    QVERIFY(colour.green() > 180 && colour.blue() < 80);

    QImage yellowFinal(fallback.size(), QImage::Format_ARGB32_Premultiplied);
    yellowFinal.fill(QColor(220, 210, 20, 255));
    QVERIFY(canvas.updatePresentationTile(fallback.rect(), yellowFinal, 0, 3, 3));
    canvas.clearTransientPresentationTiles();
    QCoreApplication::processEvents();
    colour = centreColour();
    QVERIFY(colour.red() > 180 && colour.green() > 170 && colour.blue() < 80);
}

void CanvasTests::atomicPresentationBatchRejectsWholeBatch()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(320, 192);
    canvas.show();

    QImage fallback(256, 128, QImage::Format_ARGB32_Premultiplied);
    fallback.fill(QColor(20, 40, 220, 255));
    canvas.beginTiledPresentation(fallback, fallback.size(), 12);
    canvas.actualPixels();
    canvas.beginTiledPresentationRequest(12, 13);

    QImage red(128, 128, QImage::Format_ARGB32_Premultiplied);
    red.fill(QColor(220, 30, 20, 255));
    QVector<ImageCanvas::PresentationTileUpdate> incomplete {
        {QRect(0, 0, 128, 128), red, 0},
        {QRect(128, 0, 128, 128), QImage(), 0}
    };
    QVERIFY(!canvas.updatePresentationTiles(incomplete, 12, 13));
    QCoreApplication::processEvents();

    QImage viewportImage = canvas.viewport()->grab().toImage();
    const int imageLeft = (viewportImage.width() - fallback.width()) / 2;
    const int imageTop = (viewportImage.height() - fallback.height()) / 2;
    const QColor unchanged = viewportImage.pixelColor(imageLeft + 64, imageTop + 64);
    QVERIFY(unchanged.blue() > 180);
    QVERIFY(unchanged.red() < 80);

    QImage green(128, 128, QImage::Format_ARGB32_Premultiplied);
    green.fill(QColor(20, 220, 40, 255));
    QVector<ImageCanvas::PresentationTileUpdate> complete {
        {QRect(0, 0, 128, 128), red, 0},
        {QRect(128, 0, 128, 128), green, 0}
    };
    QVERIFY(canvas.updatePresentationTiles(complete, 12, 13));
    QCoreApplication::processEvents();

    viewportImage = canvas.viewport()->grab().toImage();
    const QColor left = viewportImage.pixelColor(imageLeft + 64, imageTop + 64);
    const QColor right = viewportImage.pixelColor(imageLeft + 192, imageTop + 64);
    QVERIFY(left.red() > 180 && left.blue() < 80);
    QVERIFY(right.green() > 180 && right.blue() < 80);
}

void CanvasTests::visibleRegionTracksZoom()
{
    ImageCanvas canvas;
    canvas.resize(640, 480);
    canvas.show();
    QTest::qWait(20);

    QImage fallback(1024, 768, QImage::Format_RGBA8888);
    fallback.fill(Qt::black);
    canvas.beginTiledPresentation(fallback, fallback.size(), 1);
    const QRect fitted = canvas.visiblePreviewRegion();
    QVERIFY(!fitted.isEmpty());

    canvas.setZoom(2.0);
    QCoreApplication::processEvents();
    const QRect zoomed = canvas.visiblePreviewRegion();
    QVERIFY(!zoomed.isEmpty());
    QVERIFY(zoomed.width() < fitted.width());
    QVERIFY(zoomed.height() < fitted.height());
}


void CanvasTests::zoomButtonsUsePredictableStops()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(640, 480);
    canvas.show();
    QTest::qWait(20);

    QImage image(1024, 768, QImage::Format_RGBA8888);
    image.fill(Qt::black);
    canvas.setImage(image, image.size());

    canvas.setZoom(1.83);
    canvas.zoomOutToPreviousStop();
    QVERIFY(std::abs(canvas.zoom() - 1.75) < 0.0001);
    canvas.zoomInToNextStop();
    QVERIFY(std::abs(canvas.zoom() - 2.0) < 0.0001);

    canvas.setZoom(1.75);
    canvas.zoomOutToPreviousStop();
    QVERIFY(std::abs(canvas.zoom() - 1.5) < 0.0001);
    canvas.setZoom(1.75);
    canvas.zoomInToNextStop();
    QVERIFY(std::abs(canvas.zoom() - 2.0) < 0.0001);

    canvas.setZoom(0.20);
    canvas.zoomOutToPreviousStop();
    QVERIFY(std::abs(canvas.zoom() - 0.125) < 0.0001);
    canvas.zoomInToNextStop();
    QVERIFY(std::abs(canvas.zoom() - 0.25) < 0.0001);

    canvas.setZoom(0.125);
    canvas.zoomOutToPreviousStop();
    QVERIFY(std::abs(canvas.zoom() - 0.0625) < 0.0001);
    canvas.zoomInToNextStop();
    QVERIFY(std::abs(canvas.zoom() - 0.125) < 0.0001);

    canvas.setZoom(0.02);
    canvas.zoomOutToPreviousStop();
    QVERIFY(std::abs(canvas.zoom() - 0.02) < 0.0001);
    canvas.setZoom(32.0);
    canvas.zoomInToNextStop();
    QVERIFY(std::abs(canvas.zoom() - 32.0) < 0.0001);
}



void CanvasTests::pixelSnappingUsesDistinctGuideAndVectorLattices()
{
    QCOMPARE(snapGuideCoordinate(42.24, 127.0), 42.0);
    QCOMPARE(snapGuideCoordinate(42.26, 127.0), 42.5);
    QCOMPARE(snapGuideCoordinate(63.49, 127.0), 63.5);
    QCOMPARE(snapGuideCoordinate(200.0, 127.0), 127.0);

    QCOMPARE(snapVectorBoundaryPoint(QPointF(42.49, 63.51)),
             QPointF(42.0, 64.0));
    QCOMPARE(constrainPixelBoundaryPointTo45(QPointF(10.0, 10.0),
                                             QPointF(17.0, 14.0)),
             QPointF(17.0, 17.0));
    QCOMPARE(constrainPixelBoundaryPointTo45(QPointF(10.0, 10.0),
                                             QPointF(11.0, 18.0)),
             QPointF(10.0, 18.0));
    QVERIFY(isPixelBoundaryCoordinate(42.0));
    QVERIFY(!isPixelBoundaryCoordinate(42.5));
}

void CanvasTests::viewportStateRoundTrips()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(420, 280);
    canvas.show();
    QTest::qWait(20);

    QImage image(1600, 1200, QImage::Format_RGBA8888);
    image.fill(QColor(20, 30, 40, 255));
    canvas.setImage(image, image.size());
    QVERIFY(canvas.isFitToView());
    canvas.setZoom(1.5);
    QVERIFY(!canvas.isFitToView());
    QCoreApplication::processEvents();

    const QPoint requested(211, 137);
    canvas.setScrollPosition(requested);
    QCOMPARE(canvas.scrollPosition(), requested);
    QVERIFY(std::abs(canvas.zoom() - 1.5) < 0.0001);
}

void CanvasTests::finerTransparentTileReplacesCoarseTile()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 400);
    canvas.show();

    QImage fallback(256, 256, QImage::Format_ARGB32_Premultiplied);
    fallback.fill(Qt::transparent);
    canvas.beginTiledPresentation(fallback, fallback.size(), 3);
    canvas.beginTiledPresentationRequest(3, 4);

    QImage coarse(128, 128, QImage::Format_ARGB32_Premultiplied);
    coarse.fill(QColor(255, 0, 0, 255));
    QVERIFY(canvas.updatePresentationTile(fallback.rect(), coarse, 1, 3, 4));

    QImage fine(256, 256, QImage::Format_ARGB32_Premultiplied);
    fine.fill(Qt::transparent);
    QVERIFY(canvas.updatePresentationTile(fallback.rect(), fine, 0, 3, 4));
    QCoreApplication::processEvents();

    const QImage viewportImage = canvas.viewport()->grab().toImage();
    const QColor centre = viewportImage.pixelColor(viewportImage.width() / 2,
                                                    viewportImage.height() / 2);
    QVERIFY2(centre.red() < 100,
             "A transparent fine tile left the opaque coarse tile visible underneath");
}



void CanvasTests::fineTilePreservesCoarseOutsideCoverage()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(384, 384);
    canvas.show();

    QImage fallback(256, 256, QImage::Format_ARGB32_Premultiplied);
    fallback.fill(QColor(20, 40, 220, 255));
    canvas.beginTiledPresentation(fallback, fallback.size(), 9);
    canvas.actualPixels();
    canvas.beginTiledPresentationRequest(9, 10);

    QImage coarse(128, 128, QImage::Format_ARGB32_Premultiplied);
    coarse.fill(QColor(220, 30, 20, 255));
    QVERIFY(canvas.updatePresentationTile(fallback.rect(), coarse, 1, 9, 10));

    QImage fine(128, 256, QImage::Format_ARGB32_Premultiplied);
    fine.fill(QColor(20, 220, 40, 255));
    QVERIFY(canvas.updatePresentationTile(QRect(0, 0, 128, 256), fine, 0, 9, 10));
    QCoreApplication::processEvents();

    const QImage viewportImage = canvas.viewport()->grab().toImage();
    const int imageLeft = (viewportImage.width() - fallback.width()) / 2;
    const int imageTop = (viewportImage.height() - fallback.height()) / 2;
    const QColor refined = viewportImage.pixelColor(imageLeft + 64, imageTop + 128);
    const QColor pending = viewportImage.pixelColor(imageLeft + 192, imageTop + 128);
    const QColor boundary = viewportImage.pixelColor(imageLeft + 127, imageTop + 128);
    QVERIFY(refined.green() > 180 && refined.red() < 80);
    QVERIFY(pending.red() > 180 && pending.blue() < 80);
    QVERIFY(boundary.red() > 100 || boundary.green() > 100);
}

void CanvasTests::coarseTileDoesNotDegradeEditBacking()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(256, 256);
    canvas.show();

    QImage fallback(128, 128, QImage::Format_ARGB32_Premultiplied);
    fallback.fill(QColor(20, 40, 220, 255));
    canvas.beginTiledPresentation(fallback, fallback.size(), 5);
    canvas.actualPixels();
    canvas.beginTiledPresentationRequest(5, 6);

    QImage coarse(64, 64, QImage::Format_ARGB32_Premultiplied);
    coarse.fill(QColor(220, 30, 20, 255));
    QVERIFY(canvas.updatePresentationTile(fallback.rect(), coarse, 1, 5, 6));

    // Starting an edit must copy the last sharp level-0 backing, not the
    // navigation-only coarse tile currently painted over it.
    canvas.beginLiveCompositePreview();
    QCoreApplication::processEvents();
    const QImage viewportImage = canvas.viewport()->grab().toImage();
    const QColor centre = viewportImage.pixelColor(viewportImage.width() / 2,
                                                    viewportImage.height() / 2);
    QVERIFY(centre.blue() > 180);
    QVERIFY(centre.red() < 80);
}

void CanvasTests::committedLiveCompositeSeedsFollowingStroke()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(256, 256);
    canvas.show();

    QImage fallback(128, 128, QImage::Format_ARGB32_Premultiplied);
    fallback.fill(QColor(20, 40, 220, 255));
    canvas.beginTiledPresentation(fallback, fallback.size(), 2);
    canvas.actualPixels();
    canvas.beginLiveCompositePreview();

    QImage green(128, 128, QImage::Format_ARGB32_Premultiplied);
    green.fill(QColor(20, 220, 40, 255));
    canvas.updateLiveCompositeRegion(fallback.rect(), green);
    canvas.commitLiveCompositePreview();

    // A rapid next stroke calls beginLiveCompositePreview immediately. It
    // must start from the previous committed stroke rather than stale backing.
    canvas.beginLiveCompositePreview();
    QCoreApplication::processEvents();
    const QImage viewportImage = canvas.viewport()->grab().toImage();
    const QColor centre = viewportImage.pixelColor(viewportImage.width() / 2,
                                                    viewportImage.height() / 2);
    QVERIFY(centre.green() > 180);
    QVERIFY(centre.blue() < 80);
}

void CanvasTests::reducedLiveCompositeDoesNotReplaceAuthoritativeBacking()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(256, 256);
    canvas.show();

    QImage backing(128, 128, QImage::Format_ARGB32_Premultiplied);
    backing.fill(QColor(20, 40, 220, 255));
    canvas.beginTiledPresentation(backing, backing.size(), 4);
    canvas.actualPixels();

    QImage reduced(32, 32, QImage::Format_ARGB32_Premultiplied);
    reduced.fill(QColor(20, 220, 40, 255));
    canvas.setLiveCompositePreviewImage(reduced);
    QCoreApplication::processEvents();
    QImage viewportImage = canvas.viewport()->grab().toImage();
    QColor centre = viewportImage.pixelColor(viewportImage.width() / 2,
                                              viewportImage.height() / 2);
    QVERIFY(centre.green() > 180);
    QVERIFY(centre.blue() < 80);

    canvas.commitLiveCompositePreview();
    canvas.beginLiveCompositePreview();
    QCoreApplication::processEvents();
    viewportImage = canvas.viewport()->grab().toImage();
    centre = viewportImage.pixelColor(viewportImage.width() / 2,
                                      viewportImage.height() / 2);
    QVERIFY(centre.blue() > 180);
    QVERIFY(centre.green() < 80);
}

void CanvasTests::maskOverlayUsesCoverageWithoutChangingBacking()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(160, 120);
    canvas.show();

    QImage backing(64, 64, QImage::Format_RGBA8888);
    backing.fill(QColor(0, 0, 255, 255));
    canvas.setImage(backing, backing.size());
    canvas.actualPixels();

    QImage whiteMask(backing.size(), QImage::Format_Grayscale8);
    whiteMask.fill(255);
    canvas.setMaskOverlay(whiteMask);
    QVERIFY(canvas.hasMaskOverlay());
    QCoreApplication::processEvents();

    QImage viewportImage = canvas.viewport()->grab().toImage();
    const int imageLeft = (viewportImage.width() - backing.width()) / 2;
    const int imageTop = (viewportImage.height() - backing.height()) / 2;
    const QColor covered = viewportImage.pixelColor(imageLeft + 48, imageTop + 32);
    QVERIFY(covered.red() > 100 && covered.red() < 170);
    QVERIFY(covered.green() < 20);
    QVERIFY(covered.blue() > 90 && covered.blue() < 170);

    QImage blackHalf(QSize(32, 64), QImage::Format_Grayscale8);
    blackHalf.fill(0);
    canvas.updateMaskOverlayRegion(QRect(0, 0, 32, 64), blackHalf);
    QCoreApplication::processEvents();

    viewportImage = canvas.viewport()->grab().toImage();
    const QColor cleared = viewportImage.pixelColor(imageLeft + 16, imageTop + 32);
    const QColor stillCovered = viewportImage.pixelColor(imageLeft + 48, imageTop + 32);
    QVERIFY(cleared.red() < 20 && cleared.blue() > 230);
    QVERIFY(stillCovered.red() > 100 && stillCovered.blue() > 90);

    canvas.clearMaskOverlay();
    QVERIFY(!canvas.hasMaskOverlay());
    QCoreApplication::processEvents();
    viewportImage = canvas.viewport()->grab().toImage();
    const QColor restored = viewportImage.pixelColor(imageLeft + 48, imageTop + 32);
    QVERIFY(restored.red() < 20 && restored.blue() > 230);
}


void CanvasTests::selectionDisplayDrawsWithoutChangingBacking()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(160, 120);
    canvas.show();

    QImage backing(64, 64, QImage::Format_RGBA8888);
    backing.fill(QColor(0, 0, 255, 255));
    canvas.setImage(backing, backing.size());
    canvas.actualPixels();

    QImage coverage(backing.size(), QImage::Format_Grayscale8);
    coverage.fill(0);
    QPainter coveragePainter(&coverage);
    coveragePainter.fillRect(QRect(16, 16, 32, 32), Qt::white);
    coveragePainter.end();
    canvas.setSelectionDisplay(coverage, true, true);
    QTest::qWait(20);

    QImage viewportImage = canvas.viewport()->grab().toImage();
    const int imageLeft = (viewportImage.width() - backing.width()) / 2;
    const int imageTop = (viewportImage.height() - backing.height()) / 2;
    const QColor boundary = viewportImage.pixelColor(imageLeft + 16, imageTop + 32);
    const QColor interior = viewportImage.pixelColor(imageLeft + 32, imageTop + 32);
    QVERIFY(boundary.blue() < 230 || boundary.red() > 20 || boundary.green() > 20);
    QVERIFY(interior.blue() > 230 && interior.red() < 20 && interior.green() < 20);

    canvas.setSelectionEdgesVisible(false);
    QCoreApplication::processEvents();
    viewportImage = canvas.viewport()->grab().toImage();
    const QColor restoredBoundary = viewportImage.pixelColor(imageLeft + 16, imageTop + 32);
    QVERIFY(restoredBoundary.blue() > 230);
    QVERIFY(restoredBoundary.red() < 20 && restoredBoundary.green() < 20);

    canvas.setSelectionEdgesVisible(true);
    canvas.clearSelectionDisplay();
    QCoreApplication::processEvents();
    viewportImage = canvas.viewport()->grab().toImage();
    const QColor cleared = viewportImage.pixelColor(imageLeft + 16, imageTop + 32);
    QVERIFY(cleared.blue() > 230);
}

void CanvasTests::selectionMarqueeSupportsFixedCentreAndCommit()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(220, 220);
    canvas.show();

    QImage image(100, 100, QImage::Format_RGBA8888);
    image.fill(QColor(20, 30, 40, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setSelectionMarqueeEnabled(true);
    canvas.setSelectionMarqueeFixedOneToOne(true);
    canvas.setSelectionMarqueeFromCentre(true);
    QCoreApplication::processEvents();

    QSignalSpy finished(&canvas, &ImageCanvas::selectionMarqueeFinished);
    QWidget *viewport = canvas.viewport();
    const QPoint imageOrigin((viewport->width() - image.width()) / 2,
                             (viewport->height() - image.height()) / 2);
    const QPoint start = imageOrigin + QPoint(50, 50);
    const QPoint end = start + QPoint(20, 10);
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    sendLeftDragMove(viewport, end);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, end);

    QCOMPARE(finished.count(), 1);
    const QRectF bounds = finished.takeFirst().at(0).toRectF();
    QVERIFY(std::abs(bounds.width() - bounds.height()) < 0.01);
    QVERIFY(std::abs(bounds.width() - 40.0) < 0.75);
    QVERIFY(std::abs(bounds.center().x() - 50.0) < 0.75);
    QVERIFY(std::abs(bounds.center().y() - 50.0) < 0.75);
}

void CanvasTests::shapePreviewDoesNotLeakIntoRectangleSelection()
{
    ImageCanvas canvas;
    canvas.setSelectionMarqueePreviewMode(CanvasMarqueePreviewMode::Star, 7, 0.4, -90.0);
    QVERIFY(canvas.selectionMarqueePreviewMode() == CanvasMarqueePreviewMode::Star);

    // Rectangle Select calls setSelectionMarqueeEllipse(false). The ellipse
    // flag may already be false after drawing a vector shape, but the preview
    // mode must still be restored from Star/Polygon/etc. to Rectangle.
    canvas.setSelectionMarqueeEllipse(false);
    QVERIFY(canvas.selectionMarqueePreviewMode() == CanvasMarqueePreviewMode::Rectangle);

    canvas.setSelectionMarqueePreviewMode(CanvasMarqueePreviewMode::Polygon, 6);
    canvas.setSelectionMarqueeEllipse(true);
    QVERIFY(canvas.selectionMarqueePreviewMode() == CanvasMarqueePreviewMode::Ellipse);
    canvas.setSelectionMarqueeEllipse(false);
    QVERIFY(canvas.selectionMarqueePreviewMode() == CanvasMarqueePreviewMode::Rectangle);
}

void CanvasTests::selectionMarqueeGeometryModifiersFollowLiveKeys()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(240, 220);
    canvas.show();

    QImage image(120, 100, QImage::Format_RGBA8888);
    image.fill(QColor(30, 40, 50, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setSelectionMarqueeEnabled(true);
    canvas.setSelectionMarqueeGeometryModifiersEnabled(true);
    QCoreApplication::processEvents();

    QSignalSpy finished(&canvas, &ImageCanvas::selectionMarqueeFinished);
    QWidget *viewport = canvas.viewport();
    const QPoint imageOrigin((viewport->width() - image.width()) / 2,
                             (viewport->height() - image.height()) / 2);
    const QPoint startPoint = imageOrigin + QPoint(30, 30);
    const QPoint endPoint = startPoint + QPoint(36, 18);

    // Shift may be pressed after the drag has already started. The live preview
    // and committed geometry should immediately become 1:1.
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, startPoint);
    sendLeftDragMove(viewport, endPoint, Qt::NoModifier);
    QTest::keyPress(&canvas, Qt::Key_Shift, Qt::ShiftModifier);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::ShiftModifier, endPoint);
    QTest::keyRelease(&canvas, Qt::Key_Shift, Qt::NoModifier);

    QCOMPARE(finished.count(), 1);
    const QRectF constrained = finished.takeFirst().at(0).toRectF();
    QVERIFY(std::abs(constrained.width() - constrained.height()) < 0.01);
    QVERIFY(std::abs(constrained.width() - 36.0) < 0.75);

    // Line creation uses the same live Shift state but snaps to exact 45-degree
    // increments rather than forcing a square marquee diagonal.
    canvas.setSelectionMarqueePreviewMode(CanvasMarqueePreviewMode::Line);
    const QPoint lineEnd = startPoint + QPoint(30, 20);
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, startPoint);
    sendLeftDragMove(viewport, lineEnd, Qt::ShiftModifier);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::ShiftModifier, lineEnd);
    const QLineF line = canvas.selectionMarqueeLine();
    QVERIFY(std::abs(std::abs(line.dx()) - std::abs(line.dy())) < 0.75);
}

void CanvasTests::selectionMarqueeSpaceMovesAndEscapeCancels()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(220, 220);
    canvas.show();

    QImage image(100, 100, QImage::Format_RGBA8888);
    image.fill(QColor(40, 50, 60, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setSelectionMarqueeEnabled(true);
    QCoreApplication::processEvents();

    QSignalSpy finished(&canvas, &ImageCanvas::selectionMarqueeFinished);
    QSignalSpy cancelled(&canvas, &ImageCanvas::selectionMarqueeCancelled);
    QWidget *viewport = canvas.viewport();
    const QPoint imageOrigin((viewport->width() - image.width()) / 2,
                             (viewport->height() - image.height()) / 2);
    const QPoint start = imageOrigin + QPoint(20, 20);
    const QPoint firstEnd = imageOrigin + QPoint(40, 35);
    const QPoint movedEnd = firstEnd + QPoint(10, 5);

    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    sendLeftDragMove(viewport, firstEnd);
    QTest::keyPress(viewport, Qt::Key_Space);
    sendLeftDragMove(viewport, movedEnd);
    QTest::keyRelease(viewport, Qt::Key_Space);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, movedEnd);

    QCOMPARE(finished.count(), 1);
    QRectF bounds = finished.takeFirst().at(0).toRectF();
    QVERIFY(std::abs(bounds.left() - 30.0) < 0.75);
    QVERIFY(std::abs(bounds.top() - 25.0) < 0.75);
    QVERIFY(std::abs(bounds.width() - 20.0) < 0.75);
    QVERIFY(std::abs(bounds.height() - 15.0) < 0.75);

    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    sendLeftDragMove(viewport, firstEnd);
    QTest::keyPress(viewport, Qt::Key_Escape);
    QCOMPARE(cancelled.count(), 1);
    QCOMPARE(finished.count(), 0);
}

void CanvasTests::selectionMarqueePlainClicksRequestDeselect()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(220, 220);
    canvas.show();

    QImage image(100, 100, QImage::Format_RGBA8888);
    image.fill(QColor(40, 50, 60, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setSelectionMarqueeEnabled(true);
    QCoreApplication::processEvents();

    QSignalSpy deselect(&canvas, &ImageCanvas::selectionDeselectRequested);
    QSignalSpy cancelled(&canvas, &ImageCanvas::selectionMarqueeCancelled);
    QSignalSpy finished(&canvas, &ImageCanvas::selectionMarqueeFinished);
    QWidget *viewport = canvas.viewport();
    const QPoint imageOrigin((viewport->width() - image.width()) / 2,
                             (viewport->height() - image.height()) / 2);

    QTest::mouseClick(viewport,
                      Qt::LeftButton,
                      Qt::NoModifier,
                      imageOrigin + QPoint(40, 40));
    QCOMPARE(deselect.count(), 1);
    QCOMPARE(cancelled.count(), 0);
    QCOMPARE(finished.count(), 0);

    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
    QCOMPARE(deselect.count(), 2);
    QCOMPARE(cancelled.count(), 0);
    QCOMPARE(finished.count(), 0);

    QTest::mouseClick(viewport, Qt::LeftButton, Qt::ShiftModifier, QPoint(8, 8));
    QCOMPARE(deselect.count(), 2);
    QCOMPARE(cancelled.count(), 1);
    QCOMPARE(finished.count(), 0);

    // A drag may begin in the grey overscroll area and cross into the image.
    const QPoint voidStart(8, 8);
    const QPoint insideEnd = imageOrigin + QPoint(40, 40);
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, voidStart);
    sendLeftDragMove(viewport, insideEnd);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, insideEnd);
    QCOMPARE(finished.count(), 1);
    const QRectF outsideStartedBounds = finished.takeFirst().at(0).toRectF();
    QVERIFY(outsideStartedBounds.left() < 0.0);
    QVERIFY(outsideStartedBounds.top() < 0.0);
    QVERIFY(outsideStartedBounds.right() > 0.0);
    QVERIFY(outsideStartedBounds.bottom() > 0.0);
}

void CanvasTests::freehandLassoSamplesClosesAndCommits()
{
    qRegisterMetaType<QPainterPath>();
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(260, 240);
    canvas.show();

    QImage image(140, 120, QImage::Format_RGBA8888);
    image.fill(QColor(30, 40, 50, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setSelectionLassoMode(CanvasSelectionLassoMode::Freehand);
    QCoreApplication::processEvents();

    QSignalSpy started(&canvas, &ImageCanvas::selectionLassoStarted);
    QSignalSpy changed(&canvas, &ImageCanvas::selectionLassoChanged);
    QSignalSpy finished(&canvas, &ImageCanvas::selectionLassoFinished);
    QSignalSpy deselect(&canvas, &ImageCanvas::selectionDeselectRequested);
    QWidget *viewport = canvas.viewport();
    const QPoint imageOrigin((viewport->width() - image.width()) / 2,
                             (viewport->height() - image.height()) / 2);
    const QPoint start = imageOrigin + QPoint(25, 25);
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    for (int x = 26; x <= 95; ++x) {
        sendLeftDragMove(viewport, imageOrigin + QPoint(x, 25));
    }
    for (int y = 26; y <= 88; ++y) {
        sendLeftDragMove(viewport, imageOrigin + QPoint(95, y));
    }
    for (int x = 94; x >= 28; --x) {
        sendLeftDragMove(viewport, imageOrigin + QPoint(x, 88));
    }
    QTest::mouseRelease(viewport,
                        Qt::LeftButton,
                        Qt::NoModifier,
                        imageOrigin + QPoint(28, 88));

    QCOMPARE(started.count(), 1);
    QVERIFY(changed.count() > 0);
    QCOMPARE(finished.count(), 1);
    const QPainterPath path = qvariant_cast<QPainterPath>(
        finished.takeFirst().at(0));
    QVERIFY(path.contains(QPointF(60.0, 55.0)));
    QVERIFY(path.elementCount() < 80);
    QVERIFY(path.boundingRect().width() > 60.0);
    QVERIFY(path.boundingRect().height() > 55.0);

    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
    QCOMPARE(deselect.count(), 1);

    // Freehand lasso gestures may also begin outside the image and cross in.
    const QPoint outsideStart(8, 8);
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, outsideStart);
    sendLeftDragMove(viewport, imageOrigin + QPoint(20, 20));
    sendLeftDragMove(viewport, imageOrigin + QPoint(100, 20));
    sendLeftDragMove(viewport, imageOrigin + QPoint(100, 80));
    QTest::mouseRelease(viewport,
                        Qt::LeftButton,
                        Qt::NoModifier,
                        imageOrigin + QPoint(30, 80));
    QCOMPARE(started.count(), 3);
    QCOMPARE(finished.count(), 1);
    const QPainterPath outsideStartedPath = qvariant_cast<QPainterPath>(
        finished.takeFirst().at(0));
    QVERIFY(outsideStartedPath.boundingRect().left() < 0.0);
    QVERIFY(outsideStartedPath.contains(QPointF(60.0, 45.0)));
}

void CanvasTests::polygonalLassoSupportsVertexEditingAndCompletion()
{
    qRegisterMetaType<QPainterPath>();
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(260, 240);
    canvas.show();

    QImage image(140, 120, QImage::Format_RGBA8888);
    image.fill(QColor(50, 60, 70, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setSelectionLassoMode(CanvasSelectionLassoMode::Polygonal);
    QCoreApplication::processEvents();

    QSignalSpy started(&canvas, &ImageCanvas::selectionLassoStarted);
    QSignalSpy changed(&canvas, &ImageCanvas::selectionLassoChanged);
    QSignalSpy finished(&canvas, &ImageCanvas::selectionLassoFinished);
    QSignalSpy cancelled(&canvas, &ImageCanvas::selectionLassoCancelled);
    QWidget *viewport = canvas.viewport();
    const QPoint imageOrigin((viewport->width() - image.width()) / 2,
                             (viewport->height() - image.height()) / 2);
    const QPoint a = imageOrigin + QPoint(25, 25);
    const QPoint b = imageOrigin + QPoint(105, 30);
    const QPoint c = imageOrigin + QPoint(95, 90);
    const QPoint d = imageOrigin + QPoint(35, 95);

    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, a);
    QTest::mouseMove(viewport, b);
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, b);
    QTest::mouseMove(viewport, c);
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, c);
    QTest::keyClick(viewport, Qt::Key_Backspace);
    QTest::mouseMove(viewport, d);
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, d);
    QTest::mouseMove(viewport, c);
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, c);
    QTest::keyClick(viewport, Qt::Key_Return);

    QCOMPARE(started.count(), 1);
    QVERIFY(changed.count() >= 3);
    QCOMPARE(finished.count(), 1);
    const QPainterPath path = qvariant_cast<QPainterPath>(
        finished.takeFirst().at(0));
    QVERIFY(path.contains(QPointF(65.0, 55.0)));
    QVERIFY(path.elementCount() >= 5);

    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, a);
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, b);
    QTest::keyClick(viewport, Qt::Key_Escape);
    QCOMPARE(cancelled.count(), 1);

    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, a);
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, b);
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, c);
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, a);
    QCOMPARE(finished.count(), 1);
    finished.clear();

    // The first polygonal vertex may be placed in the canvas void.
    const QPoint outsideStart(8, 8);
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, outsideStart);
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, b);
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, c);
    QTest::keyClick(viewport, Qt::Key_Return);
    QCOMPARE(finished.count(), 1);
    const QPainterPath outsideStartedPath = qvariant_cast<QPainterPath>(
        finished.takeFirst().at(0));
    QVERIFY(outsideStartedPath.boundingRect().left() < 0.0);
    QVERIFY(outsideStartedPath.boundingRect().intersects(
        QRectF(QPointF(0.0, 0.0), QSizeF(image.size()))));
}


void CanvasTests::vignetteOverlayEditsSizeBeyondCanvasBounds()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(900, 500);
    canvas.show();

    QImage image(200, 100, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(110, 125, 140, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    QCoreApplication::processEvents();

    CanvasVignetteOverlay overlay;
    overlay.size = 250.0;
    overlay.midpoint = 50.0;
    canvas.setVignetteOverlay(overlay, true);
    QVERIFY(canvas.vignetteOverlayVisible());

    int started = 0;
    int changed = 0;
    int finished = 0;
    std::array<double, 5> values{};
    canvas.setVignetteOverlayCallbacks(
        [&started] { ++started; },
        [&changed, &values](const double size,
                            const double midpoint,
                            const double centreX,
                            const double centreY,
                            const double rotation) {
            ++changed;
            values = {size, midpoint, centreX, centreY, rotation};
        },
        [&finished] { ++finished; });

    QWidget *viewport = canvas.viewport();
    // At 250%, the right-hand size handle lies 150 document pixels beyond
    // the image edge. It must remain visible and interactive in canvas void.
    const QPointF start = canvas.mapDocumentToViewport(QPointF(350.0, 50.0));
    const QPointF end = canvas.mapDocumentToViewport(QPointF(400.0, 50.0));
    QVERIFY(viewport->rect().contains(start.toPoint()));
    QVERIFY(!canvas.mapDocumentRectToViewport(QRectF(0.0, 0.0, 200.0, 100.0))
                 .contains(start));

    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start.toPoint());
    sendLeftDragMove(viewport, end);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, end.toPoint());

    QCOMPARE(started, 1);
    QVERIFY(changed >= 1);
    QCOMPARE(finished, 1);
    QVERIFY(std::abs(values[0] - 300.0) < 2.0);
    QVERIFY(std::abs(values[1] - 50.0) < 0.01);
    QVERIFY(std::abs(values[2]) < 0.01);
    QVERIFY(std::abs(values[3]) < 0.01);
    QVERIFY(std::abs(values[4]) < 0.01);
}

void CanvasTests::committedTransformMatchesLivePreview()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(256, 256);
    canvas.show();

    QImage background(128, 128, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(25, 50, 90, 255));
    QImage foreground(128, 128, QImage::Format_ARGB32_Premultiplied);
    foreground.fill(Qt::transparent);
    QPainter foregroundPainter(&foreground);
    foregroundPainter.fillRect(QRect(16, 48, 24, 24), QColor(230, 40, 30, 255));
    foregroundPainter.end();

    canvas.beginTiledPresentation(background, background.size(), 3);
    canvas.actualPixels();
    canvas.beginTransformPreview(background, foreground, QRectF(16, 48, 24, 24));
    QTransform translation;
    translation.translate(48.0, 0.0);
    canvas.updateTransformPreview(translation);
    QCoreApplication::processEvents();
    const QImage live = canvas.viewport()->grab().toImage();

    canvas.commitTransformPreview();
    QCoreApplication::processEvents();
    const QImage committed = canvas.viewport()->grab().toImage();
    QCOMPARE(committed.size(), live.size());
    for (int y = 0; y < live.height(); ++y) {
        for (int x = 0; x < live.width(); ++x) {
            QCOMPARE(committed.pixel(x, y), live.pixel(x, y));
        }
    }
}


void CanvasTests::transformPreviewBaseTransformRestoresOffCanvasPixels()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(256, 256);
    canvas.show();

    QImage background(128, 128, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(25, 50, 90, 255));
    QImage retainedForeground(128, 128,
                              QImage::Format_ARGB32_Premultiplied);
    retainedForeground.fill(Qt::transparent);
    QPainter foregroundPainter(&retainedForeground);
    foregroundPainter.fillRect(QRect(16, 48, 24, 24),
                               QColor(230, 40, 30, 255));
    foregroundPainter.end();

    canvas.beginTiledPresentation(background, background.size(), 5);
    canvas.actualPixels();

    // This is the state after one completed move placed the layer completely
    // beyond the right document edge. The cache must retain the original
    // foreground pixels and represent that completed move as a base transform,
    // rather than storing a document-clipped (therefore empty) image.
    QTransform completedMove;
    completedMove.translate(160.0, 0.0);
    canvas.beginTransformPreview(background,
                                 retainedForeground,
                                 QRectF(176.0, 48.0, 24.0, 24.0),
                                 completedMove);

    // A following gesture moves the still-intact source back into the canvas.
    QTransform returnMove;
    returnMove.translate(-128.0, 0.0);
    canvas.updateTransformPreview(returnMove);
    QCoreApplication::processEvents();

    const QImage live = canvas.viewport()->grab().toImage();
    const QPoint imageOrigin((live.width() - background.width()) / 2,
                             (live.height() - background.height()) / 2);
    const QColor restored = live.pixelColor(imageOrigin + QPoint(52, 60));
    QVERIFY(restored.red() > 200);
    QVERIFY(restored.green() < 80);
    QVERIFY(restored.blue() < 80);

    canvas.commitTransformPreview();
    QCoreApplication::processEvents();
    const QImage committed = canvas.viewport()->grab().toImage();
    QCOMPARE(committed, live);
}


void CanvasTests::transformPreviewBoundsRestoreBakedOffCanvasRotation()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(256, 256);
    canvas.show();

    QImage background(128, 128, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(25, 50, 90, 255));

    // A committed raster rotation is baked into a compact editable payload.
    // Its document bounds can lie wholly beyond the canvas while the payload
    // itself remains intact. The following transform must draw that compact
    // surface at its real document position rather than forcing it through a
    // document-sized image and losing every off-canvas pixel.
    QImage rotatedPayload(40, 40, QImage::Format_ARGB32_Premultiplied);
    rotatedPayload.fill(Qt::transparent);
    QPainter payloadPainter(&rotatedPayload);
    QPolygon diamond;
    diamond << QPoint(20, 1) << QPoint(39, 20)
            << QPoint(20, 39) << QPoint(1, 20);
    payloadPainter.setPen(Qt::NoPen);
    payloadPainter.setBrush(QColor(230, 40, 30, 255));
    payloadPainter.drawPolygon(diamond);
    payloadPainter.end();

    const QRectF offCanvasBounds(150.0, 44.0, 40.0, 40.0);
    canvas.beginTiledPresentation(background, background.size(), 6);
    canvas.actualPixels();
    canvas.beginTransformPreview(background, rotatedPayload,
                                 offCanvasBounds, QTransform(),
                                 offCanvasBounds);

    QTransform returnMove;
    returnMove.translate(-128.0, 0.0);
    canvas.updateTransformPreview(returnMove);
    QCoreApplication::processEvents();

    const QImage live = canvas.viewport()->grab().toImage();
    const QPoint imageOrigin((live.width() - background.width()) / 2,
                             (live.height() - background.height()) / 2);
    const QColor restored = live.pixelColor(imageOrigin + QPoint(42, 64));
    QVERIFY(restored.red() > 200);
    QVERIFY(restored.green() < 80);
    QVERIFY(restored.blue() < 80);

    canvas.commitTransformPreview();
    QCoreApplication::processEvents();
    QCOMPARE(canvas.viewport()->grab().toImage(), live);
}


void CanvasTests::pretransformedVectorForegroundCommitsWithoutDoubleTransform()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(256, 256);
    canvas.show();

    QImage background(128, 128, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(25, 50, 90, 255));
    QImage originalForeground(128, 128, QImage::Format_ARGB32_Premultiplied);
    originalForeground.fill(Qt::transparent);
    QPainter originalPainter(&originalForeground);
    originalPainter.fillRect(QRect(16, 48, 24, 24), QColor(230, 40, 30, 255));
    originalPainter.end();

    QImage semanticForeground(128, 128, QImage::Format_ARGB32_Premultiplied);
    semanticForeground.fill(Qt::transparent);
    QPainter semanticPainter(&semanticForeground);
    semanticPainter.fillRect(QRect(64, 48, 24, 24), QColor(230, 40, 30, 255));
    semanticPainter.end();

    canvas.beginTiledPresentation(background, background.size(), 4);
    canvas.actualPixels();
    canvas.beginTransformPreview(background,
                                 originalForeground,
                                 QRectF(16, 48, 24, 24));
    canvas.setTransformPreviewForeground(semanticForeground, true);
    QTransform translation;
    translation.translate(48.0, 0.0);
    canvas.updateTransformPreview(translation);
    QCoreApplication::processEvents();
    const QImage live = canvas.viewport()->grab().toImage();

    canvas.commitTransformPreview();
    QCoreApplication::processEvents();
    const QImage committed = canvas.viewport()->grab().toImage();
    QCOMPARE(committed, live);
}

namespace vfx {

class ImageCanvasTestPeer final
{
public:
    static void sendTransformButton(ImageCanvas &canvas,
                                    const QEvent::Type type,
                                    const QPointF &position,
                                    const Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        const QPoint globalPoint = canvas.viewport()->mapToGlobal(position.toPoint());
        const Qt::MouseButtons buttons = type == QEvent::MouseButtonPress
            ? Qt::LeftButton : Qt::NoButton;
        QMouseEvent event(type,
                          position,
                          position,
                          QPointF(globalPoint),
                          Qt::LeftButton,
                          buttons,
                          modifiers);
        if (type == QEvent::MouseButtonPress) {
            canvas.mousePressEvent(&event);
        } else {
            Q_ASSERT(type == QEvent::MouseButtonRelease);
            canvas.mouseReleaseEvent(&event);
        }
    }

    static void sendTransformMove(ImageCanvas &canvas,
                                  const QPointF &position,
                                  const Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        const QPoint globalPoint = canvas.viewport()->mapToGlobal(position.toPoint());
        QMouseEvent event(QEvent::MouseMove,
                          position,
                          position,
                          QPointF(globalPoint),
                          Qt::NoButton,
                          Qt::LeftButton,
                          modifiers);
        canvas.mouseMoveEvent(&event);
    }
};

} // namespace vfx

namespace {

void sendTransformMove(QWidget *viewport,
                       const QPointF &position,
                       const Qt::KeyboardModifiers modifiers)
{
    const QPoint globalPoint = viewport->mapToGlobal(position.toPoint());
    QMouseEvent moveEvent(QEvent::MouseMove,
                          position,
                          position,
                          QPointF(globalPoint),
                          Qt::NoButton,
                          Qt::LeftButton,
                          modifiers);
    QCoreApplication::sendEvent(viewport, &moveEvent);
}


} // namespace

void CanvasTests::transformStartForwardsMouseModifiers()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSelectionBounds(QRectF(20.0, 40.0, 40.0, 30.0));
    QCoreApplication::processEvents();

    int startedCount = 0;
    Qt::KeyboardModifiers capturedModifiers;
    connect(&canvas,
            &ImageCanvas::transformDragStarted,
            &canvas,
            [&startedCount, &capturedModifiers](CanvasTransformMode,
                                                const QPointF &,
                                                const Qt::KeyboardModifiers modifiers) {
                ++startedCount;
                capturedModifiers = modifiers;
            });
    QWidget *viewport = canvas.viewport();
    const QPoint inside(130, 105);
    QTest::mousePress(viewport, Qt::LeftButton, Qt::AltModifier, inside);
    QCOMPARE(startedCount, 1);
    QVERIFY(capturedModifiers.testFlag(Qt::AltModifier));
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::AltModifier, inside);
}

void CanvasTests::transformMoveSnapsAndControlBypasses()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSnappingEnabled(true);
    canvas.setTransformSnapDistance(8.0);
    const QRectF selection(20.0, 40.0, 40.0, 30.0);
    canvas.setTransformSelectionBounds(selection);
    canvas.setTransformSnapBounds({QRectF(130.0, 0.0, 20.0, 20.0)});
    QCoreApplication::processEvents();

    QSignalSpy changed(&canvas, &ImageCanvas::transformDragChanged);
    QWidget *viewport = canvas.viewport();
    const QPoint start(130, 105); // Inside the selection, away from the pivot handle.
    const QPointF nearTarget(199.0, 105.0); // Right edge lands just before the other layer at x=130.
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    sendTransformMove(viewport, nearTarget, Qt::NoModifier);
    QVERIFY(changed.count() >= 1);
    const QTransform snapped = qvariant_cast<QTransform>(changed.constLast().at(0));
    QVERIFY(std::abs(snapped.mapRect(selection).right() - 130.0) < 0.01);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, nearTarget.toPoint());

    changed.clear();
    canvas.setTransformSelectionBounds(selection);
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    sendTransformMove(viewport, nearTarget, Qt::ControlModifier);
    QVERIFY(changed.count() >= 1);
    const QTransform unsnapped = qvariant_cast<QTransform>(changed.constLast().at(0));
    QVERIFY(std::abs(unsnapped.mapRect(selection).right() - 130.0) > 0.25);
    QTest::mouseRelease(viewport,
                        Qt::LeftButton,
                        Qt::ControlModifier,
                        nearTarget.toPoint());
}

void CanvasTests::transformMoveUsesWholePixelLattice()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSnappingEnabled(true);
    canvas.setTransformSnapDistance(8.0);
    canvas.setGuidesVisible(true);
    canvas.setGuides({}, {25.5});
    const QRectF selection(20.25, 40.25, 40.0, 30.0);
    canvas.setTransformSelectionBounds(selection);
    canvas.setTransformSnapBounds({});
    QCoreApplication::processEvents();

    QSignalSpy changed(&canvas, &ImageCanvas::transformDragChanged);
    // Exercise the transform handler/math contract directly. QtTest's native
    // button injection is still covered by neighbouring tests, while these
    // fractional transform regressions must not depend on platform event
    // rounding in the Windows, X11 or Wayland plugins.
    // Keep the press clear of the half-pixel guide itself. Guide dragging has
    // a 7 px hit tolerance and intentionally precedes transform hit-testing;
    // the guide is a snap target here, not the gesture being exercised.
    const QPointF start = canvas.mapDocumentToViewport(QPointF(50.0, 50.0));
    const QPointF fractionalEnd =
        canvas.mapDocumentToViewport(QPointF(55.4, 53.6));

    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonPress, start);
    ImageCanvasTestPeer::sendTransformMove(canvas, fractionalEnd);
    QVERIFY(changed.count() >= 1);
    const QTransform snapped = qvariant_cast<QTransform>(changed.constLast().at(0));
    const QRectF snappedBounds = snapped.mapRect(selection);
    QVERIFY(isPixelBoundaryCoordinate(snappedBounds.left()));
    QVERIFY(isPixelBoundaryCoordinate(snappedBounds.top()));
    QVERIFY(std::abs(snappedBounds.left() - 26.0) < 0.01);
    QVERIFY(std::abs(snappedBounds.top() - 44.0) < 0.01);
    QVERIFY(std::abs(snapped.dx() - 5.75) < 0.01);
    QVERIFY(std::abs(snapped.dy() - 3.75) < 0.01);
    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonRelease, fractionalEnd);

    changed.clear();
    canvas.setTransformSelectionBounds(selection);
    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonPress, start);
    ImageCanvasTestPeer::sendTransformMove(canvas, fractionalEnd, Qt::ControlModifier);
    QVERIFY(changed.count() >= 1);
    const QTransform free = qvariant_cast<QTransform>(changed.constLast().at(0));
    QVERIFY(std::abs(free.dx() - 5.4) < 0.01);
    QVERIFY(std::abs(free.dy() - 3.6) < 0.01);
    const QRectF freeBounds = free.mapRect(selection);
    QVERIFY(!isPixelBoundaryCoordinate(freeBounds.left()));
    QVERIFY(!isPixelBoundaryCoordinate(freeBounds.top()));
    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonRelease, fractionalEnd,
                                     Qt::ControlModifier);
}

void CanvasTests::transformResizeSnapsDraggedEdge()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSnappingEnabled(true);
    canvas.setTransformSnapDistance(8.0);
    const QRectF selection(20.0, 40.0, 40.0, 30.0);
    canvas.setTransformSelectionBounds(selection);
    canvas.setTransformSnapBounds({QRectF(130.0, 0.0, 20.0, 20.0)});
    QCoreApplication::processEvents();

    QSignalSpy changed(&canvas, &ImageCanvas::transformDragChanged);
    QWidget *viewport = canvas.viewport();
    const QPoint rightHandle(160, 105);
    const QPointF nearTarget(229.0, 105.0);
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, rightHandle);
    sendTransformMove(viewport, nearTarget, Qt::NoModifier);
    QVERIFY(changed.count() >= 1);
    const QTransform snapped = qvariant_cast<QTransform>(changed.constLast().at(0));
    const QRectF resized = snapped.mapRect(selection);
    QVERIFY(std::abs(resized.left() - selection.left()) < 0.01);
    QVERIFY(std::abs(resized.right() - 130.0) < 1.0);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, nearTarget.toPoint());
}

void CanvasTests::transformResizeUsesPixelBoundaryLattice()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSnappingEnabled(true);
    canvas.setTransformSnapDistance(8.0);
    canvas.setGuidesVisible(true);
    canvas.setGuides({}, {73.5});
    const QRectF selection(20.0, 40.0, 40.0, 30.0);
    canvas.setTransformSelectionBounds(selection);
    canvas.setTransformSnapBounds({});
    QCoreApplication::processEvents();

    QSignalSpy changed(&canvas, &ImageCanvas::transformDragChanged);
    QWidget *viewport = canvas.viewport();
    const QPoint rightHandle = canvas.mapDocumentToViewport(QPointF(60.0, 55.0)).toPoint();
    const QPointF fractionalEnd = canvas.mapDocumentToViewport(QPointF(73.4, 55.0));
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, rightHandle);
    sendTransformMove(viewport, fractionalEnd, Qt::NoModifier);
    QVERIFY(changed.count() >= 1);
    const QTransform snapped = qvariant_cast<QTransform>(changed.constLast().at(0));
    const QRectF resized = snapped.mapRect(selection);
    QVERIFY(std::abs(resized.left() - selection.left()) < 0.01);
    QVERIFY(isPixelBoundaryCoordinate(resized.right()));
    QVERIFY(std::abs(resized.right() - 73.0) < 0.01);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier,
                        fractionalEnd.toPoint());
}

void CanvasTests::persistentTransformGesturesCompose()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSnappingEnabled(false);
    canvas.setTransformSelectionBounds(QRectF(20.0, 40.0, 40.0, 30.0));
    QCoreApplication::processEvents();

    const QPointF firstStart =
        canvas.mapDocumentToViewport(QPointF(30.0, 48.0));
    const QPointF firstEnd =
        canvas.mapDocumentToViewport(QPointF(50.0, 48.0));
    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonPress, firstStart);
    ImageCanvasTestPeer::sendTransformMove(canvas, firstEnd);
    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonRelease, firstEnd);

    const QPointF secondStart =
        canvas.mapDocumentToViewport(QPointF(50.0, 48.0));
    const QPointF secondEnd =
        canvas.mapDocumentToViewport(QPointF(60.0, 58.0));
    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonPress, secondStart);
    ImageCanvasTestPeer::sendTransformMove(canvas, secondEnd);
    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonRelease, secondEnd);

    const QTransform accumulated = canvas.transformSessionTransform();
    QVERIFY(std::abs(accumulated.dx() - 30.0) < 0.01);
    QVERIFY(std::abs(accumulated.dy() - 10.0) < 0.01);
}

void CanvasTests::transformPivotMovesWithoutChangingSessionMatrix()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSelectionBounds(QRectF(20.0, 40.0, 40.0, 30.0));
    QCoreApplication::processEvents();

    QSignalSpy pivotChanged(&canvas, &ImageCanvas::transformPivotChanged);
    const QPointF pivotStart =
        canvas.mapDocumentToViewport(QPointF(40.0, 55.0));
    const QPointF pivotEnd =
        canvas.mapDocumentToViewport(QPointF(50.0, 65.0));
    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonPress, pivotStart);
    ImageCanvasTestPeer::sendTransformMove(canvas, pivotEnd);
    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonRelease, pivotEnd);

    QVERIFY(pivotChanged.count() >= 1);
    const QPointF pivot = canvas.transformPivot();
    QVERIFY(std::abs(pivot.x() - 50.0) < 0.01);
    QVERIFY(std::abs(pivot.y() - 65.0) < 0.01);
    QVERIFY(canvas.transformSessionTransform().isIdentity());
}

void CanvasTests::transformScaleMovesPivotWithContent()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSnappingEnabled(false);
    canvas.setTransformSelectionBounds(QRectF(20.0, 40.0, 40.0, 30.0));
    QCoreApplication::processEvents();

    const QPointF scaleStart =
        canvas.mapDocumentToViewport(QPointF(60.0, 55.0));
    const QPointF scaleEnd =
        canvas.mapDocumentToViewport(QPointF(80.0, 55.0));
    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonPress, scaleStart);
    ImageCanvasTestPeer::sendTransformMove(canvas, scaleEnd);
    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonRelease, scaleEnd);

    const QRectF scaled = canvas.transformSessionTransform().mapRect(
        QRectF(20.0, 40.0, 40.0, 30.0));
    QVERIFY(std::abs(scaled.width() - 60.0) < 0.01);
    QVERIFY(std::abs(canvas.transformPivot().x() - 50.0) < 0.01);
    QVERIFY(std::abs(canvas.transformPivot().y() - 55.0) < 0.01);
}

void CanvasTests::transformModesRestrictHandlesWithoutResettingSession()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSnappingEnabled(false);
    const QRectF selection(20.0, 40.0, 40.0, 30.0);
    canvas.setTransformSelectionBounds(selection);
    QCoreApplication::processEvents();

    canvas.setTransformInteractionMode(CanvasTransformInteractionMode::Rotate);
    const QPointF moveStart =
        canvas.mapDocumentToViewport(QPointF(58.0, 55.0));
    const QPointF moveEnd =
        canvas.mapDocumentToViewport(QPointF(68.0, 55.0));
    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonPress, moveStart);
    ImageCanvasTestPeer::sendTransformMove(canvas, moveEnd);
    ImageCanvasTestPeer::sendTransformButton(canvas, QEvent::MouseButtonRelease, moveEnd);
    const QTransform moved = canvas.transformSessionTransform();
    QVERIFY(std::abs(moved.mapRect(selection).width() - selection.width()) < 0.01);
    QVERIFY(std::abs(moved.dx() - 10.0) < 0.01);

    canvas.setTransformInteractionMode(CanvasTransformInteractionMode::Scale);
    QSignalSpy started(&canvas, &ImageCanvas::transformDragStarted);
    // The rotation handle is 28 viewport pixels above the transformed top edge.
    const QPoint rotationHandle =
        canvas.mapDocumentToViewport(QPointF(50.0, 12.0)).toPoint();
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier,
                      rotationHandle);
    QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier,
                        rotationHandle);
    QCOMPARE(started.count(), 0);
    QCOMPARE(canvas.transformSessionTransform(), moved);
}

void CanvasTests::skewMovesOnlyTheSelectedEdge()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSnappingEnabled(false);
    const QRectF selection(20.0, 40.0, 40.0, 30.0);
    canvas.setTransformSelectionBounds(selection);
    canvas.setTransformInteractionMode(CanvasTransformInteractionMode::Skew);
    QCoreApplication::processEvents();

    QWidget *viewport = canvas.viewport();
    // The image is centred at (100, 50); the top midpoint is (140, 90).
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(140, 90));
    sendLeftDragMove(viewport, QPointF(150.0, 90.0));
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(150, 90));

    const QTransform transform = canvas.transformSessionTransform();
    const QPointF topLeft = transform.map(selection.topLeft());
    const QPointF topRight = transform.map(selection.topRight());
    const QPointF bottomRight = transform.map(selection.bottomRight());
    const QPointF bottomLeft = transform.map(selection.bottomLeft());
    QVERIFY(std::abs(topLeft.x() - 30.0) < 0.05);
    QVERIFY(std::abs(topRight.x() - 70.0) < 0.05);
    QVERIFY(std::abs(bottomLeft.x() - 20.0) < 0.05);
    QVERIFY(std::abs(bottomRight.x() - 60.0) < 0.05);
}

void CanvasTests::distortMovesOneCornerIndependently()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSnappingEnabled(false);
    const QRectF selection(20.0, 40.0, 40.0, 30.0);
    canvas.setTransformSelectionBounds(selection);
    canvas.setTransformInteractionMode(CanvasTransformInteractionMode::Distort);
    QCoreApplication::processEvents();

    QSignalSpy selected(&canvas, &ImageCanvas::transformControlPointSelected);
    QWidget *viewport = canvas.viewport();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(120, 90));
    sendLeftDragMove(viewport, QPointF(110.0, 80.0));
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(110, 80));

    QCOMPARE(selected.count(), 1);
    QCOMPARE(selected.first().first().toInt(), 0);
    const QTransform transform = canvas.transformSessionTransform();
    QVERIFY(QLineF(transform.map(selection.topLeft()), QPointF(10.0, 30.0)).length() < 0.1);
    QVERIFY(QLineF(transform.map(selection.topRight()), selection.topRight()).length() < 0.1);
    QVERIFY(QLineF(transform.map(selection.bottomRight()), selection.bottomRight()).length() < 0.1);
    QVERIFY(QLineF(transform.map(selection.bottomLeft()), selection.bottomLeft()).length() < 0.1);
    QVERIFY(transform.type() == QTransform::TxProject);
}

void CanvasTests::distortRejectsCrossedQuadrilateral()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSnappingEnabled(false);
    const QRectF selection(20.0, 40.0, 40.0, 30.0);
    canvas.setTransformSelectionBounds(selection);
    canvas.setTransformInteractionMode(CanvasTransformInteractionMode::Distort);
    QCoreApplication::processEvents();

    QWidget *viewport = canvas.viewport();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(120, 90));
    // This would move the top-left point beyond the diagonal corner and
    // produce a crossed/inverted quadrilateral. The session must retain its
    // last valid matrix instead.
    sendLeftDragMove(viewport, QPointF(175.0, 130.0));
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(175, 130));

    QCOMPARE(canvas.transformSessionTransform(), QTransform());
    QVERIFY(QLineF(canvas.transformPivot(), selection.center()).length() < 0.01);
}

void CanvasTests::perspectiveCouplesAdjacentCorners()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSnappingEnabled(false);
    const QRectF selection(20.0, 40.0, 40.0, 30.0);
    canvas.setTransformSelectionBounds(selection);
    canvas.setTransformInteractionMode(CanvasTransformInteractionMode::Perspective);
    QCoreApplication::processEvents();

    QWidget *viewport = canvas.viewport();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(120, 90));
    sendLeftDragMove(viewport, QPointF(125.0, 90.0));
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(125, 90));

    const QTransform transform = canvas.transformSessionTransform();
    QVERIFY(QLineF(transform.map(selection.topLeft()), QPointF(25.0, 40.0)).length() < 0.1);
    QVERIFY(QLineF(transform.map(selection.topRight()), QPointF(55.0, 40.0)).length() < 0.1);
    QVERIFY(QLineF(transform.map(selection.bottomLeft()), QPointF(25.0, 70.0)).length() < 0.1);
    QVERIFY(QLineF(transform.map(selection.bottomRight()), selection.bottomRight()).length() < 0.1);
    QVERIFY(transform.type() == QTransform::TxProject);
}

void CanvasTests::transformContextMenuIsAvailableInsideBox()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSelectionBounds(QRectF(20.0, 40.0, 40.0, 30.0));
    QCoreApplication::processEvents();

    QSignalSpy requested(&canvas, &ImageCanvas::transformContextMenuRequested);
    QWidget *viewport = canvas.viewport();
    QTest::mouseClick(viewport, Qt::RightButton, Qt::NoModifier, QPoint(130, 100));
    QCOMPARE(requested.count(), 1);

    QTest::mouseClick(viewport, Qt::RightButton, Qt::NoModifier, QPoint(20, 20));
    QCOMPARE(requested.count(), 1);
}

void CanvasTests::transformPendingStateChangesBoundsAccent()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSelectionBounds(QRectF(20.0, 40.0, 40.0, 30.0));
    QCoreApplication::processEvents();

    // The top-centre scale handle is centred at viewport coordinate (140, 90).
    const QColor idle = canvas.viewport()->grab().toImage().pixelColor(140, 90);
    QVERIFY(idle.red() > 235);
    QVERIFY(idle.green() > 235);
    QVERIFY(idle.blue() > 235);

    canvas.setTransformPendingChanges(true);
    QCoreApplication::processEvents();
    const QColor pending = canvas.viewport()->grab().toImage().pixelColor(140, 90);
    QVERIFY(pending.red() > 220);
    QVERIFY(pending.green() > 140 && pending.green() < 220);
    QVERIFY(pending.blue() < 140);

    canvas.setTransformPendingChanges(false);
    QCoreApplication::processEvents();
    const QColor restored = canvas.viewport()->grab().toImage().pixelColor(140, 90);
    QVERIFY(restored.red() > 235);
    QVERIFY(restored.green() > 235);
    QVERIFY(restored.blue() > 235);
}

void CanvasTests::transformDoubleClickAppliesOnlyPendingInterior()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(400, 300);
    canvas.show();

    QImage image(200, 200, QImage::Format_RGBA8888);
    image.fill(QColor(40, 60, 80, 255));
    canvas.setImage(image, image.size());
    canvas.actualPixels();
    canvas.setTransformDragEnabled(true);
    canvas.setTransformSelectionBounds(QRectF(20.0, 40.0, 40.0, 30.0));
    QCoreApplication::processEvents();

    QSignalSpy applyRequested(&canvas, &ImageCanvas::transformApplyRequested);
    QWidget *viewport = canvas.viewport();

    QVERIFY(!canvas.transformPendingChanges());
    QTest::mouseDClick(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(130, 100));
    QCOMPARE(applyRequested.count(), 0);

    canvas.setTransformPendingChanges(true);
    QVERIFY(canvas.transformPendingChanges());
    QTest::mouseDClick(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(130, 100));
    QCOMPARE(applyRequested.count(), 1);

    // A handle and the pivot remain editing targets rather than accidental Apply targets.
    QTest::mouseDClick(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(120, 90));
    QCOMPARE(applyRequested.count(), 1);
    QTest::mouseDClick(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(140, 105));
    QCOMPARE(applyRequested.count(), 1);

    QTest::mouseDClick(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QCOMPARE(applyRequested.count(), 1);
}

void CanvasTests::viewportPublishesSettledRequest()
{
    ImageCanvas canvas;
    canvas.resize(640, 480);
    canvas.show();
    QImage fallback(1024, 768, QImage::Format_RGBA8888);
    fallback.fill(Qt::black);
    canvas.beginTiledPresentation(fallback, fallback.size(), 1);

    QSignalSpy spy(&canvas, &ImageCanvas::presentationViewportChanged);
    canvas.setZoom(1.5);
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 250);
    const auto sawSettled = [&spy] {
        for (const QList<QVariant> &arguments : spy) {
            if (arguments.size() >= 3 && arguments.at(2).toBool()) {
                return true;
            }
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(sawSettled(), 500);
}

void CanvasTests::highZoomRenderingIsViewportBounded()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(640, 480);
    canvas.show();

    QImage fallback(4096, 4096, QImage::Format_ARGB32_Premultiplied);
    fallback.fill(Qt::transparent);
    canvas.beginTiledPresentation(fallback, fallback.size(), 21);
    canvas.setZoom(32.0, QPointF(320.0, 240.0));

    // Rendering cost must be tied to the viewport, not the 131072x131072
    // device-space image rectangle produced at 3200% zoom. The old checker
    // loop attempted roughly 67 million fill operations for this frame.
    QElapsedTimer timer;
    timer.start();
    const QImage frame = canvas.viewport()->grab().toImage();
    const qint64 elapsed = timer.elapsed();
    QVERIFY(!frame.isNull());
    QVERIFY2(elapsed < 2000,
             qPrintable(QStringLiteral("3200% viewport repaint took %1 ms").arg(elapsed)));
}

void CanvasTests::redoShortcutsAreUniqueAndPortable()
{
    const QList<QKeySequence> shortcuts = redoKeySequences();
    QVERIFY(shortcuts.contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)));
    QVERIFY(shortcuts.contains(QKeySequence(Qt::CTRL | Qt::Key_Y)));

    for (int i = 0; i < shortcuts.size(); ++i) {
        QVERIFY(!shortcuts.at(i).isEmpty());
        QCOMPARE(shortcuts.indexOf(shortcuts.at(i)), i);
    }
}



void CanvasTests::layerTreeCtrlClickThumbnailPreservesLayerSelection()
{
    LayerTreeWidget tree;
    tree.setColumnCount(3);
    tree.header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree.header()->setSectionResizeMode(1, QHeaderView::Fixed);
    tree.header()->setSectionResizeMode(2, QHeaderView::Fixed);
    tree.setColumnWidth(1, 34);
    tree.setColumnWidth(2, 34);
    tree.resize(320, 100);

    const QUuid layerId = QUuid::createUuid();
    auto *item = new QTreeWidgetItem(&tree);
    item->setText(0, QStringLiteral("Layer"));
    item->setData(0, Qt::UserRole + 1, layerId.toString(QUuid::WithoutBraces));
    item->setSelected(true);
    tree.setCurrentItem(item);

    const QUuid otherLayerId = QUuid::createUuid();
    auto *otherItem = new QTreeWidgetItem(&tree);
    otherItem->setText(0, QStringLiteral("Other Layer"));
    otherItem->setData(0, Qt::UserRole + 1,
                       otherLayerId.toString(QUuid::WithoutBraces));
    tree.show();
    QTest::qWait(20);

    QSignalSpy spy(&tree, &LayerTreeWidget::thumbnailSelectionRequested);
    const QRect otherRow = tree.visualItemRect(otherItem);
    const QPoint pixelPoint(tree.header()->sectionPosition(1)
                                + tree.header()->sectionSize(1) / 2,
                            otherRow.center().y());
    QTest::mouseClick(tree.viewport(), Qt::LeftButton,
                      Qt::ControlModifier, pixelPoint);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toUuid(), otherLayerId);
    QCOMPARE(spy.at(0).at(1).toBool(), false);
    QVERIFY(item->isSelected());
    QVERIFY(!otherItem->isSelected());
    QCOMPARE(tree.currentItem(), item);

    const QRect row = tree.visualItemRect(item);
    const QPoint maskPoint(tree.header()->sectionPosition(2)
                               + tree.header()->sectionSize(2) / 2,
                           row.center().y());
    QTest::mouseClick(tree.viewport(), Qt::LeftButton,
                      Qt::ControlModifier, maskPoint);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toUuid(), layerId);
    QCOMPARE(spy.at(1).at(1).toBool(), true);
    QVERIFY(item->isSelected());
    QCOMPARE(tree.currentItem(), item);
}

void CanvasTests::layerTreeRightClickPreservesExistingMultiSelection()
{
    LayerTreeWidget tree;
    tree.setColumnCount(1);
    tree.resize(280, 140);

    auto *first = new QTreeWidgetItem(&tree);
    first->setText(0, QStringLiteral("First"));
    auto *second = new QTreeWidgetItem(&tree);
    second->setText(0, QStringLiteral("Second"));
    auto *third = new QTreeWidgetItem(&tree);
    third->setText(0, QStringLiteral("Third"));

    first->setSelected(true);
    second->setSelected(true);
    tree.setCurrentItem(first, 0, QItemSelectionModel::NoUpdate);
    tree.show();
    QTest::qWait(20);

    const QPoint secondPoint = tree.visualItemRect(second).center();
    QTest::mousePress(tree.viewport(), Qt::RightButton,
                      Qt::NoModifier, secondPoint);
    QVERIFY(first->isSelected());
    QVERIFY(second->isSelected());
    QVERIFY(!third->isSelected());
    QCOMPARE(tree.currentItem(), second);
    QTest::mouseRelease(tree.viewport(), Qt::RightButton,
                        Qt::NoModifier, secondPoint);

    // A context click outside the current selection keeps ordinary tree
    // behaviour: the clicked row becomes the sole selection.
    const QPoint thirdPoint = tree.visualItemRect(third).center();
    QTest::mouseClick(tree.viewport(), Qt::RightButton,
                      Qt::NoModifier, thirdPoint);
    QVERIFY(!first->isSelected());
    QVERIFY(!second->isSelected());
    QVERIFY(third->isSelected());
    QCOMPARE(tree.currentItem(), third);
}

void CanvasTests::documentStripHandlesLargeModelAndActivation()
{
    DocumentStripWidget strip;
    strip.resize(620, 111);
    strip.show();

    QVector<QUuid> ids;
    ids.reserve(250);
    QImage thumbnail(128, 80, QImage::Format_RGBA8888);
    thumbnail.fill(QColor(30, 50, 70, 255));
    for (int index = 0; index < 250; ++index) {
        const QUuid id = QUuid::createUuid();
        ids.push_back(id);
        strip.upsertDocument(id,
                             QStringLiteral("Document %1").arg(index + 1),
                             thumbnail,
                             index % 3 == 0,
                             index == 0 ? SessionResidency::Hot
                                        : index < 5 ? SessionResidency::Warm
                                                    : SessionResidency::Cold,
                             QSize(4096, 2160));
    }
    QCoreApplication::processEvents();

    QCOMPARE(strip.documentCount(), 250);
    strip.setActiveDocument(ids.at(173));
    QCOMPARE(strip.activeDocument(), ids.at(173));

    QListView *view = strip.findChild<QListView *>(QStringLiteral("DocumentStripView"));
    QVERIFY(view);
    QTRY_VERIFY_WITH_TIMEOUT(view->horizontalScrollBar()->maximum() > 0, 1000);

    QSignalSpy activationSpy(&strip, &DocumentStripWidget::documentActivated);
    const QModelIndex firstIndex = view->model()->index(0, 0);
    view->scrollTo(firstIndex, QAbstractItemView::EnsureVisible);
    view->setCurrentIndex(firstIndex);
    view->setFocus();
    QCoreApplication::processEvents();
    // Keyboard activation is deterministic on headless/offscreen Windows and
    // exercises the same DocumentStrip activation callback as a card click.
    QTest::keyClick(view, Qt::Key_Return);
    QCOMPARE(activationSpy.count(), 1);
    QCOMPARE(activationSpy.takeFirst().at(0).toUuid(), ids.constFirst());

    strip.removeDocument(ids.at(10));
    QCOMPARE(strip.documentCount(), 249);
    strip.clearDocuments();
    QCOMPARE(strip.documentCount(), 0);
    QVERIFY(!strip.isVisible());
}


void CanvasTests::programmaticProgressCloseKeepsCompletedCanvasResultValid()
{
    QProgressDialog progress(QStringLiteral("Preparing Canvas Size…"),
                             QStringLiteral("Cancel"),
                             0,
                             0);
    progress.setMinimumDuration(0);
    progress.show();
    QCoreApplication::processEvents();

    QSignalSpy cancelledSpy(&progress, &QProgressDialog::canceled);
    {
        const QSignalBlocker blocker(&progress);
        progress.close();
    }
    QCoreApplication::processEvents();

    QCOMPARE(cancelledSpy.count(), 0);
    QVERIFY(!progress.isVisible());
}


void CanvasTests::gradientBarEndsDragWhenMouseGrabIsLost()
{
    GradientBarWidget bar;
    GradientMapParameters parameters;
    parameters.stops = {
        {0.0, QColor(0, 0, 0)},
        {0.5, QColor(128, 64, 32)},
        {1.0, QColor(255, 255, 255)}
    };
    parameters.normalise();
    bar.setParameters(parameters);
    bar.resize(300, 72);
    bar.show();
    QCoreApplication::processEvents();

    QSignalSpy started(&bar, &GradientBarWidget::interactionStarted);
    QSignalSpy finished(&bar, &GradientBarWidget::interactionFinished);
    QTest::mousePress(&bar, Qt::LeftButton, Qt::NoModifier, QPoint(150, 58));
    QCOMPARE(started.count(), 1);

    QEvent ungrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(&bar, &ungrab);
    QCOMPARE(finished.count(), 1);
}

void CanvasTests::curvesEyedropperAddsStableSamplePoint()
{
    CurvesEditorWidget editor;
    CurvesParameters parameters;
    editor.setCurves(parameters);

    QSignalSpy started(&editor, &CurvesEditorWidget::interactionStarted);
    QSignalSpy changed(&editor, &CurvesEditorWidget::curvesChanged);
    QSignalSpy finished(&editor, &CurvesEditorWidget::interactionFinished);
    editor.applyEyedropperSample(QColor(255, 0, 0));

    QCOMPARE(started.count(), 1);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(finished.count(), 1);
    const CurvesParameters sampled = editor.curves();
    const CurveChannelParameters &master = sampled.channel(AdjustmentChannel::Rgb);
    QCOMPARE(master.points.size(), 3);
    const double expectedInput = 0.2126;
    QVERIFY(std::abs(master.points[1].input - expectedInput) < 1.0e-6);
    QVERIFY(std::abs(master.points[1].output - expectedInput) < 1.0e-6);

    // Sampling the same input selects the existing point rather than creating
    // another history entry or an almost-duplicate control point.
    editor.applyEyedropperSample(QColor(255, 0, 0));
    QCOMPARE(editor.curves().channel(AdjustmentChannel::Rgb).points.size(), 3);
    QCOMPARE(started.count(), 1);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(finished.count(), 1);
}

void CanvasTests::gradientStopUtilitiesPreserveOrderAndColours()
{
    GradientBarWidget bar;
    GradientMapParameters parameters;
    parameters.stops = {
        {0.0, QColor(10, 20, 30)},
        {0.2, QColor(60, 70, 80)},
        {0.83, QColor(120, 130, 140)},
        {1.0, QColor(240, 245, 250)}
    };
    parameters.normalise();
    bar.setParameters(parameters);

    QSignalSpy started(&bar, &GradientBarWidget::interactionStarted);
    QSignalSpy finished(&bar, &GradientBarWidget::interactionFinished);
    bar.duplicateSelectedStop();
    QCOMPARE(bar.parameters().stops.size(), 5);
    QCOMPARE(bar.parameters().stops[1].colour, QColor(10, 20, 30));

    const QVector<QColor> coloursBefore = [&bar] {
        QVector<QColor> colours;
        for (const GradientStop &stop : bar.parameters().stops) {
            colours.push_back(stop.colour);
        }
        return colours;
    }();
    bar.distributeStopsEvenly();
    const GradientMapParameters distributed = bar.parameters();
    QCOMPARE(distributed.stops.size(), 5);
    for (int index = 0; index < distributed.stops.size(); ++index) {
        QVERIFY(std::abs(distributed.stops[index].position
                         - index / 4.0) < 1.0e-9);
        QCOMPARE(distributed.stops[index].colour, coloursBefore[index]);
    }
    QCOMPARE(started.count(), 2);
    QCOMPARE(finished.count(), 2);
}

void CanvasTests::combinedSliderFieldScrubsAndKeepsClicksEditable()
{
    SliderSpinBox control;
    control.configure(0.0, 100.0, 1.0, 0);
    control.setValue(50.0);
    control.resize(180, 30);
    control.show();
    QCoreApplication::processEvents();

    QDoubleSpinBox *spin = control.spinBox();
    QVERIFY(spin);
    QCOMPARE(control.minimumHeight(), 30);
    QCOMPARE(control.maximumHeight(), 30);
    QCOMPARE(spin->minimumHeight(), 30);
    QCOMPARE(spin->maximumHeight(), 30);
    QLineEdit *editor = spin->findChild<QLineEdit *>();
    QVERIFY(editor);

    QSignalSpy started(&control, &SliderSpinBox::interactionStarted);
    QSignalSpy finished(&control, &SliderSpinBox::interactionFinished);
    QSignalSpy changed(&control, &SliderSpinBox::valueChanged);

    const QPoint pressPoint(editor->rect().center());
    QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier, pressPoint);
    const QPoint movedPoint = pressPoint + QPoint(24, 0);
    QMouseEvent move(QEvent::MouseMove,
                     QPointF(movedPoint),
                     QPointF(movedPoint),
                     QPointF(editor->mapToGlobal(movedPoint)),
                     Qt::NoButton,
                     Qt::LeftButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(editor, &move);
    QTest::mouseRelease(editor, Qt::LeftButton, Qt::NoModifier, movedPoint);

    QCOMPARE(control.value(), 74.0);
    QCOMPARE(started.count(), 1);
    QCOMPARE(finished.count(), 1);
    QVERIFY(changed.count() >= 1);

    control.setValue(50.0);
    QTest::mousePress(editor, Qt::LeftButton, Qt::ShiftModifier, pressPoint);
    const QPoint finePoint = pressPoint + QPoint(20, 0);
    QMouseEvent fineMove(QEvent::MouseMove,
                         QPointF(finePoint), QPointF(finePoint),
                         QPointF(editor->mapToGlobal(finePoint)),
                         Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
    QCoreApplication::sendEvent(editor, &fineMove);
    QTest::mouseRelease(editor, Qt::LeftButton, Qt::ShiftModifier, finePoint);
    QCOMPARE(control.value(), 52.0);

    control.setValue(50.0);
    QTest::mousePress(editor, Qt::LeftButton, Qt::ControlModifier, pressPoint);
    const QPoint coarsePoint = pressPoint + QPoint(12, 0);
    QMouseEvent coarseMove(QEvent::MouseMove,
                           QPointF(coarsePoint), QPointF(coarsePoint),
                           QPointF(editor->mapToGlobal(coarsePoint)),
                           Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
    QCoreApplication::sendEvent(editor, &coarseMove);
    QTest::mouseRelease(editor, Qt::LeftButton, Qt::ControlModifier, coarsePoint);
    QCOMPARE(control.value(), 100.0);
    QCOMPARE(started.count(), 3);
    QCOMPARE(finished.count(), 3);

    const double afterScrub = control.value();
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, pressPoint);
    QCOMPARE(control.value(), afterScrub);
    QVERIFY(editor->hasFocus());

    QTest::mouseDClick(editor, Qt::LeftButton, Qt::NoModifier, pressPoint);
    QVERIFY(!editor->selectedText().isEmpty());
}


void CanvasTests::combinedSliderFieldUsesStyleAlignedToolbarHeight()
{
    QToolBar toolBar;
    toolBar.setObjectName(QStringLiteral("ToolOptionsToolbar"));
    toolBar.setPalette(applicationPalette());
    toolBar.setStyleSheet(applicationStyleSheet());
    toolBar.setFixedHeight(44);

    auto *control = new SliderSpinBox(&toolBar);
    control->setObjectName(QStringLiteral("CompactScrubField"));
    control->setControlHeight(28);
    control->configure(0.0, 100.0, 1.0, 0);
    control->setValue(0.0);
    toolBar.addWidget(control);
    toolBar.resize(320, 44);
    toolBar.show();
    QCoreApplication::processEvents();

    QCOMPARE(control->minimumHeight(), 28);
    QCOMPARE(control->maximumHeight(), 28);
    QCOMPARE(control->spinBox()->minimumHeight(), 28);
    QCOMPARE(control->spinBox()->maximumHeight(), 28);
    QVERIFY(control->geometry().top() >= toolBar.rect().top());
    QVERIFY(control->geometry().bottom() < toolBar.rect().bottom());

    // Render the real application style, not just the layout geometry. Qt's
    // QSS min/max-height values describe the content box, so a 28 px styled
    // content height plus the two 1 px borders silently clipped the lower
    // frame inside this fixed 28 px widget. The centre of the final scanline
    // must now be the theme border rather than the input background.
    QImage rendered(control->size(), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    control->render(&rendered);
    const QColor bottomCentre = rendered.pixelColor(rendered.width() / 2,
                                                     rendered.height() - 1);
    const QColor border = themeColour(QStringLiteral("border"));
    const QColor accent = themeColour(QStringLiteral("accent"));
    QVERIFY2(bottomCentre == border || bottomCentre == accent,
             qPrintable(QStringLiteral("Unexpected bottom frame colour %1")
                            .arg(bottomCentre.name(QColor::HexArgb))));
}

void CanvasTests::gradientColourButtonUsesContainedIconSwatch()
{
    GradientMapEditorWidget editor;
    editor.resize(340, 220);
    editor.show();
    QCoreApplication::processEvents();

    QPushButton *colourButton = nullptr;
    const auto buttons = editor.findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (button->text().contains(QStringLiteral("Stop Colour"))) {
            colourButton = button;
            break;
        }
    }
    QVERIFY(colourButton);
    QVERIFY(!colourButton->icon().isNull());
    QCOMPARE(colourButton->iconSize(), QSize(16, 16));
    QVERIFY(colourButton->styleSheet().isEmpty());
}

void CanvasTests::vectorPathEditingPublishesPointerGestures()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(420, 300);
    QImage image(240, 180, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    canvas.setImage(image, image.size());
    canvas.setZoom(1.0);
    canvas.setVectorPathEditingEnabled(true);
    QVector<CanvasVectorPathNode> nodes(2);
    nodes[0].anchor = QPointF(40.0, 60.0);
    nodes[0].outHandle = QPointF(75.0, 25.0);
    nodes[0].outHandleActive = true;
    nodes[1].anchor = QPointF(170.0, 110.0);
    nodes[1].inHandle = QPointF(130.0, 145.0);
    nodes[1].inHandleActive = true;
    canvas.setVectorPathOverlay(nodes, false, QSet<int> {0}, 0);
    canvas.show();
    QCoreApplication::processEvents();

    QSignalSpy pressed(&canvas, &ImageCanvas::vectorPathPointerPressed);
    QSignalSpy moved(&canvas, &ImageCanvas::vectorPathPointerMoved);
    QSignalSpy released(&canvas, &ImageCanvas::vectorPathPointerReleased);
    QSignalSpy doubled(&canvas, &ImageCanvas::vectorPathPointerDoubleClicked);
    QWidget *viewport = canvas.viewport();
    const QPoint start = canvas.mapDocumentToViewport(QPointF(40.0, 60.0)).toPoint();
    const QPoint finish = canvas.mapDocumentToViewport(QPointF(65.0, 80.0)).toPoint();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::ShiftModifier, start);
    sendLeftDragMove(viewport, finish, Qt::ShiftModifier);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::ShiftModifier, finish);
    QCOMPARE(pressed.count(), 1);
    QVERIFY(moved.count() >= 1);
    QCOMPARE(released.count(), 1);
    QTest::mouseDClick(viewport, Qt::LeftButton, Qt::NoModifier, start);
    QCOMPARE(doubled.count(), 1);

    canvas.setVectorPathEditingEnabled(false);
    QVERIFY(!canvas.vectorPathEditingEnabled());
}


void CanvasTests::vectorHoverFeedbackUpdatesCursorAndOverlay()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(420, 300);
    QImage image(240, 180, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    canvas.setImage(image, image.size());
    canvas.setZoom(1.0);
    canvas.setToolCursor(Qt::ArrowCursor);
    canvas.setVectorPathEditingEnabled(true);

    QVector<CanvasVectorPathNode> nodes(2);
    nodes[0].anchor = QPointF(45.0, 70.0);
    nodes[0].outHandle = QPointF(80.0, 28.0);
    nodes[0].outHandleActive = true;
    nodes[1].anchor = QPointF(180.0, 112.0);
    nodes[1].inHandle = QPointF(136.0, 150.0);
    nodes[1].inHandleActive = true;

    CanvasVectorHover anchorHover;
    anchorHover.part = CanvasVectorHoverPart::Anchor;
    anchorHover.nodeIndex = 0;
    canvas.setVectorPathOverlay(nodes, false, QSet<int> {0}, -1,
                                {}, false, anchorHover);
    canvas.show();
    QCoreApplication::processEvents();
    QCOMPARE(canvas.viewport()->cursor().shape(), Qt::SizeAllCursor);

    QImage anchorFrame(canvas.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    anchorFrame.fill(Qt::transparent);
    canvas.viewport()->render(&anchorFrame);

    CanvasVectorHover handleHover;
    handleHover.part = CanvasVectorHoverPart::OutHandle;
    handleHover.nodeIndex = 0;
    canvas.setVectorPathOverlay(nodes, false, QSet<int> {0}, -1,
                                {}, false, handleHover);
    QCOMPARE(canvas.viewport()->cursor().shape(), Qt::PointingHandCursor);
    QImage handleFrame(canvas.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    handleFrame.fill(Qt::transparent);
    canvas.viewport()->render(&handleFrame);
    QVERIFY(anchorFrame != handleFrame);

    CanvasVectorHover segmentHover;
    segmentHover.part = CanvasVectorHoverPart::Segment;
    segmentHover.segmentIndex = 0;
    segmentHover.segmentPath.moveTo(nodes[0].anchor);
    segmentHover.segmentPath.cubicTo(nodes[0].outHandle,
                                    nodes[1].inHandle,
                                    nodes[1].anchor);
    canvas.setVectorPathOverlay(nodes, false, QSet<int> {0}, -1,
                                {}, false, segmentHover);
    QCOMPARE(canvas.viewport()->cursor().shape(), Qt::CrossCursor);

    CanvasVectorEndpointMarker endpoint;
    endpoint.position = nodes[1].anchor;
    endpoint.role = CanvasVectorEndpointRole::Join;
    endpoint.hovered = true;
    canvas.setVectorPathEndpointMarkers({endpoint});
    QCOMPARE(canvas.viewport()->cursor().shape(), Qt::PointingHandCursor);

    canvas.setVectorPathEndpointMarkers({});
    canvas.clearVectorPathOverlay();
    QCOMPARE(canvas.viewport()->cursor().shape(), Qt::ArrowCursor);
}

void CanvasTests::vectorPathToolsAllowMiddleButtonPanning()
{
    ImageCanvas canvas;
    canvas.setRulersVisible(false);
    canvas.resize(420, 300);
    QImage image(1400, 1000, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    canvas.setImage(image, image.size());
    canvas.setZoom(1.0);
    canvas.setVectorPathEditingEnabled(true);
    canvas.show();
    QCoreApplication::processEvents();
    canvas.setScrollPosition(QPoint(320, 240));
    const QPoint before = canvas.scrollPosition();
    QVERIFY(before.x() > 0);
    QVERIFY(before.y() > 0);

    QSignalSpy vectorMoved(&canvas, &ImageCanvas::vectorPathPointerMoved);
    QWidget *viewport = canvas.viewport();
    const QPoint start(viewport->width() / 2, viewport->height() / 2);
    const QPoint finish = start + QPoint(47, 31);
    QTest::mousePress(viewport, Qt::MiddleButton, Qt::NoModifier, start);
    sendMiddleDragMove(viewport, finish);
    QTest::mouseRelease(viewport, Qt::MiddleButton, Qt::NoModifier, finish);

    const QPoint after = canvas.scrollPosition();
    QCOMPARE(after, before - QPoint(47, 31));
    QCOMPARE(vectorMoved.count(), 0);
}

QTEST_MAIN(CanvasTests)

#include "test_canvas.moc"
